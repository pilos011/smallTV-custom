#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <Updater.h>
#include <flash_hal.h>
#include <memory>
#include <time.h>
#include "display/ClockDashboardScene.h"
#include "display/ClockDigitFont.h"
#include "display/UiTextFont.h"

namespace {

constexpr const char* FW_NAME = "SDP Clock Weather";
constexpr const char* FW_VERSION = "v1.0.1";
constexpr const char* FALLBACK_STA_SSID = "";
constexpr const char* FALLBACK_STA_PASS = "";
constexpr const char* AP_SSID = "SDP-Recovery";
constexpr const char* CONFIG_PATH = "/config.json";
constexpr const char* KMA_HOST = "apihub.kma.go.kr";
constexpr uint32_t STA_TIMEOUT_MS = 15000;
constexpr uint32_t BODY_TIMEOUT_MS = 25000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 1000;
constexpr uint32_t WEATHER_INTERVAL_MS = 30UL * 60UL * 1000UL;
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
    SCREEN_COUNT = 2,
};
constexpr uint8_t SCREEN_MASK_ALL = (1U << SCREEN_COUNT) - 1U;
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

constexpr const char* AUTH_PASSWORD = "pilos011";
constexpr const char* AUTH_COOKIE = "sdp_auth";

struct AppConfig {
    String ssid;
    String pass;
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
    uint8_t screens = 1U << SCREEN_CLOCK_WEATHER;
    uint16_t themeIntervalSeconds = 10;
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
uint32_t lastScreenSwitchMs = 0;
bool analogChromeDrawn = false;
bool analogBandReady = false;
float analogPrevHour = -999.0f;
float analogPrevMinute = -999.0f;
float analogPrevSecond = -999.0f;
String authToken;
bool uploadAuthorized = false;

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

bool canUseUiFont(const String& text, uint8_t textSize) {
    if (text.isEmpty()) return false;
    const UiTextFont::Kind kind = uiKind(textSize);
    const char* raw = text.c_str();
    const size_t len = text.length();
    size_t index = 0;
    while (index < len) {
        String ch = readUtf8Char(raw, len, index);
        if (ch == "\r" || ch == "\n" || ch == "\t") continue;
        if (UiTextFont::glyph(kind, utf8Codepoint(ch)) == nullptr) return false;
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
        const UiTextFont::Glyph* glyph = UiTextFont::glyph(kind, utf8Codepoint(ch));
        if (glyph == nullptr) continue;
        if (!first) width += font.tracking;
        width += glyph->advance;
        first = false;
    }
    return width;
}

void drawUiGlyph(int16_t x, int16_t y, uint32_t codepoint, uint8_t textSize, uint16_t fg, uint16_t bg) {
    const UiTextFont::Kind kind = uiKind(textSize);
    const UiTextFont::Glyph* glyph = UiTextFont::glyph(kind, codepoint);
    if (glyph == nullptr) return;
    const UiTextFont::FontSet& font = UiTextFont::fontSet(kind);
    const uint8_t maxAlpha = static_cast<uint8_t>((1U << font.bitsPerPixel) - 1U);
    const int16_t drawY = y + glyph->yOffset;
    const size_t pixels = static_cast<size_t>(glyph->width) * glyph->height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t alpha = readPackedAlpha(glyph->bitmap, i, font.bitsPerPixel);
        if (alpha == 0) continue;
        tft.drawPixel(x + (i % glyph->width), drawY + (i / glyph->width), blend565(fg, bg, alpha, maxAlpha));
    }
}

void drawTextAt(int16_t x, int16_t y, const String& text, uint8_t textSize, uint16_t fg, uint16_t bg) {
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
            const UiTextFont::Glyph* glyph = UiTextFont::glyph(kind, codepoint);
            if (glyph != nullptr) {
                drawUiGlyph(cursor, y, codepoint, textSize, fg, bg);
                cursor += glyph->advance;
            }
            first = false;
        }
        return;
    }
    tft.setTextColor(fg, bg);
    tft.setTextSize(textSize);
    tft.setCursor(x, y);
    tft.print(text);
    tft.setTextSize(1);
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

