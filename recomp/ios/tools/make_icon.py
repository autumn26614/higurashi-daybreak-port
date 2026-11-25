"""Draws the app icon (1024x1024 PNG) with the standard library alone: the controls' diamond
(white edge, thin red inner line) framing a sun rising over a dark horizon, on black with a
red glow. Original artwork, nothing taken from the game.
    python3 -I tools/make_icon.py App/Assets.xcassets/AppIcon.appiconset/icon.png
"""
import math
import struct
import sys
import zlib

S = 1024
C = S / 2
HORIZON = 610.0
SUN_R = 165.0


def mix(a, b, t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def cover(d):
    """Coverage of a pixel by a shape whose signed distance (negative inside) is d."""
    return 0.0 if d >= 0.5 else 1.0 if d <= -0.5 else 0.5 - d


def hexc(h):
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


BG_IN, BG_OUT = hexc("3c070b"), hexc("060203")
SKY = [(0.0, hexc("14060c")), (0.45, hexc("5a0a14")), (0.8, hexc("c41a1a")), (1.0, hexc("ff7a24"))]
SUN_IN, SUN_OUT = hexc("fff6dc"), hexc("ffad3a")
GROUND, GLOW = hexc("0a0405"), hexc("ffb04a")
WHITE, RED = (1.0, 1.0, 1.0), hexc("e0141c")


def sky(y):
    top = C - 448
    t = (y - top) / (HORIZON - top)
    for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0))
    return SKY[-1][1]


def pixel(x, y):
    # Background: a dark red glow.
    r = math.hypot(x - C, y - C) / (C * 1.42)
    col = mix(BG_IN, BG_OUT, r)
    l1 = (abs(x - C) + abs(y - C)) / math.sqrt(2)  # distance to the centre in diamond units
    # Diamond: half-diagonal 470 px -> edge distance 470 / sqrt(2).
    edge_out = l1 - 470 / math.sqrt(2)
    edge_in = l1 - 446 / math.sqrt(2)
    inside = cover(edge_in)
    if inside > 0:
        # The scene inside the diamond.
        if y < HORIZON:
            scene = sky(y)
            # Rays: soft wedges above the sun.
            ang = math.atan2(HORIZON - y, x - C)
            dist = math.hypot(x - C, y - HORIZON)
            if SUN_R < dist:
                k = (math.cos(ang * 18) + 1) / 2
                fade = max(0.0, 1 - (dist - SUN_R) / 330)
                scene = mix(scene, GLOW, 0.32 * k ** 3 * fade)
            s = cover(dist - SUN_R)
            if s > 0:
                scene = mix(scene, mix(SUN_IN, SUN_OUT, (dist / SUN_R) ** 2), s)
        else:
            scene = mix(GLOW, GROUND, (y - HORIZON) / 26)
            # Reflection of the sun on the ground.
            w = SUN_R * (1 - (y - HORIZON) / 180)
            if w > 0 and abs(x - C) < w and (int(y) // 9) % 2 == 0:
                scene = mix(scene, SUN_OUT, 0.35 * (1 - (y - HORIZON) / 180))
        # Thin red line just inside the white edge.
        ring = abs(l1 - 418 / math.sqrt(2)) - 2.0
        scene = mix(scene, RED, 0.85 * cover(ring))
        col = mix(col, scene, inside)
    # White edge.
    w = cover(max(edge_out, -edge_in))
    col = mix(col, WHITE, w)
    return col


def main(out):
    rows = []
    for y in range(S):
        row = bytearray(b"\x00")
        for x in range(S):
            # 2x2 samples per pixel.
            acc = [0.0, 0.0, 0.0]
            for sy in (0.25, 0.75):
                for sx in (0.25, 0.75):
                    c = pixel(x + sx, y + sy)
                    for i in range(3):
                        acc[i] += c[i]
            row += bytes(max(0, min(255, round(v / 4 * 255))) for v in acc)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(out, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main(sys.argv[1])
