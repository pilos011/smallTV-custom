#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <Updater.h>
#include <flash_hal.h>
#include <memory>
#include <time.h>
#include "display/ClockDashboardScene.h"
#include "display/ClockDigitFont.h"
#include "display/MondaineArt.h"
#include "display/DigitalArt.h"
#include "display/Airlines.h"
#include "display/Airports.h"
#include "display/Rotorcraft.h"
#include "display/ExtraGlyphs.h"
#include "display/UiTextFont.h"

namespace {

constexpr const char* FW_NAME = "SDP Clock Weather";
constexpr const char* FW_VERSION = "v1.0.7";
constexpr const char* FALLBACK_STA_SSID = "";
constexpr const char* FALLBACK_STA_PASS = "";
constexpr const char* AP_SSID = "SDP-Recovery";
constexpr const char* CONFIG_PATH = "/config.json";
constexpr const char* KMA_HOST = "apihub.kma.go.kr";
constexpr uint32_t STA_TIMEOUT_MS = 15000;
constexpr uint32_t BODY_TIMEOUT_MS = 25000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 1000;
constexpr uint32_t WEATHER_INTERVAL_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MS = 15UL * 1000UL;
constexpr size_t STREAM_BUF_SIZE = 1024;
constexpr uint16_t LCD_BLACK = TFT_BLACK;
constexpr int16_t SCREEN_W = 240;
constexpr int16_t SCREEN_H = 240;
constexpr int16_t HEADER_X = 8;
constexpr int16_t HEADER_Y = 4;
constexpr int16_t CURRENT_ICON_SIZE = 52;
constexpr int16_t TIME_LEFT_X = 5;
constexpr int16_t TIME_TOP_Y = 48;
constexpr int16_t DATE_Y = 108;
constexpr int16_t DATE_LINE_Y = 115;
constexpr uint8_t DATE_LINE_TEXT_SIZE = 2;
constexpr int16_t DIVIDER_Y = 130;
constexpr int16_t FORECAST_TOP = 146;
constexpr int16_t FORECAST_LEFT = 6;
constexpr int16_t FORECAST_GAP = 4;
constexpr int16_t FORECAST_WIDTH = 54;
constexpr int16_t FORECAST_HEIGHT = 89;
constexpr int16_t FORECAST_ICON_SIZE = 28;
constexpr int MINUTES_PER_DAY = 24 * 60;

// Screens that can take part in the rotation. Stored in config as a bitmask so
// new screens can be appended without breaking an existing config.json.
enum ScreenId : uint8_t {
    SCREEN_CLOCK_WEATHER = 0,
    SCREEN_ANALOG = 1,
    SCREEN_MONDAINE = 2,
    SCREEN_MONDAINE_WHITE = 3,
    SCREEN_DIGITAL = 4,
    SCREEN_WEATHER_DIGITAL = 5,
    SCREEN_DATE_DIGITAL = 6,
    SCREEN_ALBUM = 7,
    SCREEN_RADAR = 8,
    SCREEN_COUNT = 9,
};
// Eight screens is exactly a byte, so the mask is widened here rather than on
// the next screen, when a too-small type would silently drop the top bit.
constexpr uint16_t SCREEN_MASK_ALL = static_cast<uint16_t>((1U << SCREEN_COUNT) - 1U);
constexpr uint16_t THEME_INTERVAL_MIN_S = 3;
constexpr uint16_t THEME_INTERVAL_MAX_S = 3600;

// Analog face geometry. The dial sits 1 px off the top-left and 5 px off the
// bottom-right so the case colour reads as a shadow instead of a border.
constexpr int16_t ANALOG_CX = 118;
constexpr int16_t ANALOG_CY = 118;
constexpr int16_t ANALOG_R_DIAL = 117;
constexpr int16_t ANALOG_TICK_IN = 95;
constexpr int16_t ANALOG_TICK_OUT = 107;
constexpr int16_t ANALOG_R_NUM = 101;
constexpr int16_t ANALOG_NUM_H = 28;
constexpr int16_t ANALOG_L_HOUR = 62;
constexpr int16_t ANALOG_L_MIN = 90;
constexpr int16_t ANALOG_L_SEC = 106;
constexpr int16_t ANALOG_TAIL_HOUR = 12;
constexpr int16_t ANALOG_TAIL_MIN = 14;
constexpr int16_t ANALOG_TAIL_SEC = 16;
constexpr int16_t ANALOG_HUB_R = 7;
constexpr int16_t ANALOG_HAND_INNER = 6;
constexpr int16_t ANALOG_BAND_H = 24;  // 240 x 24 x 2 = 11520 bytes

// Mondaine SBB face. Centred, unlike the first analog face, because the
// reference is a symmetric railway clock. Values are fractions of the 118 px
// dial radius, resolved to pixels.
constexpr int16_t MONDAINE_CX = 120;
constexpr int16_t MONDAINE_CY = 120;
constexpr int16_t MONDAINE_R = 118;
constexpr float MONDAINE_TICK_IN = 98.5f;
constexpr float MONDAINE_TICK_OUT = 106.8f;
constexpr float MONDAINE_TICK_W = 2.8f;
constexpr float MONDAINE_BAR_IN = 80.8f;
constexpr float MONDAINE_BAR_OUT = 106.8f;
constexpr float MONDAINE_BAR_W = 8.8f;
constexpr float MONDAINE_MIN_LEN = 100.9f;
constexpr float MONDAINE_MIN_W = 6.5f;
constexpr float MONDAINE_HOUR_LEN = 70.8f;
constexpr float MONDAINE_HOUR_W = 9.2f;
constexpr float MONDAINE_SEC_LEN = 68.4f;
constexpr float MONDAINE_SEC_W = 2.0f;
constexpr float MONDAINE_SEC_DISC = 8.3f;
constexpr float MONDAINE_SEC_TAIL = 13.6f;
constexpr float MONDAINE_HAND_TAIL = 12.4f;  // hour and minute cross behind the centre
// Two offset passes rather than one: the far pass is light and the near pass
// adds to it, which reads as a soft edge rather than a hard copy of the hand.
struct ShadowPass {
    float dx;
    float dy;
    uint8_t alpha;  // of 15
};
constexpr ShadowPass MONDAINE_SHADOW[] = {{2.5f, 4.0f, 2}, {1.2f, 2.0f, 3}};
constexpr float MONDAINE_SHADOW_REACH = 5.0f;  // furthest offset, for the dirty span

// Room for the analog variants still to be built. ANALOG_FACE_COUNT is what the
// firmware can actually render, and the web UI builds its face list from it, so
// adding a face means bumping the count rather than editing the page.
constexpr uint8_t ANALOG_FACE_MAX = 6;
constexpr uint8_t ANALOG_FACE_COUNT = 6;
constexpr int16_t DIGITAL_MARGIN = 15;

// Clock placement on the weather and date faces, taken from the references.
// Shared because the blinking colon repaints on its own and has to sit exactly
// where the full paint put it.
// The icon owns everything left of the temperature column and above the clock.
// Its right edge is fixed at where the longest condition word would start - four
// syllables right-aligned at 214 - rather than at where today's word starts, so
// the icon does not shift sideways when the weather changes.
constexpr int16_t WEATHER_ICON_BOX = 88;
constexpr int16_t WEATHER_ICON_AREA_R = 134;
constexpr int16_t WEATHER_ICON_AREA_B = 144;
constexpr int16_t WEATHER_ICON_X = (WEATHER_ICON_AREA_R - WEATHER_ICON_BOX) / 2;
constexpr int16_t WEATHER_ICON_Y = (WEATHER_ICON_AREA_B - WEATHER_ICON_BOX) / 2;

constexpr float WEATHER_FACE_CLOCK_Y = 148.0f;
constexpr float WEATHER_FACE_DIGIT_H = 62.0f;
constexpr float DATE_FACE_CLOCK_Y = 26.0f;
constexpr float DATE_FACE_DIGIT_H = 78.0f;
constexpr uint32_t COLON_BLINK_MS = 500;  // on half a second, off half a second

struct AnalogFace {
    uint32_t dialRgb;
    uint32_t caseRgb;
    uint32_t lumeRgb;
    uint32_t handRgb;
    uint32_t accentRgb;  // only the digital faces use this
};

// What a device answers to before anyone has set anything. Baking the real
// password into the firmware made every build personal to one owner; it lives
// in the config now, so the same image serves anybody.
constexpr const char* AUTH_DEFAULT_PASSWORD = "0000";
constexpr const char* AUTH_COOKIE = "sdp_auth";
constexpr size_t AUTH_PASSWORD_MAX = 32;

// For the first two minutes after a boot the menu is not locked, which is the
// way back in when the password has been forgotten: power-cycle the device and
// set a new one. It is deliberately not mentioned in the UI.
constexpr uint32_t AUTH_GRACE_MS = 120000;

// A place the radar can be pointed at by name - home, the office, wherever
// the device gets carried. Only what changes with the place is stored: the
// position and the range. Poll rate, orientation and the rest describe the
// device, not the location.
struct RadarPreset {
    char name[25];      // up to eight Hangul syllables and a terminator
    float lat = 0.0f;
    float lon = 0.0f;
    uint16_t km = 10;
};
constexpr uint8_t RADAR_PRESET_MAX = 6;

struct AppConfig {
    String ssid;
    String pass;
    String webPassword = AUTH_DEFAULT_PASSWORD;
    String location = "Baekseok";
    String kmaKey;
    int nx = 57;
    int ny = 128;
    int timezoneOffsetMinutes = 540;
    bool weatherEnabled = true;
    bool clock24h = true;
    uint8_t brightness = 88;
    bool nightModeEnabled = false;
    uint8_t nightBrightness = 20;
    int nightStartMinutes = 23 * 60;
    int nightStopMinutes = 7 * 60;
    uint16_t screens = 1U << SCREEN_CLOCK_WEATHER;
    uint16_t themeIntervalSeconds = 10;
    uint16_t albumIntervalSeconds = 10;
    float radarLat = 0.0f;
    float radarLon = 0.0f;
    uint16_t radarRangeKm = 10;
    uint16_t radarPollSec = 10;
    uint16_t radarMinAltFt = 0;
    // Which compass bearing the top of the screen points at. 0 is north-up, the
    // way a chart is drawn; set it to the way the device is actually facing and
    // what is ahead of you on the dial is ahead of you in the room.
    uint16_t radarUpDeg = 0;
    // Looking a destination up is a second TLS handshake, so it can be turned
    // off by anyone who would rather have the sweep run clean.
    bool radarRoutes = true;
    RadarPreset radarPresets[RADAR_PRESET_MAX];
    uint8_t radarPresetCount = 0;
    // Rotation order. The bitmask says which screens are in the loop; this says
    // in what sequence, which the bitmask cannot express on its own.
    uint8_t screenOrder[SCREEN_COUNT] = {SCREEN_CLOCK_WEATHER, SCREEN_ANALOG, SCREEN_MONDAINE,
                                         SCREEN_MONDAINE_WHITE, SCREEN_DIGITAL, SCREEN_WEATHER_DIGITAL,
                                         SCREEN_DATE_DIGITAL, SCREEN_ALBUM, SCREEN_RADAR};
    // Per-face colours, held as 24-bit RGB and quantised to RGB565 on use.
    AnalogFace analogFaces[ANALOG_FACE_MAX] = {
        {0x000000, 0x000008, 0x00F0FF, 0xFF0000, 0x000000},  // Analog
        {0x000000, 0x000000, 0xFFFFFF, 0xD00000, 0x000000},  // Mondaine, white ink on black
        {0xFFFFFF, 0xD0D0D0, 0x000000, 0xD00000, 0x000000},  // Mondaine white
        {0x000000, 0x000000, 0xFFFFFF, 0xFFFF00, 0x000000},  // Digital
        {0x000000, 0x000000, 0xFFFFFF, 0xFFFF00, 0x008000},  // Weather digital
        {0x000000, 0xFFFFFF, 0xFFFFFF, 0xFFFF00, 0x008000},  // Date digital, case is the date line
    };
};

struct ForecastItem {
    bool valid = false;
    String hour = "--";
    time_t timestamp = 0;
    float temp = NAN;
    int sky = 1;
    int pty = 0;
    int humidity = -1;
    String rain = "";
};

struct WeatherData {
    bool valid = false;
    bool fetching = false;
    String status = "not fetched";
    float temp = NAN;
    int humidity = -1;
    String rain = "0";
    int pty = 0;
    int sky = 1;
    String updated = "--";
    ForecastItem fcst[5];
};

ESP8266WebServer server(80);
WiFiServer rawServer(8080);
TFT_eSPI tft;
TFT_eSprite analogBand(&tft);
AppConfig cfg;
WeatherData weather;
String lastStatus = "booting";
bool fsMounted = false;
uint32_t lastDisplayMs = 0;
uint32_t lastWeatherMs = 0;
// A failed fetch used to leave lastWeatherMs at 0, which reads as "last fetched
// at boot" and locked the retry out for the full interval. These track the
// failure separately so a miss is retried in a minute, not half an hour, and so
// the first fetch after a reboot waits for the clock instead of racing it.
uint32_t lastWeatherTryMs = 0;
bool weatherBootDone = false;
time_t lastDrawSecond = -1;
time_t lastStaticMinute = -1;
bool lastTimeOk = false;
String cacheIp;
String cacheHigh;
String cacheTime;
String cacheSeconds;
String cacheDate;
String cacheMetric;
String cacheWeatherStatus;
String cacheForecast[4];
int cacheCurrentIcon = -999;
int cacheForecastIcon[4] = {-999, -999, -999, -999};
bool screenChromeDrawn = false;
uint8_t lastAppliedBrightness = 255;

uint8_t activeScreen = SCREEN_CLOCK_WEATHER;
int16_t digitalLastStamp = -1;
uint32_t lastScreenSwitchMs = 0;
bool analogChromeDrawn = false;
bool analogBandReady = false;
uint32_t analogFrameUs = 0;
uint32_t analogPushUs = 0;
uint8_t analogBandsPushed = 0;
float analogPrevSecond = -999.0f;
// Angles the whole face is currently drawn at. Bands are only repainted
// when they are dirty, so every band has to agree on where the slow hands
// are. Tracking the live angle instead would leave stale bands behind and
// tear the hand into two offset halves.
float analogDrawnHour = -999.0f;
float analogDrawnMinute = -999.0f;
String authToken;

void wdtYield() {
    ESP.wdtFeed();
    delay(0);
}

int currentLocalMinuteOfDay() {
    const time_t now = time(nullptr);
    if (now < 1700000000) return -1;
    tm timeInfo;
    localtime_r(&now, &timeInfo);
    return (timeInfo.tm_hour * 60) + timeInfo.tm_min;
}

int normalizedMinute(int minute) {
    minute %= MINUTES_PER_DAY;
    if (minute < 0) minute += MINUTES_PER_DAY;
    return minute;
}

bool isNightModeActive() {
    if (!cfg.nightModeEnabled) return false;
    const int minute = currentLocalMinuteOfDay();
    if (minute < 0) return false;
    const int start = normalizedMinute(cfg.nightStartMinutes);
    const int stop = normalizedMinute(cfg.nightStopMinutes);
    if (start == stop) return true;
    if (start < stop) return minute >= start && minute < stop;
    return minute >= start || minute < stop;
}

uint8_t effectiveBrightness() {
    const uint8_t day = constrain(cfg.brightness, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
    if (!isNightModeActive()) return day;
    return constrain(cfg.nightBrightness, static_cast<uint8_t>(0), static_cast<uint8_t>(100));
}

void applyBrightness() {
    const uint8_t value = effectiveBrightness();
    if (lastAppliedBrightness == value) return;
    analogWrite(5, map(value, 0, 100, 1023, 0));
    lastAppliedBrightness = value;
}

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }
String two(int v) { return v < 10 ? String("0") + String(v) : String(v); }

String lcdString(const std::string& value) { return String(value.c_str()); }

uint8_t readPackedAlpha(const uint8_t* bitmap, size_t pixelIndex, uint8_t bitsPerPixel) {
    const size_t bitIndex = pixelIndex * bitsPerPixel;
    const uint8_t byteValue = pgm_read_byte(bitmap + (bitIndex / 8U));
    const uint8_t shift = static_cast<uint8_t>(8U - bitsPerPixel - (bitIndex % 8U));
    return static_cast<uint8_t>((byteValue >> shift) & ((1U << bitsPerPixel) - 1U));
}

uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha, uint8_t maxAlpha) {
    if (alpha == 0) return bg;
    if (alpha >= maxAlpha) return fg;
    const uint8_t fr = static_cast<uint8_t>(((fg >> 11) & 0x1F) << 3);
    const uint8_t fgG = static_cast<uint8_t>(((fg >> 5) & 0x3F) << 2);
    const uint8_t fb = static_cast<uint8_t>((fg & 0x1F) << 3);
    const uint8_t br = static_cast<uint8_t>(((bg >> 11) & 0x1F) << 3);
    const uint8_t bgG = static_cast<uint8_t>(((bg >> 5) & 0x3F) << 2);
    const uint8_t bb = static_cast<uint8_t>((bg & 0x1F) << 3);
    const uint8_t r = static_cast<uint8_t>((fr * alpha + br * (maxAlpha - alpha)) / maxAlpha);
    const uint8_t g = static_cast<uint8_t>((fgG * alpha + bgG * (maxAlpha - alpha)) / maxAlpha);
    const uint8_t b = static_cast<uint8_t>((fb * alpha + bb * (maxAlpha - alpha)) / maxAlpha);
    return rgb(r, g, b);
}

int16_t digitPixelWidth(char value, int16_t height) {
    const auto kind = height >= 40 ? ClockDigitFont::Kind::Main : ClockDigitFont::Kind::Secondary;
    const ClockDigitFont::Glyph* glyph = ClockDigitFont::glyph(kind, value);
    return glyph == nullptr ? 0 : glyph->width;
}

int16_t digitCellWidth(char value, int16_t height) {
    if (value == ':') return height >= 40 ? 20 : 5;
    int16_t maxWidth = 0;
    for (char d = '0'; d <= '9'; ++d) maxWidth = max<int16_t>(maxWidth, digitPixelWidth(d, height));
    return maxWidth;
}

void drawClockDigit(int16_t x, int16_t y, char value, int16_t height, uint16_t color) {
    const auto kind = height >= 40 ? ClockDigitFont::Kind::Main : ClockDigitFont::Kind::Secondary;
    const ClockDigitFont::Glyph* glyph = ClockDigitFont::glyph(kind, value);
    if (glyph == nullptr) return;
    const auto& font = ClockDigitFont::fontSet(kind);
    const uint8_t maxAlpha = static_cast<uint8_t>((1U << font.bitsPerPixel) - 1U);
    const int16_t drawY = static_cast<int16_t>(y + glyph->yOffset);
    const size_t pixels = static_cast<size_t>(glyph->width) * glyph->height;
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t alpha = readPackedAlpha(glyph->bitmap, i, font.bitsPerPixel);
        if (alpha == 0) continue;
        int16_t px = static_cast<int16_t>(x + (i % glyph->width));
        int16_t py = static_cast<int16_t>(drawY + (i / glyph->width));
        tft.drawPixel(px, py, blend565(color, LCD_BLACK, alpha, maxAlpha));
    }
}

void drawClockDigits(int16_t x, int16_t y, const String& text, int16_t height, uint16_t color) {
    const auto kind = height >= 40 ? ClockDigitFont::Kind::Main : ClockDigitFont::Kind::Secondary;
    const auto& font = ClockDigitFont::fontSet(kind);
    int16_t cursor = x;
    for (uint32_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        int16_t cell = digitCellWidth(c, height);
        int16_t drawX = static_cast<int16_t>(cursor + max<int16_t>((cell - digitPixelWidth(c, height)) / 2, 0));
        drawClockDigit(drawX, y, c, height, color);
        cursor = static_cast<int16_t>(cursor + cell + font.tracking);
    }
}

size_t utf8CharLength(uint8_t leadByte) {
    if ((leadByte & 0x80U) == 0U) return 1;
    if ((leadByte & 0xE0U) == 0xC0U) return 2;
    if ((leadByte & 0xF0U) == 0xE0U) return 3;
    if ((leadByte & 0xF8U) == 0xF0U) return 4;
    return 1;
}

String readUtf8Char(const char* text, size_t textLength, size_t& index) {
    const size_t charLength = min(utf8CharLength(static_cast<uint8_t>(text[index])), textLength - index);
    String value;
    value.reserve(charLength);
    for (size_t offset = 0; offset < charLength; ++offset) value += text[index + offset];
    index += charLength;
    return value;
}

uint32_t utf8Codepoint(const String& glyph) {
    const char* bytes = glyph.c_str();
    const size_t length = glyph.length();
    if (length == 0) return 0;
    const uint8_t first = static_cast<uint8_t>(bytes[0]);
    if ((first & 0x80U) == 0U) return first;
    if (length >= 2 && (first & 0xE0U) == 0xC0U) {
        return ((first & 0x1FU) << 6) | (static_cast<uint8_t>(bytes[1]) & 0x3FU);
    }
    if (length >= 3 && (first & 0xF0U) == 0xE0U) {
        return ((first & 0x0FU) << 12) | ((static_cast<uint8_t>(bytes[1]) & 0x3FU) << 6) |
               (static_cast<uint8_t>(bytes[2]) & 0x3FU);
    }
    if (length >= 4 && (first & 0xF8U) == 0xF0U) {
        return ((first & 0x07U) << 18) | ((static_cast<uint8_t>(bytes[1]) & 0x3FU) << 12) |
               ((static_cast<uint8_t>(bytes[2]) & 0x3FU) << 6) | (static_cast<uint8_t>(bytes[3]) & 0x3FU);
    }
    return first;
}

UiTextFont::Kind uiKind(uint8_t textSize) {
    return textSize >= 2 ? UiTextFont::Kind::Large : UiTextFont::Kind::Small;
}

// The bundled font carries 433 syllables but not quite every one the Korean
// weather words need, so the handful that are missing are baked separately and
// consulted here. Every lookup goes through this, which keeps measuring and
// drawing agreeing on what can be rendered.
const UiTextFont::Glyph* uiGlyph(UiTextFont::Kind kind, uint32_t codepoint) {
    const UiTextFont::Glyph* found = UiTextFont::glyph(kind, codepoint);
    if (found != nullptr) return found;
    return ExtraGlyphs::glyph(kind, codepoint);
}

bool canUseUiFont(const String& text, uint8_t textSize) {
    if (text.isEmpty()) return false;
    const UiTextFont::Kind kind = uiKind(textSize);
    const char* raw = text.c_str();
    const size_t len = text.length();
    size_t index = 0;
    while (index < len) {
        String ch = readUtf8Char(raw, len, index);
        if (ch == "\r" || ch == "\n" || ch == "\t") continue;
        if (uiGlyph(kind, utf8Codepoint(ch)) == nullptr) return false;
    }
    return true;
}

int16_t measureUiText(const String& text, uint8_t textSize) {
    const UiTextFont::Kind kind = uiKind(textSize);
    const UiTextFont::FontSet& font = UiTextFont::fontSet(kind);
    int16_t width = 0;
    bool first = true;
    const char* raw = text.c_str();
    const size_t len = text.length();
    size_t index = 0;
    while (index < len) {
        String ch = readUtf8Char(raw, len, index);
        if (ch == "\r" || ch == "\n") break;
        if (ch == "\t") {
            if (!first) width += font.tracking;
            width += font.lineHeight / 2;
            first = false;
            continue;
        }
        const UiTextFont::Glyph* glyph = uiGlyph(kind, utf8Codepoint(ch));
        if (glyph == nullptr) continue;
        if (!first) width += font.tracking;
        width += glyph->advance;
        first = false;
    }
    return width;
}

void drawUiGlyph(TFT_eSPI& g, int16_t x, int16_t y, uint32_t codepoint, uint8_t textSize, uint16_t fg,
                 uint16_t bg) {
    const UiTextFont::Kind kind = uiKind(textSize);
    const UiTextFont::Glyph* glyph = uiGlyph(kind, codepoint);
    if (glyph == nullptr) return;
    const UiTextFont::FontSet& font = UiTextFont::fontSet(kind);
    const uint8_t maxAlpha = static_cast<uint8_t>((1U << font.bitsPerPixel) - 1U);
    const int16_t drawY = y + glyph->yOffset;
    const size_t pixels = static_cast<size_t>(glyph->width) * glyph->height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t alpha = readPackedAlpha(glyph->bitmap, i, font.bitsPerPixel);
        if (alpha == 0) continue;
        g.drawPixel(x + (i % glyph->width), drawY + (i / glyph->width), blend565(fg, bg, alpha, maxAlpha));
    }
}

void drawTextAt(TFT_eSPI& g, int16_t x, int16_t y, const String& text, uint8_t textSize, uint16_t fg,
                uint16_t bg) {
    if (canUseUiFont(text, textSize)) {
        const UiTextFont::Kind kind = uiKind(textSize);
        const UiTextFont::FontSet& font = UiTextFont::fontSet(kind);
        int16_t cursor = x;
        bool first = true;
        const char* raw = text.c_str();
        const size_t len = text.length();
        size_t index = 0;
        while (index < len) {
            String ch = readUtf8Char(raw, len, index);
            if (ch == "\r" || ch == "\n") break;
            if (!first) cursor += font.tracking;
            const uint32_t codepoint = utf8Codepoint(ch);
            const UiTextFont::Glyph* glyph = uiGlyph(kind, codepoint);
            if (glyph != nullptr) {
                drawUiGlyph(g, cursor, y, codepoint, textSize, fg, bg);
                cursor += glyph->advance;
            }
            first = false;
        }
        return;
    }
    g.setTextColor(fg, bg);
    g.setTextSize(textSize);
    g.setCursor(x, y);
    g.print(text);
    g.setTextSize(1);
}

// Existing callers draw straight to the panel.
void drawTextAt(int16_t x, int16_t y, const String& text, uint8_t textSize, uint16_t fg, uint16_t bg) {
    drawTextAt(tft, x, y, text, textSize, fg, bg);
}

int16_t measureText(const String& text, uint8_t textSize) {
    if (canUseUiFont(text, textSize)) return measureUiText(text, textSize);
    tft.setTextSize(textSize);
    int16_t width = tft.textWidth(text);
    tft.setTextSize(1);
    return width;
}

uint8_t selectTextSizeToFit(const String& text, uint8_t maxSize, uint8_t minSize, int16_t maxWidth) {
    for (int size = maxSize; size >= minSize; --size) {
        if (measureText(text, static_cast<uint8_t>(size)) <= maxWidth) return static_cast<uint8_t>(size);
    }
    return minSize;
}

String trimTextToWidth(const String& text, uint8_t textSize, int16_t maxWidth) {
    if (measureText(text, textSize) <= maxWidth) return text;
    String out = text;
    while (out.length() > 0 && measureText(out + "...", textSize) > maxWidth) {
        out.remove(out.length() - 1);
    }
    return out + "...";
}

void drawCenteredText(int16_t y, const String& text, uint8_t textSize, uint16_t fg, uint16_t bg, int16_t minX,
                      int16_t maxWidth) {
    int16_t x = minX;
    const int16_t width = measureText(text, textSize);
    if (maxWidth > width) x = minX + ((maxWidth - width) / 2);
    drawTextAt(x, y, text, textSize, fg, bg);
}

