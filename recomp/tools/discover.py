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


def discover(img, extra_seeds=()):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    insns = {}          # address -> capstone insn
    functions = set()
    jump_tables = {}    # jmp address -> [targets]
    indirect_calls = 0
    indirect_jumps = 0

    seeds = [img.base + img.pe.entry_rva]
    seeds += [img.base + rva for rva, _, _ in img.pe.exports()]
    seeds += list(extra_seeds)
    # Pointers into the code from the data sections: vtables, callbacks, jump tables.
    data_ptrs = set()
    for s in img.pe.sections:
        if s.executable:
            continue
        for off in range(0, s.vsize - 3, 4):
            v = img.u32(img.base + s.va + off)
            if v is not None and img.in_code(v):
                data_ptrs.add(v)
    functions.update(seeds)            # strong: entry, exports, direct call targets (added below)
    seeds += sorted(data_ptrs)         # weak: code pointers found in data (vtables, callbacks)

    work = list(seeds)
    seen_blocks = set()
    while work:
        a = work.pop()
        if a in seen_blocks or not img.in_code(a):
            continue
        seen_blocks.add(a)
        while True:
            if a in insns:
                break
            r = a - img.base
            try:
                ins = next(md.disasm(img.mem[r:r + 16], a, 1))
            except StopIteration:
                break  # undecodable: data in code
            insns[a] = ins
            g = set(ins.groups)
            nxt = a + ins.size
            if not (X.X86_GRP_CALL in g or X.X86_GRP_JUMP in g):
                # Code addresses in immediates (`push offset fn`, `mov [x], offset fn`): the exe
                # has no relocations, so these are the other way functions get referenced.
                for op in ins.operands:
                    if op.type == X.X86_OP_IMM:
                        v = op.imm & 0xFFFFFFFF
                        if img.in_code(v) and v not in functions and v not in data_ptrs:
                            data_ptrs.add(v)
                            work.append(v)
            if X.X86_GRP_CALL in g:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    functions.add(op.imm)
                    work.append(op.imm)
                else:
                    indirect_calls += 1
                a = nxt
                continue
            if X.X86_GRP_RET in g or ins.mnemonic in ('hlt', 'int3'):
                break
            if X.X86_GRP_JUMP in g:
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    work.append(op.imm)
                    if ins.mnemonic == 'jmp':
                        break
                    a = nxt
                    continue
                # jmp dword ptr [reg*4 + table]: MSVC switch
                if op.type == X.X86_OP_MEM and op.mem.scale == 4 and op.mem.base == 0 and op.mem.index != 0:
                    table = op.mem.disp & 0xFFFFFFFF
                    targets = []
                    t = table
                    while True:
                        v = img.u32(t)
                        if v is None or not img.in_code(v) or (targets and t in functions):
                            break
                        targets.append(v)
                        t += 4
                        if len(targets) > 4096:
                            break
                    jump_tables[a] = targets
                    work.extend(targets)
                else:
                    indirect_jumps += 1
                break
            a = nxt
    weak = data_ptrs - functions
    return insns, functions, weak, jump_tables, indirect_calls, indirect_jumps


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
