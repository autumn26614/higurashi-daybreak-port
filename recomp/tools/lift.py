"""Translate a 32-bit PE's code to C (static recompilation; see recomp/README.md).

  python3 -I lift.py <pe> <out dir> [--base 0xADDR] [--prefix F] [--per-file N]

Writes <out dir>/<prefix>_NNN.c (the functions), <prefix>_table.c (address -> function, for
indirect calls) and <prefix>.h. The output is derived from the input program, so it stays
out of the repository (recomp/gen/ is git-ignored).
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(Path.home() / '.local' / 'daybreak-py'))
from capstone import x86 as X  # noqa: E402
from discover import Image, discover  # noqa: E402
from pe import PE  # noqa: E402

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
CC = {'o': 'O', 'no': 'NO', 'b': 'B', 'c': 'B', 'nae': 'B', 'ae': 'AE', 'nb': 'AE', 'nc': 'AE',
      'e': 'E', 'z': 'E', 'ne': 'NE', 'nz': 'NE', 'be': 'BE', 'na': 'BE', 'a': 'A', 'nbe': 'A',
      's': 'S', 'ns': 'NS', 'p': 'P', 'pe': 'P', 'np': 'NP', 'po': 'NP', 'l': 'L', 'nge': 'L',
      'ge': 'GE', 'nl': 'GE', 'le': 'LE', 'ng': 'LE', 'g': 'G', 'nle': 'G'}
PREFIX_BYTES = {0x9B, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3}


class Unsupported(Exception):
    pass


def hx(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


class Lifter:
    def __init__(self, img, insns, strong, weak, jump_tables, prefix):
        self.img, self.insns, self.strong, self.weak = img, insns, strong, weak
        self.jt, self.prefix = jump_tables, prefix
        self.entries = strong | weak

    def fname(self, a):
        return '%s_%08X' % (self.prefix, a)

    # ---- operands ----
    def reg_read(self, ins, r):
        n = ins.reg_name(r)
        if n in REG32:
            return n
        if n in REG16:
            return '(%s & 0xFFFFu)' % REG16[n]
        if n in REG8L:
            return '(%s & 0xFFu)' % REG8L[n]
        if n in REG8H:
            return '((%s >> 8) & 0xFFu)' % REG8H[n]
        raise Unsupported('register ' + n)

    def reg_write(self, ins, r, v):
        n = ins.reg_name(r)
        if n in REG32:
            return '%s = %s;' % (n, v)
        if n in REG16:
            g = REG16[n]
            return '%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % (g, g, v)
        if n in REG8L:
            g = REG8L[n]
            return '%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFFu);' % (g, g, v)
        if n in REG8H:
            g = REG8H[n]
            return '%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);' % (g, g, v)
        raise Unsupported('register ' + n)

    def addr(self, ins, op, lea=False):
        m = op.mem
        parts = []
        if m.segment and not lea:
            sn = ins.reg_name(m.segment)
            if sn == 'fs':
                parts.append('c->fs_base')
            elif sn not in ('ds', 'ss', 'es', 'cs'):
                raise Unsupported('segment ' + sn)
        if m.base:
            b = ins.reg_name(m.base)
            if b not in REG32:
                raise Unsupported('base ' + b)
            parts.append(b)
        if m.index:
            i = ins.reg_name(m.index)
            if i not in REG32:
                raise Unsupported('index ' + i)
            parts.append(i if m.scale == 1 else '%s * %du' % (i, m.scale))
        d = m.disp & 0xFFFFFFFF
        if d or not parts:
            if parts and d >= 0x80000000:
                return '(%s - %s)' % (' + '.join(parts), hx(-d & 0xFFFFFFFF))
            parts.append(hx(d))
        return '(uint32_t)(%s)' % ' + '.join(parts) if len(parts) > 1 else parts[0]

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(ins, op.reg)
        if op.type == X.X86_OP_IMM:
            return hx(op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return 'ld%d(%s)' % (8 * size, self.addr(ins, op))
        raise Unsupported('operand')

    def wr(self, ins, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(ins, op.reg, v)
        if op.type == X.X86_OP_MEM:
            return 'st%d(%s, %s);' % (8 * size, self.addr(ins, op), v)
        raise Unsupported('write to operand')

    # ---- control flow ----
    def target_code(self, fn_entry, t, labels):
        """Statement that transfers control to direct target t."""
        if t in self.strong and t != fn_entry:
            return 'OUT; %s(c); return;' % self.fname(t)
        if t in self.insns_of_fn:
            labels.add(t)
            return 'goto L_%08X;' % t
        if self.img.in_code(t):  # not decoded: trap at run time
            return 'OUT; rt_trap(c, %s, "undecoded jump target"); return;' % hx(t)
        return 'OUT; rt_icall(c, %s); return;' % hx(t)

    def collect(self, entry):
        """Instructions of the function at entry: everything reachable without crossing into
        another strong function."""
        body = {}
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            while True:
                if a != entry and a in self.strong:
                    break
                ins = self.insns.get(a)
                if ins is None:
                    break
                if a in body:
                    break
                body[a] = ins
                g = set(ins.groups)
                nxt = a + ins.size
                if X.X86_GRP_RET in g or ins.mnemonic in ('hlt', 'int3'):
                    break
                if X.X86_GRP_JUMP in g:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        work.append(op.imm)
                        if ins.mnemonic == 'jmp':
                            break
                    elif a in self.jt:
                        work.extend(self.jt[a])
                        break
                    else:
                        break
                a = nxt
        return body

    # ---- translation ----
    def function(self, entry):
        body = self.collect(entry)
        self.insns_of_fn = body
        labels = set()
        out = []
        addrs = sorted(body)
        for k, a in enumerate(addrs):
            ins = body[a]
            try:
                code = self.insn(entry, ins, labels)
            except Unsupported as e:
                code = ['OUT; rt_trap(c, %s, "%s: %s"); return;' % (hx(a), ins.mnemonic, e)]
            out.append((a, code))
            g = set(ins.groups)
            ends = (X.X86_GRP_RET in g or ins.mnemonic in ('jmp', 'hlt', 'int3', 'ljmp')
                    or (X.X86_GRP_JUMP in g and ins.mnemonic == 'jmp'))
            if not ends:
                nxt = a + ins.size
                if k + 1 < len(addrs) and addrs[k + 1] == nxt:
                    continue
                out.append((None, [self.target_code(entry, nxt, labels)]))
        lines = ['void %s(CPU *c) {' % self.fname(entry), '    REGS;']
        if addrs and addrs[0] != entry:  # blocks below the entry come first in the text
            labels.add(entry)
            lines.append('    goto L_%08X;' % entry)
        for a, code in out:
            if a is not None and a in labels:
                lines.append('L_%08X:;' % a)
            lines.extend('    ' + s for s in code)
        lines.append('}')
        return '\n'.join(lines)

    def cc(self, mnem):
        return 'CC_%s(f)' % CC[mnem]

    def insn(self, entry, ins, labels):
        m = ins.mnemonic
        if m.startswith('lock '):
            m = m[5:]  # guest threads run on host threads; refcount updates are rare and short
        ops = ins.operands
        a = ins.address
        nxt = a + ins.size
        if m == 'cpuid':
            return ['OUT; rt_cpuid(c); IN;']
        if m == 'rdtsc':
            return ['OUT; rt_rdtsc(c); IN;']
        if m.startswith('f') or m in ('wait', 'fwait'):
            return self.x87(ins)
        if m == 'mov':
            return [self.wr(ins, ops[0], self.rd(ins, ops[1]))]
        if m == 'movzx':
            return [self.wr(ins, ops[0], self.rd(ins, ops[1]))]
        if m == 'movsx':
            t = {1: 'int8_t', 2: 'int16_t'}[ops[1].size]
            return [self.wr(ins, ops[0], '(uint32_t)(int32_t)(%s)%s' % (t, self.rd(ins, ops[1])))]
        if m == 'lea':
            return [self.wr(ins, ops[0], self.addr(ins, ops[1], lea=True))]
        if m == 'push':
            if ops[0].size == 2:
                return ['{ uint32_t v = %s; esp -= 2; st16(esp, v); }' % self.rd(ins, ops[0])]
            return ['{ uint32_t v = %s; esp -= 4; st32(esp, v); }' % self.rd(ins, ops[0], 4)]
        if m == 'pop':
            if ops[0].size == 2:
                return ['{ uint32_t v = ld16(esp); esp += 2; %s }' % self.wr(ins, ops[0], 'v')]
            return ['{ uint32_t v = ld32(esp); esp += 4; %s }' % self.wr(ins, ops[0], 'v')]
        if m == 'xchg':
            return ['{ uint32_t x = %s, y = %s; %s %s }' % (self.rd(ins, ops[0]), self.rd(ins, ops[1]),
                                                           self.wr(ins, ops[0], 'y'), self.wr(ins, ops[1], 'x'))]
        if m in ('add', 'sub', 'adc', 'sbb', 'cmp'):
            w = ops[0].size
            fn = {'add': 'x_add', 'sub': 'x_sub', 'adc': 'x_adc', 'sbb': 'x_sbb', 'cmp': 'x_sub'}[m]
            e = '%s(&f, %s, %s, %d)' % (fn, self.rd(ins, ops[0]), self.rd(ins, ops[1], w), w)
            if m == 'cmp':
                return ['(void)%s;' % e]
            return ['{ uint32_t r = %s; %s }' % (e, self.wr(ins, ops[0], 'r'))]
        if m in ('and', 'or', 'xor', 'test'):
            w = ops[0].size
            sym = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[m]
            if m == 'xor' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return ['%s (void)x_logic(&f, 0, %d);' % (self.wr(ins, ops[0], '0'), w)]
            e = 'x_logic(&f, %s %s %s, %d)' % (self.rd(ins, ops[0]), sym, self.rd(ins, ops[1], w), w)
            if m == 'test':
                return ['(void)%s;' % e]
            return ['{ uint32_t r = %s; %s }' % (e, self.wr(ins, ops[0], 'r'))]
        if m in ('inc', 'dec', 'neg'):
            w = ops[0].size
            return ['{ uint32_t r = x_%s(&f, %s, %d); %s }' % (m, self.rd(ins, ops[0]), w, self.wr(ins, ops[0], 'r'))]
        if m == 'not':
            return [self.wr(ins, ops[0], '~%s' % self.rd(ins, ops[0]))]
        if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            w = ops[0].size
            n = self.rd(ins, ops[1], 1) if len(ops) > 1 else '1u'
            fn = 'x_shl' if m == 'sal' else 'x_' + m
            return ['{ uint32_t r = %s(&f, %s, (%s) & 31u, %d); %s }' % (fn, self.rd(ins, ops[0]), n, w,
                                                                         self.wr(ins, ops[0], 'r'))]
        if m in ('shld', 'shrd'):
            if ops[0].size != 4:
                raise Unsupported('16-bit ' + m)
            return ['{ uint32_t r = x_%s(&f, %s, %s, (%s) & 31u); %s }' % (
                m, self.rd(ins, ops[0]), self.rd(ins, ops[1]), self.rd(ins, ops[2], 1), self.wr(ins, ops[0], 'r'))]
        if m == 'imul':
            if len(ops) == 1:
                return self.mul1(ins, signed=True)
            if len(ops) == 2:
                x, y = self.rd(ins, ops[0]), self.rd(ins, ops[1])
            else:
                x, y = self.rd(ins, ops[1]), self.rd(ins, ops[2], ops[0].size)
            return ['{ uint32_t r = x_imul(&f, %s, %s, %d); %s }' % (x, y, ops[0].size, self.wr(ins, ops[0], 'r'))]
        if m == 'mul':
            return self.mul1(ins, signed=False)
        if m in ('div', 'idiv'):
            return self.div(ins, m == 'idiv')
        if m == 'cdq':
            return ['edx = (uint32_t)((int32_t)eax >> 31);']
        if m == 'cwde':
            return ['eax = (uint32_t)(int32_t)(int16_t)eax;']
        if m == 'cbw':
            return ['eax = (eax & 0xFFFF0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xFFFFu);']
        if m == 'cwd':
            return ['edx = (edx & 0xFFFF0000u) | (((int16_t)eax < 0) ? 0xFFFFu : 0u);']
        if m.startswith('set') and m[3:] in CC:
            return [self.wr(ins, ops[0], '(uint32_t)(%s)' % self.cc(m[3:]))]
        if m.startswith('cmov') and m[4:] in CC:
            return ['if (%s) { %s }' % (self.cc(m[4:]), self.wr(ins, ops[0], self.rd(ins, ops[1])))]
        if m == 'sahf':
            return ['fl_unpack(&f, (eax >> 8) & 0xFFu);']
        if m == 'lahf':
            return ['eax = (eax & 0xFFFF00FFu) | ((fl_pack(&f) & 0xFFu) << 8);']
        if m == 'pushfd':
            return ['esp -= 4; st32(esp, fl_pack(&f));']
        if m == 'popfd':
            return ['fl_unpack(&f, ld32(esp)); esp += 4;']
        if m in ('clc', 'stc', 'cmc'):
            return [{'clc': 'f.cf = 0;', 'stc': 'f.cf = 1;', 'cmc': 'f.cf = !f.cf;'}[m]]
        if m == 'nop':
            return []
        if m in ('cld', 'std'):
            return ['df = %d;' % (m == 'std')]
        if m == 'leave':
            return ['esp = ebp; ebp = ld32(esp); esp += 4;']
        if m.startswith('rep') or (m[:-1] in self.STRING_OPS and m[-1] in 'bwd'):
            return self.string(ins, m)
        if m.startswith('j') and m != 'jmp' and m[1:] in CC:
            return ['if (%s) { %s }' % (self.cc(m[1:]), self.target_code(entry, ops[0].imm, labels))]
        if m == 'jecxz':
            return ['if (ecx == 0) { %s }' % self.target_code(entry, ops[0].imm, labels)]
        if m in ('loop', 'loope', 'loopne'):
            cond = {'loop': 'ecx', 'loope': 'ecx && f.zf', 'loopne': 'ecx && !f.zf'}[m]
            return ['ecx--; if (%s) { %s }' % (cond, self.target_code(entry, ops[0].imm, labels))]
        if m == 'jmp':
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return [self.target_code(entry, op.imm, labels)]
            if a in self.jt:
                cases = []
                for t in sorted(set(self.jt[a])):
                    labels.add(t)
                    cases.append('case %s: goto L_%08X;' % (hx(t), t))
                return ['{ uint32_t t = %s; switch (t) { %s default: OUT; rt_icall(c, t); return; } }' % (
                    self.rd(ins, op, 4), ' '.join(cases))]
            return ['{ uint32_t t = %s; OUT; rt_icall(c, t); return; }' % self.rd(ins, op, 4)]
        if m == 'call':
            op = ops[0]
            push = 'esp -= 4; st32(esp, %s);' % hx(nxt)
            if op.type == X.X86_OP_IMM:
                t = op.imm
                if t in self.entries and t in self.insns:
                    return ['%s OUT; %s(c); IN;' % (push, self.fname(t))]
                return ['%s OUT; rt_icall(c, %s); IN;' % (push, hx(t))]
            return ['{ uint32_t t = %s; %s OUT; rt_icall(c, t); IN; }' % (self.rd(ins, op, 4), push)]
        if m == 'ret':
            n = ops[0].imm if ops else 0
            return ['esp += %du; OUT; return;' % (4 + n)]
        raise Unsupported('instruction')

    def mul1(self, ins, signed):
        op = ins.operands[0]
        w = op.size
        s = self.rd(ins, op)
        if w == 4:
            if signed:
                p = '(uint64_t)((int64_t)(int32_t)eax * (int32_t)%s)' % s
                chk = '(int64_t)p != (int64_t)(int32_t)(uint32_t)p'
            else:
                p = '(uint64_t)eax * %s' % s
                chk = '(p >> 32) != 0'
            return ['{ uint64_t p = %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); f.cf = f.of = %s; '
                    'fl_res(&f, eax, 4); }' % (p, chk)]
        if w == 2:
            if signed:
                p = '(uint32_t)((int32_t)(int16_t)eax * (int16_t)%s)' % s
                chk = '(int32_t)p != (int32_t)(int16_t)p'
            else:
                p = '(eax & 0xFFFFu) * %s' % s
                chk = '(p >> 16) != 0'
            return ['{ uint32_t p = %s; eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); edx = (edx & 0xFFFF0000u) | '
                    '(p >> 16); f.cf = f.of = %s; fl_res(&f, p, 2); }' % (p, chk)]
        if signed:
            p = '(uint32_t)((int32_t)(int8_t)eax * (int8_t)%s) & 0xFFFFu' % s
            chk = '(int16_t)p != (int8_t)p'
        else:
            p = '(eax & 0xFFu) * %s' % s
            chk = '(p >> 8) != 0'
        return ['{ uint32_t p = %s; eax = (eax & 0xFFFF0000u) | p; f.cf = f.of = %s; fl_res(&f, p, 1); }' % (p, chk)]

    def div(self, ins, signed):
        op = ins.operands[0]
        w = op.size
        s = self.rd(ins, op)
        trap = 'if (d == 0) { OUT; rt_trap(c, %s, "divide by zero"); return; }' % hx(ins.address)
        if w == 4:
            if signed:
                return ['{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int64_t d = (int32_t)%s; %s '
                        'eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % (s, trap)]
            return ['{ uint64_t n = ((uint64_t)edx << 32) | eax; uint64_t d = %s; %s '
                    'eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % (s, trap)]
        if w == 2:
            if signed:
                return ['{ int32_t n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); int32_t d = (int16_t)%s; '
                        '%s eax = (eax & 0xFFFF0000u) | ((uint32_t)(n / d) & 0xFFFFu); '
                        'edx = (edx & 0xFFFF0000u) | ((uint32_t)(n %% d) & 0xFFFFu); }' % (s, trap)]
            return ['{ uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); uint32_t d = %s; %s '
                    'eax = (eax & 0xFFFF0000u) | ((n / d) & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((n %% d) & 0xFFFFu); }'
                    % (s, trap)]
        if signed:
            return ['{ int32_t n = (int16_t)eax; int32_t d = (int8_t)%s; %s '
                    'eax = (eax & 0xFFFF0000u) | ((uint32_t)(n / d) & 0xFFu) | (((uint32_t)(n %% d) & 0xFFu) << 8); }'
                    % (s, trap)]
        return ['{ uint32_t n = eax & 0xFFFFu; uint32_t d = %s; %s '
                'eax = (eax & 0xFFFF0000u) | ((n / d) & 0xFFu) | (((n %% d) & 0xFFu) << 8); }' % (s, trap)]

    STRING_OPS = ('movs', 'stos', 'lods', 'scas', 'cmps')

    def string(self, ins, m):
        rep = ''
        base = m
        for p in ('repne ', 'repnz ', 'repe ', 'repz ', 'rep '):
            if m.startswith(p):
                rep, base = p.strip(), m[len(p):]
                break
        if any('xmm' in ins.reg_name(o.reg) for o in ins.operands if o.type == X.X86_OP_REG):
            raise Unsupported('SSE')
        if base[-1] not in 'bwd' or base[:-1] not in self.STRING_OPS:
            raise Unsupported('string op')
        w = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        bits = 8 * w
        d = '(df ? %s : %du)' % (hx(-w), w)
        acc = 'eax' if w == 4 else '(eax & %s)' % hx((1 << bits) - 1)
        op = base[:-1]
        if op == 'movs':
            step = 'st%d(edi, ld%d(esi)); esi += %s; edi += %s;' % (bits, bits, d, d)
        elif op == 'stos':
            step = 'st%d(edi, %s); edi += %s;' % (bits, acc, d)
        elif op == 'lods':
            reg = {1: 'al', 2: 'ax', 4: 'eax'}[w]
            step = '{ uint32_t v = ld%d(esi); esi += %s; %s }' % (bits, d, self.reg_write_name(reg, 'v'))
        elif op == 'scas':
            step = '(void)x_sub(&f, %s, ld%d(edi), %d); edi += %s;' % (acc, bits, w, d)
        else:
            step = '(void)x_sub(&f, ld%d(esi), ld%d(edi), %d); esi += %s; edi += %s;' % (bits, bits, w, d, d)
        if not rep:
            return [step]
        if op in ('scas', 'cmps'):
            stop = 'if (!f.zf) break;' if rep in ('rep', 'repe', 'repz') else 'if (f.zf) break;'
            return ['while (ecx) { %s ecx--; %s }' % (step, stop)]
        return ['while (ecx) { %s ecx--; }' % step]

    def reg_write_name(self, n, v):
        if n == 'eax':
            return 'eax = %s;' % v
        if n == 'ax':
            return 'eax = (eax & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % v
        return 'eax = (eax & 0xFFFFFF00u) | ((%s) & 0xFFu);' % v

    # ---- x87, decoded from the opcode bytes (mnemonics for the reversed forms vary) ----
    def x87(self, ins):
        b = bytes(ins.bytes)
        i = 0
        while i < len(b) and b[i] in PREFIX_BYTES:
            i += 1
        if i >= len(b) or not (0xD8 <= b[i] <= 0xDF):
            if ins.mnemonic in ('wait', 'fwait'):
                return []
            raise Unsupported('x87 opcode')
        op, modrm = b[i], b[i + 1]
        reg = (modrm >> 3) & 7
        if modrm < 0xC0:
            mem = [o for o in ins.operands if o.type == X.X86_OP_MEM]
            if not mem:
                raise Unsupported('x87 memory operand')
            A = self.addr(ins, mem[0])
            return [self.x87_mem(op, reg, A)]
        return [self.x87_reg(op, modrm)]

    ARITH = {0: '+', 1: '*', 4: '-', 5: 'r-', 6: '/', 7: 'r/'}

    def arith(self, kind, dst, src):
        # fpc() rounds to the precision the control word selects (Direct3D sets single).
        if kind == 'r-':
            return '%s = fpc(c, %s - %s);' % (dst, src, dst)
        if kind == 'r/':
            return '%s = fpc(c, %s / %s);' % (dst, src, dst)
        return '%s = fpc(c, %s %s %s);' % (dst, dst, kind, src)

    def x87_mem(self, op, reg, A):
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            src = {0xD8: 'ldf32(%s)', 0xDC: 'ldf64(%s)', 0xDA: '(double)(int32_t)ld32(%s)',
                   0xDE: '(double)(int16_t)ld16(%s)'}[op] % A
            if reg in self.ARITH:
                return self.arith(self.ARITH[reg], 'ST(0)', src)
            if reg == 2:
                return 'fcmp(c, ST(0), %s);' % src
            return 'fcmp(c, ST(0), %s); FPOP();' % src
        if op == 0xD9:
            return {0: 'FPUSH(ldf32(%s));', 2: 'stf32(%s, ST(0));', 3: 'stf32(%s, ST(0)); FPOP();',
                    5: 'c->fcw = (uint16_t)ld16(%s);', 7: 'st16(%s, c->fcw);'}.get(reg, None) % A \
                if reg in (0, 2, 3, 5, 7) else self.bad('D9 /%d' % reg)
        if op == 0xDB:
            if reg == 0:
                return 'FPUSH((double)(int32_t)ld32(%s));' % A
            if reg in (1, 2, 3):
                v = ('(uint32_t)(int32_t)fist(c, ST(0), INT32_MIN, INT32_MAX, INT32_MIN)' if reg != 1 else
                     '(uint32_t)(int32_t)(isnan(ST(0)) || fabs(ST(0)) >= 2147483648.0 ? INT32_MIN : (int32_t)ST(0))')
                return 'st32(%s, %s);%s' % (A, v, ' FPOP();' if reg != 2 else '')
            if reg == 5:
                return 'FPUSH(ldf80(%s));' % A
            if reg == 7:
                return 'stf80(%s, ST(0)); FPOP();' % A
            return self.bad('DB /%d' % reg)
        if op == 0xDD:
            if reg == 0:
                return 'FPUSH(ldf64(%s));' % A
            if reg == 2:
                return 'stf64(%s, ST(0));' % A
            if reg == 3:
                return 'stf64(%s, ST(0)); FPOP();' % A
            if reg == 7:
                return 'st16(%s, fsw_read(c));' % A
            return self.bad('DD /%d' % reg)
        if op == 0xDF:
            if reg == 0:
                return 'FPUSH((double)(int16_t)ld16(%s));' % A
            if reg in (2, 3):
                return 'st16(%s, (uint32_t)fist(c, ST(0), INT16_MIN, INT16_MAX, INT16_MIN));%s' % (
                    A, ' FPOP();' if reg == 3 else '')
            if reg == 5:
                return 'FPUSH((double)(int64_t)ld64(%s));' % A
            if reg == 7:
                return 'st64(%s, (uint64_t)fist(c, ST(0), INT64_MIN, INT64_MAX, INT64_MIN)); FPOP();' % A
            return self.bad('DF /%d' % reg)
        return self.bad('%02X /%d' % (op, reg))

    def bad(self, what):
        raise Unsupported('x87 ' + what)

    def x87_reg(self, op, modrm):
        i = modrm & 7
        reg = (modrm >> 3) & 7
        sti = 'ST(%d)' % i
        if op == 0xD8:
            if reg in self.ARITH:
                return self.arith(self.ARITH[reg], 'ST(0)', sti)
            return 'fcmp(c, ST(0), %s);%s' % (sti, ' FPOP();' if reg == 3 else '')
        if op == 0xDC:
            # DC: destination ST(i); E0 is fsubr (ST(i) = ST0 - ST(i)), E8 fsub, F0 fdivr, F8 fdiv.
            kinds = {0: '+', 1: '*', 4: 'r-', 5: '-', 6: 'r/', 7: '/'}
            if reg in kinds:
                return self.arith(kinds[reg], sti, 'ST(0)')
            return 'fcmp(c, ST(0), %s);%s' % (sti, ' FPOP();' if reg == 3 else '')
        if op == 0xDE:
            kinds = {0: '+', 1: '*', 4: 'r-', 5: '-', 6: 'r/', 7: '/'}
            if reg in kinds:
                return self.arith(kinds[reg], sti, 'ST(0)') + ' FPOP();'
            if modrm == 0xD9:
                return 'fcmp(c, ST(0), ST(1)); FPOP(); FPOP();'
            return self.bad('DE %02X' % modrm)
        if op == 0xD9:
            if reg == 0:
                return 'FPUSH(%s);' % sti
            if reg == 1:
                return '{ double t = ST(0); ST(0) = %s; %s = t; }' % (sti, sti)
            simple = {
                0xD0: '', 0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));', 0xE4: 'fcmp(c, ST(0), 0.0);',
                0xE5: 'fxam(c);', 0xE8: 'FPUSH(1.0);', 0xE9: 'FPUSH(3.321928094887362347870);',
                0xEA: 'FPUSH(1.442695040888963407360);', 0xEB: 'FPUSH(3.141592653589793238463);',
                0xEC: 'FPUSH(0.301029995663981195214);', 0xED: 'FPUSH(0.693147180559945309417);',
                0xEE: 'FPUSH(0.0);', 0xF0: 'ST(0) = exp2(ST(0)) - 1.0;',
                0xF1: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                0xF2: 'ST(0) = tan(ST(0)); FPUSH(1.0);',
                0xF3: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                0xF8: 'ST(0) = fmod(ST(0), ST(1));', 0xF5: 'ST(0) = remainder(ST(0), ST(1));',
                0xFA: 'ST(0) = fpc(c, sqrt(ST(0)));',
                0xFB: '{ double x = ST(0); ST(0) = sin(x); FPUSH(cos(x)); }',
                0xFC: 'ST(0) = fround(c, ST(0));', 0xFD: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                0xFE: 'ST(0) = sin(ST(0));', 0xFF: 'ST(0) = cos(ST(0));',
                0xF6: 'c->top = (c->top - 1) & 7;', 0xF7: 'c->top = (c->top + 1) & 7;',
            }
            if modrm in simple:
                if modrm in (0xF8, 0xF5):
                    return simple[modrm] + ' c->fsw &= ~0x0400;'  # reduction complete (C2 = 0)
                return simple[modrm]
            return self.bad('D9 %02X' % modrm)
        if op == 0xDA:
            if modrm == 0xE9:
                return 'fcmp(c, ST(0), ST(1)); FPOP(); FPOP();'
            conds = {0: 'f.cf', 1: 'f.zf', 2: '(f.cf || f.zf)', 3: 'CC_P(f)'}
            if reg in conds:
                return 'if (%s) ST(0) = %s;' % (conds[reg], sti)
            return self.bad('DA %02X' % modrm)
        if op == 0xDB:
            conds = {0: '!f.cf', 1: '!f.zf', 2: '!(f.cf || f.zf)', 3: 'CC_NP(f)'}
            if reg in conds:
                return 'if (%s) ST(0) = %s;' % (conds[reg], sti)
            if modrm in (0xE2, 0xE3):
                return 'c->fsw = 0;' if modrm == 0xE2 else 'c->fsw = 0; c->fcw = 0x37F; c->top = 0;'
            if reg in (5, 6):
                return 'fcmpi(&f, ST(0), %s);' % sti
            return self.bad('DB %02X' % modrm)
        if op == 0xDD:
            if reg == 0:
                return ''  # ffree: tags are not modelled
            if reg == 2:
                return '%s = ST(0);' % sti
            if reg == 3:
                return '%s = ST(0); FPOP();' % sti
            if reg in (4, 5):
                return 'fcmp(c, ST(0), %s);%s' % (sti, ' FPOP();' if reg == 5 else '')
            return self.bad('DD %02X' % modrm)
        if op == 0xDF:
            if modrm == 0xE0:
                return 'eax = (eax & 0xFFFF0000u) | fsw_read(c);'
            if reg in (5, 6):
                return 'fcmpi(&f, ST(0), %s); FPOP();' % sti
            return self.bad('DF %02X' % modrm)
        return self.bad('%02X %02X' % (op, modrm))


def main():
    args = sys.argv[1:]
    opt = {}
    pos = []
    k = 0
    while k < len(args):
        if args[k].startswith('--'):
            opt[args[k]] = args[k + 1]
            k += 2
        else:
            pos.append(args[k])
            k += 1
    pe_path, out_dir = pos
    base = int(opt['--base'], 16) if '--base' in opt else None
    prefix = opt.get('--prefix', 'F')
    per_file = int(opt.get('--per-file', '400'))
    img = Image(PE(pe_path), base)
    insns, strong, weak, jt, _, _ = discover(img)
    lifter = Lifter(img, insns, strong, weak, jt, prefix)
    entries = sorted(e for e in strong | weak if e in insns)
    os.makedirs(out_dir, exist_ok=True)
    traps = 0
    nfiles = 0
    for k in range(0, len(entries), per_file):
        chunk = entries[k:k + per_file]
        with open(os.path.join(out_dir, '%s_%03d.c' % (prefix, nfiles)), 'w') as fo:
            fo.write('/* Generated by recomp/tools/lift.py from %s. Do not edit or publish. */\n' %
                     os.path.basename(pe_path))
            fo.write('#include "x86.h"\n#include "%s.h"\n\n' % prefix)
            for e in chunk:
                text = lifter.function(e)
                traps += text.count('rt_trap(c, ') - text.count('"divide by zero"') - text.count('"undecoded jump')
                fo.write(text + '\n\n')
        nfiles += 1
    with open(os.path.join(out_dir, prefix + '.h'), 'w') as fo:
        fo.write('/* Generated. */\n#pragma once\n#include "x86.h"\n')
        for e in entries:
            fo.write('void %s(CPU *c);\n' % lifter.fname(e))
    with open(os.path.join(out_dir, prefix + '_table.c'), 'w') as fo:
        fo.write('/* Generated. Sorted guest address -> translated function. */\n#include "%s.h"\n' % prefix)
        fo.write('const uint32_t %s_count = %d;\n' % (prefix, len(entries)))
        fo.write('const struct { uint32_t addr; GuestFn fn; } %s_table[] = {\n' % prefix)
        for e in entries:
            fo.write('    {%s, %s},\n' % (hx(e), lifter.fname(e)))
        fo.write('};\n')
    with open(os.path.join(out_dir, prefix + '_exports.c'), 'w') as fo:
        fo.write('/* Generated. Exports: (name or NULL, ordinal, address). */\n#include "x86.h"\n')
        rows = ['    {%s, %d, %s},' % ('"%s"' % n if n else 'NULL', o, hx(img.base + rva))
                for rva, n, o in img.pe.exports()]
        fo.write('const uint32_t %s_export_count = %d;\n' % (prefix, len(rows)))
        fo.write('const ExportSlot %s_exports[] = {\n%s\n    {NULL, 0, 0}\n};\n' % (prefix, '\n'.join(rows)))
        fo.write('const uint32_t %s_base = %s;\n' % (prefix, hx(img.base)))
        fo.write('const char %s_source[] = "%s";  /* the file this was translated from */\n' %
                 (prefix, os.path.basename(pe_path)))
    with open(os.path.join(out_dir, prefix + '_imports.c'), 'w') as fo:
        fo.write('/* Generated. Import address table slots: (IAT address, DLL, name or #ordinal). */\n')
        fo.write('#include "x86.h"\n')
        rows = []
        for dll, funcs in img.pe.imports():
            for iat, name in funcs:
                n = name if isinstance(name, str) else '#%d' % name
                rows.append('    {%s, "%s", "%s"},' % (hx(img.base + iat), dll, n))
        fo.write('const uint32_t %s_import_count = %d;\n' % (prefix, len(rows)))
        fo.write('const ImportSlot %s_imports[] = {\n%s\n    {0, NULL, NULL}\n};\n' % (prefix, '\n'.join(rows)))
    print('%d functions in %d files; %d untranslated instructions (traps)' % (len(entries), nfiles, traps))


if __name__ == '__main__':
    main()
