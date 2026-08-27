"""Turn the photographs of the aircraft clock into a face the panel can draw.

    py scripts/gen/gen_borduhr.py

Reads
    scripts/gen/data/borduhr-dial.png     240x240, the dial with no hands on it
    scripts/gen/data/borduhr-hands.webp   the four hands, on transparency

Writes
    firmware/sdpro-clock-weather/data/faces/borduhr.rgb   240x240 RGB565 BE
    firmware/sdpro-clock-weather/src/display/BorduhrHands.h

Nothing here is drawn or invented. The dial is the photograph, and the hands
are the photographs of the hands - lifted off their transparency and baked as
sprites the panel turns and blends over the picture.

The sprites are baked at TWICE panel resolution and sampled bilinearly at draw
time, colour and coverage both. The first cut stored them at panel size and
took the nearest texel while turning, and the batons came out looking like saw
blades: eleven pixels across is too few for a rotation to land on whole ones,
and every angle stepped the edge differently. Supersample, premultiply,
bilinear - the classic trio - and the flash it costs (~35 KB) is nothing
against the 240 KB free.

The colour is stored premultiplied by its alpha. Bilinear-averaging straight
RGB drags in the colour of transparent texels - whatever the tool that cut the
transparency left there - and the hand grows a fringe. Premultiplied, a
transparent texel contributes exactly nothing.

Hand lengths, in panel pixels from the pivot: three photographs agree once the
time on them is read correctly. The owner's close-ups show 12:55, so the long
baton on the 11 is the MINUTE hand and the short one between 12 and 1 is the
HOUR - reading it as 10:09 was what made the first attempt assign the lengths
backwards and draw both hands a fifth too long. Cross-checked against the
original full-watch photograph, which measures minute 66, hour 52, needle 69
on this panel, and against the ratio the physical parts themselves carry.
"""
import os
import sys

import numpy as np

