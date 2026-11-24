"""Function discovery and instruction survey for the static recompiler.

Recursive descent from the entry point, the exports and every pointer into the code found in
the data sections (vtables, callbacks), following direct calls and jumps and the MSVC jump
tables (`jmp dword ptr [reg*4 + table]`). Prints coverage and which instruction kinds occur.

  PYTHONPATH=~/.local/daybreak-py python3 -I discover.py <pe> [--base 0xADDR] [--json out.json]
"""
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(Path.home() / '.local' / 'daybreak-py'))  # capstone (python -I ignores PYTHONPATH)
import capstone  # noqa: E402
from capstone import x86 as X  # noqa: E402
from pe import PE  # noqa: E402


class Image:
    def __init__(self, pe, base=None):
        self.pe = pe
        self.base = base if base is not None else pe.image_base
        img = bytearray(pe.image())
        delta = self.base - pe.image_base
        if delta:
            for r in pe.relocations():
                v = struct.unpack_from('<I', img, r)[0]
                struct.pack_into('<I', img, r, (v + delta) & 0xFFFFFFFF)
        self.mem = bytes(img)
        self.code = [s for s in pe.sections if s.executable]
        self.imports = {}  # IAT address -> (dll, name)
        for dll, funcs in pe.imports():
            for iat, name in funcs:
                self.imports[self.base + iat] = (dll, name)

    def in_code(self, a):
        r = a - self.base
        return any(s.va <= r < s.va + s.vsize for s in self.code)

    def u32(self, a):
        r = a - self.base
        return struct.unpack_from('<I', self.mem, r)[0] if 0 <= r <= len(self.mem) - 4 else None


# Instructions compiled user-mode code does not contain: a candidate entry that reaches one
# is data that only looks like code.
BAD = {'in', 'out', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw', 'arpl', 'bound', 'daa', 'das', 'aaa',
       'aas', 'aam', 'aad', 'popal', 'pushal', 'popaw', 'pushaw', 'les', 'lds', 'into', 'iretd', 'iret', 'hlt',
       'sti', 'cli', 'int1', 'salc', 'retf', 'ljmp', 'lcall', 'sldt', 'str', 'enter', 'int', 'xlatb', 'std',
       'loop', 'loope', 'loopne', 'jecxz'}


class Explorer:
    def __init__(self, img):
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def decode(self, a):
        if a in self.cache:
            return self.cache[a]
        r = a - self.img.base
        try:
            ins = next(self.md.disasm(self.img.mem[r:r + 16], a, 1))
        except StopIteration:
            ins = None
        self.cache[a] = ins
        return ins

    def explore(self, start, known, strict=True):
        """Follows the code reachable from `start` (not into addresses already in `known`).
        Returns (ok, insns, call targets, immediate code pointers, jump tables, indirect calls,
        indirect jumps); ok is False when it reaches undecodable bytes or BAD instructions."""
        img = self.img
        insns, calls, imms, tables = {}, set(), set(), {}
        icalls = ijumps = 0
        work = [start]
        seen = set()
        while work:
            a = work.pop()
            if a in seen or a in known or not img.in_code(a):
                continue
            seen.add(a)
            while True:
                if a in insns or a in known:
                    break
                ins = self.decode(a)
                if ins is None:
                    if strict:
                        return False, None, None, None, None, 0, 0
                    break
                m = ins.mnemonic.split(' ')[-1] if ins.mnemonic.startswith(('rep', 'lock')) else ins.mnemonic
                if strict and m in BAD:
                    return False, None, None, None, None, 0, 0
                for op in (ins.operands if strict else ()):
                    if op.type == X.X86_OP_MEM and op.mem.segment and ins.reg_name(op.mem.segment) not in ('fs', 'ds', 'ss', 'es'):
                        return False, None, None, None, None, 0, 0
                insns[a] = ins
                g = set(ins.groups)
                nxt = a + ins.size
                if X.X86_GRP_CALL in g:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        if img.in_code(op.imm):
                            calls.add(op.imm)
                        elif strict:
                            return False, None, None, None, None, 0, 0
                    else:
                        icalls += 1
                    a = nxt
                    continue
                if not (X.X86_GRP_JUMP in g):
                    # Code addresses in immediates (`push offset fn`): the exe has no relocations.
                    for op in ins.operands:
                        if op.type == X.X86_OP_IMM:
                            v = op.imm & 0xFFFFFFFF
                            if img.in_code(v):
                                imms.add(v)
                if X.X86_GRP_RET in g or m in ('int3',):
                    break
                if X.X86_GRP_JUMP in g:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        if not img.in_code(op.imm) and strict:
                            return False, None, None, None, None, 0, 0
                        work.append(op.imm)
                        if ins.mnemonic == 'jmp':
                            break
                        a = nxt
                        continue
                    # jmp dword ptr [reg*4 + table]: MSVC switch
                    if op.type == X.X86_OP_MEM and op.mem.scale == 4 and op.mem.base == 0 and op.mem.index != 0:
                        t = op.mem.disp & 0xFFFFFFFF
                        targets = []
                        while len(targets) < 4096:
                            v = img.u32(t)
                            if v is None or not img.in_code(v):
                                break
                            targets.append(v)
                            t += 4
                        tables[a] = targets
                        work.extend(targets)
                    else:
                        ijumps += 1
                    break
                a = nxt
        return True, insns, calls, imms, tables, icalls, ijumps