bool drawBmpIcon(const String& path, int16_t x, int16_t y, int16_t maxSize) {
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
                tft.drawPixel(static_cast<int16_t>(x + xOff + col), static_cast<int16_t>(y + yOff + rowIndex),
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

bool saveConfig() {
    if (!fsMounted && !LittleFS.begin()) return false;
    fsMounted = true;
    JsonDocument doc;
    doc["ssid"] = cfg.ssid;
    doc["pass"] = cfg.pass;
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
    doc["theme_interval_seconds"] = cfg.themeIntervalSeconds;
    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
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
    cfg.screens = static_cast<uint8_t>(doc["screens"] | cfg.screens) & SCREEN_MASK_ALL;
    if (cfg.screens == 0) cfg.screens = 1U << SCREEN_CLOCK_WEATHER;
    cfg.themeIntervalSeconds = constrain(static_cast<uint16_t>(doc["theme_interval_seconds"] | cfg.themeIntervalSeconds),
                                         THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
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
}

bool refreshWeather() {
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
    if (LittleFS.exists(path) && drawBmpIcon(path, x, y, size)) return;
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

// The panel renders these darker tones far brighter than a monitor does, so the
// dial is pure black and the case is only just above it - enough to read as a
// rim, not as a grey border.
uint16_t analogCase() { return rgb(0x05, 0x06, 0x08); }
uint16_t analogDial() { return rgb(0x00, 0x00, 0x00); }
uint16_t analogLume() { return rgb(0x00, 0xF0, 0xFF); }
uint16_t analogHandColor() { return rgb(0xFF, 0x5A, 0x1F); }
uint16_t analogHandEdge() { return rgb(0x2A, 0x0C, 0x02); }

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

// A hand is an arm plus a short counterweight, neither of which enters the hub.
void analogHandStroke(TFT_eSPI& g, int16_t yOff, float angle, int16_t len, int16_t tail, float rNear, float rFar,
                      uint16_t color, uint16_t bg) {
    const float c = cosf(angle);
    const float s = sinf(angle);
    const float cy = static_cast<float>(ANALOG_CY - yOff);
    g.drawWedgeLine(ANALOG_CX + (c * ANALOG_HAND_INNER), cy + (s * ANALOG_HAND_INNER), ANALOG_CX + (c * len),
                    cy + (s * len), rNear, rFar, color, bg);
    if (tail > ANALOG_HAND_INNER) {
        g.drawWedgeLine(ANALOG_CX - (c * ANALOG_HAND_INNER), cy - (s * ANALOG_HAND_INNER), ANALOG_CX - (c * tail),
                        cy - (s * tail), rNear, rNear, color, bg);
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

    analogHandStroke(g, yOff, aH, ANALOG_L_HOUR, ANALOG_TAIL_HOUR, 4.7f, 3.3f, edge, dial);
    analogHandStroke(g, yOff, aH, ANALOG_L_HOUR, ANALOG_TAIL_HOUR, 3.6f, 2.2f, hand, edge);
    analogHandStroke(g, yOff, aM, ANALOG_L_MIN, ANALOG_TAIL_MIN, 4.0f, 2.9f, edge, dial);
    analogHandStroke(g, yOff, aM, ANALOG_L_MIN, ANALOG_TAIL_MIN, 2.9f, 1.8f, hand, edge);
    analogHandStroke(g, yOff, aS, ANALOG_L_SEC, ANALOG_TAIL_SEC, 0.9f, 0.9f, hand, dial);
    g.fillSmoothCircle(ANALOG_CX, ANALOG_CY - yOff, ANALOG_HUB_R, edge, dial);
    g.fillSmoothCircle(ANALOG_CX, ANALOG_CY - yOff, 4, hand, edge);
}

// The dial sits one colour step above the case, so anti-aliasing its rim buys
// nothing. Row spans are also far cheaper than fillSmoothCircle, which is
// O(r*r) per call and would otherwise be paid once per band.
void analogPaintGround(TFT_eSPI& g, int16_t yOff, int16_t rows) {
    const uint16_t dial = analogDial();
    for (int16_t y = 0; y < rows; ++y) {
        const int32_t dy = static_cast<int32_t>(yOff + y) - ANALOG_CY;
        const int32_t inside = (static_cast<int32_t>(ANALOG_R_DIAL) * ANALOG_R_DIAL) - (dy * dy);
        if (inside < 0) continue;
        const int16_t dx = static_cast<int16_t>(sqrtf(static_cast<float>(inside)));
        g.drawFastHLine(static_cast<int16_t>(ANALOG_CX - dx), y, static_cast<int16_t>((dx * 2) + 1), dial);
    }
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
void drawAnalogComposited(float aH, float aM, float aS) {
    for (int16_t top = 0; top < SCREEN_H; top = static_cast<int16_t>(top + ANALOG_BAND_H)) {
        const int16_t rows = min<int16_t>(ANALOG_BAND_H, static_cast<int16_t>(SCREEN_H - top));
        analogBand.fillSprite(analogCase());
        analogPaintGround(analogBand, top, rows);
        analogPaintFace(analogBand, top, rows, aH, aM, aS);
        analogBand.pushSprite(0, top);
        wdtYield();
    }
}

// Only reached when the band sprite cannot be allocated. This one does flicker,
// but it keeps the face working under memory pressure instead of going blank.
void drawAnalogDirect(float aH, float aM, float aS) {
    tft.fillScreen(analogCase());
    analogPaintGround(tft, 0, SCREEN_H);
    wdtYield();
    analogPaintFace(tft, 0, SCREEN_H, aH, aM, aS);
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

    if (!force && analogChromeDrawn && aS == analogPrevSecond && aM == analogPrevMinute && aH == analogPrevHour) {
        return;
    }

    if (analogBandBegin()) {
        drawAnalogComposited(aH, aM, aS);
    } else {
        drawAnalogDirect(aH, aM, aS);
    }

    analogChromeDrawn = true;
    analogPrevHour = aH;
    analogPrevMinute = aM;
    analogPrevSecond = aS;
    lastTimeOk = validTime;
}

// ---------------------------------------------------------------------------
// Screen rotation
// ---------------------------------------------------------------------------

uint8_t enabledScreens() {
    const uint8_t mask = cfg.screens & SCREEN_MASK_ALL;
    return mask == 0 ? static_cast<uint8_t>(1U << SCREEN_CLOCK_WEATHER) : mask;
}

int enabledScreenCount() {
    const uint8_t mask = enabledScreens();
    int count = 0;
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
        if (mask & (1U << i)) ++count;
    }
    return count;
}

uint8_t nextEnabledScreen(uint8_t from) {
    const uint8_t mask = enabledScreens();
    for (uint8_t step = 1; step <= SCREEN_COUNT; ++step) {
        const uint8_t candidate = static_cast<uint8_t>((from + step) % SCREEN_COUNT);
        if (mask & (1U << candidate)) return candidate;
    }
    return from;
}

void applyScreenSelection() {
    const uint8_t mask = enabledScreens();
    if (!(mask & (1U << activeScreen))) activeScreen = nextEnabledScreen(activeScreen);
    lastScreenSwitchMs = millis();
    screenChromeDrawn = false;
    analogChromeDrawn = false;
    resetDisplayCache();
}

void drawActiveScreen(bool force) {
    if (activeScreen == SCREEN_ANALOG) {
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
            resetDisplayCache();
            force = true;
        }
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
    WiFiClient client = rawServer.available();
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
// The token is regenerated on every boot, so a reboot signs everyone out.
// The raw port 8080 recovery server is deliberately left unauthenticated:
// it is the way back in when the main UI is broken.
// ---------------------------------------------------------------------------

void makeAuthToken() {
    authToken = "";
    for (uint8_t i = 0; i < 4; ++i) {
        char part[9];
        snprintf(part, sizeof(part), "%08x", static_cast<unsigned>(RANDOM_REG32));
        authToken += part;
    }
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
                    "<h1>&#49440;&#51068;&#51032; &#45796;&#47785;&#51201; "
                    "&#47784;&#45768;&#53552;&#47553; &#46356;&#49828;&#54540;&#47112;&#51060;</h1>"
                    "<input type='password' name='password' placeholder='Password' autofocus>"
                    "<button type='submit'>Sign in</button>");
    if (failed) body += F("<p class='err'>Wrong password.</p>");
    body += F("</form></body></html>");
    server.send(failed ? 401 : 200, F("text/html"), body);
}

// Returns true when the request may proceed.
bool requireAuth(bool htmlClient) {
    if (hasValidSession()) return true;
    if (htmlClient) {
        server.sendHeader(F("Location"), F("/login"));
        server.send(302, F("text/plain"), F("login required\n"));
    } else {
        server.send(401, F("application/json"), F("{\"error\":\"login required\"}"));
    }
    return false;
}

void handleLoginGet() {
    if (hasValidSession()) {
        server.sendHeader(F("Location"), F("/"));
        server.send(302, F("text/plain"), F("ok\n"));
        return;
    }
    sendLoginPage(false);
}

void handleLoginPost() {
    if (server.arg("password") != AUTH_PASSWORD) {
        sendLoginPage(true);
        return;
    }
    if (authToken.length() == 0) makeAuthToken();
    server.sendHeader(F("Set-Cookie"), String(AUTH_COOKIE) + "=" + authToken + F("; Path=/; Max-Age=86400; SameSite=Lax"));
    server.sendHeader(F("Location"), F("/"));
    server.send(302, F("text/plain"), F("ok\n"));
}

void handleLogout() {
    server.sendHeader(F("Set-Cookie"), String(AUTH_COOKIE) + F("=; Path=/; Max-Age=0"));
    server.sendHeader(F("Location"), F("/login"));
    server.send(302, F("text/plain"), F("bye\n"));
}

void handleStatus() {
    if (!requireAuth(false)) return;
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
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

void handleConfigGet() {
    if (!requireAuth(false)) return;
    JsonDocument doc;
    doc["name"] = FW_NAME;
    doc["version"] = FW_VERSION;
    doc["ssid"] = cfg.ssid;
    doc["location"] = cfg.location;
    doc["kma_key"] = cfg.kmaKey;
    doc["kma_key_set"] = cfg.kmaKey.length() > 0;
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
    doc["theme_interval_seconds"] = cfg.themeIntervalSeconds;
    doc["screen_count"] = static_cast<int>(SCREEN_COUNT);
    doc["active_screen"] = activeScreen;
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

void handleConfigPost() {
    if (!requireAuth(false)) return;
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

    const bool screensChanged = doc["screens"].is<unsigned int>();
    if (screensChanged) {
        cfg.screens = static_cast<uint8_t>(doc["screens"].as<unsigned int>()) & SCREEN_MASK_ALL;
        if (cfg.screens == 0) cfg.screens = 1U << SCREEN_CLOCK_WEATHER;
    }
    if (doc["theme_interval_seconds"].is<unsigned int>()) {
        cfg.themeIntervalSeconds = constrain(static_cast<uint16_t>(doc["theme_interval_seconds"].as<unsigned int>()),
                                             THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    }

    bool ok = saveConfig();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com");
    applyBrightness();
    if (screensChanged) applyScreenSelection();
    refreshWeather();
    updateDisplay(true);
    sendText(ok ? 200 : 500, ok ? "ok\n" : "save failed\n");
}

void handleWeatherStatus() {
    if (!requireAuth(false)) return;
    JsonDocument doc;
    doc["valid"] = weather.valid;
    doc["fetching"] = weather.fetching;
    doc["status"] = weather.status;
    doc["temp"] = weather.temp;
    doc["humidity"] = weather.humidity;
    doc["rain"] = weather.rain;
    doc["updated"] = weather.updated;
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
    if (!requireAuth(false)) return;
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
    if (!requireAuth(false)) return;
    LittleFS.end();
    fsMounted = false;
    bool ok = LittleFS.format();
    fsMounted = ok && LittleFS.begin();
    sendText(ok ? 200 : 500, ok ? "format ok\n" : "format failed\n");
}

void handleRestart() {
    if (!requireAuth(false)) return;
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
    if (upload.status == UPLOAD_FILE_START) {
        uploadAuthorized = hasValidSession();
        if (!uploadAuthorized) {
            lastStatus = "ota rejected: login required";
            return;
        }
        otaStart(upload.filename, mode);
        return;
    }
    if (!uploadAuthorized) return;
    if (upload.status == UPLOAD_FILE_WRITE) otaWrite(upload);
    else if (upload.status == UPLOAD_FILE_END) otaEnd(mode);
    else if (upload.status == UPLOAD_FILE_ABORTED) Update.end();
}

// otaEnd() already replied when the upload was allowed, so this only has to
// answer the rejected case.
void otaCompletion() {
    if (!uploadAuthorized) server.send(401, F("application/json"), F("{\"error\":\"login required\"}"));
    uploadAuthorized = false;  // a body-less POST must not inherit this
}

void handleFileUpload() {
    static File file;
    static bool failed = false;
    static String path;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadAuthorized = hasValidSession();
        if (!uploadAuthorized) {
            failed = true;
            lastStatus = "file rejected: login required";
            return;
        }
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
    } else if (!uploadAuthorized) {
        return;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!failed && file && file.write(upload.buf, upload.currentSize) != upload.currentSize) failed = true;
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (file) file.close();
        lastStatus = failed ? "file write failed" : String("file ok ") + path;
    }
}

void handleFileDone() {
    const bool allowed = uploadAuthorized;
    uploadAuthorized = false;  // a body-less POST must not inherit this
    if (!allowed) {
        server.send(401, F("application/json"), F("{\"error\":\"login required\"}"));
        return;
    }
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

void setupRoutes() {
    server.collectHeaders("Cookie");
    server.on(F("/login"), HTTP_GET, handleLoginGet);
    server.on(F("/login"), HTTP_POST, handleLoginPost);
    server.on(F("/logout"), HTTP_ANY, handleLogout);
    server.on(F("/"), HTTP_GET, handleRoot);
    server.on(F("/status"), HTTP_GET, handleStatus);
    server.on(F("/config"), HTTP_GET, handleConfigGet);
    server.on(F("/api/config"), HTTP_GET, handleConfigGet);
    server.on(F("/api/config"), HTTP_POST, handleConfigPost);
    server.on(F("/weather/status"), HTTP_GET, handleWeatherStatus);
    server.on(F("/weather/refresh"), HTTP_POST, []() {
        if (!requireAuth(false)) return;
        bool ok = refreshWeather();
        updateDisplay(true);
        sendText(ok ? 200 : 500, weather.status + "\n");
    });
    server.on(F("/fs/list"), HTTP_GET, handleFsList);
    server.on(F("/format"), HTTP_POST, handleFormat);
    server.on(F("/restart"), HTTP_ANY, handleRestart);
    server.on(F("/update_ota"), HTTP_POST, otaCompletion, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fw"), HTTP_POST, otaCompletion, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fs"), HTTP_POST, otaCompletion, []() { handleMultipartOta(U_FS); });
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
    applyScreenSelection();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    setupRoutes();
    lastStatus = "ready";
    updateDisplay(true);
    if (cfg.weatherEnabled) refreshWeather();
}

void loop() {
    server.handleClient();
    handleRawServerClient();
    if (cfg.weatherEnabled && WiFi.status() == WL_CONNECTED && millis() - lastWeatherMs > WEATHER_INTERVAL_MS) {
        refreshWeather();
    }
    applyBrightness();
    updateDisplay(false);
    wdtYield();
}
