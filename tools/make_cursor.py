"""Generates the default CursorUnbound pointer as a 32-bit RGBA PNG.

Stdlib only - no Pillow. The arrow is rasterised at 4x and box-filtered down, which
gives clean antialiased edges without needing a real renderer.

    python tools/make_cursor.py dist/SKSE/Plugins/CursorUnbound/cursor.png

The shape is a vanilla-style pointer: white fill, dark outline, hotspot at (0, 0).
Replace the PNG with your own art if you use a cursor replacer - anything WIC can
decode works, and the plugin reads whatever it finds in that folder.
"""

import math
import struct
import sys
import zlib

SIZE = 32
SS = 4  # supersampling factor
OUTLINE = 1.35  # outline thickness in logical pixels

# Named looks. "vanilla" is the plain white pointer; "gold" approximates the warm
# parchment/gold arrow used by Skyrim UI overhauls like Vel'dun and ESO-style cursors.
STYLES = {
    "vanilla": {"fill": (255, 255, 255), "stroke": (26, 24, 20)},
    "gold": {"fill": (54, 48, 38), "stroke": (201, 185, 142)},
    "black": {"fill": (18, 18, 18), "stroke": (235, 235, 235)},
}

FILL = STYLES["vanilla"]["fill"]
STROKE = STYLES["vanilla"]["stroke"]

# Arrow outline in normalised (0..1) space, starting at the tip.
SHAPE = [
    (0.02, 0.02),
    (0.02, 0.72),
    (0.19, 0.56),
    (0.30, 0.86),
    (0.42, 0.81),
    (0.31, 0.52),
    (0.56, 0.52),
]


def scaled_points(size):
    return [(x * size, y * size) for x, y in SHAPE]


def point_in_polygon(px, py, poly):
    inside = False
    n = len(poly)
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > py) != (yj > py):
            x_cross = (xj - xi) * (py - yi) / (yj - yi) + xi
            if px < x_cross:
                inside = not inside
        j = i
    return inside


def distance_to_segment(px, py, ax, ay, bx, by):
    dx = bx - ax
    dy = by - ay
    length_sq = dx * dx + dy * dy
    if length_sq == 0.0:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / length_sq
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def distance_to_polygon(px, py, poly):
    best = float("inf")
    n = len(poly)
    for i in range(n):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % n]
        best = min(best, distance_to_segment(px, py, ax, ay, bx, by))
    return best


def render():
    hi = SIZE * SS
    poly = scaled_points(hi)
    half_outline = (OUTLINE * SS) / 2.0

    # Premultiplied accumulation buffers at supersampled resolution.
    samples = [[(0, 0, 0, 0)] * hi for _ in range(hi)]

    for y in range(hi):
        py = y + 0.5
        row = samples[y]
        for x in range(hi):
            px = x + 0.5
            dist = distance_to_polygon(px, py, poly)
            inside = point_in_polygon(px, py, poly)

            if dist <= half_outline:
                row[x] = (STROKE[0], STROKE[1], STROKE[2], 255)
            elif inside:
                row[x] = (FILL[0], FILL[1], FILL[2], 255)
            # else stays fully transparent

    # Box filter down to the final size, working in premultiplied space so the
    # transparent border does not bleed dark fringes into the edges.
    pixels = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            acc_r = acc_g = acc_b = acc_a = 0
            for sy in range(SS):
                src = samples[y * SS + sy]
                for sx in range(SS):
                    r, g, b, a = src[x * SS + sx]
                    acc_r += r * a
                    acc_g += g * a
                    acc_b += b * a
                    acc_a += a
            total = SS * SS
            alpha = acc_a // total
            if alpha == 0:
                row += bytes((0, 0, 0, 0))
            else:
                row += bytes(
                    (
                        min(255, acc_r // acc_a),
                        min(255, acc_g // acc_a),
                        min(255, acc_b // acc_a),
                        alpha,
                    )
                )
        pixels.append(bytes(row))
    return pixels


def write_png(path, rows):
    raw = b"".join(b"\x00" + row for row in rows)  # filter type 0 per scanline

    def chunk(tag, data):
        payload = tag + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )

    with open(path, "wb") as handle:
        handle.write(png)


def main():
    global FILL, STROKE, SIZE

    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else "cursor.png"

    for flag in sys.argv[1:]:
        if flag.startswith("--style="):
            name = flag.split("=", 1)[1]
            if name not in STYLES:
                raise SystemExit(f"unknown style '{name}'; choose from {', '.join(STYLES)}")
            FILL = STYLES[name]["fill"]
            STROKE = STYLES[name]["stroke"]
        elif flag.startswith("--size="):
            SIZE = int(flag.split("=", 1)[1])

    write_png(out, render())
    print(f"wrote {out} ({SIZE}x{SIZE} RGBA)")


if __name__ == "__main__":
    main()
