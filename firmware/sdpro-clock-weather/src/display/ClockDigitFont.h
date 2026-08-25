// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ClockDigitFont {

enum class Kind : uint8_t {
    Main = 0,
    Secondary = 1,
};

struct Glyph {
    uint8_t width;
    uint8_t height;
    uint8_t advance;
    int8_t yOffset;
    const uint8_t* bitmap;
};

struct FontSet {
    uint8_t lineHeight;
    uint8_t tracking;
    uint8_t bitsPerPixel;
};

auto glyph(Kind kind, char value) -> const Glyph*;
auto fontSet(Kind kind) -> const FontSet&;

}  // namespace ClockDigitFont