const char* iconSlot(int sky, int pty) {
    if (pty == 1 || pty == 2 || pty == 5 || pty == 6) return "rain";
    if (pty == 3 || pty == 7) return "snow";
    if (sky <= 1) return "clear";
    if (sky == 3) return "cloudy";
    return "cloudy";
}

String sizedIconPath(const char* slot, int16_t size) {
    if (size <= 16 && strcmp(slot, "umbrella") == 0) return "/weather-icons/umbrella-16.bmp";
    if (size <= 28) return String("/weather-icons/") + slot + "-28.bmp";
    if (size <= 52) return String("/weather-icons/") + slot + "-52.bmp";
    return String("/weather-icons/") + slot + ".bmp";
}

bool drawBmpIcon(TFT_eSPI& g, const String& path, int16_t x, int16_t y, int16_t maxSize) {
    if (!fsMounted || !LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, "r");
    if (!file || file.read() != 'B' || file.read() != 'M') {
        if (file) file.close();
        return false;
    }
    auto read16 = [&]() -> uint16_t {
        uint8_t lo = file.read();
        uint8_t hi = file.read();
        return static_cast<uint16_t>(lo | (hi << 8));
    };
    auto read32 = [&]() -> uint32_t {
        uint32_t b0 = file.read();
        uint32_t b1 = file.read();
        uint32_t b2 = file.read();
        uint32_t b3 = file.read();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    };
    (void)read32();
    (void)read32();
    uint32_t dataOffset = read32();
    (void)read32();
    int32_t width = static_cast<int32_t>(read32());
    int32_t height = static_cast<int32_t>(read32());
    if (read16() != 1 || read16() != 24 || width <= 0 || height == 0) {
        file.close();
        return false;
    }
    int32_t absHeight = abs(height);
    uint32_t rowSize = static_cast<uint32_t>((width * 3 + 3) & ~3);
    std::unique_ptr<uint8_t[]> row(new uint8_t[rowSize]);
    if (!row) {
        file.close();
        return false;
    }
    int16_t drawWidth = width;
    int16_t drawHeight = absHeight;
    if (drawWidth > maxSize || drawHeight > maxSize) {
        float scale = min(static_cast<float>(maxSize) / drawWidth, static_cast<float>(maxSize) / drawHeight);
        drawWidth = max<int16_t>(1, lroundf(drawWidth * scale));
        drawHeight = max<int16_t>(1, lroundf(drawHeight * scale));
    }
    int16_t xOff = static_cast<int16_t>((maxSize - drawWidth) / 2);
    int16_t yOff = static_cast<int16_t>((maxSize - drawHeight) / 2);
    for (int32_t rowIndex = 0; rowIndex < drawHeight; ++rowIndex) {
        int32_t sampleRow = min<int32_t>(absHeight - 1, (static_cast<int64_t>(rowIndex) * absHeight) / drawHeight);
        int32_t sourceRow = height > 0 ? (absHeight - 1 - sampleRow) : sampleRow;
        file.seek(dataOffset + static_cast<uint32_t>(sourceRow) * rowSize, SeekSet);
        if (file.read(row.get(), rowSize) != static_cast<int>(rowSize)) {
            file.close();
            return false;
        }
        for (int32_t col = 0; col < drawWidth; ++col) {
            int32_t sampleCol = min<int32_t>(width - 1, (static_cast<int64_t>(col) * width) / drawWidth);
            size_t off = static_cast<size_t>(sampleCol) * 3U;
            uint8_t blue = row[off];
            uint8_t green = row[off + 1U];
            uint8_t red = row[off + 2U];
            if (red > 8 || green > 8 || blue > 8) {
                g.drawPixel(static_cast<int16_t>(x + xOff + col), static_cast<int16_t>(y + yOff + rowIndex),
                              rgb(red, green, blue));
            }
        }
        wdtYield();
    }
    file.close();
    return true;
}

void resetDisplayCache() {
    lastDrawSecond = -1;
    lastStaticMinute = -1;
    cacheIp = "";
    cacheHigh = "";
    cacheTime = "";
    cacheSeconds = "";
    cacheDate = "";
    cacheMetric = "";
    cacheWeatherStatus = "";
    for (auto& item : cacheForecast) item = "";
    cacheCurrentIcon = -999;
    cacheForecastIcon[0] = cacheForecastIcon[1] = cacheForecastIcon[2] = cacheForecastIcon[3] = -999;
    screenChromeDrawn = false;
}

bool validFsPath(const String& path) {
    return path.startsWith("/") && path.indexOf("..") < 0 && path.indexOf('\\') < 0 && path.length() >= 2 &&
           path.length() <= 96;
}

void ensureParentDirs(const String& path) {
    int slash = path.indexOf('/', 1);
    while (slash > 0) {
        const String dir = path.substring(0, slash);
        if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
        slash = path.indexOf('/', slash + 1);
    }
}

void sendText(int code, const String& body) {
    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.send(code, F("text/plain"), body);
}

void sendJson(int code, const String& body) {
    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.send(code, F("application/json"), body);
}

// Replaces the whole preset list from a JSON array, which is how both the
// config file and the API hand it over. Whole-list replacement rather than
// add/remove verbs for the same reason screen_order works that way: the web
// page owns the list, and two verbs racing each other need referee code that
// a single honest assignment does not.
void loadRadarPresets(JsonVariantConst v) {
    if (!v.is<JsonArrayConst>()) return;
    cfg.radarPresetCount = 0;
    for (JsonObjectConst o : v.as<JsonArrayConst>()) {
        if (cfg.radarPresetCount >= RADAR_PRESET_MAX) break;
        const char* name = o["name"] | "";
        const float lat = o["lat"] | 0.0f;
        const float lon = o["lon"] | 0.0f;
        // 0,0 is the radar's own off switch, so it cannot be a place worth
        // naming - and a nameless place cannot be picked from a list.
        if (name[0] == 0 || (lat == 0.0f && lon == 0.0f)) continue;
        if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) continue;
        RadarPreset& p = cfg.radarPresets[cfg.radarPresetCount++];
        strlcpy(p.name, name, sizeof(p.name));
        p.lat = lat;
        p.lon = lon;
        uint32_t km = o["km"] | 10U;
        if (km < 2) km = 2;
        if (km > 400) km = 400;
        p.km = static_cast<uint16_t>(km);
    }
}

void emitRadarPresets(JsonDocument& doc) {
    JsonArray arr = doc["radar_presets"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.radarPresetCount; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = cfg.radarPresets[i].name;
        o["lat"] = cfg.radarPresets[i].lat;
        o["lon"] = cfg.radarPresets[i].lon;
        o["km"] = cfg.radarPresets[i].km;
    }
}

bool saveConfig() {
    if (!fsMounted && !LittleFS.begin()) return false;
    fsMounted = true;
    JsonDocument doc;
    doc["ssid"] = cfg.ssid;
    doc["pass"] = cfg.pass;
    doc["web_password"] = cfg.webPassword;
    doc["location"] = cfg.location;
    doc["kma_key"] = cfg.kmaKey;
    doc["nx"] = cfg.nx;
    doc["ny"] = cfg.ny;
    doc["timezone_offset_minutes"] = cfg.timezoneOffsetMinutes;
    doc["weather_enabled"] = cfg.weatherEnabled;
    doc["clock_24h"] = cfg.clock24h;
    doc["brightness"] = cfg.brightness;
    doc["night_mode_enabled"] = cfg.nightModeEnabled;
    doc["night_brightness"] = cfg.nightBrightness;
    doc["night_start_minutes"] = cfg.nightStartMinutes;
    doc["night_stop_minutes"] = cfg.nightStopMinutes;
    doc["screens"] = cfg.screens;
    doc["album_interval_seconds"] = cfg.albumIntervalSeconds;
    doc["radar_lat"] = cfg.radarLat;
    doc["radar_lon"] = cfg.radarLon;
    doc["radar_range_km"] = cfg.radarRangeKm;
    doc["radar_poll_sec"] = cfg.radarPollSec;
    doc["radar_min_alt_ft"] = cfg.radarMinAltFt;
    doc["radar_up_deg"] = cfg.radarUpDeg;
    doc["radar_routes"] = cfg.radarRoutes;
    emitRadarPresets(doc);
    {
        JsonArray order = doc["screen_order"].to<JsonArray>();
        for (uint8_t i = 0; i < SCREEN_COUNT; ++i) order.add(cfg.screenOrder[i]);
    }
    doc["theme_interval_seconds"] = cfg.themeIntervalSeconds;
    JsonArray faces = doc["analog_faces"].to<JsonArray>();
    for (uint8_t i = 0; i < ANALOG_FACE_MAX; ++i) {
        JsonObject face = faces.add<JsonObject>();
        face["dial"] = cfg.analogFaces[i].dialRgb;
        face["case"] = cfg.analogFaces[i].caseRgb;
        face["lume"] = cfg.analogFaces[i].lumeRgb;
        face["hand"] = cfg.analogFaces[i].handRgb;
        face["accent"] = cfg.analogFaces[i].accentRgb;
    }
    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

// Takes whatever the caller offers and makes a permutation of it: ids that are
// out of range or repeated are dropped, and anything left out is appended in
// its natural position. A half-written order still leaves every screen
// reachable, which a straight copy would not.
void setScreenOrder(const uint8_t* wanted, size_t count) {
    uint8_t built[SCREEN_COUNT];
    bool taken[SCREEN_COUNT] = {false};
    size_t n = 0;
    for (size_t i = 0; i < count && n < SCREEN_COUNT; ++i) {
        const uint8_t id = wanted[i];
        if (id >= SCREEN_COUNT || taken[id]) continue;
        taken[id] = true;
        built[n++] = id;
    }
    for (uint8_t id = 0; id < SCREEN_COUNT && n < SCREEN_COUNT; ++id) {
        if (!taken[id]) built[n++] = id;
    }
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) cfg.screenOrder[i] = built[i];
}

uint8_t screenOrderIndex(uint8_t screen) {
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
        if (cfg.screenOrder[i] == screen) return i;
    }
    return 0;
}

void loadConfig() {
    cfg.ssid = WiFi.SSID();
    if (!fsMounted && !LittleFS.begin()) return;
    fsMounted = true;
    if (!LittleFS.exists(CONFIG_PATH)) {
        saveConfig();
        return;
    }
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;
    cfg.ssid = doc["ssid"] | cfg.ssid;
    cfg.pass = doc["pass"] | cfg.pass;
    cfg.webPassword = doc["web_password"] | cfg.webPassword;
    if (cfg.webPassword.length() == 0) cfg.webPassword = AUTH_DEFAULT_PASSWORD;
    cfg.location = doc["location"] | cfg.location;
    cfg.kmaKey = doc["kma_key"] | cfg.kmaKey;
    cfg.nx = doc["nx"] | cfg.nx;
    cfg.ny = doc["ny"] | cfg.ny;
    cfg.timezoneOffsetMinutes = doc["timezone_offset_minutes"] | cfg.timezoneOffsetMinutes;
    cfg.weatherEnabled = doc["weather_enabled"] | cfg.weatherEnabled;
    cfg.clock24h = doc["clock_24h"] | cfg.clock24h;
    cfg.brightness = doc["brightness"] | cfg.brightness;
    cfg.nightModeEnabled = doc["night_mode_enabled"] | cfg.nightModeEnabled;
    cfg.nightBrightness = doc["night_brightness"] | cfg.nightBrightness;
    cfg.nightStartMinutes = doc["night_start_minutes"] | cfg.nightStartMinutes;
    cfg.nightStopMinutes = doc["night_stop_minutes"] | cfg.nightStopMinutes;
    cfg.screens = static_cast<uint16_t>(doc["screens"] | cfg.screens) & SCREEN_MASK_ALL;
    cfg.albumIntervalSeconds = doc["album_interval_seconds"] | cfg.albumIntervalSeconds;
    cfg.radarLat = doc["radar_lat"] | cfg.radarLat;
    cfg.radarLon = doc["radar_lon"] | cfg.radarLon;
    cfg.radarRangeKm = constrain(static_cast<uint16_t>(doc["radar_range_km"] | cfg.radarRangeKm), 2, 400);
    cfg.radarPollSec = constrain(static_cast<uint16_t>(doc["radar_poll_sec"] | cfg.radarPollSec), 5, 600);
    cfg.radarMinAltFt = doc["radar_min_alt_ft"] | cfg.radarMinAltFt;
    cfg.radarUpDeg = static_cast<uint16_t>(doc["radar_up_deg"] | cfg.radarUpDeg) % 360;
    cfg.radarRoutes = doc["radar_routes"] | cfg.radarRoutes;
    loadRadarPresets(doc["radar_presets"]);
    cfg.albumIntervalSeconds = constrain(cfg.albumIntervalSeconds, THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    if (doc["screen_order"].is<JsonArray>()) {
        uint8_t wanted[SCREEN_COUNT];
        size_t n = 0;
        for (JsonVariant v : doc["screen_order"].as<JsonArray>()) {
            if (n >= SCREEN_COUNT) break;
            wanted[n++] = static_cast<uint8_t>(v.as<unsigned int>());
        }
        setScreenOrder(wanted, n);
    }
    if (cfg.screens == 0) cfg.screens = 1U << SCREEN_CLOCK_WEATHER;
    cfg.themeIntervalSeconds = constrain(static_cast<uint16_t>(doc["theme_interval_seconds"] | cfg.themeIntervalSeconds),
                                         THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    JsonArrayConst storedFaces = doc["analog_faces"];
    if (!storedFaces.isNull()) {
        uint8_t i = 0;
        for (JsonObjectConst face : storedFaces) {
            if (i >= ANALOG_FACE_MAX) break;
            cfg.analogFaces[i].dialRgb = (face["dial"] | cfg.analogFaces[i].dialRgb) & 0xFFFFFFU;
            cfg.analogFaces[i].caseRgb = (face["case"] | cfg.analogFaces[i].caseRgb) & 0xFFFFFFU;
            cfg.analogFaces[i].lumeRgb = (face["lume"] | cfg.analogFaces[i].lumeRgb) & 0xFFFFFFU;
            cfg.analogFaces[i].handRgb = (face["hand"] | cfg.analogFaces[i].handRgb) & 0xFFFFFFU;
            cfg.analogFaces[i].accentRgb = (face["accent"] | cfg.analogFaces[i].accentRgb) & 0xFFFFFFU;
            ++i;
        }
    } else {
        // v1.0.2 stored one flat set; carry it onto face 0 so colours survive.
        cfg.analogFaces[0].dialRgb = (doc["analog_dial_rgb"] | cfg.analogFaces[0].dialRgb) & 0xFFFFFFU;
        cfg.analogFaces[0].caseRgb = (doc["analog_case_rgb"] | cfg.analogFaces[0].caseRgb) & 0xFFFFFFU;
        cfg.analogFaces[0].lumeRgb = (doc["analog_lume_rgb"] | cfg.analogFaces[0].lumeRgb) & 0xFFFFFFU;
        cfg.analogFaces[0].handRgb = (doc["analog_hand_rgb"] | cfg.analogFaces[0].handRgb) & 0xFFFFFFU;
    }
    applyBrightness();
}

bool localTime(tm& out) {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    localtime_r(&now, &out);
    return true;
}

String dateYmd(const tm& t) { return String(t.tm_year + 1900) + two(t.tm_mon + 1) + two(t.tm_mday); }
String hm(const tm& t, int minute) { return two(t.tm_hour) + two(minute); }

bool isLeap(int year) {
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

time_t makeUtcTimestamp(const String& ymd, const String& hmText) {
    if (ymd.length() < 8 || hmText.length() < 4) return 0;
    int year = ymd.substring(0, 4).toInt();
    int month = ymd.substring(4, 6).toInt();
    int day = ymd.substring(6, 8).toInt();
    int hour = hmText.substring(0, 2).toInt();
    static const uint16_t daysBeforeMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23) return 0;
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) days += isLeap(y) ? 366 : 365;
    days += daysBeforeMonth[month - 1];
    if (month > 2 && isLeap(year)) ++days;
    days += day - 1;
    return static_cast<time_t>((days * 24LL + hour) * 3600LL);
}

void makeUltraBases(String& ncstDate, String& ncstTime, String& fcstDate, String& fcstTime) {
    time_t now = time(nullptr) + cfg.timezoneOffsetMinutes * 60;
    tm kst{};
    gmtime_r(&now, &kst);
    tm ncst = kst;
    if (ncst.tm_min < 40) {
        time_t adjusted = now - 3600;
        gmtime_r(&adjusted, &ncst);
    }
    tm fcst = kst;
    if (fcst.tm_min < 45) {
        time_t adjusted = now - 3600;
        gmtime_r(&adjusted, &fcst);
    }
    ncstDate = dateYmd(ncst);
    ncstTime = hm(ncst, 0);
    fcstDate = dateYmd(fcst);
    fcstTime = hm(fcst, 30);
}

String httpGetBody(const String& path) {
    WiFiClient client;
    client.setTimeout(6000);
    if (!client.connect(KMA_HOST, 80)) return "";
    client.print(F("GET "));
    client.print(path);
    client.print(F(" HTTP/1.0\r\nHost: "));
    client.print(KMA_HOST);
    client.print(F("\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n"));
    uint32_t started = millis();
    bool headersDone = false;
    String body;
    body.reserve(14000);
    String line;
    while (client.connected() || client.available()) {
        wdtYield();
        if (millis() - started > 12000) break;
        while (client.available()) {
            char c = static_cast<char>(client.read());
            if (!headersDone) {
                if (c == '\n') {
                    line.trim();
                    if (line.length() == 0) headersDone = true;
                    line = "";
                } else if (c != '\r') {
                    line += c;
                }
            } else if (body.length() < 18000) {
                body += c;
            }
        }
        delay(1);
    }
    client.stop();
    return body;
}

int mapWeatherCode(int sky, int pty) {
    if (pty == 1 || pty == 5 || pty == 6) return 61;
    if (pty == 2) return 80;
    if (pty == 3 || pty == 7) return 71;
    if (sky <= 1) return 0;
    if (sky == 3) return 2;
    return 3;
}

void parseCurrent(const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        weather.status = "current parse failed";
        return;
    }
    JsonArray items = doc["response"]["body"]["items"]["item"].as<JsonArray>();
    for (JsonObject item : items) {
        const char* cat = item["category"] | "";
        const char* val = item["obsrValue"] | "";
        if (!strcmp(cat, "T1H")) weather.temp = atof(val);
        else if (!strcmp(cat, "REH")) weather.humidity = atoi(val);
        else if (!strcmp(cat, "RN1")) weather.rain = val;
        else if (!strcmp(cat, "PTY")) weather.pty = atoi(val);
    }
}

void parseForecast(const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        weather.status = "forecast parse failed";
        return;
    }
    JsonArray items = doc["response"]["body"]["items"]["item"].as<JsonArray>();
    int used = 0;
    String slots[5];
    for (JsonObject item : items) {
        const char* timeVal = item["fcstTime"] | "";
        const char* dateVal = item["fcstDate"] | "";
        if (strlen(timeVal) < 4) continue;
        int slot = -1;
        for (int i = 0; i < used; ++i) {
            if (slots[i] == timeVal) slot = i;
        }
        if (slot < 0 && used < 5) {
            slot = used++;
            slots[slot] = timeVal;
            weather.fcst[slot] = ForecastItem{};
            weather.fcst[slot].valid = true;
            weather.fcst[slot].hour = String(timeVal).substring(0, 2);
            weather.fcst[slot].timestamp = makeUtcTimestamp(dateVal, timeVal);
        }
        if (slot < 0) continue;
        const char* cat = item["category"] | "";
        const char* val = item["fcstValue"] | "";
        if (!strcmp(cat, "T1H") || !strcmp(cat, "TMP")) weather.fcst[slot].temp = atof(val);
        else if (!strcmp(cat, "SKY")) weather.fcst[slot].sky = atoi(val);
        else if (!strcmp(cat, "PTY")) weather.fcst[slot].pty = atoi(val);
        else if (!strcmp(cat, "REH")) weather.fcst[slot].humidity = atoi(val);
        else if (!strcmp(cat, "RN1") || !strcmp(cat, "PCP")) weather.fcst[slot].rain = val;
    }

    // The nowcast the current conditions come from has no SKY in it - the
    // observation reports precipitation type, not cloud cover - so weather.sky
    // was never assigned and kept its initial value, which every icon chooser
    // reads as clear. That is why the dashboard showed a sun whatever the
    // weather was doing. The nearest forecast slot is the current hour, so its
    // cloud cover is what "now" means here.
    for (size_t i = 0; i < (sizeof(weather.fcst) / sizeof(weather.fcst[0])); ++i) {
        if (!weather.fcst[i].valid) continue;
        weather.sky = weather.fcst[i].sky;
        break;
    }
}

bool refreshWeather() {
    // Stamped before the guards, not after. A call these refuse is still a call,
    // and if it does not move the clock the scheduler below sees the retry as
    // permanently overdue and comes straight back - which is what a device with
    // no KMA key set did, on every pass of loop().
    lastWeatherTryMs = millis();
    if (!cfg.weatherEnabled) {
        weather.status = "disabled";
        return false;
    }
    if (cfg.kmaKey.length() == 0) {
        weather.status = "KMA key missing";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        weather.status = "wifi disconnected";
        return false;
    }
    weather.fetching = true;
    weather.status = "fetching";
    String ncstDate, ncstTime, fcstDate, fcstTime;
    makeUltraBases(ncstDate, ncstTime, fcstDate, fcstTime);
    const String base = "/api/typ02/openApi/VilageFcstInfoService_2.0";
    String currentPath = base + "/getUltraSrtNcst?pageNo=1&numOfRows=10&dataType=JSON&base_date=" + ncstDate +
                         "&base_time=" + ncstTime + "&nx=" + String(cfg.nx) + "&ny=" + String(cfg.ny) +
                         "&authKey=" + cfg.kmaKey;
    String forecastPath = base + "/getUltraSrtFcst?pageNo=1&numOfRows=40&dataType=JSON&base_date=" + fcstDate +
                          "&base_time=" + fcstTime + "&nx=" + String(cfg.nx) + "&ny=" + String(cfg.ny) +
                          "&authKey=" + cfg.kmaKey;
    String currentBody = httpGetBody(currentPath);
    if (currentBody.length() == 0) {
        weather.status = "current fetch failed";
        weather.fetching = false;
        return false;
    }
    parseCurrent(currentBody);
    String forecastBody = httpGetBody(forecastPath);
    if (forecastBody.length() > 0) parseForecast(forecastBody);
    tm t{};
    if (localTime(t)) weather.updated = two(t.tm_hour) + ":" + two(t.tm_min);
    weather.valid = !isnan(weather.temp);
    weather.status = weather.valid ? "ok" : "no data";
    weather.fetching = false;
    lastWeatherMs = millis();
    return weather.valid;
}

