"""Disassemble a range of a PE as the recompiler sees it.

  python3 -I disasm.py <pe> <base hex or -> <start hex> <end hex>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(Path.home() / '.local' / 'daybreak-py'))
import capstone  # noqa: E402
from discover import Image  # noqa: E402
from pe import PE  # noqa: E402

pe = PE(sys.argv[1])
base = None if sys.argv[2] == '-' else int(sys.argv[2], 16)
img = Image(pe, base)
start, end = int(sys.argv[3], 16), int(sys.argv[4], 16)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
for i in md.disasm(img.mem[start - img.base:end - img.base], start):
    print('%08x  %-24s %s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
