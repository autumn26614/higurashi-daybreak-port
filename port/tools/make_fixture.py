"""Write a tiny synthetic daybreakNN.dat archive for tests (no game data inside).

Contents (names stored as cp932 / Shift-JIS, like the real archives):
  data/stage01/env.dat            84 zero bytes
  data/stage01/00.x               text DirectX .x: one mesh, one triangle, one material
                                  with TextureFilename "テスト草01.bmp"
  data/stage01/テスト草01.cnv      2x2 32-bpp .cnv image the texture resolves to

Expected reader results for stage 1: plain set 1 mesh / 1 tri, b set 0 / 0,
1 texture referenced, 1 resolved, name "テスト草01.bmp",
archive entry "data/stage01/テスト草01.cnv".

Optional second output, an overlay archive for the Kai-style ordered lookup
(overlay searched first, analysis/07-kai-archive-lookup.md):
  DATA/Stage01/00.X               same mesh with two triangles (different case on purpose)
With [overlay, base] stage 1 gives 1 mesh / 2 tris; the texture still resolves from the base.

Archive format: analysis/02-archive-format.md. Output is deterministic.
Usage: python3 make_fixture.py <out.dat> [<overlay-out.dat>]
"""
import struct
import sys

TEXTURE = "テスト草01.bmp"
CNV = "data/stage01/テスト草01.cnv"

X_FILE = """xof 0303txt 0032
Mesh triangle {
 3;
 0.0;0.0;0.0;,
 100.0;0.0;0.0;,
 0.0;100.0;0.0;;
 1;
 3;0,1,2;;
 MeshMaterialList {
  1;
  1;
  0;;
  Material {
   1.0;1.0;1.0;1.0;;
   0.0;
   0.0;0.0;0.0;;
   0.0;0.0;0.0;;
   TextureFilename {
    "%s";
   }
  }
 }
}
""" % TEXTURE


# Same mesh with a fourth vertex and a second face, both using material 0.
X_FILE_2 = (X_FILE
            .replace(" 3;\n 0.0;0.0;0.0;,", " 4;\n 0.0;0.0;0.0;,")
            .replace(" 0.0;100.0;0.0;;\n 1;\n 3;0,1,2;;",
                     " 0.0;100.0;0.0;,\n 100.0;100.0;0.0;;\n 2;\n 3;0,1,2;,\n 3;1,3,2;;")
            .replace("  1;\n  0;;", "  2;\n  0,\n  0;;"))


def require(ok, what):
    # Not assert: python -O would strip it and silently write a wrong fixture.
    if not ok:
        raise SystemExit("make_fixture: " + what)


require(X_FILE_2 != X_FILE and X_FILE_2.count("3;1,3,2;;") == 1, "overlay mesh edit did not apply")


def cnv_image():
    # u8 bpp, u32 width, u32 height, u32 pitch, u32 packed_size(0), then BGRA pixels
    pixels = bytes([0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255])
    return struct.pack("<BIIII", 32, 2, 2, 2, 0) + pixels


def build(entries):
    count = len(entries)
    data_start = 2 + count * 0x10C
    index = bytearray()
    payload = bytearray()
    offset = data_start
    for name, body in entries:
        raw_name = name.encode("cp932")
        require(len(raw_name) < 0x104, "name too long: " + name)
        index += raw_name.ljust(0x104, b"\0") + struct.pack("<II", len(body), offset)
        key = ((offset >> 1) | 8) & 0xFF
        payload += bytes(b ^ key for b in body)
        offset += len(body)
    key, step = 100, 100
    for i in range(len(index)):
        index[i] ^= key
        key = (key + step) & 0xFF
        step = (step + 0x4D) & 0xFF
    return struct.pack("<H", count) + bytes(index) + bytes(payload)


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    # cp932 bytes of the texture name must not contain 0x5C ('\\') or 0x22 ('"').
    # (The .x tokenizer treats them as escape/quote inside the TextureFilename string.)
    require(all(b not in (0x5C, 0x22) for b in TEXTURE.encode("cp932")), "texture name has 0x5C or 0x22")
    entries = [
        ("data/stage01/env.dat", bytes(84)),
        ("data/stage01/00.x", X_FILE.encode("cp932")),
        (CNV, cnv_image()),
    ]
    with open(sys.argv[1], "wb") as f:
        f.write(build(entries))
    if len(sys.argv) == 3:
        with open(sys.argv[2], "wb") as f:
            f.write(build([("DATA/Stage01/00.X", X_FILE_2.encode("cp932"))]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