try:
    import cv2
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("needs opencv-python and pillow:  py -m pip install opencv-python pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DIAL_SRC = os.path.join(HERE, "data", "borduhr-dial.png")
HANDS_SRC = os.path.join(HERE, "data", "borduhr-hands.webp")
DIAL_OUT = os.path.join(ROOT, "firmware", "sdpro-clock-weather", "data", "faces", "borduhr.rgb")
HDR_OUT = os.path.join(ROOT, "firmware", "sdpro-clock-weather", "src", "display", "BorduhrHands.h")

PANEL = 240
SCALE = 2                      # sprite pixels per panel pixel

# Set by the landmarks the owner pointed at on the capture, located on this
# dial by measurement: the minute track's small ticks sit at r 88..100 and the
# numerals' lower edge at r 58.
#
#   minute  - its pointed tip touches the small ticks            -> 89
#   seconds - its tip matches the minute's                       -> 89
#   hour    - the angled shoulder where the baton meets the dark
#             spike sits at the numerals' lower edge; the spike
#             is 14 percent of the hand, so the tip lands at
#             58 / 0.86                                          -> 68
#
# Transferring pixel lengths measured on a DIFFERENT photograph was the
# mistake this replaces: every photo crops the watch at its own scale, so a
# length only means something against that photo's own landmarks. Checked
# back against the first full-watch photograph in its own units: its minute
# tip touches its tick ring too.
REACH = {"hour": 68.0, "minute": 89.0, "seconds": 89.0, "register": 28.5}
# The seconds hand is counterbalanced and its black spoon is what the eye
# checks against the register: on the watch it hangs a little over half the
# needle's length below the hub. The supplied part's own tail is
# proportionally shorter, so the piece below the pivot is stretched to suit;
# the needle above the pivot is left exactly as photographed.
TAIL = {"seconds": 50.0}
# On the watch the needle's diamond rides almost at the tip: the register
# close-up puts the point a bare 4 panel pixels past it and the diamond itself
# at 12. The supplied part carries its diamond much lower, with a long thin
# run above it, so the three sections above the pivot - point, diamond, shaft -
# are remapped to these lengths. The register hand's tip, same close-up,
# touches the OUTER of the register's two circles: tip/inner-circle measures
# 1.29, and the dial's inner circle sits at r 22, hence 28.5 above.
SECONDS_POINT = 4.0
SECONDS_DIAMOND = 12.0


def rgb565(a):
    return ((a[..., 0].astype(np.uint16) >> 3) << 11) | \
           ((a[..., 1].astype(np.uint16) >> 2) << 5) | (a[..., 2].astype(np.uint16) >> 3)


def find_pivot(dl, x0, y0, x1, y1):
    """The little raised boss inside this window, to sub-pixel accuracy."""
    sub = dl[y0:y1, x0:x1]
    m = sub >= np.percentile(sub, 88)
    ys, xs = np.nonzero(m)
    w = sub[m]
    return x0 + float(np.average(xs, weights=w)), y0 + float(np.average(ys, weights=w))


def part_pivot(name, mask, rgba):
    """Where this hand turns, inside its own box.

    The batons carry a real ring hole near their foot and morphological closing
    recovers it. The seconds hand's pivot disc sits at about 68 percent of its
    height; closing yields the concavities either side of the disc (and the
    disc's own hole when it is open), all centred on the pivot, so their mean
    is the pivot - picking any single one is what must be avoided, because the
    side concavities sit forty pixels off axis. The register hand's boss is
    solid cream with a dark dot painted at its centre, so the dot is the pivot.
    """
    h, w = mask.shape
    if name == "register":
        lum = 0.299 * rgba[:, :, 0] + 0.587 * rgba[:, :, 1] + 0.114 * rgba[:, :, 2]
        dot = mask & (lum < 60)
        dn, _, dst, dcen = cv2.connectedComponentsWithStats(dot.astype(np.uint8), 8)
        best = max((j for j in range(1, dn)
                    if dst[j, cv2.CC_STAT_AREA] >= 40 and dcen[j][1] > h * 0.7),
                   key=lambda j: dst[j, cv2.CC_STAT_AREA])
        return float(dcen[best][0]), float(dcen[best][1])

    filled = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_CLOSE, np.ones((41, 41), np.uint8))
    hole = (filled > 0) & (~mask)
    hn, _, hst, hcen = cv2.connectedComponentsWithStats(hole.astype(np.uint8), 8)
    lo, hi = (0.60, 0.80) if name == "seconds" else (0.85, 0.96)
    cands = [(hcen[j][0], hcen[j][1], hst[j, cv2.CC_STAT_AREA]) for j in range(1, hn)
             if hst[j, cv2.CC_STAT_AREA] >= 60 and lo <= hcen[j][1] / h <= hi]
    if not cands:
        sys.exit("no pivot found for the %s hand - has the sheet changed?" % name)
    if name == "seconds":
        ax = sum(c[0] * c[2] for c in cands) / sum(c[2] for c in cands)
        ay = sum(c[1] * c[2] for c in cands) / sum(c[2] for c in cands)
        return float(ax), float(ay)
    best = max(cands, key=lambda c: c[2])
    return float(best[0]), float(best[1])


