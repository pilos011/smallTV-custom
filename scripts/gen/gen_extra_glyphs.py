"""Bake the Hangul the bundled UI font is missing, for both of its sets.

The font carries 433 syllables in each set. Five of the weather words fall
outside the large set; twenty-three of the airline names fall outside the small
one. Rather than replace the font, these are emitted in the same 4-bit packing
and consulted when a lookup misses, so text drawing is unchanged.

Nothing here is guessed. For each set the renderer is calibrated against
syllables the font does carry: it sweeps point size and baseline, bakes the
calibration list through the same pipeline, and keeps the setting whose height
and yOffset reproduce the font's own entries. The font's glyphs keep the full
advance box rather than being cropped to their ink - crop the ink and a syllable
sits left of and below its neighbours - so the render goes into that box and
only empty rows are trimmed.
"""
import io
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "data"))

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "firmware", "sdpro-clock-weather"))
FACE = "malgunbd.ttf"   # matched to the bundled font by pixel comparison
SS = 8


def bundled(table):
    """Codepoints the bundled font already carries in one of its two sets."""
    s = io.open(ROOT + "/src/display/UiTextFont.cpp", encoding="utf-8").read()
    start = s.index(table)
    end = s.index("LARGE_GLYPHS[] PROGMEM") if table.startswith("SMALL") else len(s)
    return set(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]{4})u,", s[start:end]))


# Hangul that appears in the firmware itself rather than in a table. The
# recovery guidance is the whole of it so far.
SCREEN_TEXT = [
    "아래 무선망에 접속한 뒤",
    "브라우저에서 아래 주소로",
    "들어가 WiFi 를 다시 정하세요",
    "WiFi 연결 안됨",
]

def small_missing():
    """Every Hangul the radar labels need that the Small set cannot draw.

    Derived, not hand-listed. The three tables are edited often, and a list kept
    by hand beside them goes stale silently - a name simply loses a character on
    the dial and nothing says so.
    """
    from airlines import AIRLINES
    from rotorcraft import ROTORCRAFT
    from airports import AIRPORTS

    used = set()
    for table in (AIRLINES, ROTORCRAFT, AIRPORTS):
        for _, name in table:
            used |= {ord(c) for c in name}
    # Screen wording is a fourth source. It is not a table, but it is Hangul the
    # firmware has to be able to draw, and leaving it out means a phrase loses a
    # syllable on a screen nobody sees until WiFi has already failed.
    for line in SCREEN_TEXT:
        used |= {ord(c) for c in line}
    used = {c for c in used if 0xAC00 <= c <= 0xD7A3}
    return sorted(used - bundled("SMALL_GLYPHS[] PROGMEM"))


SETS = [
    {
        "kind": "Large",
        "table": "LARGE_GLYPHS[] PROGMEM",
        "advance": 18,
        # The weather words, which are drawn at text size 2. Short and stable
        # enough to name outright.
        "missing": [0xAE68, 0xB208, 0xB9CE, 0xB9D1, 0xD750],
    },
    {
        "kind": "Small",
        "table": "SMALL_GLYPHS[] PROGMEM",
        "advance": 13,
        "missing": small_missing(),
    },
]

CALIBRATE = [0xAD6C, 0xB984, 0xC74C, 0xC77C, 0xBAA9, 0xC694,
             0xB144, 0xC6D4, 0xBE44, 0xC9C4, 0xB9BC, 0xD654]


def font_metrics(table):
    src = io.open(ROOT + "/src/display/UiTextFont.cpp", encoding="utf-8").read()
    start = src.index(table)
    end = src.index("LARGE_GLYPHS[] PROGMEM") if table.startswith("SMALL") else len(src)
    seg = src[start:end]
    out = {}
    for m in re.finditer(r"\{0x([0-9A-Fa-f]{4})u, \{(\d+), (\d+), (\d+), (-?\d+),", seg):
        out[int(m.group(1), 16)] = tuple(int(m.group(i)) for i in (2, 3, 4, 5))
    return out