void drawSystemScreen() {
    // This paints over whichever screen was up, so both renderers have to know
    // their background is gone. Without this a failed OTA leaves the next
    // partial redraw drawing hands on top of the system page.
    screenChromeDrawn = false;
    analogChromeDrawn = false;
    tft.fillScreen(TFT_BLACK);
    // drawString's last argument picks the font, but the glyphs are still scaled
    // by the global text size, so this page has to state what it wants rather
    // than inherit whatever the last screen drawn happened to leave behind.
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("SYSTEM", 10, 12, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(FW_NAME, 10, 52, 2);
    tft.drawString(FW_VERSION, 10, 72, 2);
    tft.drawString("STA " + WiFi.localIP().toString(), 10, 104, 2);
    tft.drawString("AP  " + WiFi.softAPIP().toString(), 10, 124, 2);
    tft.drawString(String("FS ") + (fsMounted ? "OK" : "FAIL"), 10, 154, 2);
    tft.drawString("Weather " + weather.status, 10, 174, 2);
}

float parseRainAmount(const String& text) {
    if (text.length() == 0 || text == "강수없음") return 0.0F;
    return text.toFloat();
}

uint16_t forecastMetricColor(const String& text) {
    if (text.endsWith("mm")) return rgb(80, 220, 255);
    if (text.endsWith("%")) return rgb(238, 244, 248);
    return rgb(255, 255, 255);
}

ClockDashboard::Scene buildOriginalClockScene(time_t now, bool validTime) {
    ClockDashboard::Input input;
    input.showClock = true;
    input.showWeather = cfg.weatherEnabled;
    input.use24Hour = cfg.clock24h;
    input.showSeconds = true;
    input.validTime = validTime;
    input.isAccessPointMode = WiFi.status() != WL_CONNECTED;
    input.showLegacyUpdateRoute = false;
    input.now = now;
    input.accessPointSsid = AP_SSID;

    if (cfg.weatherEnabled && weather.valid) {
        input.weather.hasData = true;
        input.weather.isRaining = weather.pty != 0;
        input.weather.currentTemperature = weather.temp;
        input.weather.currentRain = parseRainAmount(weather.rain);
        input.weather.currentPrecipitation = parseRainAmount(weather.rain);
        input.weather.currentHumidity = weather.humidity;
        input.weather.currentWeatherCode = mapWeatherCode(weather.sky, weather.pty);
        input.weather.timezone = "Asia/Seoul";
        input.weather.locationName = cfg.location.c_str();
        input.weather.status = weather.status.c_str();

        for (size_t i = 0; i < input.weather.forecast.size() && i < 5; ++i) {
            const ForecastItem& src = weather.fcst[i];
            if (!src.valid) continue;
            input.weather.forecast[i].hasData = true;
            input.weather.forecast[i].timestamp = src.timestamp;
            input.weather.forecast[i].temperature = src.temp;
            input.weather.forecast[i].precipitation = parseRainAmount(src.rain);
            input.weather.forecast[i].humidity = src.humidity;
            input.weather.forecast[i].weatherCode = mapWeatherCode(src.sky, src.pty);
        }
    }

    return ClockDashboard::buildScene(input);
}

void drawWeatherIconOriginal(int weatherCode, int16_t x, int16_t y, int16_t size, bool useColor) {
    const char* slot = ClockDashboard::weatherIconSlot(weatherCode);
    const String path = sizedIconPath(slot, size);
    tft.fillRect(x, y, size, size, LCD_BLACK);
    if (LittleFS.exists(path) && drawBmpIcon(tft, path, x, y, size)) return;
}

void drawOriginalChrome(bool showWeather) {
    tft.fillScreen(LCD_BLACK);
    if (!showWeather) return;
    for (int16_t index = 1; index < 4; ++index) {
        const int16_t separatorX = ClockDashboard::FORECAST_LEFT +
                                   (index * (ClockDashboard::FORECAST_WIDTH + ClockDashboard::FORECAST_GAP)) -
                                   (ClockDashboard::FORECAST_GAP / 2);
        tft.drawFastVLine(separatorX, ClockDashboard::FORECAST_TOP, ClockDashboard::FORECAST_HEIGHT - 9,
                          rgb(112, 132, 142));
    }
}

void drawDashboard(bool force = false) {
    const time_t now = time(nullptr);
    const bool validTime = now > 1700000000;
    const bool showWeather = cfg.weatherEnabled;
    const time_t drawTick = now;
    const time_t minuteTick = now / 60;

    if (!force && lastDrawSecond == drawTick && lastTimeOk == validTime) return;

    if (!force && validTime && lastTimeOk == validTime && lastStaticMinute == minuteTick) {
        tm* timeInfo = localtime(&now);
        char secondsDigits[3] = "";
        if (timeInfo != nullptr) snprintf(secondsDigits, sizeof(secondsDigits), "%02d", timeInfo->tm_sec);
        String seconds = secondsDigits;
        if (cacheSeconds != seconds) {
            const int16_t secX = ClockDashboard::TIME_LEFT_X + 212;
            const int16_t secY = ClockDashboard::TIME_TOP_Y + ClockDigitFont::fontSet(ClockDigitFont::Kind::Main).lineHeight -
                                 ((ClockDigitFont::fontSet(ClockDigitFont::Kind::Secondary).lineHeight * 2) + 2);
            tft.fillRect(secX, secY, 23, 34, LCD_BLACK);
            drawClockDigits(secX + 5, secY, seconds.substring(0, 1), 16, rgb(170, 230, 255));
            drawClockDigits(secX + 5, secY + 18, seconds.substring(1, 2), 16, rgb(170, 230, 255));
            cacheSeconds = seconds;
        }
        lastDrawSecond = drawTick;
        return;
    }

    const bool forceStaticRedraw = force || !screenChromeDrawn || lastTimeOk != validTime;
    if (forceStaticRedraw) {
        resetDisplayCache();
        drawOriginalChrome(showWeather);
        screenChromeDrawn = true;
    }
    lastTimeOk = validTime;
    lastDrawSecond = drawTick;

    const ClockDashboard::Scene scene = buildOriginalClockScene(now, validTime);
    if (scene.waitLine1.length() || scene.waitLine2.length()) {
        String wait1 = lcdString(scene.waitLine1);
        String wait2 = lcdString(scene.waitLine2);
        if (cacheWeatherStatus != wait1 + "|" + wait2) {
            tft.fillScreen(LCD_BLACK);
            drawCenteredText(ClockDashboard::WAIT_LINE_1_Y, wait1, 2, TFT_WHITE, LCD_BLACK, 0, ClockDashboard::SCREEN_W);
            drawCenteredText(ClockDashboard::WAIT_LINE_2_Y, wait2, 2, TFT_WHITE, LCD_BLACK, 0, ClockDashboard::SCREEN_W);
            cacheWeatherStatus = wait1 + "|" + wait2;
        }
        return;
    }

    const String ip = WiFi.localIP().toString() == "0.0.0.0" ? String("") : WiFi.localIP().toString();
    if (cacheIp != ip) {
        tft.fillRect(8, 4, 122, 25, LCD_BLACK);
        if (ip.length()) drawTextAt(8, 4, trimTextToWidth(ip, 1, 122), 1, rgb(36, 40, 44), LCD_BLACK);
        cacheIp = ip;
    }

    const int16_t currentIconX = ClockDashboard::SCREEN_W - ClockDashboard::HEADER_RIGHT_PADDING -
                                 ClockDashboard::CURRENT_ICON_SIZE;
    const String high = lcdString(scene.todayHighLabel);
    if (cacheHigh != high) {
        const int16_t todayHighWidth = currentIconX - 118 - 4;
        tft.fillRect(118, 4, todayHighWidth, 20, LCD_BLACK);
        if (high.length() && todayHighWidth > 0) {
            drawTextAt(118, 4, trimTextToWidth(high, 2, todayHighWidth), 2, rgb(255, 220, 80), LCD_BLACK);
        }
        cacheHigh = high;
    }

    if (cacheCurrentIcon != scene.weatherIconCode || forceStaticRedraw) {
        drawWeatherIconOriginal(scene.weatherIconCode, currentIconX, 0, ClockDashboard::CURRENT_ICON_SIZE, true);
        cacheCurrentIcon = scene.weatherIconCode;
    }

    const String primaryTime = scene.clockPrimary.empty() ? lcdString(scene.clockTime) : lcdString(scene.clockPrimary);
    const int16_t primaryHeight = ClockDigitFont::fontSet(ClockDigitFont::Kind::Main).lineHeight;
    if (cacheTime != primaryTime) {
        tft.fillRect(ClockDashboard::TIME_LEFT_X, ClockDashboard::TIME_TOP_Y, 212, primaryHeight + 2, LCD_BLACK);
        drawClockDigits(ClockDashboard::TIME_LEFT_X, ClockDashboard::TIME_TOP_Y, primaryTime, primaryHeight,
                        rgb(245, 247, 248));
        cacheTime = primaryTime;
    }

    tm* timeInfo = localtime(&now);
    char secondsDigits[3] = "";
    if (validTime && timeInfo != nullptr) snprintf(secondsDigits, sizeof(secondsDigits), "%02d", timeInfo->tm_sec);
    const String seconds = secondsDigits;
    if (cacheSeconds != seconds) {
        const int16_t secX = ClockDashboard::TIME_LEFT_X + 212;
        const int16_t secY = ClockDashboard::TIME_TOP_Y + primaryHeight -
                             ((ClockDigitFont::fontSet(ClockDigitFont::Kind::Secondary).lineHeight * 2) + 2);
        tft.fillRect(secX, secY, 23, 34, LCD_BLACK);
        if (seconds.length() == 2) {
            drawClockDigits(secX + 5, secY, seconds.substring(0, 1), 16, rgb(170, 230, 255));
            drawClockDigits(secX + 5, secY + 18, seconds.substring(1, 2), 16, rgb(170, 230, 255));
        }
        cacheSeconds = seconds;
    }

    String dateLine = lcdString(scene.locationName);
    if (!scene.clockDate.empty()) {
        if (!dateLine.isEmpty()) dateLine += " ";
        dateLine += lcdString(scene.clockDate);
    }
    const uint8_t dateSize = DATE_LINE_TEXT_SIZE;
    const int16_t dateHeight = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    const int16_t dateY = DATE_LINE_Y;
    const String dateKey = dateLine + "|" + String(dateY) + "|" + String(dateSize);
    if (cacheDate != dateKey) {
        tft.fillRect(0, dateY, ClockDashboard::SCREEN_W, dateHeight + 3, LCD_BLACK);
        if (!dateLine.isEmpty()) {
            drawCenteredText(dateY, trimTextToWidth(dateLine, dateSize, ClockDashboard::SCREEN_W - 4), dateSize,
                             rgb(226, 238, 244), LCD_BLACK, 0, ClockDashboard::SCREEN_W);
        }
        cacheDate = dateKey;
    }

    for (int16_t index = 0; index < static_cast<int16_t>(scene.weatherForecastVisuals.size()); ++index) {
        const auto& visual = scene.weatherForecastVisuals[static_cast<size_t>(index)];
        String key;
        if (visual.hasData) {
            key = lcdString(visual.hourLabel + "|" + visual.temperatureLabel + "|" + visual.precipitationLabel + "|" +
                            std::to_string(visual.weatherCode));
        }
        if (!forceStaticRedraw && cacheForecast[index] == key) continue;

        const int16_t cardX = ClockDashboard::FORECAST_LEFT +
                              (index * (ClockDashboard::FORECAST_WIDTH + ClockDashboard::FORECAST_GAP));
        const int16_t cardY = ClockDashboard::FORECAST_TOP;
        const int16_t iconX = cardX + ((ClockDashboard::FORECAST_WIDTH - ClockDashboard::FORECAST_ICON_SIZE) / 2);
        const int16_t iconY = cardY + ClockDashboard::FORECAST_ICON_Y_OFFSET - 18;
        tft.fillRect(cardX, cardY, ClockDashboard::FORECAST_WIDTH, ClockDashboard::FORECAST_HEIGHT, LCD_BLACK);
        cacheForecastIcon[index] = -999;

        if (visual.hasData) {
            const String hourLabel = lcdString(visual.hourLabel);
            const uint8_t hourSize = selectTextSizeToFit(hourLabel, 2, 1, ClockDashboard::FORECAST_WIDTH - 8);
            drawCenteredText(cardY, hourLabel, hourSize, rgb(80, 225, 255), LCD_BLACK, cardX + 2,
                             ClockDashboard::FORECAST_WIDTH - 4);
            drawWeatherIconOriginal(visual.weatherCode, iconX, iconY, ClockDashboard::FORECAST_ICON_SIZE, true);
            drawCenteredText(cardY + 52, lcdString(visual.temperatureLabel), 2, rgb(255, 126, 54), LCD_BLACK,
                             cardX + 2, ClockDashboard::FORECAST_WIDTH - 4);
            if (!visual.precipitationLabel.empty()) {
                const String precipitation = lcdString(visual.precipitationLabel);
                drawCenteredText(cardY + 72, precipitation, 1, forecastMetricColor(precipitation), LCD_BLACK,
                                 cardX + 2, ClockDashboard::FORECAST_WIDTH - 4);
            }
        }
        cacheForecast[index] = key;
    }

    lastStaticMinute = minuteTick;
}

// ---------------------------------------------------------------------------
// Analog face
// ---------------------------------------------------------------------------

// Face colours come from config as 24-bit RGB and are quantised here. Note that
// RGB565 leaves almost nothing between a black dial and the case: the only
// steps above black are 0x0001, 0x0002, 0x0020 and 0x0021, and green carries
// most of the luminance, so a blue-only value is the darkest visible rim. Two
// different picks in the web UI can therefore land on the same panel colour,
// which is why the UI shows the resulting RGB565 value.
uint16_t rgb24(uint32_t v) {
    return rgb(static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
               static_cast<uint8_t>(v & 0xFF));
}

uint8_t analogFaceIndex() {
    switch (activeScreen) {
        case SCREEN_MONDAINE: return 1;
        case SCREEN_MONDAINE_WHITE: return 2;
        case SCREEN_DIGITAL: return 3;
        case SCREEN_WEATHER_DIGITAL: return 4;
        case SCREEN_DATE_DIGITAL: return 5;
        default: return 0;
    }
}
const AnalogFace& analogFace() { return cfg.analogFaces[analogFaceIndex()]; }

uint16_t analogCase() { return rgb24(analogFace().caseRgb); }
uint16_t analogDial() { return rgb24(analogFace().dialRgb); }
uint16_t analogLume() { return rgb24(analogFace().lumeRgb); }
uint16_t analogHandColor() { return rgb24(analogFace().handRgb); }
uint16_t analogAccent() { return rgb24(analogFace().accentRgb); }

// The outline just needs to separate the hand from the dial, so it follows the
// hand colour rather than being a fifth thing to configure.
uint16_t analogHandEdge() {
    const uint32_t v = analogFace().handRgb;
    return rgb(static_cast<uint8_t>((((v >> 16) & 0xFF) * 48) / 255), static_cast<uint8_t>((((v >> 8) & 0xFF) * 48) / 255),
               static_cast<uint8_t>(((v & 0xFF) * 48) / 255));
}

// The clock font is a fixed 55 px with no scaler, so the dial numerals are
// box-filtered down. 4-bit alpha survives the reduction as smooth edges.
int16_t scaledDigitWidth(char value, int16_t targetH) {
    const ClockDigitFont::Glyph* glyph = ClockDigitFont::glyph(ClockDigitFont::Kind::Main, value);
    if (glyph == nullptr || glyph->height == 0) return 0;
    const int32_t w = (static_cast<int32_t>(glyph->width) * targetH + (glyph->height / 2)) / glyph->height;
    return static_cast<int16_t>(max<int32_t>(1, w));
}

// Every face routine takes its target and a y offset, so the same code paints
// the panel directly or one band of the off-screen sprite.
void drawScaledDigit(TFT_eSPI& g, int16_t x, int16_t y, char value, int16_t targetH, uint16_t fg, uint16_t bg) {
    const ClockDigitFont::Glyph* glyph = ClockDigitFont::glyph(ClockDigitFont::Kind::Main, value);
    if (glyph == nullptr) return;
    const auto& font = ClockDigitFont::fontSet(ClockDigitFont::Kind::Main);
    const uint8_t maxAlpha = static_cast<uint8_t>((1U << font.bitsPerPixel) - 1U);
    const int16_t sw = glyph->width;
    const int16_t sh = glyph->height;
    const int16_t tw = scaledDigitWidth(value, targetH);
    if (sw == 0 || sh == 0 || tw == 0) return;

    for (int16_t ty = 0; ty < targetH; ++ty) {
        const int16_t sy0 = static_cast<int16_t>((static_cast<int32_t>(ty) * sh) / targetH);
        int16_t sy1 = static_cast<int16_t>((static_cast<int32_t>(ty + 1) * sh) / targetH);
        if (sy1 <= sy0) sy1 = static_cast<int16_t>(sy0 + 1);
        for (int16_t tx = 0; tx < tw; ++tx) {
            const int16_t sx0 = static_cast<int16_t>((static_cast<int32_t>(tx) * sw) / tw);
            int16_t sx1 = static_cast<int16_t>((static_cast<int32_t>(tx + 1) * sw) / tw);
            if (sx1 <= sx0) sx1 = static_cast<int16_t>(sx0 + 1);
            uint32_t sum = 0;
            uint16_t count = 0;
            for (int16_t sy = sy0; sy < sy1 && sy < sh; ++sy) {
                for (int16_t sx = sx0; sx < sx1 && sx < sw; ++sx) {
                    sum += readPackedAlpha(glyph->bitmap, static_cast<size_t>(sy) * sw + sx, font.bitsPerPixel);
                    ++count;
                }
            }
            if (count == 0) continue;
            const uint8_t alpha = static_cast<uint8_t>(sum / count);
            if (alpha == 0) continue;
            g.drawPixel(x + tx, y + ty, blend565(fg, bg, alpha, maxAlpha));
        }
    }
}

void drawScaledLabel(TFT_eSPI& g, int16_t cx, int16_t cy, const char* text, int16_t targetH, uint16_t fg,
                     uint16_t bg) {
    int16_t total = 0;
    for (const char* p = text; *p != 0; ++p) {
        total = static_cast<int16_t>(total + scaledDigitWidth(*p, targetH));
        if (*(p + 1) != 0) total = static_cast<int16_t>(total + 1);
    }
    int16_t x = static_cast<int16_t>(cx - (total / 2));
    const int16_t y = static_cast<int16_t>(cy - (targetH / 2));
    for (const char* p = text; *p != 0; ++p) {
        drawScaledDigit(g, x, y, *p, targetH, fg, bg);
        x = static_cast<int16_t>(x + scaledDigitWidth(*p, targetH) + 1);
    }
}

float analogHourAngle(int index) { return ((index / 12.0f) * TWO_PI) - HALF_PI; }

// drawWedgeLine scans the whole bounding box of the line it is given. For a
// diagonal hand that box is enormous compared with the pixels actually covered:
// a 45 degree minute hand crossing a 24 row band occupies about 24x6 pixels but
// is scanned as 90x24. Clipping only trims the box to the sprite, not to the
// line, so the waste stays.
//
// Cutting the line down to the part that falls inside the band first makes the
// box tight. The sub-segment is collinear and its radii are interpolated, so the
// taper is unchanged, and it is extended past the band by the stroke radius so
// the rounded cap lands outside the visible rows instead of bulging at the seam.
void wedgeSegment(TFT_eSPI& g, float x0, float y0, float x1, float y1, float r0, float r1, float bandBot,
                  uint16_t color, uint16_t bg) {
    const float pad = fmaxf(r0, r1) + 2.0f;
    const float lo = -pad;
    const float hi = bandBot + pad;
    const float dy = y1 - y0;

    float t0 = 0.0f;
    float t1 = 1.0f;
    if (fabsf(dy) < 0.001f) {
        if (y0 < lo || y0 > hi) return;
    } else {
        float ta = (lo - y0) / dy;
        float tb = (hi - y0) / dy;
        if (ta > tb) {
            const float swap = ta;
            ta = tb;
            tb = swap;
        }
        t0 = fmaxf(0.0f, ta);
        t1 = fminf(1.0f, tb);
        if (t0 >= t1) return;
    }

    const float dx = x1 - x0;
    const float dr = r1 - r0;
    g.drawWedgeLine(x0 + (dx * t0), y0 + (dy * t0), x0 + (dx * t1), y0 + (dy * t1), r0 + (dr * t0), r0 + (dr * t1),
                    color, bg);
}

// A hand is an arm plus a short counterweight, neither of which enters the hub.
void analogHandStroke(TFT_eSPI& g, int16_t yOff, int16_t clipH, float angle, int16_t len, int16_t tail, float rNear,
                      float rFar, uint16_t color, uint16_t bg) {
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float cy = static_cast<float>(ANALOG_CY - yOff);
    const float bot = static_cast<float>(clipH - 1);

    wedgeSegment(g, ANALOG_CX + (c * ANALOG_HAND_INNER), cy + (s * ANALOG_HAND_INNER), ANALOG_CX + (c * len),
                 cy + (s * len), rNear, rFar, bot, color, bg);
    if (tail > ANALOG_HAND_INNER) {
        wedgeSegment(g, ANALOG_CX - (c * ANALOG_HAND_INNER), cy - (s * ANALOG_HAND_INNER), ANALOG_CX - (c * tail),
                     cy - (s * tail), rNear, rNear, bot, color, bg);
    }
}

void analogDrawTick(TFT_eSPI& g, int16_t yOff, int index) {
    const float ang = analogHourAngle(index);
    const float c = cosf(ang);
    const float s = sinf(ang);
    const float cy = static_cast<float>(ANALOG_CY - yOff);
    g.drawWideLine(ANALOG_CX + (c * ANALOG_TICK_IN), cy + (s * ANALOG_TICK_IN), ANALOG_CX + (c * ANALOG_TICK_OUT),
                   cy + (s * ANALOG_TICK_OUT), 5.0f, analogLume(), analogDial());
}

void analogNumeralCenter(int slot, int16_t& x, int16_t& y) {
    const float ang = analogHourAngle(slot * 3);
    x = static_cast<int16_t>(ANALOG_CX + (cosf(ang) * ANALOG_R_NUM));
    y = static_cast<int16_t>(ANALOG_CY + (sinf(ang) * ANALOG_R_NUM));
}

// Paints the complete face unconditionally. With an off-screen target there is
// nothing to erase and no half-finished state that could reach the panel.
void analogPaintFace(TFT_eSPI& g, int16_t yOff, int16_t clipH, float aH, float aM, float aS) {
    static const char* const LABELS[4] = {"12", "3", "6", "9"};
    const uint16_t dial = analogDial();
    const uint16_t lume = analogLume();
    const uint16_t hand = analogHandColor();
    const uint16_t edge = analogHandEdge();

    for (int i = 0; i < 12; ++i) {
        if (i % 3 == 0) continue;  // 12 / 3 / 6 / 9 carry numerals instead
        analogDrawTick(g, yOff, i);
    }

    for (int slot = 0; slot < 4; ++slot) {
        int16_t nx = 0;
        int16_t ny = 0;
        analogNumeralCenter(slot, nx, ny);
        // Skip glyphs this band cannot show. Unlike the clipped wedge calls the
        // box filter has no early out, so it has to be skipped explicitly.
        const int16_t top = static_cast<int16_t>(ny - (ANALOG_NUM_H / 2) - yOff);
        if (top >= clipH || (top + ANALOG_NUM_H) < 0) continue;
        drawScaledLabel(g, nx, static_cast<int16_t>(ny - yOff), LABELS[slot], ANALOG_NUM_H, lume, dial);
    }

    analogHandStroke(g, yOff, clipH, aH, ANALOG_L_HOUR, ANALOG_TAIL_HOUR, 4.7f, 3.3f, edge, dial);
    analogHandStroke(g, yOff, clipH, aH, ANALOG_L_HOUR, ANALOG_TAIL_HOUR, 3.6f, 2.2f, hand, edge);
    analogHandStroke(g, yOff, clipH, aM, ANALOG_L_MIN, ANALOG_TAIL_MIN, 4.0f, 2.9f, edge, dial);
    analogHandStroke(g, yOff, clipH, aM, ANALOG_L_MIN, ANALOG_TAIL_MIN, 2.9f, 1.8f, hand, edge);
    analogHandStroke(g, yOff, clipH, aS, ANALOG_L_SEC, ANALOG_TAIL_SEC, 0.9f, 0.9f, hand, dial);
    const int16_t hubTop = static_cast<int16_t>(ANALOG_CY - ANALOG_HUB_R - yOff);
    if (hubTop < clipH && (hubTop + (2 * ANALOG_HUB_R)) >= 0) {
        g.fillSmoothCircle(ANALOG_CX, ANALOG_CY - yOff, ANALOG_HUB_R, edge, dial);
        g.fillSmoothCircle(ANALOG_CX, ANALOG_CY - yOff, 4, hand, edge);
    }
}

// The dial sits one colour step above the case, so anti-aliasing its rim buys
// nothing. Row spans are also far cheaper than fillSmoothCircle, which is
// O(r*r) per call and would otherwise be paid once per band.
void analogPaintGround(TFT_eSPI& g, int16_t yOff, int16_t rows, int16_t cx, int16_t cy, int16_t radius) {
    const uint16_t dial = analogDial();
    for (int16_t y = 0; y < rows; ++y) {
        const int32_t dy = static_cast<int32_t>(yOff + y) - cy;
        const int32_t inside = (static_cast<int32_t>(radius) * radius) - (dy * dy);
        if (inside < 0) continue;
        const int16_t dx = static_cast<int16_t>(sqrtf(static_cast<float>(inside)));
        g.drawFastHLine(static_cast<int16_t>(cx - dx), y, static_cast<int16_t>((dx * 2) + 1), dial);
    }
}

// Vertical extent a stroke occupies, used to work out which bands changed.
bool mondaineActive() {
    return activeScreen == SCREEN_MONDAINE || activeScreen == SCREEN_MONDAINE_WHITE;
}

void analogSpanY(float angle, int16_t len, int16_t tail, float r, int16_t& yMin, int16_t& yMax) {
    const float s = sinf(angle);
    const float cy = static_cast<float>(mondaineActive() ? MONDAINE_CY : ANALOG_CY);
    const float tip = cy + (s * len);
    const float back = cy - (s * tail);
    const int16_t lo = static_cast<int16_t>(floorf(fminf(tip, back) - r - 1.0f));
    const int16_t hi = static_cast<int16_t>(ceilf(fmaxf(tip, back) + r + 1.0f));
    if (lo < yMin) yMin = lo;
    if (hi > yMax) yMax = hi;
}

// True once the tip has moved far enough to land on a different pixel.
bool handMoved(float prev, float now, int16_t len) {
    if (prev < -900.0f) return true;
    float d = fabsf(now - prev);
    if (d > PI) d = TWO_PI - d;
    return (d * len) >= 0.7f;
}

// ---------------------------------------------------------------------------
// Mondaine SBB face
//
// Markers and hands are flat-ended bars, not capsules, so they are filled as
// quads rather than drawn with the anti-aliased wedge helper. The lettering,
// including the curved footer, is baked into 4-bit alpha masks at build time by
// scripts/gen_mondaine_art.py: it never changes, and the display library cannot
// rotate glyphs at runtime.
// ---------------------------------------------------------------------------

// Flat-ended radial bar. Both triangles are built from the same rounded corners
// so the shared edge cannot leave a seam.
void mondaineBar(TFT_eSPI& g, int16_t yOff, float angle, float r0, float r1, float halfW, uint16_t color) {
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float cy = static_cast<float>(MONDAINE_CY - yOff);
    const float nx = -s * halfW;
    const float ny = c * halfW;

    const int32_t ax = lroundf(MONDAINE_CX + (c * r0) + nx);
    const int32_t ay = lroundf(cy + (s * r0) + ny);
    const int32_t bx = lroundf(MONDAINE_CX + (c * r1) + nx);
    const int32_t by = lroundf(cy + (s * r1) + ny);
    const int32_t cx2 = lroundf(MONDAINE_CX + (c * r1) - nx);
    const int32_t cy2 = lroundf(cy + (s * r1) - ny);
    const int32_t dx = lroundf(MONDAINE_CX + (c * r0) - nx);
    const int32_t dy = lroundf(cy + (s * r0) - ny);

    g.fillTriangle(ax, ay, bx, by, cx2, cy2, color);
    g.fillTriangle(ax, ay, cx2, cy2, dx, dy, color);
}

void mondaineBlit(TFT_eSPI& g, int16_t yOff, int16_t clipH, const MondaineArt::DialArt& art, uint16_t fg,
                  uint16_t bg) {
    const int16_t x0 = static_cast<int16_t>(lroundf(MONDAINE_CX - art.centreDx));
    const int16_t y0 = static_cast<int16_t>(lroundf(MONDAINE_CY - art.centreDy) - yOff);
    if (y0 >= clipH || (y0 + art.height) < 0) return;

    for (int16_t row = 0; row < art.height; ++row) {
        const int16_t dy = static_cast<int16_t>(y0 + row);
        if (dy < 0 || dy >= clipH) continue;
        for (int16_t col = 0; col < art.width; ++col) {
            const size_t index = (static_cast<size_t>(row) * art.width) + col;
            const uint8_t alpha = readPackedAlpha(art.bitmap, index, 4);
            if (alpha == 0) continue;
            g.drawPixel(x0 + col, dy, blend565(fg, bg, alpha, 15));
        }
    }
}

// Hands sit above the dial and drop a soft shadow, which the reference shows
// falling on the dial and on the hands underneath. It is drawn by blending what
// is already there toward black rather than by painting grey: that darkens the
// white dial without lightening the black markers it crosses, and it is what
// makes a hand-on-hand shadow come out right.
//
// Reading pixels back only works on the off-screen sprite. The panel has no
// MISO line wired, so the direct-draw fallback skips shadows rather than
// blending against garbage.
//
// Both routines walk scanlines rather than the shape's bounding box. A diagonal
// hand covers a small fraction of its box, and scanning the box was costing
// more than the rest of the frame put together.
void mondaineShadowSpan(TFT_eSPI& g, int16_t y, float xa, float xb, uint8_t alpha) {
    int16_t x0 = static_cast<int16_t>(floorf(fminf(xa, xb)));
    int16_t x1 = static_cast<int16_t>(ceilf(fmaxf(xa, xb)));
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    for (int16_t x = x0; x <= x1; ++x) {
        g.drawPixel(x, y, blend565(0x0000, g.readPixel(x, y), alpha, 15));
    }
}

void mondaineShadowBar(TFT_eSPI& g, int16_t yOff, int16_t clipH, float angle, float r0, float r1,
                       float halfW, const ShadowPass& pass) {
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float ox = MONDAINE_CX + pass.dx;
    const float oy = static_cast<float>(MONDAINE_CY - yOff) + pass.dy;
    const float nx = -s * halfW;
    const float ny = c * halfW;

    const float qx[4] = {ox + (c * r0) + nx, ox + (c * r1) + nx, ox + (c * r1) - nx, ox + (c * r0) - nx};
    const float qy[4] = {oy + (s * r0) + ny, oy + (s * r1) + ny, oy + (s * r1) - ny, oy + (s * r0) - ny};

    float top = qy[0];
    float bottom = qy[0];
    for (int i = 1; i < 4; ++i) {
        top = fminf(top, qy[i]);
        bottom = fmaxf(bottom, qy[i]);
    }
    int16_t y0 = static_cast<int16_t>(floorf(top));
    int16_t y1 = static_cast<int16_t>(ceilf(bottom));
    if (y0 < 0) y0 = 0;
    if (y1 > clipH - 1) y1 = static_cast<int16_t>(clipH - 1);

    for (int16_t y = y0; y <= y1; ++y) {
        const float row = static_cast<float>(y);
        float xa = 0.0f;
        float xb = 0.0f;
        bool have = false;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            if ((qy[i] <= row) == (qy[j] <= row)) continue;  // edge does not cross this row
            const float t = (row - qy[i]) / (qy[j] - qy[i]);
            const float x = qx[i] + (t * (qx[j] - qx[i]));
            if (!have) {
                xa = x;
                xb = x;
                have = true;
            } else {
                xa = fminf(xa, x);
                xb = fmaxf(xb, x);
            }
        }
        if (have) mondaineShadowSpan(g, y, xa, xb, pass.alpha);
    }
}

