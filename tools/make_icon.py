"""Draws takt4's icon and writes every file made from it, into assets/icon/.

The mark (the operator's pick of 2026-10-08, "Lock", drawn as its small-size art at every size):
four heavy corner brackets round a white disc on a near-black plate, the top-left bracket red —
the bar's first beat — in the red of PANIC and the beat disc, the window's own. On a 64-cell grid of a 1024 square,
so at 16 px every edge falls on a whole pixel.

    python tools/make_icon.py

writes takt4.svg (the master), takt4-<n>.png, takt4.ico (Windows: the exe's icon) and
takt4.icns (the macOS bundle's). Needs Pillow. Run it again after changing anything below, and
commit what it writes.
"""

import io
import os
import sys

from PIL import Image, ImageDraw

PLATE = (0x0E, 0x0E, 0x0E, 255)
INK = (0xEF, 0xEE, 0xE9, 255)
RED = (0xD8, 0x30, 0x1F, 255)  # PANIC's red (Wf.panic), the operator's call of 2026-10-08

# In units of a 1024 square.
PLATE_RADIUS = 96
DISC_RADIUS = 160
BRACKET_OUT = 128  # from the plate's edge to the bracket's outer edge
BRACKET_ARM = 256  # how far each arm runs from the corner
BRACKET_THICK = 128

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]
PNG_SIZES = [16, 32, 64, 256, 1024]
SUPERSAMPLE = 8

# macOS draws an app icon on its own grid: the plate is 824 of the 1024 square, centred, with
# corners of 185 (Apple's template, Big Sur on).
MAC_PLATE = 824
MAC_RADIUS = 185


def brackets():
    """The four brackets as polygons in 1024 units, the red one first."""
    o, a, t = BRACKET_OUT, BRACKET_OUT + BRACKET_ARM, BRACKET_OUT + BRACKET_THICK
    top_left = [(o, o), (a, o), (a, t), (t, t), (t, a), (o, a)]

    def mirror(points, x, y):
        return [((1024 - px) if x else px, (1024 - py) if y else py) for px, py in points]

    return [
        (top_left, RED),
        (mirror(top_left, True, False), INK),
        (mirror(top_left, True, True), INK),
        (mirror(top_left, False, True), INK),
    ]


def draw(size, mac=False):
    """The icon at `size` pixels, supersampled and averaged down so an edge's pixels are covered
    exactly as much as the shape covers them."""
    big = size * SUPERSAMPLE
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    pen = ImageDraw.Draw(image)
    if mac:
        offset = (1024 - MAC_PLATE) / 2
        scale = MAC_PLATE / 1024
        plate_radius = MAC_RADIUS
    else:
        offset, scale, plate_radius = 0.0, 1.0, PLATE_RADIUS

    def at(v):  # a 1024-unit coordinate of the art to this image's pixels
        return (offset + v * scale) * big / 1024

    plate_lo = offset * big / 1024
    plate_hi = (offset + 1024 * scale) * big / 1024 - 1
    pen.rounded_rectangle([plate_lo, plate_lo, plate_hi, plate_hi],
                          radius=plate_radius * big / 1024, fill=PLATE)
    for points, colour in brackets():
        pen.polygon([(at(x), at(y)) for x, y in points], fill=colour)
    r = DISC_RADIUS
    pen.ellipse([at(512 - r), at(512 - r), at(512 + r) - 1, at(512 + r) - 1], fill=INK)
    return image.reduce(SUPERSAMPLE)


def svg():
    paths = []
    for points, colour in brackets():
        d = "M" + "L".join(f"{x} {y}" for x, y in points) + "Z"
        paths.append(f'  <path d="{d}" fill="#{colour[0]:02x}{colour[1]:02x}{colour[2]:02x}"/>')
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" '
        'height="1024">\n'
        "  <title>takt4</title>\n"
        f'  <rect width="1024" height="1024" rx="{PLATE_RADIUS}" fill="#0e0e0e"/>\n'
        + "\n".join(paths)
        + f'\n  <circle cx="512" cy="512" r="{DISC_RADIUS}" fill="#efeee9"/>\n</svg>\n'
    )


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "assets", "icon")
    os.makedirs(out, exist_ok=True)
    with io.open(os.path.join(out, "takt4.svg"), "w", encoding="utf-8", newline="\n") as f:
        f.write(svg())
    for size in PNG_SIZES:
        draw(size).save(os.path.join(out, f"takt4-{size}.png"), optimize=True)
    ico = [draw(size) for size in ICO_SIZES]
    ico[-1].save(os.path.join(out, "takt4.ico"), format="ICO",
                 sizes=[(s, s) for s in ICO_SIZES], append_images=ico[:-1])
    icns = [draw(size, mac=True) for size in ICNS_SIZES]
    icns[-1].save(os.path.join(out, "takt4.icns"), format="ICNS", append_images=icns[:-1])
    print("wrote", ", ".join(sorted(os.listdir(out))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