def bake(ch, face, baseline, advance):
    box_h = advance * 3
    big = Image.new("L", (advance * SS, box_h * SS), 0)
    ImageDraw.Draw(big).text((advance * SS / 2.0, baseline * SS), ch, font=face, fill=255, anchor="ms")
    small = big.resize((advance, box_h), Image.LANCZOS)
    alpha = [p * 15 // 255 for p in small.getdata()]
    rows = [y for y in range(box_h) if any(alpha[(y * advance) + x] for x in range(advance))]
    if not rows:
        return None
    top, bottom = rows[0], rows[-1]
    return top, bottom - top + 1, alpha[top * advance:(bottom + 1) * advance]


def calibrate(advance, want):
    best = None
    for size4 in range(int(advance * SS * 0.6) * 4, int(advance * SS * 1.4) * 4, 2):
        face = ImageFont.truetype(FACE, size4 / 4.0)
        for baseline4 in range(int(advance * 0.6) * 4, int(advance * 1.6) * 4):
            baseline = baseline4 / 4.0
            score = 0
            for cp in CALIBRATE:
                if cp not in want:
                    continue
                got = bake(chr(cp), face, baseline, advance)
                if got is None:
                    score += 99
                    continue
                y_off, height, _ = got
                score += abs(height - want[cp][1]) + abs(y_off - want[cp][3])
            if best is None or score < best[0]:
                best = (score, size4 / 4.0, baseline)
    return best


def main():
    nl = chr(10)
    out = io.StringIO()
    out.write("// SPDX-License-Identifier: GPL-3.0-or-later" + nl)
    out.write("// Generated by scripts/gen_extra_glyphs.py - do not edit by hand." + nl)
    out.write("// Hangul the bundled UI font does not carry. Same 4-bit packing, and metrics" + nl)
    out.write("// calibrated against its own entries, so this is a lookup fallback rather than" + nl)
    out.write("// a replacement." + nl)
    out.write("#pragma once" + nl + nl)
    out.write("#include <pgmspace.h>" + nl + "#include <cstdint>" + nl + "#include <cstring>" + nl + nl)
    out.write('#include "UiTextFont.h"' + nl + nl)
    out.write("namespace ExtraGlyphs {" + nl + nl)
    out.write("struct Entry {" + nl + "    uint32_t codepoint;" + nl)
    out.write("    UiTextFont::Glyph glyph;" + nl + "};" + nl + nl)

    tables = []
    total = 0
    for spec in SETS:
        adv = spec["advance"]
        want = font_metrics(spec["table"])
        score, size, baseline = calibrate(adv, want)
        face = ImageFont.truetype(FACE, size)
        exact = 0
        for cp in CALIBRATE:
            if cp not in want:
                continue
            y_off, height, _ = bake(chr(cp), face, baseline, adv)
            if (height, y_off) == (want[cp][1], want[cp][3]):
                exact += 1
        print("  %-5s advance %2d: size %.1f baseline %.1f, error %d, %d/%d calibration glyphs exact"
              % (spec["kind"], adv, size, baseline, score, exact, len(CALIBRATE)))

        entries = []
        for cp in spec["missing"]:
            y_off, height, alpha = bake(chr(cp), face, baseline, adv)
            data = bytearray()
            for i in range(0, len(alpha), 2):
                hi = alpha[i] & 0x0F
                lo = alpha[i + 1] & 0x0F if i + 1 < len(alpha) else 0
                data.append((hi << 4) | lo)
            total += len(data)
            name = "%s_%04X" % (spec["kind"].upper(), cp)
            out.write("const uint8_t %s[] PROGMEM = {" % name + nl)
            for i in range(0, len(data), 16):
                out.write("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + "," + nl)
            out.write("};" + nl)
            entries.append((cp, adv, height, adv, y_off, name))

        arr = "ENTRIES_%s" % spec["kind"].upper()
        out.write(nl + "const Entry %s[] PROGMEM = {" % arr + nl)
        for cp, w, h, a, yo, name in entries:
            out.write("    {0x%04Xu, {%d, %d, %d, %d, %s}}," % (cp, w, h, a, yo, name) + nl)
        out.write("};" + nl + nl)
        tables.append((spec["kind"], arr))
        print("        baked %d syllables" % len(entries))

    out.write("inline auto glyph(UiTextFont::Kind kind, uint32_t codepoint) -> const UiTextFont::Glyph* {" + nl)
    out.write("    const Entry* table = nullptr;" + nl)
    out.write("    uint8_t count = 0;" + nl)
    out.write("    switch (kind) {" + nl)
    for kind, arr in tables:
        out.write("        case UiTextFont::Kind::%s:" % kind + nl)
        out.write("            table = %s;" % arr + nl)
        out.write("            count = sizeof(%s) / sizeof(%s[0]);" % (arr, arr) + nl)
        out.write("            break;" + nl)
    out.write("        default:" + nl + "            return nullptr;" + nl + "    }" + nl)
    out.write("    static UiTextFont::Glyph match{};" + nl)
    out.write("    for (uint8_t i = 0; i < count; ++i) {" + nl)
    out.write("        Entry entry{};" + nl)
    out.write("        memcpy_P(&entry, &table[i], sizeof(Entry));" + nl)
    out.write("        if (entry.codepoint != codepoint) continue;" + nl)
    out.write("        match = entry.glyph;" + nl)
    out.write("        return &match;" + nl)
    out.write("    }" + nl)
    out.write("    return nullptr;" + nl)
    out.write("}" + nl + nl)
    out.write("}  // namespace ExtraGlyphs" + nl)

    dest = ROOT + "/src/display/ExtraGlyphs.h"
    io.open(dest, "w", encoding="utf-8", newline="").write(out.getvalue())
    print("  total %d bytes" % total)
    print("  wrote", dest)


main()