void mondaineShadowDisc(TFT_eSPI& g, int16_t yOff, int16_t clipH, float cx, float cy, float radius,
                        const ShadowPass& pass) {
    const float ox = cx + pass.dx;
    const float oy = cy + pass.dy;
    int16_t y0 = static_cast<int16_t>(floorf(oy - radius));
    int16_t y1 = static_cast<int16_t>(ceilf(oy + radius));
    if (y0 < 0) y0 = 0;
    if (y1 > clipH - 1) y1 = static_cast<int16_t>(clipH - 1);

    for (int16_t y = y0; y <= y1; ++y) {
        const float dy = static_cast<float>(y) - oy;
        const float inside = (radius * radius) - (dy * dy);
        if (inside < 0.0f) continue;
        const float dx = sqrtf(inside);
        mondaineShadowSpan(g, y, ox - dx, ox + dx, pass.alpha);
    }
}

void mondainePaintFace(TFT_eSPI& g, int16_t yOff, int16_t clipH, float aH, float aM, float aS, bool offscreen) {
    const uint16_t dial = analogDial();
    const uint16_t ink = analogLume();       // markers and the hour/minute hands
    const uint16_t red = analogHandColor();  // seconds hand and the SBB badge

    for (int i = 0; i < 60; ++i) {
        if (i % 5 == 0) continue;
        const float ang = ((i / 60.0f) * TWO_PI) - HALF_PI;
        mondaineBar(g, yOff, ang, MONDAINE_TICK_IN, MONDAINE_TICK_OUT, MONDAINE_TICK_W / 2.0f, ink);
    }
    for (int i = 0; i < 12; ++i) {
        const float ang = ((i / 12.0f) * TWO_PI) - HALF_PI;
        mondaineBar(g, yOff, ang, MONDAINE_BAR_IN, MONDAINE_BAR_OUT, MONDAINE_BAR_W / 2.0f, ink);
    }

    const int32_t badgeX = lroundf(MONDAINE_CX + MondaineArt::SBB_BADGE_X);
    const int32_t badgeY = lroundf(MONDAINE_CY + MondaineArt::SBB_BADGE_Y) - yOff;
    const int32_t badgeW = lroundf(MondaineArt::SBB_BADGE_W);
    const int32_t badgeH = lroundf(MondaineArt::SBB_BADGE_H);
    g.fillRect(badgeX, badgeY, badgeW, badgeH, red);

    // The Swiss cross is white on the red field whatever the dial ink is, which
    // is why it is drawn rather than baked into the label mask.
    const uint16_t crossColor = rgb(0xFF, 0xFF, 0xFF);
    const int32_t arm = max<int32_t>(1, lroundf(MondaineArt::SBB_CROSS_ARM));
    const int32_t leg = max<int32_t>(1, lroundf(MondaineArt::SBB_CROSS_LEG));
    const int32_t crossCx = badgeX + (badgeW / 2);
    const int32_t crossCy = badgeY + (badgeH / 2);
    g.fillRect(crossCx - (arm / 2), crossCy - (leg / 2), arm, leg, crossColor);
    g.fillRect(crossCx - (leg / 2), crossCy - (arm / 2), leg, arm, crossColor);

    mondaineBlit(g, yOff, clipH, MondaineArt::LOGO, ink, dial);
    mondaineBlit(g, yOff, clipH, MondaineArt::SBB, ink, dial);
    mondaineBlit(g, yOff, clipH, MondaineArt::FOOTER, ink, dial);

    if (offscreen) {
        for (const auto& pass : MONDAINE_SHADOW) {
            mondaineShadowBar(g, yOff, clipH, aH, -MONDAINE_HAND_TAIL, MONDAINE_HOUR_LEN, MONDAINE_HOUR_W / 2.0f, pass);
        }
    }
    mondaineBar(g, yOff, aH, -MONDAINE_HAND_TAIL, MONDAINE_HOUR_LEN, MONDAINE_HOUR_W / 2.0f, ink);

    if (offscreen) {
        for (const auto& pass : MONDAINE_SHADOW) {
            mondaineShadowBar(g, yOff, clipH, aM, -MONDAINE_HAND_TAIL, MONDAINE_MIN_LEN, MONDAINE_MIN_W / 2.0f, pass);
        }
    }
    mondaineBar(g, yOff, aM, -MONDAINE_HAND_TAIL, MONDAINE_MIN_LEN, MONDAINE_MIN_W / 2.0f, ink);

    const float discX = MONDAINE_CX + (cosf(aS) * MONDAINE_SEC_LEN);
    const float discY = static_cast<float>(MONDAINE_CY - yOff) + (sinf(aS) * MONDAINE_SEC_LEN);
    if (offscreen) {
        for (const auto& pass : MONDAINE_SHADOW) {
            mondaineShadowBar(g, yOff, clipH, aS, -MONDAINE_SEC_TAIL, MONDAINE_SEC_LEN, MONDAINE_SEC_W / 2.0f, pass);
            mondaineShadowDisc(g, yOff, clipH, discX, discY, MONDAINE_SEC_DISC, pass);
        }
    }
    mondaineBar(g, yOff, aS, -MONDAINE_SEC_TAIL, MONDAINE_SEC_LEN, MONDAINE_SEC_W / 2.0f, red);
    g.fillSmoothCircle(static_cast<int32_t>(lroundf(discX)), static_cast<int32_t>(lroundf(discY)),
                       static_cast<int32_t>(lroundf(MONDAINE_SEC_DISC)), red, dial);
}

bool analogBandBegin() {
    if (analogBandReady) return true;
    analogBand.setColorDepth(16);
    if (analogBand.createSprite(SCREEN_W, ANALOG_BAND_H) == nullptr) return false;
    analogBandReady = true;
    return true;
}

void analogBandEnd() {
    if (!analogBandReady) return;
    analogBand.deleteSprite();
    analogBandReady = false;
}

// Composite each band in RAM and push it whole, so the panel only ever shows
// finished pixels. No erase step is visible, which removes the flicker rather
// than merely shrinking the window in which it happens.
// Geometry that differs between faces and is needed outside the painters.
int16_t faceSecLen() {
    return mondaineActive() ? static_cast<int16_t>(MONDAINE_SEC_LEN + MONDAINE_SEC_DISC + 2) : ANALOG_L_SEC;
}
int16_t faceSecTail() { return mondaineActive() ? static_cast<int16_t>(MONDAINE_SEC_TAIL) : ANALOG_TAIL_SEC; }
int16_t faceMinLen() { return mondaineActive() ? static_cast<int16_t>(MONDAINE_MIN_LEN) : ANALOG_L_MIN; }
int16_t faceMinTail() { return mondaineActive() ? static_cast<int16_t>(MONDAINE_HAND_TAIL) : ANALOG_TAIL_MIN; }
int16_t faceHourLen() { return mondaineActive() ? static_cast<int16_t>(MONDAINE_HOUR_LEN) : ANALOG_L_HOUR; }
int16_t faceHourTail() { return mondaineActive() ? static_cast<int16_t>(MONDAINE_HAND_TAIL) : ANALOG_TAIL_HOUR; }

void paintWholeFace(TFT_eSPI& g, int16_t yOff, int16_t rows, float aH, float aM, float aS, bool offscreen) {
    if (mondaineActive()) {
        analogPaintGround(g, yOff, rows, MONDAINE_CX, MONDAINE_CY, MONDAINE_R);
        mondainePaintFace(g, yOff, rows, aH, aM, aS, offscreen);
    } else {
        analogPaintGround(g, yOff, rows, ANALOG_CX, ANALOG_CY, ANALOG_R_DIAL);
        analogPaintFace(g, yOff, rows, aH, aM, aS);
    }
}

void drawAnalogComposited(float aH, float aM, float aS, int16_t yMin, int16_t yMax) {
    analogBandsPushed = 0;
    analogPushUs = 0;
    for (int16_t top = 0; top < SCREEN_H; top = static_cast<int16_t>(top + ANALOG_BAND_H)) {
        const int16_t rows = min<int16_t>(ANALOG_BAND_H, static_cast<int16_t>(SCREEN_H - top));
        // Untouched bands still hold the right pixels, so leave them alone.
        // Sending all ten is what made the second hand look like it was being
        // wiped from the top down.
        if ((top + rows - 1) < yMin || top > yMax) continue;
        analogBand.fillSprite(analogCase());
        paintWholeFace(analogBand, top, rows, aH, aM, aS, true);
        const uint32_t t0 = micros();
        analogBand.pushSprite(0, top);
        analogPushUs += micros() - t0;
        ++analogBandsPushed;
        wdtYield();
    }
}

// Only reached when the band sprite cannot be allocated. This one does flicker,
// but it keeps the face working under memory pressure instead of going blank.
void drawAnalogDirect(float aH, float aM, float aS) {
    tft.fillScreen(analogCase());
    paintWholeFace(tft, 0, SCREEN_H, aH, aM, aS, false);
    wdtYield();
}

