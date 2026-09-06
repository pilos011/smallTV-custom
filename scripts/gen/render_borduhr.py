"""Draw the Borduhr face the way the panel will, from what the panel will read.

    py scripts/gen/render_borduhr.py 12:55:34 [out.png]

Reads the GENERATED artefacts - `BorduhrHands.h` and `faces/borduhr.rgb` - and
nothing else. That distinction is the whole point: a simulator that re-derives
the sprites from the source photographs agrees with itself no matter what
gen_borduhr.py did, and would have said the hands were right while the
firmware drew them wrong. This reads the same bytes the firmware does.

The sampling matches `bordPaint` in main.cpp: for every panel pixel inside a
hand's box, rotate into sprite space, sample bilinearly, and blend with the
premultiplied colour against the dial underneath.
"""
import os
import re
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, "firmware", "sdpro-clock-weather")
HDR = os.path.join(FW, "src", "display", "BorduhrHands.h")
FACE = os.path.join(FW, "data", "faces", "borduhr.rgb")
PANEL = 240


def parse_header(text):
    """Sprites, pivots and the two centres, straight out of the header."""

    def const(name):
        m = re.search(r"constexpr\s+\w+\s+" + name + r"\s*=\s*(-?\d+)", text)
        return int(m.group(1))

    # The alpha planes are named <HAND>_A, not <HAND>_ALPHA, and are declared
    # `const` rather than `constexpr`. Read both names out of the Sprite line
    # instead of assuming either.
    def array(name):
        m = re.search(name + r"\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", text, re.S)
        if m is None:
            raise SystemExit("no array named %s in %s" % (name, HDR))
        body = m.group(1)
        return np.array([int(v, 0) for v in re.findall(r"0[xX][0-9A-Fa-f]+|[0-9]+", body)])

    sprites = {}
    for key in ("HOUR", "MINUTE", "SECONDS", "REGISTER"):
        m = re.search(
            r"constexpr\s+Sprite\s+" + key +
            r"\s*=\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
            r"\s*(\w+)\s*,\s*(\w+)\s*\}", text)
        if m is None:
            raise SystemExit("no Sprite named %s in %s" % (key, HDR))
        w, h, px16, py16 = (int(g) for g in m.groups()[:4])
        rgb = array(m.group(5))
        alpha = array(m.group(6))
        if rgb.size != w * h or alpha.size != w * h:
            raise SystemExit("%s: %dx%d needs %d values, header has rgb=%d alpha=%d"
                             % (key, w, h, w * h, rgb.size, alpha.size))
        sprites[key] = dict(w=w, h=h, px=px16 / 16.0, py=py16 / 16.0,
                            rgb=rgb.reshape(h, w), alpha=alpha.reshape(h, w))
    return sprites, const("SPRITE_SCALE"), (
        const("HUB_X16") / 16.0, const("HUB_Y16") / 16.0,
        const("REG_X16") / 16.0, const("REG_Y16") / 16.0)


def rgb565_to_rgb(v):
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return np.stack([r, g, b], axis=-1).astype(np.float32)


def draw_hand(panel, sp, scale, cx, cy, deg):
    """One hand, bilinear, premultiplied - the same arithmetic as bordPaint."""
    th = np.radians(deg)
    ct, st = np.cos(th), np.sin(th)
    # Every panel pixel the sprite could possibly touch: the pivot can sit at a
    # corner, so bound by the diagonal rather than the longer side.
    reach = np.hypot(sp["w"], sp["h"]) / float(scale) + 2
    y0, y1 = int(max(0, cy - reach)), int(min(PANEL, cy + reach + 1))
    x0, x1 = int(max(0, cx - reach)), int(min(PANEL, cx + reach + 1))
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    dx, dy = xx - cx, yy - cy
    u = sp["px"] + ((dx * ct) + (dy * st)) * scale
    v = sp["py"] + ((-dx * st) + (dy * ct)) * scale

    u0, v0 = np.floor(u).astype(int), np.floor(v).astype(int)
    fu, fv = u - u0, v - v0
    inside = (u0 >= 0) & (v0 >= 0) & (u0 < sp["w"] - 1) & (v0 < sp["h"] - 1)
    if not inside.any():
        return
    u0c, v0c = np.clip(u0, 0, sp["w"] - 2), np.clip(v0, 0, sp["h"] - 2)

    def bilinear(plane):
        a = plane[v0c, u0c]
        b = plane[v0c, u0c + 1]
        c = plane[v0c + 1, u0c]
        d = plane[v0c + 1, u0c + 1]
        return (a * (1 - fu) * (1 - fv) + b * fu * (1 - fv) +
                c * (1 - fu) * fv + d * fu * fv)

    alpha = bilinear(sp["alpha"].astype(np.float32)) / 255.0
    colour_planes = rgb565_to_rgb(sp["rgb"])
    colour = np.stack([bilinear(colour_planes[..., k]) for k in range(3)], axis=-1)
    alpha = np.where(inside, alpha, 0.0)[..., None]
    colour = np.where(inside[..., None], colour, 0.0)
    # Colour is stored premultiplied, so it is added rather than mixed.
    sub = panel[y0:y1, x0:x1]
    panel[y0:y1, x0:x1] = np.clip(colour + sub * (1 - alpha), 0, 255)


def main():
    when = sys.argv[1] if len(sys.argv) > 1 else "12:55:34"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "borduhr-render.png")
    h, m, s = (int(x) for x in when.split(":"))

    text = open(HDR, encoding="utf-8").read()
    sprites, scale, (hx, hy, rx, ry) = parse_header(text)
    face = np.fromfile(FACE, dtype=">u2").reshape(PANEL, PANEL)
    panel = rgb565_to_rgb(face)

    deg_hour = ((h % 12) + m / 60.0 + s / 3600.0) / 12.0 * 360.0
    deg_min = (m + s / 60.0) / 60.0 * 360.0
    deg_sec = s / 60.0 * 360.0
    # The register is a stopwatch counter, not a clock: the firmware pins it
    # where the photograph has it rather than driving it from the time.
    deg_reg = 64.0

    draw_hand(panel, sprites["HOUR"], scale, hx, hy, deg_hour)
    draw_hand(panel, sprites["MINUTE"], scale, hx, hy, deg_min)
    draw_hand(panel, sprites["REGISTER"], scale, rx, ry, deg_reg)
    draw_hand(panel, sprites["SECONDS"], scale, hx, hy, deg_sec)

    img = Image.fromarray(panel.astype(np.uint8))
    img.resize((PANEL * 3, PANEL * 3), Image.NEAREST).save(out)
    print("%s  at %s   scale %d   hub %.2f,%.2f   reg %.2f,%.2f"
          % (out, when, scale, hx, hy, rx, ry))
    for key in ("HOUR", "MINUTE", "SECONDS", "REGISTER"):
        sp = sprites[key]
        print("   %-9s %3dx%-4d  pivot %6.2f,%6.2f  reach %5.1f panel px"
              % (key, sp["w"], sp["h"], sp["px"], sp["py"], sp["py"] / scale))


if __name__ == "__main__":
    main()
