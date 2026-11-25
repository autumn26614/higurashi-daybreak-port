"""Wraps a statically linked 32-bit ELF (from `ld -m elf_i386 -Ttext 0x401000`) into a minimal
PE32 image at the same addresses, exporting its CASE functions, for tools/lift.py.

  python3 -I mkpe.py <elf> <out.exe>
"""
import struct
import sys


def elf_sections(data):
    shoff = struct.unpack_from('<I', data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link))
    strtab = secs[shstrndx]
    for s in secs:
        o = strtab['off'] + s['name']
        s['name'] = data[o:data.index(b'\0', o)].decode()
    return secs


def symbols(data, secs):
    out = {}
    for s in secs:
        if s['type'] != 2:  # SHT_SYMTAB
            continue
        strs = secs[s['link']]
        for i in range(s['size'] // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', data, s['off'] + i * 16)
            o = strs['off'] + name
            n = data[o:data.index(b'\0', o)].decode()
            if (info & 0xF) == 2:  # STT_FUNC
                out[n] = value
    return out


def main():
    data = open(sys.argv[1], 'rb').read()
    secs = elf_sections(data)
    syms = symbols(data, secs)
    base = 0x400000
    alloc = [s for s in secs if s['flags'] & 2 and s['addr']]  # SHF_ALLOC
    lo = min(s['addr'] for s in alloc)
    hi = max(s['addr'] + s['size'] for s in alloc)
    assert lo >= base + 0x1000
    image = bytearray(((hi - base) + 0xFFF) & ~0xFFF)
    for s in alloc:
        if s['type'] != 8:  # not NOBITS
            image[s['addr'] - base:s['addr'] - base + s['size']] = data[s['off']:s['off'] + s['size']]
    code = [s for s in alloc if s['flags'] & 4]
    code_lo = min(s['addr'] for s in code) - base
    code_hi = max(s['addr'] + s['size'] for s in code) - base

    cases = sorted((n, a) for n, a in syms.items() if n.startswith('c_'))
    # Export directory in a page after the image.
    exp_rva = len(image)
    names = [n.encode() + b'\0' for n, _ in cases]
    n = len(cases)
    dir_size = 40
    funcs_rva = exp_rva + dir_size
    names_rva = funcs_rva + 4 * n
    ords_rva = names_rva + 4 * n
    strs_rva = ords_rva + 2 * n
    exp = bytearray()
    exp += struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, strs_rva, 1, n, n, funcs_rva, names_rva, ords_rva)
    exp += b''.join(struct.pack('<I', a - base) for _, a in cases)
    str_offsets = []
    blob = bytearray(b'cases.exe\0')
    for nm in names:
        str_offsets.append(len(blob))
        blob += nm
    exp += b''.join(struct.pack('<I', strs_rva + o) for o in str_offsets)
    exp += b''.join(struct.pack('<H', i) for i in range(n))
    exp += blob
    image += exp
    image += b'\0' * ((-len(image)) & 0xFFF)

    # Headers: DOS stub, PE header, optional header, one section covering everything.
    pe = bytearray(0x1000)
    pe[0:2] = b'MZ'
    struct.pack_into('<I', pe, 0x3C, 0x40)
    pe[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', pe, 0x44, 0x14C, 2, 0, 0, 0, 0xE0, 0x0102)
    opt = 0x58
    struct.pack_into('<HBBIIIIII', pe, opt, 0x10B, 0, 0, code_hi - code_lo, 0, 0, code_lo, code_lo, 0)
    struct.pack_into('<IIIHHHHHHIIIIHHIIIIII', pe, opt + 28, base, 0x1000, 0x200, 4, 0, 0, 0, 4, 0, 0,
                     len(image), 0x1000, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    struct.pack_into('<II', pe, opt + 96, exp_rva, len(exp))  # export directory
    sec = opt + 0xE0
    # .text: the code range (executable); .data: everything else from the end of the code.
    text_start = code_lo & ~0xFFF
    text_end = (code_hi + 0xFFF) & ~0xFFF
    struct.pack_into('<8sIIIIIIHHI', pe, sec, b'.text', text_end - text_start, text_start, text_end - text_start,
                     0x1000, 0, 0, 0, 0, 0x60000020)
    struct.pack_into('<8sIIIIIIHHI', pe, sec + 40, b'.data', len(image) - text_end, text_end, len(image) - text_end,
                     0x1000 + (text_end - text_start), 0, 0, 0, 0, 0xC0000040)
    out = bytes(pe) + bytes(image[text_start:text_end]) + bytes(image[text_end:])
    open(sys.argv[2], 'wb').write(out)
    print('%d cases, image %#x bytes' % (n, len(image)))


if __name__ == '__main__':
    main()