void drawAnalog(bool force) {
    const time_t now = time(nullptr);
    const bool validTime = now > 1700000000;

    tm timeInfo{};
    if (validTime) localtime_r(&now, &timeInfo);
    const float sec = validTime ? static_cast<float>(timeInfo.tm_sec) : 0.0f;
    const float minute = (validTime ? static_cast<float>(timeInfo.tm_min) : 0.0f) + (sec / 60.0f);
    const float hour = (validTime ? static_cast<float>(timeInfo.tm_hour % 12) : 0.0f) + (minute / 60.0f);

    const float aH = ((hour / 12.0f) * TWO_PI) - HALF_PI;
    const float aM = ((minute / 60.0f) * TWO_PI) - HALF_PI;
    const float aS = ((sec / 60.0f) * TWO_PI) - HALF_PI;

    const bool fullRedraw = force || !analogChromeDrawn;

    // A slow hand only advances once its tip clears a pixel. Compare against the
    // angle it is actually drawn at, not against last second, or the threshold is
    // never reached and the hand is never scheduled for a repaint.
    const bool moveMinute = fullRedraw || handMoved(analogDrawnMinute, aM, faceMinLen());
    const bool moveHour = fullRedraw || handMoved(analogDrawnHour, aH, faceHourLen());

    if (!fullRedraw && aS == analogPrevSecond && !moveMinute && !moveHour) return;

    const uint32_t frameStart = micros();

    const float drawM = moveMinute ? aM : analogDrawnMinute;
    const float drawH = moveHour ? aH : analogDrawnHour;

    // Rows that can differ from what the panel already holds.
    int16_t yMin = 0;
    int16_t yMax = static_cast<int16_t>(SCREEN_H - 1);
    if (!fullRedraw) {
        yMin = static_cast<int16_t>(SCREEN_H);
        yMax = -1;
        analogSpanY(analogPrevSecond, faceSecLen(), faceSecTail(), 2.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
        analogSpanY(aS, faceSecLen(), faceSecTail(), 2.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
        if (moveMinute) {
            analogSpanY(analogDrawnMinute, faceMinLen(), faceMinTail(), 5.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
            analogSpanY(aM, faceMinLen(), faceMinTail(), 5.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
        }
        if (moveHour) {
            analogSpanY(analogDrawnHour, faceHourLen(), faceHourTail(), 6.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
            analogSpanY(aH, faceHourLen(), faceHourTail(), 6.0f + (mondaineActive() ? MONDAINE_SHADOW_REACH : 0.0f), yMin, yMax);
        }
        // the hub is repainted every frame, so its rows always count
        if (!mondaineActive()) {
            if (ANALOG_CY - ANALOG_HUB_R - 1 < yMin) yMin = static_cast<int16_t>(ANALOG_CY - ANALOG_HUB_R - 1);
            if (ANALOG_CY + ANALOG_HUB_R + 1 > yMax) yMax = static_cast<int16_t>(ANALOG_CY + ANALOG_HUB_R + 1);
        }
        if (yMin < 0) yMin = 0;
        if (yMax > SCREEN_H - 1) yMax = static_cast<int16_t>(SCREEN_H - 1);
    }

    if (analogBandBegin()) {
        drawAnalogComposited(drawH, drawM, aS, yMin, yMax);
    } else {
        drawAnalogDirect(drawH, drawM, aS);
    }

    analogFrameUs = micros() - frameStart;
    analogChromeDrawn = true;
    analogDrawnHour = drawH;
    analogDrawnMinute = drawM;
    analogPrevSecond = aS;
    lastTimeOk = validTime;
}

// ---------------------------------------------------------------------------
// Digital face
//
// Hours top-left, minutes bottom-right, matching the reference's proportions:
// the digit pair is 1.18 times as wide as it is tall, so the cells are squeezed
// horizontally when baked. Nothing here moves within a minute, so the whole
// face is repainted on the minute rather than tracked band by band.
// ---------------------------------------------------------------------------

void digitalBlitCell(TFT_eSPI& g, int16_t x, int16_t y, int16_t clipH, uint8_t digit, uint16_t fg, uint16_t bg) {
    if (digit > 9) return;
    if (y >= clipH || (y + DigitalArt::CELL_H) < 0) return;
    const uint8_t* cell = reinterpret_cast<const uint8_t*>(pgm_read_ptr(&DigitalArt::DIGITS[digit]));

    for (int16_t row = 0; row < DigitalArt::CELL_H; ++row) {
        const int16_t dy = static_cast<int16_t>(y + row);
        if (dy < 0 || dy >= clipH) continue;
        for (int16_t col = 0; col < DigitalArt::CELL_W; ++col) {
            const size_t index = (static_cast<size_t>(row) * DigitalArt::CELL_W) + col;
            const uint8_t alpha = readPackedAlpha(cell, index, 4);
            if (alpha == 0) continue;
            g.drawPixel(x + col, dy, blend565(fg, bg, alpha, 15));
        }
    }
}

void digitalPaintFace(TFT_eSPI& g, int16_t yOff, int16_t clipH, int hour, int minute) {
    const uint16_t bg = analogDial();
    const uint16_t hoursColor = analogLume();
    const uint16_t minsColor = analogHandColor();

    const int16_t pairW = static_cast<int16_t>(DigitalArt::CELL_W * 2);
    const int16_t hx = DIGITAL_MARGIN;
    const int16_t hy = static_cast<int16_t>(DIGITAL_MARGIN - yOff);
    const int16_t mx = static_cast<int16_t>(SCREEN_W - DIGITAL_MARGIN - pairW);
    const int16_t my = static_cast<int16_t>(SCREEN_H - DIGITAL_MARGIN - DigitalArt::CELL_H - yOff);

    digitalBlitCell(g, hx, hy, clipH, static_cast<uint8_t>(hour / 10), hoursColor, bg);
    digitalBlitCell(g, static_cast<int16_t>(hx + DigitalArt::CELL_W), hy, clipH,
                    static_cast<uint8_t>(hour % 10), hoursColor, bg);
    digitalBlitCell(g, mx, my, clipH, static_cast<uint8_t>(minute / 10), minsColor, bg);
    digitalBlitCell(g, static_cast<int16_t>(mx + DigitalArt::CELL_W), my, clipH,
                    static_cast<uint8_t>(minute % 10), minsColor, bg);
}

void drawDigital(bool force) {
    const time_t now = time(nullptr);
    const bool validTime = now > 1700000000;

    tm timeInfo{};
    if (validTime) localtime_r(&now, &timeInfo);
    int hour = validTime ? timeInfo.tm_hour : 0;
    const int minute = validTime ? timeInfo.tm_min : 0;
    if (!cfg.clock24h) {
        hour %= 12;
        if (hour == 0) hour = 12;
    }

    const int16_t stamp = static_cast<int16_t>((hour * 60) + minute);
    if (!force && analogChromeDrawn && stamp == digitalLastStamp) return;

    const uint32_t frameStart = micros();
    if (analogBandBegin()) {
        analogBandsPushed = 0;
        analogPushUs = 0;
        for (int16_t top = 0; top < SCREEN_H; top = static_cast<int16_t>(top + ANALOG_BAND_H)) {
            const int16_t rows = min<int16_t>(ANALOG_BAND_H, static_cast<int16_t>(SCREEN_H - top));
            analogBand.fillSprite(analogDial());
            digitalPaintFace(analogBand, top, rows, hour, minute);
            const uint32_t t0 = micros();
            analogBand.pushSprite(0, top);
            analogPushUs += micros() - t0;
            ++analogBandsPushed;
            wdtYield();
        }
    } else {
        tft.fillScreen(analogDial());
        digitalPaintFace(tft, 0, SCREEN_H, hour, minute);
        wdtYield();
    }

    analogFrameUs = micros() - frameStart;
    analogChromeDrawn = true;
    digitalLastStamp = stamp;
    lastTimeOk = validTime;
}

// ---------------------------------------------------------------------------
// The weather and date faces
//
// Both set their time in the same font as the digital face, so the three
// digital screens read as one family. Everything else reuses what the project
// already has - the bundled weather icons, and the UI font, whose Hangul covers
// the weekday and the date outright and the weather words with a handful of
// extra glyphs baked alongside it.
//
// Layout is taken from the references and left alone: what changed here is
// colour and wording, not proportion.
// ---------------------------------------------------------------------------

// Both faces set their digits from the digital face's own font rather than
// from segments, so all three digital screens read as one family. The cells are
// baked at one size and scaled down here, which costs no extra flash.
void digitalBlitScaled(TFT_eSPI& g, int16_t x, int16_t y, int16_t clipH, uint8_t digit, int16_t w, int16_t h,
                       uint16_t fg, uint16_t bg) {
    if (digit > 9 || w <= 0 || h <= 0) return;
    if (y >= clipH || (y + h) < 0) return;
    const uint8_t* cell = reinterpret_cast<const uint8_t*>(pgm_read_ptr(&DigitalArt::DIGITS[digit]));

    for (int16_t row = 0; row < h; ++row) {
        const int16_t dy = static_cast<int16_t>(y + row);
        if (dy < 0 || dy >= clipH) continue;
        const uint16_t sy0 = static_cast<uint16_t>((static_cast<uint32_t>(row) * DigitalArt::CELL_H) / h);
        uint16_t sy1 = static_cast<uint16_t>((static_cast<uint32_t>(row + 1) * DigitalArt::CELL_H) / h);
        if (sy1 <= sy0) sy1 = static_cast<uint16_t>(sy0 + 1);

        for (int16_t col = 0; col < w; ++col) {
            const uint16_t sx0 = static_cast<uint16_t>((static_cast<uint32_t>(col) * DigitalArt::CELL_W) / w);
            uint16_t sx1 = static_cast<uint16_t>((static_cast<uint32_t>(col + 1) * DigitalArt::CELL_W) / w);
            if (sx1 <= sx0) sx1 = static_cast<uint16_t>(sx0 + 1);

            // Box filter over the source rect this destination pixel covers, so
            // the reduction keeps the edges soft instead of dropping rows.
            uint16_t sum = 0;
            uint16_t count = 0;
            for (uint16_t sy = sy0; sy < sy1; ++sy) {
                const size_t base = static_cast<size_t>(sy) * DigitalArt::CELL_W;
                for (uint16_t sx = sx0; sx < sx1; ++sx) {
                    sum = static_cast<uint16_t>(sum + readPackedAlpha(cell, base + sx, 4));
                    ++count;
                }
            }
            if (sum == 0) continue;
            const uint8_t alpha = static_cast<uint8_t>((sum + (count / 2)) / count);
            if (alpha == 0) continue;
            g.drawPixel(static_cast<int16_t>(x + col), dy, blend565(fg, bg, alpha, 15));
        }
    }
}

// HH:MM centred on cx. The digit height is what the reference gives; the cell
// aspect follows the baked font, and the gap and colon keep the reference's
// spacing relative to that height.
struct ColonGeom {
    int16_t x;
    int16_t yTop;
    int16_t yBottom;
    int16_t r;
};

ColonGeom digitalColonGeom(float cx, float y, float digitH) {
    const int16_t w = static_cast<int16_t>(lroundf(digitH * DigitalArt::CELL_W / static_cast<float>(DigitalArt::CELL_H)));
    const float space = digitH * 0.11f;
    const float colonW = digitH * 0.26f;
    const float step = static_cast<float>(w) + space;
    const float total = (step * 4.0f) - space + colonW;
    const float x = cx - (total / 2.0f);

    ColonGeom out;
    out.x = static_cast<int16_t>(lroundf(x + (step * 2.0f) - (space / 2.0f) + (colonW / 2.0f)));
    out.yTop = static_cast<int16_t>(lroundf(y + (digitH * 0.32f)));
    out.yBottom = static_cast<int16_t>(lroundf(y + (digitH * 0.68f)));
    out.r = static_cast<int16_t>(max<int32_t>(2, lroundf(digitH * 0.06f)));
    return out;
}

// Phase of the blink. Derived from the clock rather than counted, so a repaint
// in the middle of a second lands on the same state the blink is showing.
bool colonLit() { return ((millis() / COLON_BLINK_MS) & 1UL) == 0UL; }

void digitalClockRow(TFT_eSPI& g, float cx, float y, float digitH, int16_t clipH, int hour, int minute,
                     uint16_t hoursColor, uint16_t minsColor, uint16_t colonColor, uint16_t bg) {
    const int16_t h = static_cast<int16_t>(lroundf(digitH));
    const int16_t w = static_cast<int16_t>(lroundf(digitH * DigitalArt::CELL_W / static_cast<float>(DigitalArt::CELL_H)));
    const float space = digitH * 0.11f;
    const float colonW = digitH * 0.26f;
    const float step = static_cast<float>(w) + space;
    const float total = (step * 4.0f) - space + colonW;
    const float x = cx - (total / 2.0f);
    const int16_t top = static_cast<int16_t>(lroundf(y));

    digitalBlitScaled(g, static_cast<int16_t>(lroundf(x)), top, clipH,
                      static_cast<uint8_t>(hour / 10), w, h, hoursColor, bg);
    digitalBlitScaled(g, static_cast<int16_t>(lroundf(x + step)), top, clipH,
                      static_cast<uint8_t>(hour % 10), w, h, hoursColor, bg);

    const ColonGeom colon = digitalColonGeom(cx, y, digitH);
    const uint16_t colonInk = colonLit() ? colonColor : bg;
    g.fillSmoothCircle(colon.x, colon.yTop, colon.r, colonInk, bg);
    g.fillSmoothCircle(colon.x, colon.yBottom, colon.r, colonInk, bg);

    digitalBlitScaled(g, static_cast<int16_t>(lroundf(x + (step * 2.0f) + colonW)), top, clipH,
                      static_cast<uint8_t>(minute / 10), w, h, minsColor, bg);
    digitalBlitScaled(g, static_cast<int16_t>(lroundf(x + (step * 3.0f) + colonW)), top, clipH,
                      static_cast<uint8_t>(minute % 10), w, h, minsColor, bg);
}

int16_t uiTextWidth(const String& text, uint8_t size) { return measureUiText(text, size); }

// Width of a run drawn with drawScaledLabel, which uses the clock font rather
// than the UI font and so needs its own measurement.
int16_t scaledLabelWidth(const char* text, int16_t targetH) {
    int16_t total = 0;
    for (const char* p = text; *p != 0; ++p) {
        total = static_cast<int16_t>(total + scaledDigitWidth(*p, targetH));
        if (*(p + 1) != 0) total = static_cast<int16_t>(total + 1);
    }
    return total;
}

// Word for the current sky in Korean, matching the icon the face is already
// showing. Written as UTF-8 byte escapes so the source file stays ASCII.
const char* conditionLabel(int sky, int pty) {
    switch (pty) {
        case 1:
        case 4:
        case 5:
            return "\xeb\xb9\x84";
        case 2:
        case 6:
            return "\xec\xa7\x84\xeb\x88\x88\xea\xb9\xa8\xeb\xb9\x84";
        case 3:
        case 7:
            return "\xeb\x88\x88";
        default:
            break;
    }
    if (sky >= 4) return "\xed\x9d\x90\xeb\xa6\xbc";
    if (sky == 3) return "\xea\xb5\xac\xeb\xa6\x84\xeb\xa7\x8e\xec\x9d\x8c";
    return "\xeb\xa7\x91\xec\x9d\x8c";
}

// --- weather face ----------------------------------------------------------
void weatherFacePaint(TFT_eSPI& g, int16_t yOff, int16_t clipH, int hour, int minute) {
    const uint16_t bg = analogDial();
    const uint16_t primary = analogLume();
    const uint16_t secondary = analogHandColor();

    String temp = "--";
    if (weather.valid && !isnan(weather.temp)) temp = String(static_cast<int>(lroundf(weather.temp)));
    const int16_t degR = 5;
    const int16_t right = 228;
    const int16_t degCx = static_cast<int16_t>(right - degR);
    const int16_t degCy = static_cast<int16_t>(33 - yOff);
    g.drawSmoothCircle(degCx, degCy, degR, primary, bg);
    g.drawSmoothCircle(degCx, degCy, degR - 1, primary, bg);

    const int16_t tempW = scaledLabelWidth(temp.c_str(), 36);
    drawScaledLabel(g, static_cast<int16_t>(degCx - degR - 6 - (tempW / 2)), static_cast<int16_t>(46 - yOff),
                    temp.c_str(), 36, primary, bg);

    const String condition = weather.valid ? String(conditionLabel(weather.sky, weather.pty)) : String("--");
    const int16_t condW = uiTextWidth(condition, 2);
    drawTextAt(g, static_cast<int16_t>(214 - condW), static_cast<int16_t>(92 - yOff), condition, 2, secondary, bg);

    digitalClockRow(g, SCREEN_W / 2.0f, WEATHER_FACE_CLOCK_Y - yOff, WEATHER_FACE_DIGIT_H, clipH, hour, minute,
                    primary, secondary, primary, bg);
}

// --- date face -------------------------------------------------------------
void dateFacePaint(TFT_eSPI& g, int16_t yOff, int16_t clipH, int hour, int minute, const tm& t, bool validTime) {
    const uint16_t bg = analogDial();
    const uint16_t primary = analogLume();
    const uint16_t secondary = analogHandColor();
    const uint16_t tertiary = analogCase();
    const uint16_t accent = analogAccent();

    digitalClockRow(g, SCREEN_W / 2.0f, DATE_FACE_CLOCK_Y - yOff, DATE_FACE_DIGIT_H, clipH, hour, minute,
                    primary, secondary, primary, bg);

    static const char* const WEEKDAY[7] = {
        "\xec\x9d\xbc\xec\x9a\x94\xec\x9d\xbc",  // 일요일
        "\xec\x9b\x94\xec\x9a\x94\xec\x9d\xbc",  // 월요일
        "\xed\x99\x94\xec\x9a\x94\xec\x9d\xbc",  // 화요일
        "\xec\x88\x98\xec\x9a\x94\xec\x9d\xbc",  // 수요일
        "\xeb\xaa\xa9\xec\x9a\x94\xec\x9d\xbc",  // 목요일
        "\xea\xb8\x88\xec\x9a\x94\xec\x9d\xbc",  // 금요일
        "\xed\x86\xa0\xec\x9a\x94\xec\x9d\xbc",  // 토요일
    };
    const String day = validTime ? String(WEEKDAY[t.tm_wday % 7]) : String("--");
    const int16_t dayW = uiTextWidth(day, 2);
    const int16_t boxW = static_cast<int16_t>(dayW + 18);
    const int16_t boxX = static_cast<int16_t>((SCREEN_W - boxW) / 2);
    g.fillRoundRect(boxX, static_cast<int16_t>(140 - yOff), boxW, 30, 6, accent);
    drawTextAt(g, static_cast<int16_t>((SCREEN_W - dayW) / 2), static_cast<int16_t>(146 - yOff), day, 2, primary,
               accent);

    // Korean date, as byte escapes: 2026 year, 08 month, 20 day.
    String date = "\x2d\x2d\x2d\x2d\xeb\x85\x84\x20\x2d\x2d\xec\x9b\x94\x20\x2d\x2d\xec\x9d\xbc";
    if (validTime) {
        char buf[40];
        snprintf(buf, sizeof(buf), "\x25\x30\x34\x64\xeb\x85\x84\x20\x25\x30\x32\x64\xec\x9b\x94\x20\x25\x30\x32\x64\xec\x9d\xbc", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        date = buf;
    }
    const int16_t dateW = uiTextWidth(date, 2);
    drawTextAt(g, static_cast<int16_t>((SCREEN_W - dateW) / 2), static_cast<int16_t>(184 - yOff), date, 2, tertiary, bg);
}

// The icon comes off LittleFS, so it is drawn once straight to the panel after
// the bands have gone up rather than once per band. Nothing animated overlaps
// it, so there is nothing to composite it with.
void weatherFaceIcon() {
    if (!fsMounted) return;
    const String path = String("/weather-icons/") + iconSlot(weather.sky, weather.pty) + ".bmp";
    if (!LittleFS.exists(path)) return;
    // drawBmpIcon centres the artwork inside the box it is given, and these
    // icons are not square, so centring the box centres what is drawn.
    drawBmpIcon(tft, path, WEATHER_ICON_X, WEATHER_ICON_Y, WEATHER_ICON_BOX);
}

// Only the weather face needs the distinction: it is the one with an icon to
// draw after the bands go up.
bool weatherFaceActive() { return activeScreen == SCREEN_WEATHER_DIGITAL; }
bool minuteFaceActive() {
    return activeScreen == SCREEN_WEATHER_DIGITAL || activeScreen == SCREEN_DATE_DIGITAL;
}

bool colonDrawnLit = true;

// Repaints the two dots and nothing else. Called at loop rate so the half
// second is honoured; it returns immediately unless the phase actually flipped.
void minuteFaceColonTick(bool force) {
    if (!minuteFaceActive()) return;
    const bool lit = colonLit();
    if (!force && lit == colonDrawnLit) return;
    colonDrawnLit = lit;

    const bool wf = weatherFaceActive();
    const ColonGeom colon = digitalColonGeom(SCREEN_W / 2.0f,
                                             wf ? WEATHER_FACE_CLOCK_Y : DATE_FACE_CLOCK_Y,
                                             wf ? WEATHER_FACE_DIGIT_H : DATE_FACE_DIGIT_H);
    const uint16_t bg = analogDial();
    const uint16_t ink = lit ? analogLume() : bg;
    tft.fillSmoothCircle(colon.x, colon.yTop, colon.r, ink, bg);
    tft.fillSmoothCircle(colon.x, colon.yBottom, colon.r, ink, bg);
}

// Neither face changes within a minute, so both repaint on the minute.
void drawMinuteFace(bool force) {
    const time_t now = time(nullptr);
    const bool validTime = now > 1700000000;

    tm timeInfo{};
    if (validTime) localtime_r(&now, &timeInfo);
    int hour = validTime ? timeInfo.tm_hour : 0;
    const int minute = validTime ? timeInfo.tm_min : 0;
    if (!cfg.clock24h) {
        hour %= 12;
        if (hour == 0) hour = 12;
    }

    const int16_t stamp = static_cast<int16_t>((hour * 60) + minute);
    if (!force && analogChromeDrawn && stamp == digitalLastStamp) return;

    const uint32_t frameStart = micros();
    const bool wf = weatherFaceActive();

    if (analogBandBegin()) {
        analogBandsPushed = 0;
        analogPushUs = 0;
        for (int16_t top = 0; top < SCREEN_H; top = static_cast<int16_t>(top + ANALOG_BAND_H)) {
            const int16_t rows = min<int16_t>(ANALOG_BAND_H, static_cast<int16_t>(SCREEN_H - top));
            analogBand.fillSprite(analogDial());
            if (wf) {
                weatherFacePaint(analogBand, top, rows, hour, minute);
            } else {
                dateFacePaint(analogBand, top, rows, hour, minute, timeInfo, validTime);
            }
            const uint32_t t0 = micros();
            analogBand.pushSprite(0, top);
            analogPushUs += micros() - t0;
            ++analogBandsPushed;
            wdtYield();
        }
        if (wf) weatherFaceIcon();
    } else {
        tft.fillScreen(analogDial());
        if (wf) {
            weatherFacePaint(tft, 0, SCREEN_H, hour, minute);
            weatherFaceIcon();
        } else {
            dateFacePaint(tft, 0, SCREEN_H, hour, minute, timeInfo, validTime);
        }
        wdtYield();
    }

    analogFrameUs = micros() - frameStart;
    analogChromeDrawn = true;
    digitalLastStamp = stamp;
    lastTimeOk = validTime;
}

// ---------------------------------------------------------------------------
// Screen rotation
// ---------------------------------------------------------------------------


uint16_t enabledScreens() {
    const uint16_t mask = cfg.screens & SCREEN_MASK_ALL;
    return mask == 0 ? static_cast<uint16_t>(1U << SCREEN_CLOCK_WEATHER) : mask;
}

int enabledScreenCount() {
    const uint16_t mask = enabledScreens();
    int count = 0;
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
        if (mask & (1U << i)) ++count;
    }
    return count;
}

uint8_t nextEnabledScreen(uint8_t from) {
    const uint16_t mask = enabledScreens();
    const uint8_t at = screenOrderIndex(from);
    for (uint8_t step = 1; step <= SCREEN_COUNT; ++step) {
        const uint8_t candidate = cfg.screenOrder[(at + step) % SCREEN_COUNT];
        if (mask & (1U << candidate)) return candidate;
    }
    return from;
}

void applyScreenSelection() {
    const uint16_t mask = enabledScreens();
    if (!(mask & (1U << activeScreen))) activeScreen = nextEnabledScreen(activeScreen);
    lastScreenSwitchMs = millis();
    screenChromeDrawn = false;
    analogChromeDrawn = false;
    digitalLastStamp = -1;
    resetDisplayCache();
}

// ---------------------------------------------------------------------------
// Photo album
//
// The panel wants RGB565, and this device has neither the flash for an image
// decoder nor the heap to hold a decoded frame - 240x240x2 is 115 KB against
// 80 KB of RAM. So the browser does the decoding: it crops and resizes
// whatever the user picked onto a canvas and uploads the pixels already in the
// panel's own format. Showing a photo is then a copy from LittleFS to SPI,
// with nothing in between.
//
// The byte order is the file's, not the CPU's. On ESP8266 pushPixels memcpy's
// the buffer straight into the SPI FIFO, so the bytes reach the panel exactly
// as they sit in the file. The files are written big-endian, which is what the
// panel reads, and no swap happens anywhere.
//
// Order and on/off live in a small manifest rather than in the filenames.
// Reordering rewrites 200 bytes instead of moving 115 KB files around.
// ---------------------------------------------------------------------------

constexpr int16_t ALBUM_W = 240;
constexpr int16_t ALBUM_H = 240;
constexpr size_t ALBUM_BYTES = static_cast<size_t>(ALBUM_W) * ALBUM_H * 2U;
constexpr int16_t ALBUM_THUMB = 40;
constexpr size_t ALBUM_THUMB_BYTES = static_cast<size_t>(ALBUM_THUMB) * ALBUM_THUMB * 2U;
constexpr uint8_t ALBUM_MAX = 16;  // about what 1.8 MB of filesystem holds at 118 KB a photo
constexpr int16_t ALBUM_ROWS_PER_READ = 8;  // 3840 B, well inside the free heap

const char* const ALBUM_DIR = "/album";
const char* const ALBUM_INDEX = "/album/index.json";

struct AlbumEntry {
    String id;
    String name;
    bool on = true;
};

AlbumEntry albumEntries[ALBUM_MAX];
uint8_t albumCount = 0;
int16_t albumCursor = -1;
uint32_t albumLastSwitchMs = 0;
bool albumDrawn = false;
uint32_t albumFrameUs = 0;

String albumPath(const String& id, const char* ext) {
    return String(ALBUM_DIR) + "/" + id + ext;
}

// Ids become filenames, so they are kept to a safe alphabet rather than
// trusted. Anything else would let an upload escape /album.
bool albumIdOk(const String& id) {
    if (id.length() == 0 || id.length() > 24) return false;
    for (size_t i = 0; i < id.length(); ++i) {
        const char c = id[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

int16_t albumFind(const String& id) {
    for (uint8_t i = 0; i < albumCount; ++i) {
        if (albumEntries[i].id == id) return static_cast<int16_t>(i);
    }
    return -1;
}

void albumLoadIndex() {
    albumCount = 0;
    if (!fsMounted || !LittleFS.exists(ALBUM_INDEX)) return;
    File f = LittleFS.open(ALBUM_INDEX, "r");
    if (!f) return;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    for (JsonObject item : doc["photos"].as<JsonArray>()) {
        if (albumCount >= ALBUM_MAX) break;
        const String id = item["id"] | "";
        if (!albumIdOk(id)) continue;
        // A manifest entry without its pixels would draw a blank screen, so the
        // file is the authority on what exists.
        if (!LittleFS.exists(albumPath(id, ".rgb"))) continue;
        albumEntries[albumCount].id = id;
        albumEntries[albumCount].name = item["name"] | id;
        albumEntries[albumCount].on = item["on"] | true;
        ++albumCount;
    }
}

bool albumSaveIndex() {
    if (!fsMounted && !LittleFS.begin()) return false;
    fsMounted = true;
    if (!LittleFS.exists(ALBUM_DIR)) LittleFS.mkdir(ALBUM_DIR);
    File f = LittleFS.open(ALBUM_INDEX, "w");
    if (!f) return false;
    JsonDocument doc;
    JsonArray arr = doc["photos"].to<JsonArray>();
    for (uint8_t i = 0; i < albumCount; ++i) {
        JsonObject item = arr.add<JsonObject>();
        item["id"] = albumEntries[i].id;
        item["name"] = albumEntries[i].name;
        item["on"] = albumEntries[i].on;
    }
    serializeJson(doc, f);
    f.close();
    return true;
}

uint8_t albumEnabledCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < albumCount; ++i) {
        if (albumEntries[i].on) ++n;
    }
    return n;
}

int16_t albumNextEnabled(int16_t from) {
    if (albumCount == 0) return -1;
    for (uint8_t step = 1; step <= albumCount; ++step) {
        const int16_t candidate = static_cast<int16_t>((from + step) % albumCount);
        if (albumEntries[candidate].on) return candidate;
    }
    return -1;
}

// Streams one photo from LittleFS to the panel. Nothing is decoded and nothing
// is converted: the file already holds what the panel wants.
bool albumRender(const String& id) {
    const String path = albumPath(id, ".rgb");
    if (!fsMounted || !LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    if (f.size() < ALBUM_BYTES) {
        f.close();
        return false;
    }

    const size_t chunk = static_cast<size_t>(ALBUM_W) * ALBUM_ROWS_PER_READ * 2U;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[chunk]);
    if (!buf) {
        f.close();
        return false;
    }

    const uint32_t start = micros();
    tft.setSwapBytes(false);  // send the file's bytes through untouched
    for (int16_t y = 0; y < ALBUM_H; y += ALBUM_ROWS_PER_READ) {
        const int16_t rows = min<int16_t>(ALBUM_ROWS_PER_READ, ALBUM_H - y);
        const size_t want = static_cast<size_t>(ALBUM_W) * rows * 2U;
        if (f.read(buf.get(), want) != static_cast<int>(want)) {
            f.close();
            return false;
        }
        tft.pushImage(0, y, ALBUM_W, rows, reinterpret_cast<uint16_t*>(buf.get()));
        wdtYield();
    }
    albumFrameUs = micros() - start;
    f.close();
    return true;
}

void albumShowMessage(const String& line1, const String& line2) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    drawCenteredText(104, line1, 2, TFT_DARKGREY, TFT_BLACK, 0, SCREEN_W);
    if (line2.length()) drawCenteredText(132, line2, 2, TFT_DARKGREY, TFT_BLACK, 0, SCREEN_W);
}

void drawAlbum(bool force) {
    if (force) {
        analogBandEnd();  // hand the band memory back before allocating the row buffer
        albumDrawn = false;
    }

    if (albumEnabledCount() == 0) {
        if (!albumDrawn) {
            albumShowMessage(F("Photo album"), F("no photos"));
            albumDrawn = true;
        }
        return;
    }

    const uint32_t now = millis();
    const uint32_t intervalMs = static_cast<uint32_t>(cfg.albumIntervalSeconds) * 1000UL;
    bool advance = !albumDrawn;
    if (albumDrawn && albumEnabledCount() > 1 && now - albumLastSwitchMs >= intervalMs) advance = true;
    if (!advance) return;

    const int16_t next = albumNextEnabled(albumCursor);
    if (next < 0) return;
    albumCursor = next;
    albumLastSwitchMs = now;
    if (!albumRender(albumEntries[albumCursor].id)) {
        albumShowMessage(F("Photo album"), F("read failed"));
    }
    albumDrawn = true;
}

// ---------------------------------------------------------------------------
// Plane radar - the feed
//
// Ported from giovi321/smalltv-mod (WTFPL), whose radar in turn reimplements
// MatixYo/ESP32-Plane-Radar. Same hardware family, so the approach carries over
// intact; what changed is the drawing layer, TFT_eSPI here rather than
// Arduino_GFX.
//
// The device fetches for itself, which means TLS on a chip with 40 KB of heap.
// Three things make that fit, and all three come from the original:
//   - the response is parsed straight off the socket through an ArduinoJson
//     filter, so only the seven fields that get plotted are ever held;
//   - the TLS receive buffer is sized by asking the server what it will accept
//     rather than by assuming, which is the difference between 512 bytes and
//     16 KB;
//   - a fetch is skipped outright when the largest free block is too small to
//     survive a handshake. Skipping a poll costs nothing; running out of heap
//     mid-handshake costs a reboot.
// ---------------------------------------------------------------------------

constexpr uint8_t RADAR_MAX_AIRCRAFT = 24;
constexpr uint8_t RADAR_MAX_AIRPORTS = 6;
constexpr const char* ADSB_HOST = "opendata.adsb.fi";
constexpr const char* ADSB_PATH = "/api/v3/lat/";
constexpr const char* ADSB_USER_AGENT = "Mozilla/5.0 (SmallTV)";
// Below this the handshake is not attempted. The original uses free heap; the
// largest contiguous block is the number that actually decides, because that is
// what the TLS buffers need.
constexpr uint32_t RADAR_MIN_BLOCK = 18000;
// How many polls in a row the heap guard may refuse before it has to let one
// through. Six of them is about a minute at the default poll, which is long
// enough for a genuine squeeze to pass and short enough that a wedged radar
// comes back on its own rather than waiting for someone to power-cycle it.
constexpr uint8_t RADAR_REFUSALS_MAX = 6;
uint8_t radarBlockRefusals = 0;

struct Aircraft {
    float lat, lon;
    float track;        // ground track, degrees, 0 = N; NAN if unknown
    float gs;           // ground speed, knots; NAN if unknown
    int32_t altFt;      // barometric altitude; 0 when on ground or unknown
    bool rotor;         // ADS-B emitter category A7
    char type[5];       // ICAO type designator, e.g. H60
    char callsign[9];
    float distKm;
    float bearingDeg;
};

Aircraft radarAc[RADAR_MAX_AIRCRAFT];   // nearest first
uint8_t radarAcCount = 0;
uint32_t radarLastOkMs = 0;
uint32_t radarLastTryMs = 0;
bool radarErrorFlag = false;
uint16_t radarTlsRx = 0;
String radarStatus = "idle";
uint32_t radarFetchMs = 0;
uint32_t radarHeapLow = 0;
// Split out so the pause the sweep shows at the top of a revolution can be
// attributed rather than guessed at.
uint32_t radarRepaintMs = 0;
uint32_t radarContentsMs = 0;
uint32_t radarDnsMs = 0;
uint32_t radarConnectMs = 0;
uint32_t radarBodyMs = 0;

// Flat earth around home. At radar ranges the error is far below one pixel, and
// it costs two multiplies instead of a haversine.
void radarGeo(float homeLat, float homeLon, float lat, float lon, float& distKm, float& brg) {
    const float dLat = (lat - homeLat) * 111.0f;
    const float dLon = (lon - homeLon) * 111.0f * cosf(homeLat * PI / 180.0f);
    distKm = sqrtf((dLat * dLat) + (dLon * dLon));
    brg = atan2f(dLon, dLat) * 180.0f / PI;
    if (brg < 0) brg += 360.0f;
}

void radarInsertNearest(const Aircraft& t) {
    if (radarAcCount == RADAR_MAX_AIRCRAFT && t.distKm >= radarAc[radarAcCount - 1].distKm) return;
    uint8_t i = (radarAcCount < RADAR_MAX_AIRCRAFT) ? radarAcCount
                                                    : static_cast<uint8_t>(RADAR_MAX_AIRCRAFT - 1);
    while (i > 0 && radarAc[i - 1].distKm > t.distKm) {
        radarAc[i] = radarAc[i - 1];
        --i;
    }
    radarAc[i] = t;
    if (radarAcCount < RADAR_MAX_AIRCRAFT) ++radarAcCount;
}

void radarTrimTail(char* s) {
    int n = static_cast<int>(strlen(s));
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// Asked once. A server that honours a 512-byte maximum fragment lets BearSSL
// hold a buffer a thirtieth the size of the default, which is the whole reason
// this fits.
void radarProbeTls() {
    if (radarTlsRx != 0) return;
    if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(ADSB_HOST, 443, 512)) radarTlsRx = 512;
    else if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(ADSB_HOST, 443, 1024)) radarTlsRx = 1024;
    else radarTlsRx = 4096;
}

uint16_t radarRangeNm(uint16_t km) {
    const uint16_t nm = static_cast<uint16_t>(lroundf(km / 1.852f)) + 1;  // +1 covers the ring edge
    return nm < 1 ? 1 : nm;
}

String radarUrl() {
    String u = "https://";
    u += ADSB_HOST;
    u += ADSB_PATH;
    u += String(cfg.radarLat, 4);
    u += "/lon/";
    u += String(cfg.radarLon, 4);
    u += "/dist/";
    u += String(radarRangeNm(cfg.radarRangeKm));
    return u;
}

// Only the fields that get plotted are pulled out of the stream; the rest of
// each aircraft object is walked past without being stored.
bool radarParse(Stream& stream) {
    JsonDocument filter;
    JsonObject fe = filter["ac"][0].to<JsonObject>();
    fe["lat"] = true;
    fe["lon"] = true;
    fe["track"] = true;
    fe["gs"] = true;
    fe["flight"] = true;
    fe["hex"] = true;
    fe["alt_baro"] = true;
    fe["category"] = true;
    fe["t"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, stream, DeserializationOption::Filter(filter))) return false;
    JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
    if (ac.isNull()) return false;

    radarAcCount = 0;
    for (JsonObjectConst a : ac) {
        if (!a["lat"].is<float>() && !a["lat"].is<int>()) continue;
        if (!a["lon"].is<float>() && !a["lon"].is<int>()) continue;

        Aircraft t{};
        t.lat = a["lat"].as<float>();
        t.lon = a["lon"].as<float>();
        t.track = (a["track"].is<float>() || a["track"].is<int>()) ? a["track"].as<float>() : NAN;
        t.gs = (a["gs"].is<float>() || a["gs"].is<int>()) ? a["gs"].as<float>() : NAN;
        t.altFt = a["alt_baro"].is<int>() ? a["alt_baro"].as<int>() : 0;  // "ground" parses as 0
        if (cfg.radarMinAltFt > 0 && t.altFt < static_cast<int32_t>(cfg.radarMinAltFt)) continue;

        strlcpy(t.type, a["t"] | "", sizeof(t.type));
        // The emitter category is what an aircraft says it is, and A7 means
        // rotorcraft - but it is only as good as the transponder's programming.
        // A Bell 206 overhead broadcasts A1, "light aircraft", and was drawn as
        // a plane. So the type table gets a say: it already knows B06 is a
        // helicopter, and a code it recognises settles the matter whatever the
        // category claims.
        const char* cat = a["category"] | "";
        t.rotor = (cat[0] == 'A' && cat[1] == '7') || Rotorcraft::knows(t.type);
        // The hex fallback has to run AFTER the field is inspected, not inside
        // the JSON lookup: ArduinoJson's | operator only falls back when the
        // key is absent, and a transponder with no callsign programmed still
        // broadcasts the field - as eight spaces, or as eight '@'s, which is
        // what an empty Mode S callsign register reads as. Both sailed past
        // the old fallback, and the aircraft was drawn as a bare symbol with
        // its type and altitude known but nowhere to hang them. A callsign is
        // taken only if it contains at least one letter or digit; anything
        // else means the hex is the only name there is.
        strlcpy(t.callsign, a["flight"] | "", sizeof(t.callsign));
        radarTrimTail(t.callsign);
        bool usable = false;
        for (const char* c = t.callsign; *c != 0 && !usable; ++c) {
            usable = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                     (*c >= '0' && *c <= '9');
        }
        if (!usable) {
            strlcpy(t.callsign, a["hex"] | "", sizeof(t.callsign));
            radarTrimTail(t.callsign);
        }

        radarGeo(cfg.radarLat, cfg.radarLon, t.lat, t.lon, t.distKm, t.bearingDeg);
        radarInsertNearest(t);
    }
    radarLastOkMs = millis();
    radarErrorFlag = false;
    return true;
}

bool radarFetch() {
    radarLastTryMs = millis();
    if (cfg.radarLat == 0.0f && cfg.radarLon == 0.0f) {
        radarStatus = "home not set";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        radarStatus = "wifi disconnected";
        return false;
    }
    const uint32_t start = millis();
    radarProbeTls();

    // Resolved here rather than inside begin() so the lookup shows up separately
    // from the handshake. The answer is cached: the host does not move, and a
    // lookup every poll is time spent for nothing.
    static IPAddress cached;
    static uint32_t cachedAt = 0;
    if (cachedAt == 0 || millis() - cachedAt > 3600000UL) {
        const uint32_t d0 = millis();
        IPAddress found;
        if (WiFi.hostByName(ADSB_HOST, found)) {
            cached = found;
            cachedAt = millis();
        }
        radarDnsMs = millis() - d0;
    } else {
        radarDnsMs = 0;
    }

    bool ok = false;
    {
        // Built for each poll and let go again. Holding one open was tried: the
        // server does keep an idle connection alive, but on the device the reuse
        // only survived about half the time, and 174 ms off an average fetch is
        // not worth 16 KB of permanent heap on a chip that has 40.
        // The guard keeps a handshake from being started in a hole too small to
        // finish it. On its own, though, it is a one-way door: uploading the web
        // files leaves the heap split with about 17 KB as the largest piece,
        // just under this floor, and from then on every poll is refused - and a
        // refused poll allocates nothing, so the fragmentation it is waiting on
        // can never change. The radar stayed dead until the next reboot.
        //
        // So the guard yields. After enough consecutive refusals it steps aside
        // for one attempt; if the heap really is too small the allocation below
        // fails and the null check turns it away, which costs a poll rather than
        // the whole feature.
        const uint32_t block = ESP.getMaxFreeBlockSize();
        if (block < RADAR_MIN_BLOCK && radarBlockRefusals < RADAR_REFUSALS_MAX) {
            ++radarBlockRefusals;
            radarStatus = "heap too low: " + String(block);
            return false;
        }
        radarBlockRefusals = 0;
        std::unique_ptr<BearSSL::WiFiClientSecure> holder(new BearSSL::WiFiClientSecure());
        BearSSL::WiFiClientSecure* client = holder.get();
        if (client == nullptr) {
            radarStatus = "no client";
            return false;
        }
        // Read-only public feed, and a trust store costs heap this chip does not
        // have to spare. The risk taken is a spoofed aircraft list.
        client->setInsecure();
        client->setBufferSizes(radarTlsRx, 512);
        // Resuming the previous session skips the expensive half of the
        // handshake, which is most of the pause the sweep shows when a poll
        // lands. The session is held across fetches for exactly that reason.
        static BearSSL::Session tlsSession;
        client->setSession(&tlsSession);

        HTTPClient http;
        http.setTimeout(8000);
        http.setReuse(false);
        // Ask in HTTP/1.0, which has no chunked transfer encoding, because the
        // body is handed straight to the JSON parser. getStream() returns the
        // raw socket - HTTPClient only unchunks inside writeToStream() - so a
        // chunked reply reaches the parser as "60d\r\n{"ac":[...", and it reads
        // the chunk length as the document and stops. The feed used to answer
        // with Content-Length and started chunking on 2026-08-27, which broke
        // every poll at once, empty sky or full.
        http.useHTTP10(true);
        if (http.begin(*client, radarUrl())) {
            http.addHeader("Accept", "application/json");
            // HTTP/1.0 makes HTTPClient drop its own Accept-Encoding header
            // (it is emitted only under !_useHTTP10), and a request with no
            // Accept-Encoding lets the server pick any coding it likes. This
            // body goes straight into the JSON parser, so say identity out
            // loud - the same pairing httpGetBody() already uses for KMA.
            http.addHeader("Accept-Encoding", "identity");
            http.setUserAgent(ADSB_USER_AGENT);
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            const uint32_t c0 = millis();
            const int code = http.GET();
            radarConnectMs = millis() - c0;
            if (code == HTTP_CODE_OK) {
                radarHeapLow = ESP.getFreeHeap();
                const uint32_t b0 = millis();
                ok = radarParse(http.getStream());
                // The parser stops at the closing brace and the rest of the body
                // is left in the socket, which used to be drained here so that
                // HTTPClient would consider the response finished and reuse the
                // connection. Under HTTP/1.0 there is no connection to reuse -
                // the server closes it and http.end() drops it either way - so
                // the drain was only spending up to 200 ms a poll, one byte at a
                // time, on a socket about to go away.
                radarBodyMs = millis() - b0;
                radarStatus = ok ? "ok" : "parse failed";
            } else {
                radarStatus = "http " + String(code);
            }
            http.end();
        } else {
            radarStatus = "begin failed";
        }
    }

    radarFetchMs = millis() - start;
    if (!ok) radarErrorFlag = true;
    return ok;
}

// ---------------------------------------------------------------------------
// Where the flight is going
//
// ADS-B carries no route: an aircraft broadcasts where it is, not where it is
// bound. The destination has to be looked up by callsign, and that is another
// TLS handshake on a chip where one already costs the sweep half a second.
//
// So it is rationed. A callsign is looked up once and remembered; while it stays
// in range nothing more is asked. At most one lookup happens per revolution, and
// only for the nearest aircraft still unknown, which means a busy sky costs one
// extra fetch a turn rather than one per aircraft. The answers outlive the
// aircraft leaving the ring, so one that circles back is free.
// ---------------------------------------------------------------------------

constexpr const char* ROUTE_HOST = "api.adsbdb.com";
constexpr uint8_t ROUTE_CACHE = 24;

struct RouteEntry {
    char callsign[9];
    char orig[5];       // IATA, or empty when there is no route to show
    char dest[5];
    uint32_t used;      // a counter, not a clock: see below
};

RouteEntry routeCache[ROUTE_CACHE];
uint8_t routeCount = 0;
// Eviction ranks entries by when they were last touched, and millis() is the
// wrong ruler for that: it wraps at about 49 days, after which the smallest
// value is the newest entry rather than the oldest, and the cache would evict
// exactly what it should keep. A counter that only ever goes up has no such
// day. It would wrap too, in about thirteen million years of continuous use.
uint32_t routeClock = 0;
uint16_t routeTlsRx = 0;
uint32_t routeLastMs = 0;
String routeStatus = "idle";

int16_t routeFind(const char* callsign) {
    for (uint8_t i = 0; i < routeCount; ++i) {
        if (strcmp(routeCache[i].callsign, callsign) == 0) return static_cast<int16_t>(i);
    }
    return -1;
}

// A miss and a known-no-route are both remembered. Storing the negative answer
// is the point: without it an aircraft the service does not know would be asked
// about on every single revolution.
void routeStore(const char* callsign, const char* orig, const char* dest) {
    int16_t at = routeFind(callsign);
    if (at < 0) {
        if (routeCount < ROUTE_CACHE) {
            at = static_cast<int16_t>(routeCount++);
        } else {
            at = 0;
            for (uint8_t i = 1; i < routeCount; ++i) {
                if (routeCache[i].used < routeCache[at].used) at = static_cast<int16_t>(i);
            }
        }
    }
    strlcpy(routeCache[at].callsign, callsign, sizeof(routeCache[at].callsign));
    strlcpy(routeCache[at].orig, orig, sizeof(routeCache[at].orig));
    strlcpy(routeCache[at].dest, dest, sizeof(routeCache[at].dest));
    routeCache[at].used = ++routeClock;
}

// Both ends at once rather than two lookups: taken separately they could touch
// the cache between calls and come back from different entries.
bool routeLeg(const char* callsign, const char** orig, const char** dest) {
    *orig = nullptr;
    *dest = nullptr;
    const int16_t at = routeFind(callsign);
    if (at < 0) return false;
    routeCache[at].used = ++routeClock;
    if (routeCache[at].orig[0] != 0) *orig = routeCache[at].orig;
    if (routeCache[at].dest[0] != 0) *dest = routeCache[at].dest;
    return true;
}

void routeProbeTls() {
    if (routeTlsRx != 0) return;
    if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(ROUTE_HOST, 443, 512)) routeTlsRx = 512;
    else if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(ROUTE_HOST, 443, 1024)) routeTlsRx = 1024;
    else routeTlsRx = 4096;
}

// Returns having recorded something for this callsign, whatever happened. That
// matters more than it sounds: routeService takes the first aircraft with no
// entry and stops there, so an attempt that recorded nothing would be repeated
// on the next revolution, and the next, while every other aircraft in the ring
// waited behind it forever. A failure is written down as "no route" and the
// aircraft moves out of the queue; it gets another chance when the entry is
// eventually evicted, or when it leaves and comes back.
bool routeFetch(const char* callsign) {
    if (WiFi.status() != WL_CONNECTED) return false;
    const uint32_t block = ESP.getMaxFreeBlockSize();
    if (block < RADAR_MIN_BLOCK) {
        routeStatus = "heap too low";
        return false;
    }
    routeProbeTls();

    const uint32_t start = millis();
    bool stored = false;
    {
        std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
        if (!client) return false;
        client->setInsecure();
        client->setBufferSizes(routeTlsRx, 512);
        static BearSSL::Session routeSession;
        client->setSession(&routeSession);

        String url = "https://";
        url += ROUTE_HOST;
        url += "/v0/callsign/";
        url += callsign;

        HTTPClient http;
        http.setTimeout(8000);
        http.setReuse(false);
        // Same reason as the aircraft feed: this body is streamed into the JSON
        // parser, and a chunked reply would arrive with its length lines still
        // in it. This host sends Content-Length today, which is exactly why the
        // guard belongs here - the other one did too, until it did not.
        http.useHTTP10(true);
        if (http.begin(*client, url)) {
            http.addHeader("Accept", "application/json");
            // HTTP/1.0 makes HTTPClient drop its own Accept-Encoding header
            // (it is emitted only under !_useHTTP10), and a request with no
            // Accept-Encoding lets the server pick any coding it likes. This
            // body goes straight into the JSON parser, so say identity out
            // loud - the same pairing httpGetBody() already uses for KMA.
            http.addHeader("Accept-Encoding", "identity");
            http.setUserAgent(ADSB_USER_AGENT);
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            const int code = http.GET();
            if (code == HTTP_CODE_OK) {
                // Only the one field is kept; the reply also carries the origin,
                // both airports in full and the airline, none of which is drawn.
                JsonDocument filter;
                filter["response"]["flightroute"]["origin"]["iata_code"] = true;
                filter["response"]["flightroute"]["destination"]["iata_code"] = true;
                JsonDocument doc;
                if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
                    const char* from = doc["response"]["flightroute"]["origin"]["iata_code"] | "";
                    const char* to = doc["response"]["flightroute"]["destination"]["iata_code"] | "";
                    routeStore(callsign, from, to);
                    stored = true;
                    // Only join the two when there are two. This field is what
                    // tells a working lookup from a broken one, and reporting
                    // "ok -GMP" for a route with no origin on file sends the
                    // next reader hunting a parsing bug that is not there.
                    if (to[0] == 0) routeStatus = "no route";
                    else if (from[0] == 0) routeStatus = String("ok ") + to;
                    else routeStatus = String("ok ") + from + "-" + to;
                }
            } else if (code == HTTP_CODE_NOT_FOUND) {
                // The service knows the callsign is unknown, which is an answer
                // worth keeping so it is not asked again every turn.
                routeStore(callsign, "", "");
                stored = true;
                routeStatus = "unknown";
            } else {
                routeStatus = "http " + String(code);
            }
            http.end();
        } else {
            routeStatus = "begin failed";
        }
    }
    routeLastMs = millis() - start;
    if (!stored) {
        // Nothing came back that could be believed. Recorded anyway, so the
        // queue moves on.
        routeStore(callsign, "", "");
        if (routeStatus.length() == 0 || routeStatus == "idle") routeStatus = "failed";
    }
    return stored;
}

