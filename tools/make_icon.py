"""Renders the Pictogram Parser icon into a multi-size .ico (PNG entries) plus a preview PNG."""
import struct
import sys
import zlib

OUT_ICO = sys.argv[1]
OUT_PREVIEW = sys.argv[2]
SIZES = [16, 24, 32, 48, 64, 128, 256]

PICTOS = [
    [".##.##.",
     "#######",
     "#######",
     "#######",
     ".#####.",
     "..###..",
     "...#..."],
    ["...#...",
     "..###..",
     ".#####.",
     "#######",
     "..###..",
     "..###..",
     "..###.."],
    [".#####.",
     "#######",
     "##.#.##",
     "#######",
     "#.###.#",
     "##...##",
     ".#####."],
    ["...#...",
     "..###..",
     ".#####.",
     "#######",
     ".##.##.",
     ".##.##.",
     ".#####."],
]
MARKED = 1  # top-right cell is "selected"

G0, G1 = 0.15, 0.85          # grid extents
LINE = 0.028                 # grid line width
CELL = (G1 - G0) / 2


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def over(dst, src, alpha):
    return tuple(dst[i] * (1 - alpha) + src[i] * alpha for i in range(3))


def in_round_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def sample(x, y):
    """Returns (r, g, b, a) in 0..1 for a point in unit coordinates."""
    if not in_round_rect(x, y, 0.02, 0.02, 0.98, 0.98, 0.2):
        return (0, 0, 0, 0)
    # background gradient, deep navy -> indigo
    col = lerp((0.13, 0.20, 0.38), (0.06, 0.08, 0.18), y * 0.7 + x * 0.3)
    # soft top highlight
    if y < 0.5:
        col = over(col, (1, 1, 1), 0.08 * (1 - y / 0.5))

    # white paper sheet behind the grid
    if in_round_rect(x, y, G0 - 0.04, G0 - 0.04, G1 + 0.04, G1 + 0.04, 0.06):
        col = (0.97, 0.97, 0.99)

    if G0 <= x <= G1 and G0 <= y <= G1:
        c = min(int((x - G0) / CELL), 1)
        r = min(int((y - G0) / CELL), 1)
        idx = r * 2 + c
        if idx == MARKED:
            col = over(col, (0.15, 0.85, 0.40), 0.40)
        # pictogram pixels
        cx0 = G0 + c * CELL
        cy0 = G0 + r * CELL
        pad = CELL * 0.17
        px = (x - cx0 - pad) / (CELL - 2 * pad) * 7
        py = (y - cy0 - pad) / (CELL - 2 * pad) * 7
        if 0 <= px < 7 and 0 <= py < 7:
            if PICTOS[idx][int(py)][int(px)] == "#":
                col = (0.05, 0.07, 0.12) if idx != MARKED else (0.02, 0.30, 0.12)

    # grid lines (cyan, like the app)
    for k in range(3):
        g = G0 + k * CELL
        hit_v = abs(x - g) <= LINE / 2 and G0 - LINE / 2 <= y <= G1 + LINE / 2
        hit_h = abs(y - g) <= LINE / 2 and G0 - LINE / 2 <= x <= G1 + LINE / 2
        if hit_v or hit_h:
            col = (0.0, 0.67, 1.0)
    # the highlighted (dragged) line in yellow: the middle vertical line
    g = G0 + CELL
    if abs(x - g) <= LINE * 0.75 and G0 - 0.06 <= y <= G1 + 0.06:
        col = (1.0, 0.78, 0.0)
    return (col[0], col[1], col[2], 1.0)


def render(size):
    ss = 4 if size >= 64 else 6
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    r, g, b, a = sample((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                rgb = [min(255, int(acc[i] / acc[3] * 255 + 0.5)) for i in range(3)]
            else:
                rgb = [0, 0, 0]
            row += bytes(rgb + [int(a * 255 + 0.5)])
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


images = []
for s in SIZES:
    data = png(s, render(s))
    images.append((s, data))
    if s == 256:
        with open(OUT_PREVIEW, "wb") as f:
            f.write(data)

header = struct.pack("<HHH", 0, 1, len(images))
offset = 6 + 16 * len(images)
entries = b""
blobs = b""
for s, data in images:
    dim = 0 if s >= 256 else s
    entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
    blobs += data
with open(OUT_ICO, "wb") as f:
    f.write(header + entries + blobs)
print("ok", [s for s, _ in images])