def discover(img, extra_seeds=()):
    ex = Explorer(img)
    insns = {}
    functions = set()   # strong: entry, exports, direct call targets
    weak = set()        # accepted code pointers from data, relocations and immediates
    jump_tables = {}
    counts = [0, 0]

    strong_work = [img.base + img.pe.entry_rva] + [img.base + rva for rva, _, _ in img.pe.exports()]
    strong_work += list(extra_seeds)
    candidates = set()
    for s in img.pe.sections:
        if s.executable:
            continue
        for off in range(0, s.vsize - 3, 4):
            v = img.u32(img.base + s.va + off)
            if v is not None and img.in_code(v):
                candidates.add(v)
    # Every absolute pointer a DLL holds is listed in its relocations, wherever it lives
    # (d3dx9 keeps its read-only tables inside .text).
    for r in img.pe.relocations():
        v = img.u32(img.base + r)
        if v is not None and img.in_code(v):
            candidates.add(v)

    def take(start, strong):
        ok, got, calls, imms, tables, ic, ij = ex.explore(start, insns, strict=not strong)
        if not ok:
            return False
        insns.update(got)
        jump_tables.update(tables)
        counts[0] += ic
        counts[1] += ij
        (functions if strong else weak).add(start)
        strong_work.extend(calls)
        candidates.update(imms)
        return True

    rejected = set()
    while strong_work or candidates:
        while strong_work:
            a = strong_work.pop()
            if a in functions:
                continue
            if a in insns:  # already decoded as part of other code: just mark it an entry
                functions.add(a)
                continue
            if not take(a, True):
                functions.add(a)  # a call target we cannot decode: keep it so calls trap clearly
        case_targets = {t for ts in jump_tables.values() for t in ts}
        batch = sorted(candidates - weak - functions - rejected)
        candidates.clear()
        for a in batch:
            if a in insns:  # an instruction of code already found: an entry too, unless a switch case
                if a not in case_targets:
                    weak.add(a)
                continue
            if not take(a, False):
                rejected.add(a)
        candidates -= weak | functions | rejected
    weak -= functions
    return insns, functions, weak, jump_tables, counts[0], counts[1]


def main():
    args = sys.argv[1:]
    base = None
    out = None
    if '--base' in args:
        base = int(args[args.index('--base') + 1], 16)
    if '--json' in args:
        out = args[args.index('--json') + 1]
    img = Image(PE(args[0]), base)
    insns, functions, weak, jt, icalls, ijumps = discover(img)
    print('weak entries (code pointers in data): %d' % len(weak))
    code_bytes = sum(s.vsize for s in img.code)
    covered = sum(i.size for i in insns.values())
    print('code %d bytes, decoded %d instructions covering %d bytes (%.1f%%)' % (
        code_bytes, len(insns), covered, 100.0 * covered / code_bytes))
    print('functions (seeds and call targets): %d; jump tables: %d (%d targets); indirect calls %d, '
          'other indirect jumps %d' % (len(functions), len(jt), sum(len(v) for v in jt.values()), icalls, ijumps))
    mnem = collections.Counter(i.mnemonic for i in insns.values())
    print('distinct mnemonics: %d' % len(mnem))
    for m, c in sorted(mnem.items(), key=lambda kv: -kv[1]):
        print('  %-12s %d' % (m, c))
    prefixes = collections.Counter()
    seg = 0
    for i in insns.values():
        if i.prefix[0] in (0xF2, 0xF3):
            prefixes['rep' if i.prefix[0] == 0xF3 else 'repne'] += 1
        if i.prefix[1] in (0x64, 0x65):
            seg += 1
    print('rep prefixes', dict(prefixes), 'fs/gs accesses', seg)
    if out:
        json.dump({'functions': sorted(functions), 'jump_tables': {str(k): v for k, v in jt.items()}},
                  open(out, 'w'))


if __name__ == '__main__':
    main()