// One per revolution, nearest first. Anything already answered is skipped, so a
// sky that has not changed costs nothing at all.
void routeService() {
    if (!cfg.radarRoutes) return;
    for (uint8_t i = 0; i < radarAcCount; ++i) {
        const char* cs = radarAc[i].callsign;
        if (cs[0] == 0 || radarAc[i].rotor) continue;   // helicopters do not fly routes
        if (routeFind(cs) >= 0) continue;
        routeFetch(cs);
        return;
    }
}

// ---------------------------------------------------------------------------
// Plane radar - the screen
//
// The layout is the original's, unchanged: same centre and outer radius, same
// two rings and crosshair, same heading triangles and speed vectors, same
// label declutter, same rim dots for traffic beyond the ring, same overlays.
// Only the drawing calls differ, TFT_eSPI instead of Arduino_GFX.
//
// What is new is the sweep. A real PPI paints one radius at a time as the
// antenna turns, and that is what happens here: the scene is drawn once, then a
// line walks round it, one revolution per poll, so the picture is refreshed
// exactly as new data lands. Nothing is buffered - 240x240x2 is 115 KB - so the
// sweep erases the radius it is leaving and repairs what that radius crossed,
// which is a few hundred pixels a frame rather than a whole screen.
// ---------------------------------------------------------------------------

constexpr int16_t RADAR_CX = SCREEN_W / 2;
constexpr int16_t RADAR_CY = SCREEN_H / 2;
constexpr int16_t RADAR_RR = 112;
// The furniture is the sweep's own green: rings, crosshair, north marker and
// the headers, which is what a monochrome radar screen looks like. The rings sit
// a shade under the sweep head so the head still reads as the brightest thing on
// the dial.
constexpr uint16_t RADAR_C_DGRAY = 0x0480;
constexpr uint16_t RADAR_C_GRAY = 0x07E0;
constexpr uint16_t RADAR_C_RED = 0xF800;
constexpr uint16_t RADAR_C_GREEN = 0x07E0;
constexpr uint16_t RADAR_C_BLUE = 0x041F;
constexpr uint16_t RADAR_C_MAGENTA = 0xF81F;
constexpr uint16_t RADAR_C_YELLOW = 0xFFE0;

// Sweep. The trail is drawn as a handful of dimming radii behind the head; more
// than this and the repair cost per frame stops being worth the look.
// The sweep is counted in whole steps rather than accumulated in degrees. A
// float that is added to every frame and wrapped at 360 does not give back the
// same value for the same bearing twice, and the erase then misses the line it
// is aiming at by a pixel. Those near misses are what was left smeared round
// the dial. An integer step always maps to the identical angle, so the erase
// lands exactly on the pixels the draw put down.
// 240 steps of 1.5 degrees rather than 120 of 3. Two reasons. The trail is
// what reads as a smear - three radii three degrees apart is a nine degree
// wedge, twenty-two pixels wide out at the ring - and halving the step halves
// how much of the dial it covers. And it decouples smoothness from speed: the
// frame interval is the revolution divided by the step count, so a slower sweep
// now means a longer revolution at the same frame rate, instead of the same
// frames arriving half as often and the line visibly jumping.
constexpr uint16_t RADAR_STEPS = 240;                 // 1.5 degrees each
constexpr float RADAR_STEP_DEG = 360.0f / RADAR_STEPS;
constexpr uint8_t RADAR_TRAIL = 2;
// Index 0 is the radius furthest behind the head, so the list runs dark to
// bright and the tail fades out rather than ending in a hard edge.
constexpr uint16_t RADAR_TRAIL_TINT[RADAR_TRAIL] = {0x0100, 0x0280};

// Everything on the dial is set in the UI font, so the callsign, the header and
// the altitude are one family rather than the built-in 6x8 for some of it. Sizes
// are matched to what the built-in font was giving: its large set inks 15 px
// against the old size 2's 14, and its small set 10, which is the altitude.
constexpr uint8_t RADAR_LABEL_SIZE = 2;    // callsigns
// setTextSize only multiplies, so there is no 1.5 to ask for: size 1 is 7 px of
// ink and size 2 is 14. The UI font's small set sits between them at 10, and it
// carries every character the altitude needs, so the second line uses that
// instead of the built-in font.
constexpr int16_t RADAR_ALT_LINE = 17;     // the small set's line height
constexpr uint8_t RADAR_HEADER_SIZE = 2;   // range and count along the top
constexpr int16_t RADAR_LABEL_GAP = 4;     // between the two lines of a label

uint16_t radarSweepStep = 0;
uint32_t radarSweepLastMs = 0;
bool radarSceneDrawn = false;
bool radarWantFetch = true;
bool radarNeedsRepaint = false;

float radarStepDeg(int32_t step) {
    return static_cast<float>(((step % RADAR_STEPS) + RADAR_STEPS) % RADAR_STEPS) * RADAR_STEP_DEG;
}

// A compass bearing, turned into the angle it occupies on screen. Everything
// that comes off the feed is a true bearing; everything drawn is a screen angle,
// and this is the only place the two meet. The sweep and the crosshair are
// screen furniture and stay where they are.
float radarScreenDeg(float bearing) {
    float d = bearing - static_cast<float>(cfg.radarUpDeg);
    while (d < 0.0f) d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d;
}

void radarPolar(float distPx, float brgDeg, int16_t& x, int16_t& y) {
    const float a = brgDeg * PI / 180.0f;
    x = static_cast<int16_t>(RADAR_CX + lroundf(distPx * sinf(a)));
    y = static_cast<int16_t>(RADAR_CY - lroundf(distPx * cosf(a)));
}

int16_t radarScaleR(float base) {
    const int16_t v = static_cast<int16_t>(lroundf(base));
    return v < 2 ? 2 : v;
}

// A rotor disc: the circle the blades sweep, with two of them across it, turned
// to the track. Nothing like the fixed-wing triangle at a glance, which is the
// point - a helicopter over the city is doing something different from an
// airliner on approach.
void radarRotor(int16_t x, int16_t y, float trackDeg, uint16_t color) {
    const int16_t r = radarScaleR(7);
    tft.drawCircle(x, y, r, color);
    const float th = trackDeg * PI / 180.0f;
    for (uint8_t b = 0; b < 2; ++b) {
        const float a = th + (b * PI / 2.0f);
        const int16_t dx = static_cast<int16_t>(lroundf(sinf(a) * r));
        const int16_t dy = static_cast<int16_t>(lroundf(cosf(a) * r));
        tft.drawLine(static_cast<int16_t>(x - dx), static_cast<int16_t>(y + dy),
                     static_cast<int16_t>(x + dx), static_cast<int16_t>(y - dy), color);
    }
    tft.fillCircle(x, y, 2, color);
}

// Filled heading triangle, nose along the track. Local axes are (right, nose),
// mapped to screen so that a track of 0 points up.
void radarPlaneTri(int16_t x, int16_t y, float trackDeg, uint16_t color) {
    const float L = 12.0f, W = 8.0f, B = 7.0f;
    const float th = trackDeg * PI / 180.0f;
    const float ct = cosf(th), st = sinf(th);
    const int16_t nx = static_cast<int16_t>(x + lroundf(L * st));
    const int16_t ny = static_cast<int16_t>(y - lroundf(L * ct));
    const int16_t lx = static_cast<int16_t>(x + lroundf((-W * ct) + (-B * st)));
    const int16_t ly = static_cast<int16_t>(y + lroundf((-W * st) - (-B * ct)));
    const int16_t rx = static_cast<int16_t>(x + lroundf((W * ct) + (-B * st)));
    const int16_t ry = static_cast<int16_t>(y + lroundf((W * st) - (-B * ct)));
    tft.fillTriangle(nx, ny, lx, ly, rx, ry, color);
}

struct RadarLabel {
    int16_t x, y, w, h;
};

bool radarBoxHit(const RadarLabel& a, const RadarLabel& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

// Where the aircraft were before the last fetch replaced them. A repaint has to
// rub out the old markers, and by then radarAc holds the new ones. 24 boxes of
// eight bytes.
// Where the N marker sits: on the ring at true north, wherever that has ended up
// once the dial is turned.
RadarLabel radarNorthBox() {
    int16_t nx, ny;
    radarPolar(static_cast<float>(RADAR_RR - 14), radarScreenDeg(0.0f), nx, ny);
    const int16_t w = measureText("N", RADAR_LABEL_SIZE);
    const int16_t line = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    return {static_cast<int16_t>(nx - (w / 2)), static_cast<int16_t>(ny - (line / 2)), w, line};
}

RadarLabel radarOldHull[RADAR_MAX_AIRCRAFT];
uint8_t radarOldHullCount = 0;

// What the full pass settled on, per aircraft: the label box the ladder chose
// and the hull around everything. The plots are constant between fetches, yet
// the sweep repair was recomputing them for every aircraft on every one of the
// 240 steps a revolution - and a plot now means three PROGMEM table scans and
// the five-rung hull union. The step tests these instead, and rebuilds only
// the aircraft its radius actually crossed, which is one or two a frame.
// Refilled by every full pass, which runs after every fetch, so the cache can
// never describe positions the panel is not showing.
RadarLabel radarBoxCache[RADAR_MAX_AIRCRAFT];
RadarLabel radarHullCache[RADAR_MAX_AIRCRAFT];
uint8_t radarCacheCount = 0;

// The two header strings, where they last landed. The UI font blends instead of
// painting a background, so unlike the old built-in text these do not rub
// themselves out - and going from "10 ac" to "1 ac" left the wider one showing
// through the narrower. Cleared from the recorded extents rather than a fixed
// box, which would bite into the ring behind them.
RadarLabel radarHdrLeft = {0, 0, 0, 0};
RadarLabel radarHdrRight = {0, 0, 0, 0};

// Where an aircraft's marker sits, what its label reads, and the box the pair
// occupies. Drawing and sweep repair both go through this, so the box the sweep
// tests against is by construction the box the drawing used - two copies of this
// arithmetic would drift apart and the sweep would start clipping labels.
struct RadarPlot {
    int16_t x, y;          // marker centre
    bool beyond;           // past the ring, so a rim dot rather than a marker
    // Altitude and destination, either or both may be absent. Sized so that
    // -Wformat-truncation is satisfied rather than merely so that real values
    // fit: the compiler cannot know an altitude is bounded, so it budgets a
    // full long for the kilometres, and 16 bytes covers that plus "km " and
    // the separator. The name after it is Korean, three bytes a syllable.
    char fl[16 + (2 * Airports::MAX_NAME)];
    // The same line with the route cut back, and then with it gone. Kept as
    // whole strings rather than sliced back out of fl, because the names are
    // Korean and cutting UTF-8 by byte count is how half a syllable ends up on
    // the dial.
    char flShort[16 + Airports::MAX_NAME];   // altitude and destination only
    char alt[16];                            // altitude alone
    char dest[Airports::MAX_NAME];           // where it is going
    char leg[2 * Airports::MAX_NAME];        // 아오모리-김포, when both ends are known
    // The line above the callsign: an airline for an airliner, a model for a
    // helicopter. Sized for whichever table has the longer entries.
    char airline[Airlines::MAX_NAME > Rotorcraft::MAX_NAME ? Airlines::MAX_NAME
                                                           : Rotorcraft::MAX_NAME];
    RadarLabel label;      // where the text goes
    RadarLabel hull;       // marker and label together
};

// "12.3km 아오모리-김포" from its parts, and whichever part is present when the
// other is not. Both being absent leaves the line empty, which is how a label
// with no altitude and no route ends up as a bare callsign.
void radarJoin(char* out, size_t size, const char* left, const char* right) {
    if (left[0] != 0 && right[0] != 0) snprintf(out, size, "%s %s", left, right);
    else if (left[0] != 0) strlcpy(out, left, size);
    else if (right[0] != 0) strlcpy(out, right, size);
    else out[0] = 0;
}

// Where a label of exactly these lines would sit. Either extra line may be an
// empty string, and the box shrinks to match - which is what lets a crowded
// dial shed the airline or the destination and try again rather than giving up
// on the whole label. One copy of the arithmetic, so what the sweep repairs is
// what the draw put down.
//
// Measures every line, not just the callsign: "12.3km 프랑크푸르트" is far wider
// than a six-character callsign, and measuring only the first line once let the
// rest escape the collision test and run off the panel.
RadarLabel radarLabelBox(int16_t px, int16_t py, const char* callsign, const char* airline,
                         const char* fl) {
    const bool hasAirline = airline != nullptr && airline[0] != 0;
    const bool hasFl = fl != nullptr && fl[0] != 0;
    const int16_t csW = measureText(callsign, RADAR_LABEL_SIZE);
    const int16_t flW = hasFl ? measureText(fl, 1) : 0;
    const int16_t alW = hasAirline ? measureText(airline, 1) : 0;
    int16_t lw = csW > flW ? csW : flW;
    if (alW > lw) lw = alW;
    const int16_t csH = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    const int16_t lh = static_cast<int16_t>((hasAirline ? (RADAR_ALT_LINE + RADAR_LABEL_GAP) : 0) +
                                            csH + (hasFl ? (RADAR_LABEL_GAP + RADAR_ALT_LINE) : 0));
    int16_t lx = static_cast<int16_t>(px + 9);
    if (lx + lw > SCREEN_W - 2) lx = static_cast<int16_t>(px - 9 - lw);
    if (lx < 2) lx = 2;
    if (lx + lw > SCREEN_W - 2) lx = static_cast<int16_t>(SCREEN_W - 2 - lw);
    // The callsign stays level with the marker; the airline sits above it, so
    // the block grows upward rather than pushing the callsign off the aircraft.
    const int16_t top = static_cast<int16_t>(py - (csH / 2) -
                                             (hasAirline ? (RADAR_ALT_LINE + RADAR_LABEL_GAP) : 0));
    return {lx, top, lw, lh};
}

// The rungs a label steps down when the dial is crowded, richest first: the
// origin goes, then the destination, then the airline, and the callsign is
// what is left. Defined once and read by both the draw, which picks a rung,
// and the hull, which has to cover every rung the draw might pick.
constexpr uint8_t RADAR_LABEL_LEVELS = 5;

void radarLabelLines(const RadarPlot& p, uint8_t level, const char** airline, const char** fl) {
    *airline = level <= 2 ? p.airline : "";
    switch (level) {
        case 0: *fl = p.fl; break;         // altitude, origin and destination
        case 1: *fl = p.flShort; break;    // altitude and destination
        case 2:
        case 3: *fl = p.alt; break;        // altitude alone
        default: *fl = ""; break;          // the callsign on its own
    }
}

RadarPlot radarPlotOf(uint8_t i) {
    const Aircraft& a = radarAc[i];
    const float range = static_cast<float>(cfg.radarRangeKm);
    RadarPlot p{};
    p.beyond = a.distKm > range;
    radarPolar(p.beyond ? static_cast<float>(RADAR_RR) : (a.distKm / range * RADAR_RR),
               radarScreenDeg(a.bearingDeg), p.x, p.y);
    // A helicopter almost never belongs to an airline, so the line that would
    // name one carries its type instead - which the feed gives directly, so
    // there is no table here to be wrong.
    p.airline[0] = 0;
    if (!p.beyond) {
        if (a.rotor) {
            // Named where the model is one a reader would know, and left as the
            // bare ICAO code where it is not - still useful, and never wrong.
            if (!Rotorcraft::nameFor(a.type, p.airline, sizeof(p.airline))) {
                strlcpy(p.airline, a.type, sizeof(p.airline));
            }
        } else {
            Airlines::nameFor(a.callsign, p.airline, sizeof(p.airline));
        }
    }
    p.fl[0] = 0;
    p.alt[0] = 0;
    p.leg[0] = 0;
    p.dest[0] = 0;
    if (!p.beyond) {
        // Altitude and route share a line, the route to its right. Any of it can
        // be absent: an aircraft on the ground reports no altitude, and plenty
        // of callsigns have no route on file.
        const char* fromIata = nullptr;
        const char* toIata = nullptr;
        if (!a.rotor) routeLeg(a.callsign, &fromIata, &toIata);
        // The service answers with three-letter codes, which read as nothing at
        // a glance. Airports in the table get their Korean name; the rest keep
        // the bare code, which is still useful and cannot be wrong. The cache
        // keeps storing the codes, not the names - four bytes against twenty-two,
        // and the table can be re-cut without the cache going stale.
        char orig[Airports::MAX_NAME];
        orig[0] = 0;
        if (fromIata != nullptr && !Airports::nameFor(fromIata, orig, sizeof(orig))) {
            strlcpy(orig, fromIata, sizeof(orig));
        }
        if (toIata != nullptr && !Airports::nameFor(toIata, p.dest, sizeof(p.dest))) {
            strlcpy(p.dest, toIata, sizeof(p.dest));
        }
        // Where it came from and where it is going, read as one leg: 아오모리-김포.
        // Only when both ends are known - a dash with nothing on one side of it
        // says less than the single name would on its own.
        if (orig[0] != 0 && p.dest[0] != 0) {
            snprintf(p.leg, sizeof(p.leg), "%s-%s", orig, p.dest);
        }
        // Tenths of a kilometre, formatted as two integers. The %f conversion is
        // not linked into this build's printf, so asking for one wrote control
        // bytes into the label instead of a number.
        const int32_t tenths = lroundf(static_cast<float>(a.altFt) * 0.003048f);
        if (a.altFt > 0) {
            snprintf(p.alt, sizeof(p.alt), "%ld.%ldkm", static_cast<long>(tenths / 10),
                     static_cast<long>(tenths % 10));
        }
        // Three readings of the same line, richest first, so a crowded dial can
        // step down one at a time instead of losing the line altogether.
        radarJoin(p.fl, sizeof(p.fl), p.alt, p.leg[0] != 0 ? p.leg : p.dest);
        radarJoin(p.flShort, sizeof(p.flShort), p.alt, p.dest);
    }

    p.label = radarLabelBox(p.x, p.y, a.callsign, p.airline, p.fl);

    // The hull has to cover wherever the label may actually land, and that is
    // not simply the full one's box. radarLabelBox starts at px+9 and flips to
    // the left of the marker only when the box would run off the panel, so a
    // wide variant flips where a narrow one stays put - and a label that sheds
    // its route can end up on the opposite side of the aircraft from where the
    // full one would have sat, entirely outside it. Both the erase and the
    // sweep repair work from this box, so neither would have touched that text:
    // it would sit on the dial until the next full redraw. Union of every rung.
    //
    // The marker itself is a triangle about 14 px across whichever way it points.
    int16_t x0 = static_cast<int16_t>(p.x - 14);
    int16_t y0 = static_cast<int16_t>(p.y - 14);
    int16_t x1 = static_cast<int16_t>(p.x + 14);
    int16_t y1 = static_cast<int16_t>(p.y + 14);
    for (uint8_t level = 0; level < RADAR_LABEL_LEVELS; ++level) {
        const char* airline = nullptr;
        const char* fl = nullptr;
        radarLabelLines(p, level, &airline, &fl);
        const RadarLabel b = radarLabelBox(p.x, p.y, a.callsign, airline, fl);
        if (b.x < x0) x0 = b.x;
        if (b.y < y0) y0 = b.y;
        if (b.x + b.w > x1) x1 = static_cast<int16_t>(b.x + b.w);
        if (b.y + b.h > y1) y1 = static_cast<int16_t>(b.y + b.h);
    }
    p.hull = {x0, y0, static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
    return p;
}

// Redraw one aircraft the sweep crossed, using the box the full pass settled
// on. The ladder is not re-run: its verdict IS the cached box, and the level is
// recovered by asking which rung produces that exact box - same arithmetic,
// same answer, no second copy of the decision.
void radarRedrawCrossed(uint8_t i) {
    const Aircraft& a = radarAc[i];
    const RadarPlot p = radarPlotOf(i);

    if (p.beyond) {
        tft.fillCircle(p.x, p.y, radarScaleR(3), a.rotor ? RADAR_C_YELLOW : RADAR_C_RED);
        return;
    }
    if (!isnan(a.gs) && !isnan(a.track)) {
        const float th = a.track * PI / 180.0f;
        const float len = constrain(a.gs * 0.08f, 6.0f, 24.0f);
        tft.drawLine(p.x, p.y, static_cast<int16_t>(p.x + lroundf(sinf(th) * len)),
                     static_cast<int16_t>(p.y - lroundf(cosf(th) * len)), RADAR_C_MAGENTA);
    }
    const uint16_t ink = a.rotor ? RADAR_C_YELLOW : RADAR_C_RED;
    if (a.rotor) radarRotor(p.x, p.y, isnan(a.track) ? 0.0f : a.track, ink);
    else if (!isnan(a.track)) radarPlaneTri(p.x, p.y, a.track, ink);
    else tft.fillCircle(p.x, p.y, radarScaleR(4), ink);

    if (a.callsign[0] == 0 || i >= radarCacheCount) return;
    const RadarLabel want = radarBoxCache[i];
    for (uint8_t v = 0; v < RADAR_LABEL_LEVELS; ++v) {
        const char* airlineLine = nullptr;
        const char* flLine = nullptr;
        radarLabelLines(p, v, &airlineLine, &flLine);
        const RadarLabel b = radarLabelBox(p.x, p.y, a.callsign, airlineLine, flLine);
        if (b.x != want.x || b.y != want.y || b.w != want.w || b.h != want.h) continue;
        int16_t ty = b.y;
        if (airlineLine[0] != 0) {
            drawTextAt(tft, b.x, ty, airlineLine, 1, RADAR_C_GRAY, TFT_BLACK);
            ty = static_cast<int16_t>(ty + RADAR_ALT_LINE + RADAR_LABEL_GAP);
        }
        drawTextAt(tft, b.x, ty, a.callsign, RADAR_LABEL_SIZE, RADAR_C_GRAY, TFT_BLACK);
        if (flLine[0] != 0) {
            const int16_t ay = static_cast<int16_t>(
                ty + UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight + RADAR_LABEL_GAP);
            drawTextAt(tft, b.x, ay, flLine, 1, RADAR_C_GRAY, TFT_BLACK);
        }
        return;
    }
}

// One aircraft, marker and label, and the cache entries the sweep repair will
// read. Only the full pass calls this - the per-step repair goes through
// radarRedrawCrossed, which replays this function's cached verdict instead of
// re-deriving it.
void radarDrawAircraft(uint8_t i, RadarLabel* placed, uint8_t& placedCount) {
    const Aircraft& a = radarAc[i];
    const RadarPlot p = radarPlotOf(i);

    if (p.beyond) {
        if (i < RADAR_MAX_AIRCRAFT) {
            radarHullCache[i] = p.hull;
            radarBoxCache[i] = p.label;
            if (static_cast<uint8_t>(i + 1) > radarCacheCount) radarCacheCount = static_cast<uint8_t>(i + 1);
        }
        tft.fillCircle(p.x, p.y, radarScaleR(3), a.rotor ? RADAR_C_YELLOW : RADAR_C_RED);
        return;
    }

    const int16_t x = p.x;
    const int16_t y = p.y;

    if (!isnan(a.gs) && !isnan(a.track)) {
        const float th = a.track * PI / 180.0f;
        const float len = constrain(a.gs * 0.08f, 6.0f, 24.0f);
        tft.drawLine(x, y, static_cast<int16_t>(x + lroundf(sinf(th) * len)),
                     static_cast<int16_t>(y - lroundf(cosf(th) * len)), RADAR_C_MAGENTA);
    }

    {
        const uint16_t ink = a.rotor ? RADAR_C_YELLOW : RADAR_C_RED;
        if (a.rotor) radarRotor(x, y, isnan(a.track) ? 0.0f : a.track, ink);
        else if (!isnan(a.track)) radarPlaneTri(x, y, a.track, ink);
        else tft.fillCircle(x, y, radarScaleR(4), ink);
    }

    // Even an unlabelled or beyond-the-ring aircraft goes in the cache: its
    // marker still has to be repainted when the sweep crosses it.
    if (i < RADAR_MAX_AIRCRAFT) {
        radarHullCache[i] = p.hull;
        radarBoxCache[i] = p.label;
        if (static_cast<uint8_t>(i + 1) > radarCacheCount) radarCacheCount = static_cast<uint8_t>(i + 1);
    }

    if (a.callsign[0] == 0) return;

    // The label gives up its parts rather than itself. Abandoning the whole
    // thing on the first collision left a dial with seven aircraft showing two
    // callsigns, because these boxes are tall - airline, callsign, altitude and
    // a Korean route stacked - and in a crowd almost everything touches
    // something. So the parts fall away in the order of how little they are
    // missed: the origin first, then the destination, then the airline, and the
    // callsign last of all.
    //
    // If no rung is clear the last one is used anyway, and the bare callsign is
    // drawn where it falls. Overlapping text is untidy, but a radar that hides
    // the one thing worth reading in order to stay tidy is not doing its job,
    // and nothing is corrupted by it - every revolution rubs out the full hulls
    // and lays the whole dial down again.
    const char* airlineLine = nullptr;
    const char* flLine = nullptr;
    radarLabelLines(p, RADAR_LABEL_LEVELS - 1, &airlineLine, &flLine);
    RadarLabel box = radarLabelBox(x, y, a.callsign, airlineLine, flLine);
    for (uint8_t v = 0; v < RADAR_LABEL_LEVELS; ++v) {
        const char* airlineTry = nullptr;
        const char* flTry = nullptr;
        radarLabelLines(p, v, &airlineTry, &flTry);
        const RadarLabel candidate = radarLabelBox(x, y, a.callsign, airlineTry, flTry);
        bool hit = false;
        for (uint8_t j = 0; j < placedCount && !hit; ++j) hit = radarBoxHit(candidate, placed[j]);
        if (!hit) {
            box = candidate;
            airlineLine = airlineTry;
            flLine = flTry;
            break;
        }
    }

    if (placedCount < RADAR_MAX_AIRCRAFT) placed[placedCount++] = box;
    if (i < RADAR_MAX_AIRCRAFT) radarBoxCache[i] = box;

    int16_t ty = box.y;
    if (airlineLine[0] != 0) {
        drawTextAt(tft, box.x, ty, airlineLine, 1, RADAR_C_GRAY, TFT_BLACK);
        ty = static_cast<int16_t>(ty + RADAR_ALT_LINE + RADAR_LABEL_GAP);
    }
    drawTextAt(tft, box.x, ty, a.callsign, RADAR_LABEL_SIZE, RADAR_C_GRAY, TFT_BLACK);
    if (flLine[0] != 0) {
        // No clear first: the sweep passes this text several times a revolution
        // and blanking it each time is what made it blink. Drawing the same
        // glyphs over themselves is harmless, because the reading only changes
        // on a fetch and a fetch always rubs the whole label out beforehand.
        const int16_t ay = static_cast<int16_t>(
            ty + UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight + RADAR_LABEL_GAP);
        drawTextAt(tft, box.x, ay, flLine, 1, RADAR_C_GRAY, TFT_BLACK);
    }
}

void radarDrawHome() {
    tft.fillCircle(RADAR_CX, RADAR_CY, radarScaleR(4), RADAR_C_GREEN);
}

void radarDrawRings() {
    tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_RR, RADAR_C_DGRAY);
    tft.drawCircle(RADAR_CX, RADAR_CY, RADAR_RR / 2, RADAR_C_DGRAY);
    tft.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_RR, 2 * RADAR_RR, RADAR_C_DGRAY);
    tft.drawFastHLine(RADAR_CX - RADAR_RR, RADAR_CY, 2 * RADAR_RR, RADAR_C_DGRAY);
    const RadarLabel n = radarNorthBox();
    drawTextAt(tft, n.x, n.y, "N", RADAR_LABEL_SIZE, RADAR_C_GRAY, TFT_BLACK);
}