def main():
    # ------------------------------------------------------------- the dial --
    dial = np.asarray(Image.open(DIAL_SRC).convert("RGB"))
    if dial.shape[0] != PANEL or dial.shape[1] != PANEL:
        dial = cv2.resize(dial, (PANEL, PANEL), interpolation=cv2.INTER_AREA)
    dl = 0.299 * dial[:, :, 0] + 0.587 * dial[:, :, 1] + 0.114 * dial[:, :, 2]

    hub = find_pivot(dl, 108, 110, 132, 134)
    reg = find_pivot(dl, 108, 152, 132, 174)
    print("pivots: hub (%.2f, %.2f)   register (%.2f, %.2f)" % (hub + reg))

    # The red index at twelve stands proud of the dial, and on the real watch
    # the seconds hand runs underneath it. Found rather than typed in, so it
    # follows the picture if the picture is ever recut.
    dr = dial.astype(np.int16)
    red = (dr[:, :, 0] - dr[:, :, 1] > 40) & (dr[:, :, 0] - dr[:, :, 2] > 40) & (dr[:, :, 0] > 70)
    ys, xs = np.nonzero(red)
    if len(xs) == 0:
        sys.exit("no red index found on the dial - has the picture changed?")
    mark = (int(xs.min()) - 1, int(ys.min()) - 1, int(xs.max()) + 1, int(ys.max()) + 1)
    print("red index: x %d..%d  y %d..%d" % (mark[0], mark[2], mark[1], mark[3]))

    v = rgb565(dial)
    raw = np.empty((PANEL, PANEL, 2), np.uint8)
    raw[:, :, 0] = (v >> 8).astype(np.uint8)      # high byte first: the panel's order
    raw[:, :, 1] = (v & 0xFF).astype(np.uint8)
    os.makedirs(os.path.dirname(DIAL_OUT), exist_ok=True)
    with open(DIAL_OUT, "wb") as fh:
        fh.write(raw.tobytes())
    print("wrote %s (%d bytes)" % (os.path.relpath(DIAL_OUT, ROOT), PANEL * PANEL * 2))

    # ------------------------------------------------------------ the hands --
    sheet = np.asarray(Image.open(HANDS_SRC).convert("RGBA"))
    n, lab, st, _ = cv2.connectedComponentsWithStats((sheet[:, :, 3] > 110).astype(np.uint8), 8)
    parts = sorted([i for i in range(1, n) if st[i, cv2.CC_STAT_AREA] > 900],
                   key=lambda i: st[i, cv2.CC_STAT_LEFT])
    if len(parts) != 4:
        sys.exit("expected four hands on the sheet, found %d" % len(parts))

    order = {"hour": 0, "minute": 1, "seconds": 2, "register": 3}
    sprites = []
    for name in ("hour", "minute", "seconds", "register"):
        i = parts[order[name]]
        x, y, w, h, _ = st[i]
        mask = (lab == i)[y:y + h, x:x + w]
        rgba = sheet[y:y + h, x:x + w].copy()
        rgba[:, :, 3] = np.where(mask, rgba[:, :, 3], 0)
        px, py = part_pivot(name, mask, rgba)

        # The register hand was photographed leaning; the others stand within a
        # third of a degree of vertical, which the turn at draw time absorbs.
        # This one leans twenty-odd degrees and has to be stood up about its
        # own pivot, or "two minutes past" would point at six.
        tipmask = rgba[:, :, 3] > 110
        tys, txs = np.nonzero(tipmask)
        cut = tys < (tys.min() + max(3, h // 20))
        lean = float(np.degrees(np.arctan2(txs[cut].mean() - px, py - tys[cut].mean())))
        if abs(lean) > 2.0:
            # getRotationMatrix2D's positive angle turns counter-clockwise as
            # seen on screen, which is the direction that brings an up-right
            # tip back to vertical. Negating it - the intuitive reading of
            # "undo the lean" - doubles the lean instead.
            M = cv2.getRotationMatrix2D((px, py), lean, 1.0)
            pad = int(h * 0.5)
            M[0, 2] += pad
            M[1, 2] += pad
            rgba = cv2.warpAffine(rgba, M, (w + 2 * pad, h + 2 * pad),
                                  flags=cv2.INTER_LINEAR, borderValue=(0, 0, 0, 0))
            px, py = px + pad, py + pad
            keep = np.nonzero(rgba[:, :, 3] > 8)
            y0, y1 = int(keep[0].min()), int(keep[0].max()) + 1
            x0, x1 = int(keep[1].min()), int(keep[1].max()) + 1
            rgba = rgba[y0:y1, x0:x1]
            px, py = px - x0, py - y0
            h, w = rgba.shape[:2]
            print("  %-9s stood up by %.1f deg" % (name, lean))

        # Scale so pivot-to-tip is the reach, at SCALE times panel resolution.
        k = (REACH[name] * SCALE) / py
        tw, thh = max(3, int(round(w * k))), max(5, int(round(h * k)))
        small = cv2.resize(rgba, (tw, thh), interpolation=cv2.INTER_AREA)
        pivot_row = int(round(py * k))
        if name == "seconds":
            # Find the diamond by its width: the widest run of rows in the
            # upper half, well over the shaft's width. Then rebuild the head as
            # point / diamond / shaft at the measured lengths.
            head = small[:pivot_row]
            widths = (head[:, :, 3] > 110).sum(axis=1)
            shaft_w = float(np.median(widths[int(pivot_row * 0.45):int(pivot_row * 0.75)]))
            wide = np.nonzero(widths[:int(pivot_row * 0.55)] > shaft_w * 1.5)[0]
            if len(wide) == 0:
                sys.exit("no diamond found on the seconds hand - has the sheet changed?")
            d_top, d_bot = int(wide.min()), int(wide.max()) + 1
            n_pt = int(round(SECONDS_POINT * SCALE))
            n_di = int(round(SECONDS_DIAMOND * SCALE))
            n_sh = pivot_row - n_pt - n_di
            seg = lambda a, hh: cv2.resize(a, (tw, hh), interpolation=cv2.INTER_AREA)
            small = np.vstack([seg(head[:d_top], n_pt), seg(head[d_top:d_bot], n_di),
                               seg(head[d_bot:], n_sh), small[pivot_row:]])
            thh = small.shape[0]
            print("           diamond moved: rows %d..%d -> point %d + diamond %d + shaft %d"
                  % (d_top, d_bot, n_pt, n_di, n_sh))
        if name in TAIL:
            want = int(round(TAIL[name] * SCALE))
            head, tail = small[:pivot_row], small[pivot_row:]
            if tail.shape[0] >= 2 and want >= 2:
                tail = cv2.resize(tail, (tw, want), interpolation=cv2.INTER_AREA)
                small = np.vstack([head, tail])
                thh = small.shape[0]

        if name == "seconds":
            # The counterweight is black metal, but the cutout left it a rim of
            # pale bronze - luminance up to 90 against a dial that sits at 15 -
            # and on the panel that rim glowed as a whitish outline round the
            # spoon. It cannot just be deleted: it IS the part's edge, and
            # without it the silhouette shrinks. So it is pressed down to the
            # spoon's own tone instead - everything below the pivot is capped
            # at luminance 45, which leaves a faint dark-metal edge and kills
            # the glow.
            tail_px = small[pivot_row:]
            tl = (0.299 * tail_px[:, :, 0].astype(np.float32) +
                  0.587 * tail_px[:, :, 1].astype(np.float32) +
                  0.114 * tail_px[:, :, 2].astype(np.float32))
            bright = tl > 45.0
            f = np.ones_like(tl)
            f[bright] = 45.0 / tl[bright]
            tail_px[:, :, :3] = (tail_px[:, :, :3].astype(np.float32) * f[:, :, None]).astype(np.uint8)
            print("           tail rim pressed down: %d px" % int(bright[tail_px[:, :, 3] > 0].sum()))

        # Premultiply, then down to 565. The alpha rides along at full byte.
        a16 = small[:, :, 3:4].astype(np.uint16)
        pm = (small[:, :, :3].astype(np.uint16) * a16 // 255).astype(np.uint8)
        sprites.append({
            "name": name, "w": tw, "h": thh,
            "px": px * k, "py": float(pivot_row),
            "rgb": rgb565(pm), "a": small[:, :, 3],
        })
        print("  %-9s %3dx%-4d (at %dx)  pivot (%6.2f,%7.2f)  reach %4.1f  tail %4.1f"
              % (name, tw, thh, SCALE, px * k, float(pivot_row),
                 REACH[name], (thh - pivot_row) / float(SCALE)))

    # ------------------------------------------------------------ the header --
    total = sum(s["w"] * s["h"] for s in sprites)
    out = []
    out.append("// Generated by scripts/gen/gen_borduhr.py - do not edit by hand.")
    out.append("//")
    out.append("// The watch's own hands, photographed, cut off their transparency and")
    out.append("// baked at twice panel resolution. Colour is stored premultiplied by the")
    out.append("// alpha, so the bilinear sampling at draw time can average four texels")
    out.append("// without transparent ones bleeding whatever colour they happen to hold.")
    out.append("// That pairing - supersample, premultiply, bilinear - is what keeps an")
    out.append("// eleven-pixel hand from turning into a saw blade as it goes round.")
    out.append("//")
    out.append("// Each hand turns on its own hole, carried in sixteenths of a sprite")
    out.append("// pixel: half a pixel of error at the hub is a visible wobble at the tip.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <Arduino.h>")
    out.append("")
    out.append("namespace BorduhrHands {")
    out.append("")
    out.append("// Sprite pixels per panel pixel.")
    out.append("constexpr uint8_t SPRITE_SCALE = %d;" % SCALE)
    out.append("")
    out.append("struct Sprite {")
    out.append("    uint16_t w, h;")
    out.append("    int16_t pivotX16, pivotY16;   // sixteenths of a sprite pixel")
    out.append("    const uint16_t* rgb;          // RGB565, premultiplied by alpha")
    out.append("    const uint8_t* alpha;         // 0 transparent, 255 solid")
    out.append("};")
    out.append("")
    for s in sprites:
        nm = s["name"].upper()
        flat_rgb = s["rgb"].reshape(-1)
        flat_a = s["a"].reshape(-1)
        out.append("const uint16_t %s_RGB[] PROGMEM = {" % nm)
        for i in range(0, len(flat_rgb), 12):
            out.append("    " + ", ".join("0x%04X" % int(q) for q in flat_rgb[i:i + 12]) + ",")
        out.append("};")
        out.append("const uint8_t %s_A[] PROGMEM = {" % nm)
        for i in range(0, len(flat_a), 20):
            out.append("    " + ", ".join("%d" % int(q) for q in flat_a[i:i + 20]) + ",")
        out.append("};")
        out.append("")
    for s in sprites:
        out.append("constexpr Sprite %-8s = {%d, %d, %d, %d, %s_RGB, %s_A};"
                   % (s["name"].upper(), s["w"], s["h"],
                      round(s["px"] * 16), round(s["py"] * 16),
                      s["name"].upper(), s["name"].upper()))
    out.append("")
    out.append("// Where they turn, measured off the dial photograph, in sixteenths of a")
    out.append("// panel pixel.")
    out.append("constexpr int16_t HUB_X16 = %d;" % round(hub[0] * 16))
    out.append("constexpr int16_t HUB_Y16 = %d;" % round(hub[1] * 16))
    out.append("constexpr int16_t REG_X16 = %d;" % round(reg[0] * 16))
    out.append("constexpr int16_t REG_Y16 = %d;" % round(reg[1] * 16))
    out.append("")
    out.append("// The red index at twelve stands proud of the dial and the seconds hand")
    out.append("// passes under it, so this rectangle of the dial is put back on top of")
    out.append("// whatever was drawn.")
    out.append("constexpr int16_t MARK_X0 = %d;" % mark[0])
    out.append("constexpr int16_t MARK_Y0 = %d;" % mark[1])
    out.append("constexpr int16_t MARK_X1 = %d;" % mark[2])
    out.append("constexpr int16_t MARK_Y1 = %d;" % mark[3])
    out.append("")
    out.append("}  // namespace BorduhrHands")
    out.append("")
    os.makedirs(os.path.dirname(HDR_OUT), exist_ok=True)
    with open(HDR_OUT, "w", newline="\n") as fh:
        fh.write("\n".join(out))
    print("wrote %s (%d sprite px, %d bytes of flash)"
          % (os.path.relpath(HDR_OUT, ROOT), total, total * 3))


if __name__ == "__main__":
    main()
