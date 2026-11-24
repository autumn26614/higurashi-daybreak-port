"""Minimal 32-bit PE reader for the recompiler: headers, sections, imports, exports,
base relocations. Pure Python, no third-party modules."""
import struct


class Section:
    def __init__(self, name, va, vsize, raw_off, raw_size, flags):
        self.name, self.va, self.vsize = name, va, vsize
        self.raw_off, self.raw_size, self.flags = raw_off, raw_size, flags

    @property
    def executable(self):
        return bool(self.flags & 0x20000000)

    def __repr__(self):
        return '%s va=%#x vsize=%#x raw=%#x+%#x flags=%#x' % (self.name, self.va, self.vsize, self.raw_off,
                                                           self.raw_size, self.flags)


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        self.machine, nsec, _, _, _, opt_size, self.characteristics = struct.unpack_from('<HHIIIHH', d, pe + 4)
        opt = pe + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        if magic != 0x10B:
            raise ValueError('not PE32')
        self.entry_rva = struct.unpack_from('<I', d, opt + 16)[0]
        self.image_base = struct.unpack_from('<I', d, opt + 28)[0]
        self.image_size = struct.unpack_from('<I', d, opt + 56)[0]
        ndirs = struct.unpack_from('<I', d, opt + 92)[0]
        self.dirs = [struct.unpack_from('<II', d, opt + 96 + i * 8) for i in range(min(ndirs, 16))]
        self.sections = []
        for i in range(nsec):
            o = opt + opt_size + i * 40
            name = d[o:o + 8].split(b'\0')[0].decode('latin1')
            vsize, va, raw_size, raw_off = struct.unpack_from('<IIII', d, o + 8)
            flags = struct.unpack_from('<I', d, o + 36)[0]
            self.sections.append(Section(name, va, vsize, raw_off, raw_size, flags))

    # -- address helpers (RVA-based) --
    def section_of(self, rva):
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                return s
        return None

    def read(self, rva, n):
        s = self.section_of(rva)
        if not s:
            raise KeyError('rva %#x outside the image' % rva)
        off = rva - s.va
        out = self.data[s.raw_off + off:s.raw_off + min(off + n, s.raw_size)]
        return out + b'\0' * (n - len(out))  # virtual (bss) part reads as zero

    def u32(self, rva):
        return struct.unpack('<I', self.read(rva, 4))[0]

    def cstr(self, rva, limit=512):
        return self.read(rva, limit).split(b'\0')[0].decode('latin1')

    def image(self):
        """The mapped image as bytes (size image_size), as the loader lays it out."""
        img = bytearray(self.image_size)
        img[:0x1000] = self.data[:0x1000]
        for s in self.sections:
            raw = self.data[s.raw_off:s.raw_off + min(s.raw_size, s.vsize or s.raw_size)]
            img[s.va:s.va + len(raw)] = raw
        return bytes(img)

    # -- directories --
    def imports(self):
        """[(dll, [(iat_rva, name or ordinal)])]"""
        rva, size = self.dirs[1]
        out = []
        while rva and size:
            olt, _, _, name_rva, iat = struct.unpack('<IIIII', self.read(rva, 20))
            if not (olt or name_rva or iat):
                break
            dll = self.cstr(name_rva)
            funcs = []
            lookup = olt or iat
            i = 0
            while True:
                entry = self.u32(lookup + i * 4)
                if entry == 0:
                    break
                if entry & 0x80000000:
                    funcs.append((iat + i * 4, entry & 0xFFFF))
                else:
                    funcs.append((iat + i * 4, self.cstr(entry + 2)))
                i += 1
            out.append((dll, funcs))
            rva += 20
        return out

    def exports(self):
        """[(rva, name or None, ordinal)]"""
        rva, size = self.dirs[0]
        if not rva:
            return []
        _, _, _, _, _, base, nfunc, nnames, funcs, names, ords = struct.unpack('<IIHHIIIIIII', self.read(rva, 40))
        by_ord = {}
        for i in range(nnames):
            nrva = self.u32(names + i * 4)
            o = struct.unpack('<H', self.read(ords + i * 2, 2))[0]
            by_ord[o] = self.cstr(nrva)
        out = []
        for i in range(nfunc):
            f = self.u32(funcs + i * 4)
            if f:
                out.append((f, by_ord.get(i), base + i))
        return out

    def relocations(self):
        """RVAs of 32-bit absolute fixups (type 3)."""
        rva, size = self.dirs[5]
        out = []
        end = rva + size
        while rva < end:
            page, block = struct.unpack('<II', self.read(rva, 8))
            if block < 8:
                break
            for i in range((block - 8) // 2):
                e = struct.unpack('<H', self.read(rva + 8 + i * 2, 2))[0]
                if e >> 12 == 3:
                    out.append(page + (e & 0xFFF))
            rva += block
        return out


if __name__ == '__main__':
    import sys
    p = PE(sys.argv[1])
    print('base %#x entry %#x size %#x machine %#x' % (p.image_base, p.image_base + p.entry_rva, p.image_size,
                                                    p.machine))
    for s in p.sections:
        print(' ', s)
    print('relocations:', len(p.relocations()))
    ex = p.exports()
    print('exports:', len(ex))
    for dll, funcs in p.imports():
        print('%s: %d' % (dll, len(funcs)))
        if '-v' in sys.argv:
            print('   ', ', '.join(str(f[1]) for f in funcs))