void radarDrawOverlays() {
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%d km", cfg.radarRangeKm);
    const int16_t line = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    const int16_t hdrW = measureText(hdr, RADAR_HEADER_SIZE);
    drawTextAt(tft, 3, 1, hdr, RADAR_HEADER_SIZE, RADAR_C_GRAY, TFT_BLACK);
    radarHdrLeft = {3, 1, hdrW, line};

    char cnt[10];
    snprintf(cnt, sizeof(cnt), "%d ac", radarAcCount);
    const int16_t cntW = measureText(cnt, RADAR_HEADER_SIZE);
    const int16_t cntX = static_cast<int16_t>(SCREEN_W - cntW - 3);
    drawTextAt(tft, cntX, 1, cnt, RADAR_HEADER_SIZE, RADAR_C_GRAY, TFT_BLACK);
    radarHdrRight = {cntX, 1, cntW, line};

    tft.fillCircle(6, SCREEN_H - 7, 4, radarErrorFlag ? RADAR_C_RED : TFT_BLACK);
}

void radarDrawContents() {
    // Before the rings, so whatever the clear takes out of them is put straight
    // back rather than left as a notch.
    for (const RadarLabel* b : {&radarHdrLeft, &radarHdrRight}) {
        if (b->w > 0) tft.fillRect(b->x, b->y, b->w, b->h, TFT_BLACK);
    }
    radarDrawRings();
    radarCacheCount = 0;
    RadarLabel placed[RADAR_MAX_AIRCRAFT];
    uint8_t placedCount = 0;
    for (uint8_t i = 0; i < radarAcCount; ++i) {
        radarDrawAircraft(i, placed, placedCount);
        wdtYield();
    }
    radarDrawHome();
    radarDrawOverlays();
}

// The first draw, and only that: clearing the whole panel is right when there
// is nothing on it worth keeping.
void radarDrawScene() {
    tft.fillScreen(TFT_BLACK);
    radarDrawContents();
    radarSceneDrawn = true;
}

// Every revolution after the first. Clearing the panel first is what made the
// screen blink at twelve o'clock once a turn, so instead the few things that
// actually change are rubbed out: the radii the sweep has lit, and the boxes
// the previous aircraft occupied. The rings are thin enough to simply redraw.
void radarRepaint() {
    const uint32_t t0 = millis();
    for (uint8_t t = 0; t <= RADAR_TRAIL; ++t) {
        int16_t ex, ey;
        radarPolar(static_cast<float>(RADAR_RR),
                   radarStepDeg(static_cast<int32_t>(radarSweepStep) - t), ex, ey);
        tft.drawLine(RADAR_CX, RADAR_CY, ex, ey, TFT_BLACK);
    }
    for (uint8_t i = 0; i < radarOldHullCount; ++i) {
        const RadarLabel& b = radarOldHull[i];
        tft.fillRect(b.x, b.y, b.w, b.h, TFT_BLACK);
    }
    wdtYield();
    const uint32_t t1 = millis();
    radarDrawContents();
    radarContentsMs = millis() - t1;
    radarRepaintMs = millis() - t0;
}

// Does the segment from the centre out to (ex, ey) touch this box? Liang-
// Barsky, which answers it with four divisions and no square roots.
bool radarSegHitsBox(int16_t ex, int16_t ey, const RadarLabel& b) {
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float dx = static_cast<float>(ex - RADAR_CX);
    const float dy = static_cast<float>(ey - RADAR_CY);
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {static_cast<float>(RADAR_CX - b.x),
                        static_cast<float>((b.x + b.w) - RADAR_CX),
                        static_cast<float>(RADAR_CY - b.y),
                        static_cast<float>((b.y + b.h) - RADAR_CY)};
    for (uint8_t i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;   // parallel to this edge and outside it
            continue;
        }
        const float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    return true;
}

// Erases one radius and puts back what it crossed. Cheaper than it looks: the
// rings meet a radius in two pixels, the crosshair only near the cardinals, and
// an aircraft only when the radius genuinely runs through its marker or label.
void radarRepairRadius(float deg) {
    int16_t ex, ey;
    radarPolar(static_cast<float>(RADAR_RR), deg, ex, ey);
    tft.drawLine(RADAR_CX, RADAR_CY, ex, ey, TFT_BLACK);

    // Rings. A circle and a radius can share several pixels where they cross,
    // so the mend covers a short arc either side rather than the one or two
    // points the crossing nominally occupies - otherwise the sweep chews a
    // notch out of each ring as it goes by.
    for (int16_t r : {static_cast<int16_t>(RADAR_RR), static_cast<int16_t>(RADAR_RR / 2)}) {
        const float arc = 360.0f / (2.0f * PI * static_cast<float>(r));  // one pixel, in degrees
        for (int8_t k = -2; k <= 2; ++k) {
            int16_t x, y;
            radarPolar(static_cast<float>(r), deg + (arc * k), x, y);
            tft.drawPixel(x, y, RADAR_C_DGRAY);
        }
    }

    // crosshair, only where the radius actually lies on it
    const float m = fmodf(deg + 360.0f, 90.0f);
    if (m < 4.0f || m > 86.0f) {
        tft.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_RR, 2 * RADAR_RR, RADAR_C_DGRAY);
        tft.drawFastHLine(RADAR_CX - RADAR_RR, RADAR_CY, 2 * RADAR_RR, RADAR_C_DGRAY);
    }
    // North is no longer at the top, so whether the radius went through it is a
    // question about where it actually is.
    const RadarLabel north = radarNorthBox();
    if (radarSegHitsBox(ex, ey, north)) {
        drawTextAt(tft, north.x, north.y, "N", RADAR_LABEL_SIZE, RADAR_C_GRAY, TFT_BLACK);
    }

    // Anything the radius genuinely runs through, tested against the hull the
    // full pass cached. A bearing window was the wrong question - a label near
    // the centre spans a wide angle while occupying very little of the dial -
    // and a symmetric worst-case hull was worse still, some 220 px wide, which
    // caught nearly everything. This picks out one or two a frame, and only
    // those get their plot rebuilt.
    for (uint8_t i = 0; i < radarAcCount && i < radarCacheCount; ++i) {
        if (radarSegHitsBox(ex, ey, radarHullCache[i])) radarRedrawCrossed(i);
    }
    radarDrawHome();
}

void radarDrawSweep() {
    for (uint8_t t = RADAR_TRAIL; t > 0; --t) {
        int16_t ex, ey;
        radarPolar(static_cast<float>(RADAR_RR),
                   radarStepDeg(static_cast<int32_t>(radarSweepStep) - t), ex, ey);
        // t counts backwards from the head, so the further behind a radius is
        // the darker it gets. Indexing the other way round put the brightest
        // part of the tail at its far end, which reads as a smear rather than
        // a fade.
        tft.drawLine(RADAR_CX, RADAR_CY, ex, ey, RADAR_TRAIL_TINT[RADAR_TRAIL - t]);
    }
    int16_t ex, ey;
    radarPolar(static_cast<float>(RADAR_RR), radarStepDeg(radarSweepStep), ex, ey);
    tft.drawLine(RADAR_CX, RADAR_CY, ex, ey, RADAR_C_GREEN);
    radarDrawHome();
}

void drawRadar(bool force) {
    if (force) {
        // TLS needs a big contiguous block and the band sprite is the biggest
        // thing standing in its way, so it goes back before the first fetch.
        analogBandEnd();
        radarSceneDrawn = false;
        radarNeedsRepaint = false;
        radarOldHullCount = 0;
        radarSweepLastMs = 0;
        radarWantFetch = true;
    }

    if (cfg.radarLat == 0.0f && cfg.radarLon == 0.0f) {
        if (!radarSceneDrawn) {
            tft.fillScreen(TFT_BLACK);
            drawCenteredText(104, F("Plane radar"), 2, RADAR_C_YELLOW, TFT_BLACK, 0, SCREEN_W);
            drawCenteredText(132, F("set home location"), 2, RADAR_C_GRAY, TFT_BLACK, 0, SCREEN_W);
            radarSceneDrawn = true;
        }
        return;
    }

    const uint32_t now = millis();
    if (!radarSceneDrawn) {
        radarDrawScene();
        radarNeedsRepaint = false;
        radarSweepStep = 0;
        radarSweepLastMs = now;
        return;
    }
    if (radarNeedsRepaint) {
        radarRepaint();
        radarNeedsRepaint = false;
        radarSweepStep = 0;
        radarSweepLastMs = now;
        return;
    }

    // One revolution per poll, so the sweep arrives back at north just as the
    // next set of positions does.
    const uint32_t periodMs = static_cast<uint32_t>(cfg.radarPollSec) * 1000UL;
    const uint32_t frameMs = periodMs / RADAR_STEPS;
    if (now - radarSweepLastMs < frameMs) return;
    radarSweepLastMs = now;

    // Advance first, then erase, because which radius has just fallen off the
    // trail is only known once the head has moved.
    ++radarSweepStep;
    if (radarSweepStep >= RADAR_STEPS) {
        radarSweepStep = 0;
        // A revolution is finished, so this is when the next positions are asked
        // for. Tying the two together is what a real set does, and it keeps the
        // pause the fetch causes at north rather than at a drifting bearing.
        radarWantFetch = true;
    }
    radarRepairRadius(radarStepDeg(static_cast<int32_t>(radarSweepStep) - (RADAR_TRAIL + 1)));
    radarDrawSweep();
}

// Polls on its own clock, and only while the radar is the screen being shown:
// a TLS handshake is the most heap-hungry thing this firmware does and there is
// no reason to run it for a screen nobody is looking at.
void radarService() {
    if (activeScreen != SCREEN_RADAR) return;
    if (cfg.radarLat == 0.0f && cfg.radarLon == 0.0f) return;
    if (!radarWantFetch) return;
    // The feed refuses more than one request a second and asks for restraint
    // beyond that, so the revolution can ask but the clock still decides.
    const uint32_t periodMs = static_cast<uint32_t>(cfg.radarPollSec) * 1000UL;
    if (radarLastTryMs != 0 && millis() - radarLastTryMs < periodMs) return;
    radarWantFetch = false;
    // The fetch blocks for the best part of a second and there is no second core
    // to hide it on. A line frozen mid-dial reads as a fault; no line reads as
    // the gap between one sweep and the next, which is what a real set shows
    // anyway. Driving the sweep off a clock and letting it hurry to catch up was
    // tried instead, and the hurrying read as stuttering.
    for (uint8_t t = 0; t <= RADAR_TRAIL; ++t) {
        radarRepairRadius(radarStepDeg(static_cast<int32_t>(radarSweepStep) - t));
    }
    // Snapshot where the current aircraft sit before the reply overwrites them;
    // the repaint needs those boxes to rub the old markers out.
    radarOldHullCount = 0;
    for (uint8_t i = 0; i < radarAcCount && radarOldHullCount < RADAR_MAX_AIRCRAFT; ++i) {
        radarOldHull[radarOldHullCount++] = radarPlotOf(i).hull;
    }
    radarFetch();
    // After the positions, not before: which callsigns are in range is exactly
    // what the reply just told us.
    routeService();
    radarNeedsRepaint = true;   // new positions, drawn over the old without a blink
}

void drawActiveScreen(bool force) {
    if (activeScreen == SCREEN_RADAR) {
        drawRadar(force);
    } else if (activeScreen == SCREEN_ALBUM) {
        drawAlbum(force);
    } else if (activeScreen == SCREEN_WEATHER_DIGITAL || activeScreen == SCREEN_DATE_DIGITAL) {
        drawMinuteFace(force);
    } else if (activeScreen == SCREEN_DIGITAL) {
        drawDigital(force);
    } else if (activeScreen != SCREEN_CLOCK_WEATHER) {
        drawAnalog(force);
    } else {
        analogBandEnd();  // give the band memory back while another screen owns the panel
        drawDashboard(force);
    }
}

void updateDisplay(bool force = false) {
    uint32_t now = millis();

    if (enabledScreenCount() > 1) {
        const uint32_t intervalMs = static_cast<uint32_t>(cfg.themeIntervalSeconds) * 1000UL;
        if (now - lastScreenSwitchMs >= intervalMs) {
            activeScreen = nextEnabledScreen(activeScreen);
            lastScreenSwitchMs = now;
            screenChromeDrawn = false;
            analogChromeDrawn = false;
            digitalLastStamp = -1;
            resetDisplayCache();
            force = true;
        }
    }

    // Ahead of the one-second gate: the colon has to flip twice a second, and
    // this repaints two dots rather than a screen.
    minuteFaceColonTick(false);
    if (activeScreen == SCREEN_RADAR) {
        // force has to survive: it is what hands the band sprite back, and a TLS
        // handshake needs those 11.5 KB. The sweep is what bypasses the
        // one-second gate, not the flag.
        radarService();
        drawRadar(force);
        if (force) lastDisplayMs = now;
        return;
    }

    if (!force && now - lastDisplayMs < DISPLAY_INTERVAL_MS) return;
    lastDisplayMs = now;
    drawActiveScreen(force);
}

bool readRawLine(WiFiClient& client, String& line, uint32_t timeoutMs = 10000) {
    const uint32_t start = millis();
    line = "";
    while (millis() - start < timeoutMs) {
        while (client.available() > 0) {
            const char c = static_cast<char>(client.read());
            if (c == '\n') {
                line.trim();
                return true;
            }
            if (c != '\r') line += c;
        }
        wdtYield();
        delay(1);
    }
    return false;
}

void rawReply(WiFiClient& client, int code, const char* text, const String& body) {
    client.print(F("HTTP/1.0 "));
    client.print(code);
    client.print(' ');
    client.println(text);
    client.println(F("Connection: close"));
    client.println(F("Content-Type: text/plain"));
    client.print(F("Content-Length: "));
    client.println(body.length());
    client.println();
    client.print(body);
}

void rawUpdateFromClient(WiFiClient& client, int mode, size_t contentLength) {
    const size_t capacity =
        mode == U_FS ? static_cast<size_t>(FS_PHYS_SIZE) : ((ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U);
    if (mode == U_FS) {
        LittleFS.end();
        fsMounted = false;
        delay(50);
    }
    if (!Update.begin(capacity, mode)) {
        rawReply(client, 500, "Begin Failed", String("begin failed ") + Update.getErrorString() + "\n");
        return;
    }
    uint8_t buf[STREAM_BUF_SIZE];
    size_t total = 0;
    uint32_t lastRead = millis();
    bool failed = false;
    String error;
    while (total < contentLength) {
        wdtYield();
        int available = client.available();
        if (available <= 0) {
            if (millis() - lastRead > BODY_TIMEOUT_MS) {
                failed = true;
                error = "timeout";
                break;
            }
            delay(1);
            continue;
        }
        const size_t want = min(static_cast<size_t>(available), min(sizeof(buf), contentLength - total));
        const int got = client.read(buf, want);
        if (got <= 0) {
            failed = true;
            error = "read failed";
            break;
        }
        lastRead = millis();
        if (Update.write(buf, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
            failed = true;
            error = Update.getErrorString();
            break;
        }
        total += got;
    }
    if (!failed && !Update.end(true)) {
        failed = true;
        error = Update.getErrorString();
    }
    if (failed) {
        Update.end();
        rawReply(client, 500, "Update Failed", String("failed ") + total + " " + error + "\n");
        return;
    }
    rawReply(client, 200, "OK", String("ok ") + total + "\n");
    delay(800);
    ESP.restart();
}

void handleRawServerClient() {
    WiFiClient client = rawServer.accept();
    if (!client) return;
    client.setTimeout(10000);
    String line;
    if (!readRawLine(client, line)) {
        client.stop();
        return;
    }
    const bool rawFw = line.startsWith(F("POST /rawfw "));
    const bool rawFs = line.startsWith(F("POST /rawfs "));
    const bool status = line.startsWith(F("GET / ")) || line.startsWith(F("GET /status "));
    size_t contentLength = 0;
    while (readRawLine(client, line)) {
        if (line.length() == 0) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith(F("content-length:"))) contentLength = line.substring(line.indexOf(':') + 1).toInt();
    }
    if (status) rawReply(client, 200, "OK", String(F("SDP clock weather raw recovery\nPOST /rawfw or /rawfs\nlast=")) + lastStatus + "\n");
    else if (rawFw || rawFs) rawUpdateFromClient(client, rawFs ? U_FS : U_FLASH, contentLength);
    else rawReply(client, 404, "Not Found", F("POST /rawfw or /rawfs only\n"));
    client.stop();
}

// ---------------------------------------------------------------------------
// Web UI password
//
// This gates the web menu only: the served page and its assets. The APIs, OTA,
// /file and the raw port 8080 server stay open, so existing tooling and the
// recovery paths keep working exactly as they did before the login existed.
// Anyone who can reach the device can still drive it over HTTP; the password
// keeps the menu itself from being casually opened, nothing more.
//
// The token is regenerated on every boot, so a reboot signs everyone out.
// ---------------------------------------------------------------------------

void makeAuthToken() {
    authToken = "";
    for (uint8_t i = 0; i < 4; ++i) {
        char part[9];
        snprintf(part, sizeof(part), "%08x", static_cast<unsigned>(RANDOM_REG32));
        authToken += part;
    }
}

// True while the boot grace window is open.
//
// This is a latch, not a comparison, and the difference matters. millis() wraps
// to zero about every 49 days, and a bare `millis() < AUTH_GRACE_MS` would let
// the window swing open again for two minutes every time it did - on a device
// that had been sitting there for weeks, with nobody watching. The latch is
// closed once by loop() and never reopens without a real boot.
bool authGraceOpen = true;

bool authGraceActive() {
    return authGraceOpen;
}

bool hasValidSession() {
    if (authToken.length() == 0) return false;
    const String cookies = server.header(F("Cookie"));
    if (cookies.length() == 0) return false;
    const String needle = String(AUTH_COOKIE) + "=";
    int at = cookies.indexOf(needle);
    while (at >= 0) {
        // must start the header or follow a "; " separator, otherwise this is
        // a different cookie whose name merely ends with ours
        const bool boundary = (at == 0) || (cookies.charAt(at - 1) == ' ') || (cookies.charAt(at - 1) == ';');
        if (boundary) {
            const int from = at + needle.length();
            int end = cookies.indexOf(';', from);
            if (end < 0) end = cookies.length();
            return cookies.substring(from, end) == authToken;
        }
        at = cookies.indexOf(needle, at + 1);
    }
    return false;
}

void sendLoginPage(bool failed) {
    String body = F("<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Sign in</title><style>"
                    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
                    "background:#0d0f12;color:#e8edf3;font-family:system-ui,-apple-system,sans-serif}"
                    "form{background:#161a20;border:1px solid #262c35;border-radius:10px;padding:28px;width:min(92vw,320px)}"
                    "h1{font-size:17px;margin:0 0 18px;font-weight:600}"
                    "input{width:100%;box-sizing:border-box;padding:11px;border-radius:6px;border:1px solid #333a44;"
                    "background:#0f1216;color:#e8edf3;font-size:15px}"
                    "button{width:100%;margin-top:14px;padding:11px;border:0;border-radius:6px;"
                    "background:#2f81f7;color:#fff;font-size:15px;font-weight:600;cursor:pointer}"
                    "p.err{color:#f8836b;font-size:13px;margin:12px 0 0}"
                    "</style></head><body><form method='post' action='/login'>"
                    "<h1>ESP &#45796;&#47785;&#51201; &#47784;&#45768;&#53552;&#47553; &#51109;&#52824;</h1>"
                    "<input type='password' name='password' placeholder='Password' autofocus>"
                    "<button type='submit'>Sign in</button>");
    if (failed) body += F("<p class='err'>Wrong password.</p>");
    body += F("</form></body></html>");
    server.send(failed ? 401 : 200, F("text/html"), body);
}

// Returns true when the request may proceed.
bool requireAuth(bool htmlClient) {
    if (hasValidSession() || authGraceActive()) return true;
    if (htmlClient) {
        server.sendHeader(F("Location"), F("/login"));
        server.send(302, F("text/plain"), F("login required\n"));
    } else {
        server.send(401, F("application/json"), F("{\"error\":\"login required\"}"));
    }
    return false;
}

void handleLoginGet() {
    if (hasValidSession() || authGraceActive()) {
        server.sendHeader(F("Location"), F("/"));
        server.send(302, F("text/plain"), F("ok\n"));
        return;
    }
    sendLoginPage(false);
}

void handleLoginPost() {
    if (server.arg("password") != cfg.webPassword) {
        sendLoginPage(true);
        return;
    }
    if (authToken.length() == 0) makeAuthToken();
    server.sendHeader(F("Set-Cookie"), String(AUTH_COOKIE) + "=" + authToken + F("; Path=/; Max-Age=86400; SameSite=Lax"));
    server.sendHeader(F("Location"), F("/"));
    server.send(302, F("text/plain"), F("ok\n"));
}

// Changing the password is the one part of the web API that is not wide open.
// Two ways in: an established session that also knows the current password, or
// the boot grace window, which is there precisely for the case where nobody
// knows it any more. Everything else - OTA, /file, the raw port - is untouched,
// so the recovery paths still work the way they always have.
void handlePasswordPost() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendText(400, F("invalid json\n"));
        return;
    }
    const String next = doc["new"] | String();
    if (next.length() == 0 || next.length() > AUTH_PASSWORD_MAX) {
        sendText(400, F("password must be 1 to 32 characters\n"));
        return;
    }
    if (!authGraceActive()) {
        if (!hasValidSession()) {
            sendText(401, F("login required\n"));
            return;
        }
        const String current = doc["current"] | String();
        if (current != cfg.webPassword) {
            sendText(403, F("current password does not match\n"));
            return;
        }
    }
    const String previous = cfg.webPassword;
    cfg.webPassword = next;
    if (!saveConfig()) {
        // Reporting success here would be the worst kind of lie: the caller
        // walks away using a password that dies with the next reboot.
        cfg.webPassword = previous;
        sendText(500, F("could not write the config, password unchanged\n"));
        return;
    }
    // Every session issued under the old password stops working. The browser
    // that just made the change gets the replacement cookie in this response,
    // so it stays signed in and nobody else does.
    makeAuthToken();
    server.sendHeader(F("Set-Cookie"), String(AUTH_COOKIE) + "=" + authToken + F("; Path=/; Max-Age=86400; SameSite=Lax"));
    lastStatus = "web password changed";
    sendText(200, F("ok\n"));
}

