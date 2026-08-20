"""Converts an arbitrary PNG into a cursor-ready 32-bit RGBA PNG.

Stdlib only - no Pillow. Handles the two things that make an exported cursor image
unusable: no alpha channel, and a full-canvas export with the pointer floating in the
middle of it.

    python tools/convert_cursor.py in.png out.png
    python tools/convert_cursor.py in.png out.png --size 32 --key white --tolerance 40

What it does, in order:
  1. Decodes the PNG (greyscale / RGB / palette / with or without alpha, 8-bit).
  2. If there is no alpha channel, keys out the background colour to build one.
     --key white (default) / black / auto (samples the top-left pixel).
  3. Crops to the bounding box of everything still opaque.
  4. Scales the long edge down to --size (default 32) with a box filter.

The hotspot is not stored in a PNG - set HotspotX / HotspotY in CursorUnbound.ini. After
cropping, the pointer tip is at the top-left of the cropped image, so 0,0 is usually right.
"""

import argparse
import struct
import sys
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_png(path):
    """Returns (width, height, pixels) where pixels is a list of rows of (r,g,b,a)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path} is not a PNG")

    idat = b""
    palette = None
    trns = None
    width = height = depth = color_type = interlace = None

    i = 8
    while i < len(data):
        length = struct.unpack(">I", data[i : i + 4])[0]
        tag = data[i + 4 : i + 8]
        chunk = data[i + 8 : i + 8 + length]
        if tag == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif tag == b"PLTE":
            palette = chunk
        elif tag == b"tRNS":
            trns = chunk
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
        i += 12 + length

    if depth != 8:
        raise SystemExit(f"only 8-bit PNGs are supported (this one is {depth}-bit)")
    if interlace:
        raise SystemExit("interlaced PNGs are not supported; re-save without interlacing")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    raw = zlib.decompress(idat)
    stride = width * channels

    # Undo the per-scanline filters.
    out = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                line[x] = (line[x] + _paeth(a, b, c)) & 0xFF
        out += line
        prev = line

    # Normalise everything to RGBA rows.
    pixels = []
    for y in range(height):
        row = []
        base = y * stride
        for x in range(width):
            o = base + x * channels
            if color_type == 0:
                v = out[o]
                row.append((v, v, v, 255))
            elif color_type == 4:
                v = out[o]
                row.append((v, v, v, out[o + 1]))
            elif color_type == 2:
                row.append((out[o], out[o + 1], out[o + 2], 255))
            elif color_type == 6:
                row.append((out[o], out[o + 1], out[o + 2], out[o + 3]))
            elif color_type == 3:
                idx = out[o]
                r, g, b = palette[idx * 3 : idx * 3 + 3]
                alpha = trns[idx] if trns and idx < len(trns) else 255
                row.append((r, g, b, alpha))
        pixels.append(row)

    return width, height, pixels, color_type in (4, 6) or (color_type == 3 and trns)


def key_out_background(pixels, mode, tolerance):
    if mode == "auto":
        target = pixels[0][0][:3]
    elif mode == "black":
        target = (0, 0, 0)
    else:
        target = (255, 255, 255)

    for row in pixels:
        for x, (r, g, b, _) in enumerate(row):
            distance = max(abs(r - target[0]), abs(g - target[1]), abs(b - target[2]))
            row[x] = (r, g, b, 0 if distance <= tolerance else 255)
    return target


def crop_to_content(pixels):
    height = len(pixels)
    width = len(pixels[0])
    min_x, min_y, max_x, max_y = width, height, -1, -1
    for y in range(height):
        for x in range(width):
            if pixels[y][x][3] > 8:
                min_x = min(min_x, x)
                max_x = max(max_x, x)
                min_y = min(min_y, y)
                max_y = max(max_y, y)
    if max_x < 0:
        raise SystemExit("image is fully transparent after keying - try a different --key")
    return [row[min_x : max_x + 1] for row in pixels[min_y : max_y + 1]]


def box_scale(pixels, target):
    height = len(pixels)
    width = len(pixels[0])
    longest = max(width, height)
    if longest <= target:
        return pixels

    new_w = max(1, round(width * target / longest))
    new_h = max(1, round(height * target / longest))

    out = []
    for ny in range(new_h):
        row = []
        y0, y1 = ny * height // new_h, max(ny * height // new_h + 1, (ny + 1) * height // new_h)
        for nx in range(new_w):
            x0, x1 = nx * width // new_w, max(nx * width // new_w + 1, (nx + 1) * width // new_w)
            ar = ag = ab = aa = n = 0
            for y in range(y0, y1):
                for x in range(x0, x1):
                    r, g, b, a = pixels[y][x]
                    ar += r * a
                    ag += g * a
                    ab += b * a
                    aa += a
                    n += 1
            if aa == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((ar // aa, ag // aa, ab // aa, aa // n))
        out.append(row)
    return out


def write_png(path, pixels):
    height = len(pixels)
    width = len(pixels[0])
    raw = b""
    for row in pixels:
        raw += b"\x00" + bytes(v for px in row for v in px)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    open(path, "wb").write(png)
    return width, height


def main():
    parser = argparse.ArgumentParser(description="Convert an image into a cursor-ready RGBA PNG.")
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--size", type=int, default=32, help="target long edge (default 32)")
    parser.add_argument("--key", choices=["white", "black", "auto", "none"], default="white",
                        help="background colour to make transparent when the source has no alpha")
    parser.add_argument("--tolerance", type=int, default=40, help="colour distance treated as background")
    args = parser.parse_args()

    width, height, pixels, has_alpha = decode_png(args.input)
    print(f"source: {width}x{height}, alpha={'yes' if has_alpha else 'no'}")

    if not has_alpha and args.key != "none":
        target = key_out_background(pixels, args.key, args.tolerance)
        print(f"keyed out background rgb{target} with tolerance {args.tolerance}")

    pixels = crop_to_content(pixels)
    print(f"cropped to {len(pixels[0])}x{len(pixels)}")

    pixels = box_scale(pixels, args.size)
    out_w, out_h = write_png(args.output, pixels)
    print(f"wrote {args.output} ({out_w}x{out_h} RGBA)")


if __name__ == "__main__":
    main()