void handleLogout() {
    server.sendHeader(F("Set-Cookie"), String(AUTH_COOKIE) + F("=; Path=/; Max-Age=0"));
    server.sendHeader(F("Location"), F("/login"));
    server.send(302, F("text/plain"), F("bye\n"));
}

void handleStatus() {
    JsonDocument doc;
    doc["name"] = FW_NAME;
    doc["version"] = FW_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["ap_ip"] = WiFi.softAPIP().toString();
    doc["fs_mounted"] = fsMounted;
    doc["last"] = lastStatus;
    doc["weather_status"] = weather.status;
    doc["night_mode_active"] = isNightModeActive();
    doc["effective_brightness"] = effectiveBrightness();
    // largest contiguous block matters more than the total: a sprite needs one
    doc["free_heap"] = ESP.getFreeHeap();
    doc["max_free_block"] = ESP.getMaxFreeBlockSize();
    doc["heap_frag_pct"] = ESP.getHeapFragmentation();
    doc["analog_frame_us"] = analogFrameUs;
    doc["analog_push_us"] = analogPushUs;
    doc["analog_bands"] = analogBandsPushed;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

void handleConfigGet() {
    JsonDocument doc;
    doc["name"] = FW_NAME;
    doc["version"] = FW_VERSION;
    doc["ssid"] = cfg.ssid;
    doc["location"] = cfg.location;
    doc["kma_key"] = cfg.kmaKey;
    doc["kma_key_set"] = cfg.kmaKey.length() > 0;
    doc["web_password_is_default"] = cfg.webPassword == AUTH_DEFAULT_PASSWORD;
    doc["nx"] = cfg.nx;
    doc["ny"] = cfg.ny;
    doc["timezone_offset_minutes"] = cfg.timezoneOffsetMinutes;
    doc["weather_enabled"] = cfg.weatherEnabled;
    doc["clock_24h"] = cfg.clock24h;
    doc["brightness"] = cfg.brightness;
    doc["night_mode_enabled"] = cfg.nightModeEnabled;
    doc["night_brightness"] = cfg.nightBrightness;
    doc["night_start_minutes"] = cfg.nightStartMinutes;
    doc["night_stop_minutes"] = cfg.nightStopMinutes;
    doc["night_mode_active"] = isNightModeActive();
    doc["effective_brightness"] = effectiveBrightness();
    doc["screens"] = cfg.screens;
    doc["album_interval_seconds"] = cfg.albumIntervalSeconds;
    doc["radar_lat"] = cfg.radarLat;
    doc["radar_lon"] = cfg.radarLon;
    doc["radar_range_km"] = cfg.radarRangeKm;
    doc["radar_poll_sec"] = cfg.radarPollSec;
    doc["radar_min_alt_ft"] = cfg.radarMinAltFt;
    doc["radar_up_deg"] = cfg.radarUpDeg;
    doc["radar_routes"] = cfg.radarRoutes;
    emitRadarPresets(doc);
    {
        JsonArray order = doc["screen_order"].to<JsonArray>();
        for (uint8_t i = 0; i < SCREEN_COUNT; ++i) order.add(cfg.screenOrder[i]);
    }
    doc["theme_interval_seconds"] = cfg.themeIntervalSeconds;
    doc["screen_count"] = static_cast<int>(SCREEN_COUNT);
    doc["active_screen"] = activeScreen;
    doc["analog_face_count"] = static_cast<int>(ANALOG_FACE_COUNT);
    JsonArray outFaces = doc["analog_faces"].to<JsonArray>();
    for (uint8_t i = 0; i < ANALOG_FACE_COUNT; ++i) {
        JsonObject face = outFaces.add<JsonObject>();
        face["dial"] = cfg.analogFaces[i].dialRgb;
        face["case"] = cfg.analogFaces[i].caseRgb;
        face["lume"] = cfg.analogFaces[i].lumeRgb;
        face["hand"] = cfg.analogFaces[i].handRgb;
        face["accent"] = cfg.analogFaces[i].accentRgb;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

void handleConfigPost() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendText(400, F("invalid json\n"));
        return;
    }
    cfg.ssid = doc["ssid"] | cfg.ssid;
    cfg.pass = doc["pass"] | cfg.pass;
    cfg.location = doc["location"] | cfg.location;
    cfg.kmaKey = doc["kma_key"] | cfg.kmaKey;
    cfg.nx = doc["nx"] | cfg.nx;
    cfg.ny = doc["ny"] | cfg.ny;
    cfg.timezoneOffsetMinutes = doc["timezone_offset_minutes"] | cfg.timezoneOffsetMinutes;
    cfg.weatherEnabled = doc["weather_enabled"] | cfg.weatherEnabled;
    cfg.clock24h = doc["clock_24h"] | cfg.clock24h;
    cfg.brightness = doc["brightness"] | cfg.brightness;
    cfg.nightModeEnabled = doc["night_mode_enabled"] | cfg.nightModeEnabled;
    cfg.nightBrightness = doc["night_brightness"] | cfg.nightBrightness;
    cfg.nightStartMinutes = doc["night_start_minutes"] | cfg.nightStartMinutes;
    cfg.nightStopMinutes = doc["night_stop_minutes"] | cfg.nightStopMinutes;

    bool screensChanged = doc["screens"].is<unsigned int>();
    if (screensChanged) {
        cfg.screens = static_cast<uint16_t>(doc["screens"].as<unsigned int>()) & SCREEN_MASK_ALL;
        if (cfg.screens == 0) cfg.screens = 1U << SCREEN_CLOCK_WEATHER;
    }
    // A reorder changes the rotation just as a tick does, so it takes the same
    // path: the active screen is re-seated and the switch clock restarted.
    if (doc["screen_order"].is<JsonArray>()) {
        uint8_t wanted[SCREEN_COUNT];
        size_t n = 0;
        for (JsonVariant v : doc["screen_order"].as<JsonArray>()) {
            if (n >= SCREEN_COUNT) break;
            wanted[n++] = static_cast<uint8_t>(v.as<unsigned int>());
        }
        setScreenOrder(wanted, n);
        screensChanged = true;
    }
    if (doc["theme_interval_seconds"].is<unsigned int>()) {
        cfg.themeIntervalSeconds = constrain(static_cast<uint16_t>(doc["theme_interval_seconds"].as<unsigned int>()),
                                             THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    }
    if (doc["radar_lat"].is<float>() || doc["radar_lat"].is<int>()) cfg.radarLat = doc["radar_lat"].as<float>();
    if (doc["radar_lon"].is<float>() || doc["radar_lon"].is<int>()) cfg.radarLon = doc["radar_lon"].as<float>();
    if (doc["radar_range_km"].is<unsigned int>()) {
        cfg.radarRangeKm = constrain(static_cast<uint16_t>(doc["radar_range_km"].as<unsigned int>()), 2, 400);
    }
    if (doc["radar_poll_sec"].is<unsigned int>()) {
        cfg.radarPollSec = constrain(static_cast<uint16_t>(doc["radar_poll_sec"].as<unsigned int>()), 5, 600);
    }
    if (doc["radar_min_alt_ft"].is<unsigned int>()) {
        cfg.radarMinAltFt = static_cast<uint16_t>(doc["radar_min_alt_ft"].as<unsigned int>());
    }
    if (doc["radar_up_deg"].is<unsigned int>()) {
        cfg.radarUpDeg = static_cast<uint16_t>(doc["radar_up_deg"].as<unsigned int>()) % 360;
    }
    if (doc["radar_routes"].is<bool>()) cfg.radarRoutes = doc["radar_routes"].as<bool>();
    loadRadarPresets(doc["radar_presets"]);

    bool coloursChanged = false;
    JsonArrayConst postedFaces = doc["analog_faces"];
    if (!postedFaces.isNull()) {
        uint8_t i = 0;
        for (JsonObjectConst posted : postedFaces) {
            if (i >= ANALOG_FACE_MAX) break;
            AnalogFace& face = cfg.analogFaces[i];
            const struct {
                const char* key;
                uint32_t* target;
            } fields[] = {
                {"dial", &face.dialRgb},
                {"case", &face.caseRgb},
                {"lume", &face.lumeRgb},
                {"hand", &face.handRgb},
                {"accent", &face.accentRgb},
            };
            for (const auto& field : fields) {
                if (!posted[field.key].is<unsigned int>()) continue;
                const uint32_t value = posted[field.key].as<unsigned int>() & 0xFFFFFFU;
                if (*field.target != value) {
                    *field.target = value;
                    coloursChanged = true;
                }
            }
            ++i;
        }
    }

    bool ok = saveConfig();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com");
    applyBrightness();
    // Only dirty bands are repainted, so a colour change needs the whole face
    // redrawn or the old colour survives everywhere the second hand has not been.
    if (coloursChanged) analogChromeDrawn = false;
    if (screensChanged) applyScreenSelection();
    refreshWeather();
    updateDisplay(true);
    sendText(ok ? 200 : 500, ok ? "ok\n" : "save failed\n");
}

void handleWeatherStatus() {
    JsonDocument doc;
    doc["valid"] = weather.valid;
    doc["fetching"] = weather.fetching;
    doc["status"] = weather.status;
    doc["temp"] = weather.temp;
    doc["humidity"] = weather.humidity;
    doc["rain"] = weather.rain;
    doc["updated"] = weather.updated;
    // Cloud cover and precipitation type, which decide every weather icon on
    // the device. Exposed because a wrong icon is otherwise impossible to
    // diagnose without standing in front of the screen.
    doc["sky"] = weather.sky;
    doc["pty"] = weather.pty;
    JsonArray arr = doc["forecast"].to<JsonArray>();
    for (auto& f : weather.fcst) {
        JsonObject o = arr.add<JsonObject>();
        o["valid"] = f.valid;
        o["hour"] = f.hour;
        o["temp"] = f.temp;
        o["humidity"] = f.humidity;
        o["rain"] = f.rain;
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

String contentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    return "text/plain";
}

void handleRoot() {
    if (!requireAuth(true)) return;
    if (fsMounted && LittleFS.exists("/web/index.html")) {
        File f = LittleFS.open("/web/index.html", "r");
        server.streamFile(f, "text/html");
        f.close();
        return;
    }
    String body = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>SDP Clock Weather</title></head><body><h1>SDP Clock Weather</h1><p>Fallback UI. Upload LittleFS for full UI.</p><p><a href='/status'>status</a> <a href='/weather/status'>weather</a> <a href='/fs/list'>fs</a></p><form method='post' action='/update_ota' enctype='multipart/form-data'><input type='file' name='update'><button>Firmware</button></form><form method='post' action='/api/ota/fs' enctype='multipart/form-data'><input type='file' name='fs'><button>LittleFS</button></form></body></html>");
    server.send(200, "text/html", body);
}

void handleStatic() {
    if (!requireAuth(true)) return;
    String path = server.uri();
    if (path == "/") path = "/web/index.html";
    else path = "/web" + path;
    if (fsMounted && LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        server.streamFile(f, contentType(path));
        f.close();
        return;
    }
    sendText(404, String("not found: ") + server.uri() + "\n");
}

void handleFsList() {
    if (!fsMounted && !LittleFS.begin()) {
        sendText(500, F("LittleFS mount failed\n"));
        return;
    }
    fsMounted = true;
    String body;
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) body += dir.fileName() + "\t" + String(dir.fileSize()) + "\n";
    sendText(200, body.length() ? body : String(F("(empty)\n")));
}

void handleFormat() {
    LittleFS.end();
    fsMounted = false;
    bool ok = LittleFS.format();
    fsMounted = ok && LittleFS.begin();
    sendText(ok ? 200 : 500, ok ? "format ok\n" : "format failed\n");
}

void handleRestart() {
    sendText(200, F("restarting\n"));
    delay(300);
    ESP.restart();
}

void otaStart(const String& filename, int mode) {
    lastStatus = String("ota start ") + filename;
    drawSystemScreen();
    const size_t capacity =
        mode == U_FS ? static_cast<size_t>(FS_PHYS_SIZE) : ((ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U);
    if (mode == U_FS) {
        LittleFS.end();
        fsMounted = false;
        delay(50);
    }
    if (!Update.begin(capacity, mode)) lastStatus = String("begin failed ") + Update.getErrorString();
}

void otaWrite(HTTPUpload& upload) {
    if (!Update.hasError() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        lastStatus = String("write failed ") + Update.getErrorString();
    }
    wdtYield();
}

void otaEnd(int mode) {
    if (!Update.hasError() && Update.end(true)) {
        server.send(200, F("text/plain"), F("OK"));
        delay(800);
        ESP.restart();
        return;
    }
    server.send(500, F("text/plain"), String("ota failed ") + Update.getErrorString() + "\n");
}

void handleMultipartOta(int mode) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) otaStart(upload.filename, mode);
    else if (upload.status == UPLOAD_FILE_WRITE) otaWrite(upload);
    else if (upload.status == UPLOAD_FILE_END) otaEnd(mode);
    else if (upload.status == UPLOAD_FILE_ABORTED) Update.end();
}

void handleFileUpload() {
    static File file;
    static bool failed = false;
    static String path;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        path = server.arg(F("path"));
        failed = false;
        if (!validFsPath(path) || (!fsMounted && !LittleFS.begin())) {
            failed = true;
            return;
        }
        fsMounted = true;
        ensureParentDirs(path);
        file = LittleFS.open(path, "w");
        if (!file) failed = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!failed && file && file.write(upload.buf, upload.currentSize) != upload.currentSize) failed = true;
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (file) file.close();
        lastStatus = failed ? "file write failed" : String("file ok ") + path;
    }
}

void handleFileDone() {
    sendText(lastStatus.indexOf("failed") >= 0 ? 500 : 200, lastStatus + "\n"); }

bool connectSta(const char* ssid, const char* pass, bool stored) {
    WiFi.mode(WIFI_STA);
    if (stored) WiFi.begin();
    else WiFi.begin(ssid, pass);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < STA_TIMEOUT_MS) {
        wdtYield();
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

void setupNetwork() {
    WiFi.persistent(true);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    bool staOk = false;
    if (cfg.ssid.length() > 0 && cfg.pass.length() > 0) staOk = connectSta(cfg.ssid.c_str(), cfg.pass.c_str(), false);
    if (!staOk) staOk = connectSta(nullptr, nullptr, true);
    if (!staOk) staOk = connectSta(FALLBACK_STA_SSID, FALLBACK_STA_PASS, false);
    WiFi.mode(staOk ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID);
}

// --- album API -------------------------------------------------------------
// Photo pixels go up through the existing /file route, which already streams an
// upload into LittleFS. What is left is the manifest, the thumbnails and the
// storage figures the card needs to tell the user how much room is left.

void albumFsInfo(size_t& total, size_t& used) {
    FSInfo info{};
    if (fsMounted && LittleFS.info(info)) {
        total = info.totalBytes;
        used = info.usedBytes;
    } else {
        total = 0;
        used = 0;
    }
}

void handleAlbumGet() {
    size_t total = 0;
    size_t used = 0;
    albumFsInfo(total, used);

    JsonDocument doc;
    doc["interval_seconds"] = cfg.albumIntervalSeconds;
    doc["max_photos"] = ALBUM_MAX;
    doc["slot_bytes"] = ALBUM_BYTES + ALBUM_THUMB_BYTES;
    doc["width"] = ALBUM_W;
    doc["height"] = ALBUM_H;
    doc["thumb"] = ALBUM_THUMB;
    doc["fs_total"] = total;
    doc["fs_used"] = used;
    doc["fs_free"] = total > used ? (total - used) : 0;
    doc["frame_us"] = albumFrameUs;
    JsonArray arr = doc["photos"].to<JsonArray>();
    for (uint8_t i = 0; i < albumCount; ++i) {
        JsonObject item = arr.add<JsonObject>();
        item["id"] = albumEntries[i].id;
        item["name"] = albumEntries[i].name;
        item["on"] = albumEntries[i].on;
    }
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

// Takes the whole list at once: order is the array order, so a reorder and a
// toggle are the same request. Entries whose pixels are missing are dropped
// rather than kept as a promise the renderer cannot honour.
void handleAlbumPost() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        sendText(400, F("bad json\n"));
        return;
    }

    if (doc["interval_seconds"].is<unsigned int>()) {
        uint16_t v = static_cast<uint16_t>(doc["interval_seconds"].as<unsigned int>());
        cfg.albumIntervalSeconds = constrain(v, THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    }

    if (doc["photos"].is<JsonArray>()) {
        // Built straight into the live array rather than into a stack copy: on
        // a 4 KB stack, sixteen entries of two Strings each is a lot to spend
        // inside a request handler, and the write is ordered so nothing is read
        // after it has been overwritten.
        uint8_t n = 0;
        for (JsonObject item : doc["photos"].as<JsonArray>()) {
            if (n >= ALBUM_MAX) break;
            const String id = item["id"] | "";
            if (!albumIdOk(id) || !LittleFS.exists(albumPath(id, ".rgb"))) continue;
            albumEntries[n].id = id;
            albumEntries[n].name = item["name"] | id;
            albumEntries[n].on = item["on"] | true;
            ++n;
        }
        // Release what the tail entries were holding; they are past the count
        // now and their Strings would otherwise sit on the heap until the list
        // grew back to that length.
        for (uint8_t i = n; i < albumCount; ++i) albumEntries[i] = AlbumEntry{};
        albumCount = n;
        // The cursor indexed the old order, so start the rotation over rather
        // than landing on whatever now sits at that position.
        albumCursor = -1;
        albumDrawn = false;
    }

    if (!albumSaveIndex()) {
        sendText(500, F("index write failed\n"));
        return;
    }
    saveConfig();
    if (activeScreen == SCREEN_ALBUM) drawAlbum(true);
    sendText(200, F("ok\n"));
}

void handleAlbumDelete() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id\n"));
        return;
    }
    LittleFS.remove(albumPath(id, ".rgb"));
    LittleFS.remove(albumPath(id, ".thm"));
    const int16_t at = albumFind(id);
    if (at >= 0) {
        for (uint8_t i = static_cast<uint8_t>(at); i + 1 < albumCount; ++i) {
            albumEntries[i] = albumEntries[i + 1];
        }
        albumEntries[albumCount - 1] = AlbumEntry{};
        --albumCount;
    }
    albumCursor = -1;
    albumDrawn = false;
    albumSaveIndex();
    if (activeScreen == SCREEN_ALBUM) drawAlbum(true);
    sendText(200, F("ok\n"));
}

// The card draws its grid from these rather than from the full photos: 40x40 is
// 3.2 KB against 115 KB, and a dozen full frames would take longer to fetch
// than anyone will wait for a settings page.
void handleAlbumThumb() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id\n"));
        return;
    }
    const String path = albumPath(id, ".thm");
    if (!fsMounted || !LittleFS.exists(path)) {
        sendText(404, F("no thumb\n"));
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendText(500, F("open failed\n"));
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "application/octet-stream");
    f.close();
}

void setupRoutes() {
    server.collectHeaders("Cookie");
    server.on(F("/login"), HTTP_GET, handleLoginGet);
    server.on(F("/login"), HTTP_POST, handleLoginPost);
    server.on(F("/logout"), HTTP_ANY, handleLogout);
    server.on(F("/api/password"), HTTP_POST, handlePasswordPost);
    server.on(F("/"), HTTP_GET, handleRoot);
    server.on(F("/status"), HTTP_GET, handleStatus);
    server.on(F("/config"), HTTP_GET, handleConfigGet);
    server.on(F("/api/config"), HTTP_GET, handleConfigGet);
    server.on(F("/api/config"), HTTP_POST, handleConfigPost);
    server.on(F("/weather/status"), HTTP_GET, handleWeatherStatus);
    server.on(F("/weather/refresh"), HTTP_POST, []() {
        bool ok = refreshWeather();
        updateDisplay(true);
        sendText(ok ? 200 : 500, weather.status + "\n");
    });
    server.on(F("/fs/list"), HTTP_GET, handleFsList);
    server.on(F("/api/radar"), HTTP_GET, []() {
        JsonDocument doc;
        doc["status"] = radarStatus;
        doc["tls_rx"] = radarTlsRx;
        doc["fetch_ms"] = radarFetchMs;
        doc["heap_during"] = radarHeapLow;
        doc["repaint_ms"] = radarRepaintMs;
        doc["contents_ms"] = radarContentsMs;
        doc["dns_ms"] = radarDnsMs;
        doc["connect_ms"] = radarConnectMs;
        doc["body_ms"] = radarBodyMs;
        doc["route_status"] = routeStatus;
        doc["route_ms"] = routeLastMs;
        doc["routes_cached"] = routeCount;
        doc["heap_now"] = ESP.getFreeHeap();
        doc["block_now"] = ESP.getMaxFreeBlockSize();
        doc["count"] = radarAcCount;
        JsonArray arr = doc["ac"].to<JsonArray>();
        for (uint8_t i = 0; i < radarAcCount; ++i) {
            JsonObject o = arr.add<JsonObject>();
            o["cs"] = radarAc[i].callsign;
            o["km"] = radarAc[i].distKm;
            o["brg"] = radarAc[i].bearingDeg;
            o["alt"] = radarAc[i].altFt;
            // The label as it is actually composed, so what the screen says can be
            // checked without standing in front of it.
            const RadarPlot plot = radarPlotOf(i);
            // Wrapped in String because the members are const char arrays, and
            // ArduinoJson stores a const char* by reference rather than copying
            // it. Assigned raw, every row ended up pointing at the last plot's
            // stack - which is how this diagnostic came to report one aircraft's
            // airline against another's callsign, and freed memory as its
            // altitude.
            o["top"] = String(plot.airline);
            o["bottom"] = String(plot.fl);
        }
        String body;
        serializeJson(doc, body);
        server.send(200, "application/json", body);
    });
    server.on(F("/api/radar/fetch"), HTTP_POST, []() {
        const bool ok = radarFetch();
        sendText(ok ? 200 : 500, radarStatus + " (" + String(radarFetchMs) + " ms)" + String(static_cast<char>(10)));
    });
    server.on(F("/api/album"), HTTP_GET, handleAlbumGet);
    server.on(F("/api/album"), HTTP_POST, handleAlbumPost);
    server.on(F("/api/album/delete"), HTTP_POST, handleAlbumDelete);
    server.on(F("/api/album/thumb"), HTTP_GET, handleAlbumThumb);
    server.on(F("/format"), HTTP_POST, handleFormat);
    server.on(F("/restart"), HTTP_ANY, handleRestart);
    server.on(F("/update_ota"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fw"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fs"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FS); });
    server.on(F("/file"), HTTP_POST, handleFileDone, handleFileUpload);
    server.onNotFound(handleStatic);
    server.begin();
    rawServer.begin();
    rawServer.setNoDelay(true);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    ESP.wdtEnable(WDTO_8S);
    tft.init();
    tft.setRotation(0);
    pinMode(5, OUTPUT);
    analogWriteRange(1023);
    applyBrightness();
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("SDP BOOT", 10, 10, 2);
    makeAuthToken();
    fsMounted = LittleFS.begin();
    loadConfig();
    setupNetwork();
    // After the network wait, snap the active screen onto the enabled set and
    // start the rotation clock here. Leaving lastScreenSwitchMs at 0 would make
    // the very first interval expire instantly, and leaving activeScreen at its
    // default would render a screen the user had switched off.
    albumLoadIndex();
    applyScreenSelection();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    setupRoutes();
    lastStatus = "ready";
    updateDisplay(true);
    // No fetch here: NTP has not answered yet. The loop runs it as soon as the
    // clock is set, which is within a few seconds of the network coming up.
}

void loop() {
    server.handleClient();
    handleRawServerClient();
    // Shut the boot grace window, once, and for this boot only. Nothing is
    // reported when it closes: lastStatus is a diagnostic of the last real
    // operation, and the window is not something to advertise either.
    if (authGraceOpen && millis() >= AUTH_GRACE_MS) authGraceOpen = false;
    if (cfg.weatherEnabled && WiFi.status() == WL_CONNECTED) {
        const uint32_t now = millis();
        // The KMA request carries a base date and time, so the very first fetch
        // has to wait for NTP: run it before the clock is set and it asks for a
        // slot that does not exist yet and comes back empty.
        const bool clockReady = time(nullptr) > 1700000000;
        // Two clocks, and both have to agree. `due` is about the data being
        // stale; `cooled` is about not asking again too soon. Only success moves
        // lastWeatherMs, so during an outage `due` stays true and `cooled` is
        // the only thing standing between us and a request per loop pass.
        const bool due = now - lastWeatherMs > WEATHER_INTERVAL_MS;
        const bool cooled = now - lastWeatherTryMs >= WEATHER_RETRY_MS;
        if (clockReady && (!weatherBootDone || (due && cooled))) {
            weatherBootDone = true;
            refreshWeather();
        }
    }
    applyBrightness();
    updateDisplay(false);
    wdtYield();
}
