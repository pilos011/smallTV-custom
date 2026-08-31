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
#include "display/BorduhrHands.h"
#include <TJpg_Decoder.h>

namespace {

constexpr const char* FW_NAME = "SDP Clock Weather";
constexpr const char* FW_VERSION = "v1.0.26";
constexpr const char* FALLBACK_STA_SSID = "";
constexpr const char* FALLBACK_STA_PASS = "";
constexpr const char* AP_SSID = "SDP-Recovery";
// Whether the access point is actually on the air, which is not the same
// question as whether it was asked for.
bool apRunning = false;
// Long enough for a drop to settle before the screen makes an announcement
// about it, short enough that someone standing there is not left guessing.
constexpr uint32_t OFFLINE_GRACE_MS = 12000;
// How often an offline device asks again. setupNetwork runs once, in setup, so
// without this a boot that missed its window stayed on the access point until
// somebody power-cycled it - a router rebooting overnight turned into a dead
// clock by morning. Twenty seconds is longer than the SDK needs to answer and
// short enough that a returning network is picked up while nobody is looking.
constexpr uint32_t STA_RETRY_MS = 20000;
// A hinted join is either quick or wrong: naming the channel and the radio
// skips the search entirely, so it lands in a couple of seconds or the hint has
// gone stale and no amount of waiting will fix it. Six seconds, then the ordinary
// attempt gets its full fifteen.
constexpr uint32_t STA_HINT_MS = 6000;
uint32_t staDownSinceMs = 0;
bool offlineScreenDrawn = false;

// The address is worth having on screen right after a restart, when it may have
// changed and nobody knows it yet. It is not worth having there forever, so it
// goes away on its own and the screen underneath is redrawn whole.
constexpr uint32_t IP_SHOW_MS = 3UL * 60UL * 1000UL;
bool ipBadgeShowing = false;
String ipBadgeText;
// Whether the dashboard currently has the address in its own top left corner.
// Not the same question as "is the dashboard up": while it waits for weather it
// clears the screen and prints two lines instead, and that wait is exactly the
// minute after boot when the badge is the only thing saying where to find the
// device.
bool dashboardShowsIp = false;
constexpr const char* CONFIG_PATH = "/config.json";
constexpr const char* KMA_HOST = "apihub.kma.go.kr";
constexpr uint32_t STA_TIMEOUT_MS = 15000;
constexpr uint32_t BODY_TIMEOUT_MS = 25000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 1000;
constexpr uint32_t WEATHER_INTERVAL_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MS = 15UL * 1000UL;
// Consecutive failures, and the wait doubles with each one up to the ordinary
// refresh interval. Fifteen seconds flat is right for a hiccup and wrong for an
// outage: nothing here moves lastWeatherMs unless the fetch worked, so a
// forecast that stays broken means two KMA calls every fifteen seconds for as
// long as it lasts - eleven thousand a day against a quota, to learn the same
// thing each time. The current conditions carry the screen meanwhile.
uint8_t weatherFailStreak = 0;

uint32_t weatherRetryDelay() {
    const uint8_t shift = weatherFailStreak > 7 ? 7 : weatherFailStreak;
    const uint32_t d = WEATHER_RETRY_MS << shift;
    return d > WEATHER_INTERVAL_MS ? WEATHER_INTERVAL_MS : d;
}
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
    SCREEN_BORDUHR = 9,
    SCREEN_FORECAST = 10,
    SCREEN_COUNT = 11,
};
// A name for each screen, in enum order, and the config is written in these
// rather than in the numbers.
//
// The enum is positional. Take a screen out of the middle and every screen
// after it moves down one - but the saved config still says "bit 7" and "order
// [0,1,...]", and those numbers now point at different screens. Someone who had
// the album on would find the radar on instead, with nothing to tell them why.
// Names do not move. A name the firmware no longer has is ignored, a name it
// has gained is simply absent from the file and takes its default, and every
// other screen keeps the setting its owner chose.
//
// Never renumber these strings to match a new enum order, and never reuse a
// name for a different screen - the file is the other half of the contract.
constexpr const char* SCREEN_KEYS[SCREEN_COUNT] = {
    "clockweather", "analog", "mondaine", "mondaine_white", "digital",
    "weather_digital", "date_digital", "album", "radar", "borduhr", "forecast",
};

// The screen a name belongs to, or -1 when this firmware has no such screen.
int8_t screenByKey(const char* key) {
    if (key == nullptr || key[0] == 0) return -1;
    for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
        if (!strcmp(SCREEN_KEYS[i], key)) return static_cast<int8_t>(i);
    }
    return -1;
}

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

// A place the weather is asked about, by name. The grid pair is what the KMA
// wants and neither number says anything to a reader, so they travel under a
// name that does.
//
// The label is kept apart from that name on purpose. The name is the browser's
// word for the place and can be anything; the label is drawn on the panel, and
// the panel can only draw the Hangul this build baked - the syllables the
// airline, airport and rotorcraft tables happened to need. So a location the
// page calls 백석동 can still show as "Baekseok" on the screen, and neither has
// to be bent to suit the other.
//
// The API key and the timezone stay out: they describe the account and the
// clock, not the place.
struct WeatherPreset {
    char name[25];      // up to eight Hangul syllables and a terminator
    char label[25];     // what the dashboard prints
    uint16_t nx = 0;
    uint16_t ny = 0;
};
constexpr uint8_t WEATHER_PRESET_MAX = 6;
// The KMA short-term grid is 149 x 253 cells laid over the peninsula. A pair
// outside it is not a place, and the forecast for it comes back empty every
// time without ever saying why - so it is refused at the door instead.
constexpr uint16_t KMA_GRID_NX_MAX = 149;
constexpr uint16_t KMA_GRID_NY_MAX = 253;

// A place the radar can be pointed at by name - home, the office, wherever
// the device gets carried. Everything that changes with the place is stored:
// the position, the range, which way the device sits on that desk, and the
// altitude floor that makes sense there. Only the poll rate stays out - that
// describes the feed, not the location.
// A network the device is allowed to join, kept so that carrying it between
// places needs no reconfiguration: at boot every profile is tried before the
// recovery AP goes up. The primary SSID in the System menu stays what it is -
// these are the alternates.
struct WifiProfile {
    char ssid[33] = "";
    char pass[65] = "";
};
constexpr uint8_t WIFI_PROFILE_MAX = 5;

struct RadarPreset {
    char name[25];      // up to eight Hangul syllables and a terminator
    float lat = 0.0f;
    float lon = 0.0f;
    uint16_t km = 10;
    uint16_t upDeg = 0;
    uint16_t minAlt = 0;
    char bg[25] = "";   // background image id, empty for the plain dial
};
constexpr uint8_t RADAR_PRESET_MAX = 6;

// The week ahead, from OpenWeather.
//
// Plain HTTP on purpose. The 2.5 endpoints answer on port 80, which spares this
// screen everything the radar pays for TLS - no BearSSL context, no eighteen
// kilobytes of contiguous heap, no handshake inside the watchdog's budget. The
// key travels in the clear, which is what a free weather key is worth.
//
// Six days rather than seven: this key cannot reach the daily endpoints. One
// Call 3.0 wants a separate subscription and forecast/daily is a paid plan, so
// what is available is forty three-hourly entries, and those fall across six
// calendar days with the first and last of them partial.
constexpr const char* OW_HOST = "api.openweathermap.org";
constexpr uint8_t FC_PRESET_MAX = 4;
constexpr uint8_t FC_DAYS = 4;
// Thirty minutes. The upstream only recomputes every three hours, and the free
// tier allows sixty calls a minute, so this is neither stale nor greedy.
constexpr uint32_t FC_REFRESH_MS = 30UL * 60UL * 1000UL;
// After a failure, though, try again in two minutes rather than thirty. The
// first fetch of a boot often lands before the network has settled, and half an
// hour of "http -1" on screen for a fault that cleared in ten seconds is the
// wrong trade.
constexpr uint32_t FC_RETRY_MS = 2UL * 60UL * 1000UL;
// The reply is about seventeen kilobytes and the filtered document still has to
// be built. Not the eighteen the radar needs for TLS - there is no handshake
// here - but enough that starting the parse into a hole this small only wastes
// the six seconds it takes to fail.
constexpr uint32_t FC_MIN_BLOCK = 9000;
// Today's row is resampled from the KMA nowcast every two hours. The rest of
// the week can only be a forecast, but the first row is a day that is already
// happening, and OpenWeather's 3 pm slot is a poor answer for it.
constexpr uint32_t FC_NOW_MS = 2UL * 60UL * 60UL * 1000UL;
// How old the nowcast may be and still stand in for today. It is refreshed
// half-hourly, so an hour means two misses in a row; past that the row goes
// back to OpenWeather rather than showing a reading from before the outage as
// though it were current.
constexpr uint32_t FC_NOW_STALE_MS = 2UL * WEATHER_INTERVAL_MS;
// A sample that found nothing is retried in a minute rather than in two hours.
// The first one of a boot always finds nothing: it runs on the first pass and
// the nowcast cannot arrive until NTP has set the clock, because the KMA
// request carries a base date and time. Booking the next attempt two hours out
// meant the row spent the first two hours of every boot on OpenWeather.
constexpr uint32_t FC_NOW_RETRY_MS = 60UL * 1000UL;
// Both are held as "when it last ran" and "how long to wait", never as an
// absolute deadline: millis() wraps at 49.7 days, and `millis() >= deadline`
// stays false for the seven weeks it takes the counter to climb back.

struct ForecastPreset {
    char name[25] = "";   // up to eight Hangul syllables and a terminator
    float lat = 0.0f;
    float lon = 0.0f;
};

// One row of the screen. Everything is already reduced to what is drawn, so
// nothing here has to be recomputed on a repaint.
struct ForecastDay {
    bool valid = false;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t weekday = 0;    // 0 = Monday, to match the label table
    char icon[4] = "";      // OpenWeather's code, "10d" and the like
    uint8_t humidity = 0;
    int16_t feels = 0;      // the warmest the day is expected to feel
};

struct AppConfig {
    String ssid;

    String pass;
    // Where the card's network was last found. WiFi.begin can be told the
    // channel and the exact radio, which lets the SDK skip scanning for the
    // access point - that search is most of what a reconnect spends its time
    // on. Zero means not known yet, or the last hinted attempt failed and it
    // was cleared rather than left to fail again.
    uint8_t wifiChannel = 0;
    uint8_t wifiBssid[6] = {0, 0, 0, 0, 0, 0};
    WifiProfile wifiProfiles[WIFI_PROFILE_MAX];
    uint8_t wifiProfileCount = 0;
    String webPassword = AUTH_DEFAULT_PASSWORD;
    String location = "Baekseok";
    String kmaKey;
    int nx = 57;
    int ny = 128;
    WeatherPreset weatherPresets[WEATHER_PRESET_MAX];
    uint8_t weatherPresetCount = 0;
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
    // Background image id: /radar/<id>.rgb, 240x240 RGB565 big-endian, made
    // by the browser the same way an album photo is. Empty means the plain
    // black dial. A satellite tile of the preset's own area turns the radar
    // into a live map.
    String radarBg;
    // Which saved location the radar settings came from, by name. The page used
    // to work this out by comparing every value, which cannot tell two places
    // apart when they hold the same settings and differ only in having a map -
    // both showed IN USE - and loses the answer entirely the moment a value is
    // edited, which is when it is most wanted. A name survives both.
    String radarPreset;
    RadarPreset radarPresets[RADAR_PRESET_MAX];
    uint8_t radarPresetCount = 0;
    // OpenWeather. The key is configuration, not source - the same rule the KMA
    // key has followed since v1.0.6, and for the same reason: this file is
    // published.
    String owKey;
    ForecastPreset fcPresets[FC_PRESET_MAX] = {
        {"백석동", 37.6437f, 126.7879f},
        {"서초동", 37.4882f, 127.0175f},
    };
    uint8_t fcPresetCount = 2;
    uint8_t fcPresetIdx = 0;
    // Rotation order. The bitmask says which screens are in the loop; this says
    // in what sequence, which the bitmask cannot express on its own.
    uint8_t screenOrder[SCREEN_COUNT] = {SCREEN_CLOCK_WEATHER, SCREEN_ANALOG, SCREEN_MONDAINE,
                                         SCREEN_MONDAINE_WHITE, SCREEN_DIGITAL, SCREEN_WEATHER_DIGITAL,
                                         SCREEN_DATE_DIGITAL, SCREEN_ALBUM, SCREEN_RADAR,
                                         SCREEN_BORDUHR, SCREEN_FORECAST};
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
    // Lightning. The ultra-short forecast has carried an LGT category all along
    // and this firmware was not reading it, so the storm icon that has been
    // sitting in /weather-icons since the beginning could never be chosen: SKY
    // and PTY between them have no way to say "thunder".
    bool lgt = false;
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
    // Taken from the nearest forecast slot, like sky: the nowcast reports what
    // is falling, not whether it is thundering.
    bool lgt = false;
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

// Whether the last few boots got anywhere. Every recovery route this firmware
// has - /status, the OTA endpoints, the raw port, even the recovery AP - is
// opened by setup(), and setup() does a great deal of filesystem work before it
// gets there. Anything that stalls that work past the eight-second watchdog
// resets the device before a single route exists, and since the next boot does
// exactly the same thing, it never gets out. That is not a hypothetical: it
// cost this project a device, recovered only over the UART pads inside the
// case, because a filesystem sweep at boot took too long and nothing was
// listening by the time it mattered.
//
// So the count lives in RTC memory, which survives a reset and is cleared by
// pulling the power - meaning a person unplugging the device is itself a way
// out. Three failures and the next boot skips every heavy thing and just puts
// the recovery AP up.
// Bumped whenever BootMark's layout changes: an old marker read into a new
// struct hands back rubbish and invents a diagnosis.
constexpr uint32_t BOOT_MARK_MAGIC = 0x53445014;
// Three boots may fail; the fourth gives up and goes to safe mode. Named for
// what it counts, because "= 3" beside "> 3" reads like an off-by-one.
constexpr uint32_t BOOT_FAILS_ALLOWED = 3;
constexpr uint32_t BOOT_SETTLED_MS = 10000;

// How far a boot got. A device that resets before it can report anything
// leaves nothing behind to diagnose - which is exactly why the failure that
// cost a device was never explained. The marker carries it across the reset,
// so the next boot can say where the last one stopped.
enum BootPhase : uint32_t {
    PH_START = 0, PH_COUNTED = 1, PH_FS = 2, PH_CONFIG = 3, PH_WIFI = 4,
    PH_ROUTES = 5, PH_SWEEP = 6, PH_ALBUM = 7, PH_READY = 8, PH_SETTLED = 9,
    PH_SAFE = 20
};

// Eight boots of history, because one was not enough. When the device came
// back on its access point the marker could say the previous boot reached
// READY and died inside ten seconds - and nothing at all about the boot before
// that, which was the one worth asking about. RTC memory is 512 bytes and this
// used sixteen of them.
//
// Each slot answers three questions about one boot: how far it got, what ended
// it, and how little heap it had at its worst. The reset reason comes from the
// SDK and distinguishes a watchdog from an exception from someone pulling the
// power - which is exactly the distinction guesswork could not make.
constexpr uint8_t BOOT_HISTORY = 8;

// What the loop was busy with. phase stops being useful once setup() ends -
// everything after that reads READY - so a watchdog inside the loop said only
// "somewhere in the main loop", which is where guessing started and stayed.
// This narrows it to the call that was running when the device died.
enum WorkMark : uint8_t {
    W_IDLE = 0, W_HTTP = 1, W_WEATHER = 2, W_DISPLAY = 3,
    W_RADAR_FETCH = 4, W_RADAR_DRAW = 5, W_ALBUM = 6,
    // Inside the album, because "somewhere in drawAlbum" was as far as the
    // coarse marks could narrow it, and two fixes aimed at the wrong half.
    W_ALB_BAND = 8,    // handing the analog band memory back
    W_ALB_EXISTS = 9,  // asking the filesystem whether the photo is there
    W_ALB_JPG = 10,    // inside TJpgDec.drawFsJpg
    W_ALB_RAW = 11,    // the raw RGB565 path
    W_ALB_MSG = 12     // drawing the fallback message
};

struct BootMark {
    uint32_t magic;
    uint32_t fails;
    uint32_t phase;
    uint32_t prevPhase;
    uint16_t minHeap;                 // this boot's low-water mark, live
    uint8_t  work;                    // what the loop is doing right now
    uint8_t  detail;                  // which item that work was on - the photo index
    uint8_t  histPhase[BOOT_HISTORY]; // [0] is the most recent finished boot
    uint8_t  histReason[BOOT_HISTORY];
    uint8_t  histWork[BOOT_HISTORY];  // what it was doing when it stopped
    uint8_t  histDetail[BOOT_HISTORY];
    uint16_t histMinHeap[BOOT_HISTORY];
};

BootMark bootMark{};
bool bootSafeMode = false;
bool bootMarkCleared = false;

void bootMarkWrite() {
    ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&bootMark), sizeof(bootMark));
}

// RTC memory is memory, not flash, so recording progress at every step costs
// nothing worth counting.
void bootMarkPhase(uint32_t p) {
    bootMark.phase = p;
    bootMarkWrite();
}

// Tracked as the boot runs so the NEXT boot can report how close this one came
// to running out. Written only when it drops meaningfully, because the point is
// the low-water mark, not a log of every allocation.
// Written only when it changes, so a loop that stays in one place costs one
// write, not one per pass.
// Whether the last boot died inside a particular piece of work. Safe mode is a
// blunt instrument - it turns everything off after three failures - and this is
// the sharp one: if the previous boot was killed by the watchdog while doing X,
// this boot does everything except X. The device keeps working and the failing
// piece is the only thing lost, which also leaves it available to be diagnosed
// instead of hidden behind a device that will not stay up.
constexpr uint8_t RESET_HW_WATCHDOG = 1;

bool workKilledLastBoot(uint8_t w) {
    return bootMark.histWork[0] == w && bootMark.histReason[0] == RESET_HW_WATCHDOG;
}

void bootMarkWork(uint8_t w) {
    if (bootMark.work == w) return;
    bootMark.work = w;
    bootMarkWrite();
}

// Which item the work is on. "died in the JPEG decoder" narrowed the fault to
// one function; this narrows it to one photograph, which is the difference
// between a suspect and a file that can be opened and looked at.
void bootMarkDetail(uint8_t d) {
    if (bootMark.detail == d) return;
    bootMark.detail = d;
    bootMarkWrite();
}

void bootMarkHeap() {
    const uint32_t h = ESP.getFreeHeap();
    const uint16_t v = h > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(h);
    if (v + 512 < bootMark.minHeap) {
        bootMark.minHeap = v;
        bootMarkWrite();
    }
}

// Counted up front and written before anything else is attempted, because a
// count that is only saved on the way out is never saved on the boot that
// matters.
void bootMarkBegin() {
    ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&bootMark), sizeof(bootMark));
    if (bootMark.magic != BOOT_MARK_MAGIC) {
        bootMark = BootMark{};
        bootMark.magic = BOOT_MARK_MAGIC;
        bootMark.phase = PH_START;
    }
    // Shift the finished boot into the history before this one overwrites it.
    for (uint8_t i = BOOT_HISTORY - 1; i > 0; --i) {
        bootMark.histPhase[i] = bootMark.histPhase[i - 1];
        bootMark.histReason[i] = bootMark.histReason[i - 1];
        bootMark.histWork[i] = bootMark.histWork[i - 1];
        bootMark.histDetail[i] = bootMark.histDetail[i - 1];
        bootMark.histMinHeap[i] = bootMark.histMinHeap[i - 1];
    }
    bootMark.histPhase[0] = static_cast<uint8_t>(bootMark.phase);
    bootMark.histWork[0] = bootMark.work;
    bootMark.histDetail[0] = bootMark.detail;
    bootMark.histMinHeap[0] = bootMark.minHeap;
    // Why THIS boot started, which is the same as how the last one ended.
    const rst_info* ri = ESP.getResetInfoPtr();
    bootMark.histReason[0] = ri ? static_cast<uint8_t>(ri->reason) : 0xFF;

    bootMark.prevPhase = bootMark.phase;
    ++bootMark.fails;
    bootMark.phase = PH_COUNTED;
    bootMark.minHeap = 0xFFFF;
    bootMarkWrite();
    bootSafeMode = bootMark.fails > BOOT_FAILS_ALLOWED;
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

// Which characters of a string the UI font has no glyph for. One of them is
// enough: canUseUiFont is all-or-nothing, so a single missing syllable sends
// the whole string to the built-in font and the Hangul comes out as broken
// bytes. Only 438 of the 11172 Hangul syllables are baked - the rest would cost
// most of a megabyte - so a place name has to be checked rather than assumed,
// and the page needs to be able to say which character is the problem.
String missingGlyphs(const String& text, uint8_t textSize) {
    String out;
    const char* raw = text.c_str();
    const size_t len = text.length();
    size_t index = 0;
    while (index < len) {
        const String ch = readUtf8Char(raw, len, index);
        if (ch == " " || ch.length() == 0) continue;
        if (!canUseUiFont(ch, textSize) && out.indexOf(ch) < 0) out += ch;
    }
    return out;
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

// The bitmap and the Korean word for a given sky. They were two functions
// walking the same five branches in the same order, one returning each half,
// and nothing but care kept them agreeing - a PTY code added to one and not the
// other would have put a rain word under a snow icon with the build still
// green. One ladder, both answers.
struct SkyLook {
    const char* slot;
    const char* word;
};

SkyLook skyLook(int sky, int pty, bool lgt) {
    if (lgt) return {"storm", "뇌우"};
    if (pty == 1 || pty == 2 || pty == 5 || pty == 6) return {"umbrella", "비"};
    if (pty == 3 || pty == 7) return {"snow", "눈"};
    if (sky <= 1) return {"clear", "맑음"};
    if (sky == 3) return {"cloudy", "구름"};
    return {"cloudy", "흐림"};
}

const char* iconSlot(int sky, int pty, bool lgt) { return skyLook(sky, pty, lgt).slot; }

String sizedIconPath(const char* slot, int16_t size) {
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

// Shared with the album: an id that becomes a filename passes one gate.
bool albumIdOk(const String& id);

// Replaces the whole preset list from a JSON array, which is how both the
// config file and the API hand it over. Whole-list replacement rather than
// add/remove verbs for the same reason screen_order works that way: the web
// page owns the list, and two verbs racing each other need referee code that
// a single honest assignment does not.
// Whole-list replacement, the same bargain the radar's list and screen_order
// strike: the page owns the list, and two verbs racing each other need referee
// code that a single honest assignment does not.
// strlcpy cuts at a byte, and every one of these fields holds a name the user
// typed. Pure Hangul happens to land on a boundary in a 25-byte field - eight
// syllables is exactly twenty-four - but anything mixed does not, and the
// leftover one or two bytes of a half-copied character reach the panel's UTF-8
// reader and the JSON the config route serialises.
void copyName(char* dst, const char* src, size_t size) {
    if (size == 0) return;
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        // Back off over any continuation bytes, then over the lead byte they
        // belonged to. 0x80 is the continuation mask; 0xC0 marks a lead.
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

void loadWeatherPresets(JsonVariantConst v) {
    if (!v.is<JsonArrayConst>()) return;
    cfg.weatherPresetCount = 0;
    for (JsonObjectConst o : v.as<JsonArrayConst>()) {
        if (cfg.weatherPresetCount >= WEATHER_PRESET_MAX) break;
        const char* name = o["name"] | "";
        // A nameless place cannot be picked out of a list, so it is not a
        // place worth keeping.
        if (name[0] == 0) continue;
        const uint32_t nx = o["nx"] | 0U;
        const uint32_t ny = o["ny"] | 0U;
        if (nx < 1 || nx > KMA_GRID_NX_MAX || ny < 1 || ny > KMA_GRID_NY_MAX) continue;
        WeatherPreset& p = cfg.weatherPresets[cfg.weatherPresetCount++];
        copyName(p.name, name, sizeof(p.name));
        // An entry saved before the label existed, or one written by hand,
        // still has a name - and the name is a better guess than nothing.
        copyName(p.label, o["label"] | name, sizeof(p.label));
        p.nx = static_cast<uint16_t>(nx);
        p.ny = static_cast<uint16_t>(ny);
    }
}

void emitWeatherPresets(JsonDocument& doc) {
    JsonArray arr = doc["weather_presets"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.weatherPresetCount; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = cfg.weatherPresets[i].name;
        o["label"] = cfg.weatherPresets[i].label;
        o["nx"] = cfg.weatherPresets[i].nx;
        o["ny"] = cfg.weatherPresets[i].ny;
    }
}

// Whole-list replacement, like the radar's presets - with one wrinkle: the
// API never returns passwords, so the page cannot echo them back. An entry
// posted without a password keeps the stored password of the same SSID; only
// a new SSID, or an entry that names a password, writes one. mergeOld is off
// when reading config.json, which does hold the passwords.
void loadWifiProfiles(JsonVariantConst v, bool mergeOld) {
    if (!v.is<JsonArrayConst>()) return;
    WifiProfile before[WIFI_PROFILE_MAX];
    uint8_t beforeCount = cfg.wifiProfileCount;
    if (mergeOld) memcpy(before, cfg.wifiProfiles, sizeof(before));
    cfg.wifiProfileCount = 0;
    for (JsonObjectConst o : v.as<JsonArrayConst>()) {
        if (cfg.wifiProfileCount >= WIFI_PROFILE_MAX) break;
        const char* ssid = o["ssid"] | "";
        if (ssid[0] == 0) continue;
        WifiProfile& p = cfg.wifiProfiles[cfg.wifiProfileCount++];
        strlcpy(p.ssid, ssid, sizeof(p.ssid));
        const char* pass = o["pass"] | "";
        if (pass[0] == 0 && mergeOld) {
            for (uint8_t i = 0; i < beforeCount; ++i) {
                if (strcmp(before[i].ssid, ssid) == 0) {
                    pass = before[i].pass;
                    break;
                }
            }
        }
        strlcpy(p.pass, pass, sizeof(p.pass));
    }
}

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
        copyName(p.name, name, sizeof(p.name));
        p.lat = lat;
        p.lon = lon;
        uint32_t km = o["km"] | 10U;
        if (km < 2) km = 2;
        if (km > 400) km = 400;
        p.km = static_cast<uint16_t>(km);
        p.upDeg = static_cast<uint16_t>((o["up"] | 0U) % 360U);
        uint32_t minAlt = o["min_alt"] | 0U;
        if (minAlt > 60000) minAlt = 60000;
        p.minAlt = static_cast<uint16_t>(minAlt);
        const char* bg = o["bg"] | "";
        if (albumIdOk(bg)) strlcpy(p.bg, bg, sizeof(p.bg));
        else p.bg[0] = 0;
    }
}

void loadFcPresets(JsonVariantConst v) {
    if (!v.is<JsonArrayConst>()) return;
    uint8_t n = 0;
    for (JsonObjectConst o : v.as<JsonArrayConst>()) {
        if (n >= FC_PRESET_MAX) break;
        const char* name = o["name"] | "";
        if (name[0] == 0) continue;
        ForecastPreset& p = cfg.fcPresets[n];
        p = ForecastPreset{};
        copyName(p.name, name, sizeof(p.name));
        p.lat = o["lat"] | 0.0f;
        p.lon = o["lon"] | 0.0f;
        ++n;
    }
    // An empty or unusable list leaves the built-in pair standing rather than
    // taking away the screen's only reason to exist.
    if (n > 0) cfg.fcPresetCount = n;
}

void emitFcPresets(JsonDocument& doc) {
    JsonArray arr = doc["fc_presets"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.fcPresetCount; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = cfg.fcPresets[i].name;
        o["lat"] = cfg.fcPresets[i].lat;
        o["lon"] = cfg.fcPresets[i].lon;
        // Empty when the title will draw. The screen puts the name at size 2
        // followed by 예보, and that whole string is what has to be drawable.
        o["missing"] = missingGlyphs(String(cfg.fcPresets[i].name) + " 예보", 2);
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
        o["up"] = cfg.radarPresets[i].upDeg;
        o["min_alt"] = cfg.radarPresets[i].minAlt;
        o["bg"] = cfg.radarPresets[i].bg;
    }
}

// Refuses to write while safe mode is on, and this is not a nicety. Safe mode
// skips loadConfig() by design, so every field is sitting at its compiled-in
// default: no API key, no presets, no WiFi profiles. Saving from there does not
// save the device's settings - it replaces them with blanks. It cost the owner
// a weather key, two location presets, four radar presets and both WiFi
// profiles, because a single screens=1 written from safe mode took the whole
// file down with it. Nothing about safe mode is worth a write.
bool saveConfigAllowed();

// aa:bb:cc:dd:ee:ff, or empty when nothing is remembered. A BSSID is public -
// it is in every beacon the access point sends - so it is fine in the reply.
String bssidText(const uint8_t* b) {
    bool any = false;
    for (uint8_t i = 0; i < 6; ++i) any = any || b[i] != 0;
    if (!any) return String();
    char out[18];
    snprintf(out, sizeof(out), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return String(out);
}

bool bssidParse(const char* text, uint8_t* out) {
    unsigned v[6];
    if (text == nullptr || sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (uint8_t i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

bool saveConfig() {
    if (!saveConfigAllowed()) return false;
    if (!fsMounted && !LittleFS.begin()) return false;
    fsMounted = true;
    JsonDocument doc;
    doc["ssid"] = cfg.ssid;
    doc["pass"] = cfg.pass;
    {
        JsonArray arr = doc["wifi_profiles"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.wifiProfileCount; ++i) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = cfg.wifiProfiles[i].ssid;
            o["pass"] = cfg.wifiProfiles[i].pass;
        }
    }
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
    {
        JsonArray on = doc["screens_on"].to<JsonArray>();
        for (uint8_t i = 0; i < SCREEN_COUNT; ++i) {
            if (cfg.screens & (1U << i)) on.add(SCREEN_KEYS[i]);
        }
    }
    doc["album_interval_seconds"] = cfg.albumIntervalSeconds;
    doc["wifi_channel"] = cfg.wifiChannel;
    doc["wifi_bssid"] = bssidText(cfg.wifiBssid);
    doc["radar_lat"] = cfg.radarLat;
    doc["radar_lon"] = cfg.radarLon;
    doc["radar_range_km"] = cfg.radarRangeKm;
    doc["radar_poll_sec"] = cfg.radarPollSec;
    doc["radar_min_alt_ft"] = cfg.radarMinAltFt;
    doc["radar_up_deg"] = cfg.radarUpDeg;
    doc["radar_routes"] = cfg.radarRoutes;
    doc["radar_bg"] = cfg.radarBg;
    doc["radar_preset"] = cfg.radarPreset;
    emitWeatherPresets(doc);
    emitRadarPresets(doc);
    {
        JsonArray order = doc["screen_order"].to<JsonArray>();
        for (uint8_t i = 0; i < SCREEN_COUNT; ++i) order.add(SCREEN_KEYS[cfg.screenOrder[i]]);
    }
    doc["theme_interval_seconds"] = cfg.themeIntervalSeconds;
    doc["ow_key"] = cfg.owKey;
    doc["fc_preset_idx"] = cfg.fcPresetIdx;
    emitFcPresets(doc);
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

// True once the config file has actually been read. The sweep refuses to run
// before that, since the defaults claim nothing is in use.
// Defined far below; the weather fetch needs them earlier so it can let go of
// the pictures it is holding open before asking for heap.
void radarBgRelease();
void bordRelease();

bool configLoaded = false;
// Whether this firmware holds a password for a network - the WiFi card's own
// field, or any saved profile naming it. Says only yes or no: the page needs
// to know a password exists so it can show that instead of an empty box, and
// nothing more than that should ever cross the wire.
bool wifiPasswordKnown(const String& ssid);
// Set by anything that changes what is on the filesystem, so the cached
// LittleFS.info below is re-read on the next look. Without it a delete read
// as a failure: the space came back but the gauge held the old number for
// up to thirty seconds, which looks exactly like nothing having happened.
extern bool fsInfoStale;
// Every removal goes through here, so a caller cannot forget to say it
// changed something.
bool fsRemove(const String& path);
// Defined with the album, far below; /status needs the same cached numbers.
void albumFsInfo(size_t& total, size_t& used);
size_t albumBytes();
uint8_t albumOrphans(String* out, uint8_t cap, bool* more);
// The heap as it stood when setup finished: the honest 100 percent for a RAM
// gauge. The chip has no "total heap" to ask for - what is left after the
// statics and the boot-time allocations is simply whatever it is, and it
// changes with every build, so it is measured rather than assumed.
uint32_t heapAtBoot = 0;

bool wifiPasswordKnown(const String& ssid) {
    if (ssid.length() == 0) return false;
    if (cfg.ssid == ssid && cfg.pass.length() > 0) return true;
    for (uint8_t i = 0; i < cfg.wifiProfileCount; ++i) {
        if (ssid == cfg.wifiProfiles[i].ssid && cfg.wifiProfiles[i].pass[0] != 0) return true;
    }
    return false;
}

void loadConfig() {
    cfg.ssid = WiFi.SSID();
    if (!fsMounted && !LittleFS.begin()) return;
    fsMounted = true;
    if (!LittleFS.exists(CONFIG_PATH)) {
        // A device with no config yet has nothing to contradict, so what is on
        // disk is authoritative by default.
        configLoaded = true;
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
    loadWifiProfiles(doc["wifi_profiles"], false);
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
    // Names first, numbers second. A config written before this change has only
    // the number, and reading it once is what migrates it - the next save is in
    // names. A name this firmware does not know is skipped rather than guessed
    // at, which is the whole point of writing names.
    if (doc["screens_on"].is<JsonArrayConst>()) {
        uint16_t mask = 0;
        for (JsonVariantConst v : doc["screens_on"].as<JsonArrayConst>()) {
            const int8_t id = screenByKey(v.as<const char*>());
            if (id >= 0) mask |= static_cast<uint16_t>(1U << id);
        }
        cfg.screens = mask & SCREEN_MASK_ALL;
    } else {
        cfg.screens = static_cast<uint16_t>(doc["screens"] | cfg.screens) & SCREEN_MASK_ALL;
    }
    cfg.albumIntervalSeconds = doc["album_interval_seconds"] | cfg.albumIntervalSeconds;
    cfg.wifiChannel = doc["wifi_channel"] | cfg.wifiChannel;
    if (doc["wifi_bssid"].is<const char*>()) bssidParse(doc["wifi_bssid"], cfg.wifiBssid);
    cfg.radarLat = doc["radar_lat"] | cfg.radarLat;
    cfg.radarLon = doc["radar_lon"] | cfg.radarLon;
    cfg.radarRangeKm = constrain(static_cast<uint16_t>(doc["radar_range_km"] | cfg.radarRangeKm), 2, 400);
    cfg.radarPollSec = constrain(static_cast<uint16_t>(doc["radar_poll_sec"] | cfg.radarPollSec), 5, 600);
    cfg.radarMinAltFt = doc["radar_min_alt_ft"] | cfg.radarMinAltFt;
    cfg.radarUpDeg = static_cast<uint16_t>(doc["radar_up_deg"] | cfg.radarUpDeg) % 360;
    cfg.radarRoutes = doc["radar_routes"] | cfg.radarRoutes;
    {
        const char* bg = doc["radar_bg"] | cfg.radarBg.c_str();
        cfg.radarBg = albumIdOk(bg) ? bg : "";
    }
    cfg.radarPreset = doc["radar_preset"] | cfg.radarPreset;
    loadWeatherPresets(doc["weather_presets"]);
    loadRadarPresets(doc["radar_presets"]);
    cfg.owKey = doc["ow_key"] | cfg.owKey;
    loadFcPresets(doc["fc_presets"]);
    cfg.fcPresetIdx = doc["fc_preset_idx"] | cfg.fcPresetIdx;
    if (cfg.fcPresetIdx >= cfg.fcPresetCount) cfg.fcPresetIdx = 0;
    // A name pointing at a place that is no longer in the list means nothing,
    // and would come back to life the day somebody reuses that name for
    // somewhere else. Checked after the list is read, because that is when the
    // answer is knowable.
    if (cfg.radarPreset.length() > 0) {
        bool found = false;
        for (uint8_t i = 0; i < cfg.radarPresetCount && !found; ++i) {
            found = (cfg.radarPreset == cfg.radarPresets[i].name);
        }
        if (!found) cfg.radarPreset = "";
    }
    cfg.albumIntervalSeconds = constrain(cfg.albumIntervalSeconds, THEME_INTERVAL_MIN_S, THEME_INTERVAL_MAX_S);
    if (doc["screen_order"].is<JsonArray>()) {
        // Either form: names as written now, numbers as written before. Anything
        // unrecognised drops out, and setScreenOrder appends whatever the file
        // did not mention - which is how a newly added screen finds a place.
        uint8_t wanted[SCREEN_COUNT];
        size_t n = 0;
        for (JsonVariant v : doc["screen_order"].as<JsonArray>()) {
            if (n >= SCREEN_COUNT) break;
            const int8_t id = v.is<const char*>() ? screenByKey(v.as<const char*>())
                                                  : static_cast<int8_t>(v.as<unsigned int>());
            if (id >= 0 && id < SCREEN_COUNT) wanted[n++] = static_cast<uint8_t>(id);
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
    configLoaded = true;
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

// `expect` is what the reply is actually likely to weigh. It used to reserve a
// flat fourteen kilobytes for everything, which for the nowcast is nine times
// too much and for the forecast two and a half - and that is the same heap the
// forecast's parser has to grow into moments later. The value only has to be
// close: too small costs a realloc, too large costs the parse.
String httpGetBody(const String& path, size_t expect) {
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
    body.reserve(expect);
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

int mapWeatherCode(int sky, int pty, bool lgt) {
    if (lgt) return 95;   // weatherIconSlot already sends 95 to the storm icon
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
    JsonDocument filter;
    filter["response"]["body"]["items"]["item"][0]["category"] = true;
    filter["response"]["body"]["items"]["item"][0]["fcstDate"] = true;
    filter["response"]["body"]["items"]["item"][0]["fcstTime"] = true;
    filter["response"]["body"]["items"]["item"][0]["fcstValue"] = true;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        weather.status = String("forecast parse failed: ") + err.c_str();
        return;
    }
    for (auto& item : weather.fcst) item = ForecastItem{};
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
        // atof, not atoi: LGT arrives as a decimal the way RN1 beside it does,
        // and truncating would read every value below one as no lightning.
        else if (!strcmp(cat, "LGT")) weather.fcst[slot].lgt = atof(val) > 0.0f;
        else if (!strcmp(cat, "REH")) weather.fcst[slot].humidity = atoi(val);
        else if (!strcmp(cat, "RN1") || !strcmp(cat, "PCP")) weather.fcst[slot].rain = val;
    }

    // The nowcast the current conditions come from has no SKY in it - the
    // observation reports precipitation type, not cloud cover - so weather.sky
    // was never assigned and kept its initial value, which every icon chooser
    // reads as clear. That is why the dashboard showed a sun whatever the
    // weather was doing. The nearest forecast slot is the current hour, so its
    // cloud cover is what "now" means here.
    // Cleared before the sweep, unlike sky. A stale cloud is a cloud nobody
    // looks twice at; a stale thunderstorm outranks every other condition and
    // would sit on the dial for as long as the fetch kept failing.
    weather.lgt = false;
    for (size_t i = 0; i < (sizeof(weather.fcst) / sizeof(weather.fcst[0])); ++i) {
        if (!weather.fcst[i].valid) continue;
        weather.sky = weather.fcst[i].sky;
        weather.lgt = weather.fcst[i].lgt;
        break;
    }
}

size_t validForecastCount() {
    size_t count = 0;
    for (const auto& item : weather.fcst) {
        if (item.valid) ++count;
    }
    return count;
}

bool refreshWeather() {
    // Stamped before the guards, not after. A call these refuse is still a call,
    // and if it does not move the clock the scheduler below sees the retry as
    // permanently overdue and comes straight back - which is what a device with
    // no KMA key set did, on every pass of loop().
    lastWeatherTryMs = millis();
    if (!cfg.weatherEnabled) {
        weather.status = "disabled";
        if (weatherFailStreak < 7) ++weatherFailStreak;
        return false;
    }
    if (cfg.kmaKey.length() == 0) {
        weather.status = "KMA key missing";
        if (weatherFailStreak < 7) ++weatherFailStreak;
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        weather.status = "wifi disconnected";
        if (weatherFailStreak < 7) ++weatherFailStreak;
        return false;
    }
    weather.fetching = true;
    weather.status = "fetching";
    String ncstDate, ncstTime, fcstDate, fcstTime;
    makeUltraBases(ncstDate, ncstTime, fcstDate, fcstTime);
    const String base = "/api/typ02/openApi/VilageFcstInfoService_2.0";
    // Ten rows of nowcast measure about 1.5 KB.
    String currentPath = base + "/getUltraSrtNcst?pageNo=1&numOfRows=10&dataType=JSON&base_date=" + ncstDate +
                         "&base_time=" + ncstTime + "&nx=" + String(cfg.nx) + "&ny=" + String(cfg.ny) +
                         "&authKey=" + cfg.kmaKey;
    String currentBody = httpGetBody(currentPath, 2560);
    if (currentBody.length() == 0) {
        weather.status = "current fetch failed";
        weather.fetching = false;
        return false;
    }
    parseCurrent(currentBody);
    // Hand the nowcast's buffer back before asking for the forecast. httpGetBody
    // reserves fourteen kilobytes whatever the reply turns out to weigh - the
    // nowcast is about one and a half - and holding two of those at once leaves
    // the forecast's document nothing to grow into. It fitted for months and
    // then stopped: the parse began reporting NoMemory with thirty kilobytes
    // free, every one of them already spoken for.
    currentBody = String();
    currentPath = String();
    // The radar keeps its background image open between draws, and the Borduhr
    // its dial, and each of those files holds a filesystem cache in the same
    // heap the handshake wants. Nothing is being drawn during a fetch, so both
    // can wait. Missing this one cost the radar its TLS buffer once already.
    radarBgRelease();
    bordRelease();
    // Built here rather than above: it carries the API key and has no business
    // sitting in the heap across the nowcast's fetch and parse. Forty rows of
    // forecast measured 5,524 bytes against the live API, so eight kilobytes
    // covers a fatter day without reserving the parser's room.
    //
    // Forty is also exactly enough, and not by luck: the reply is ordered by
    // category, and the six this firmware reads - LGT, PTY, RN1, SKY, T1H, REH
    // - come first, six hours each for thirty-six rows. Measured against the
    // live API on 2026-08-31. Sixty rows would bring the wind categories too
    // and weigh 8,148 bytes, which leaves the reserve above forty-four bytes of
    // room, so this number is load bearing in both directions.
    const String forecastPath = base + "/getUltraSrtFcst?pageNo=1&numOfRows=40&dataType=JSON&base_date=" +
                                fcstDate + "&base_time=" + fcstTime + "&nx=" + String(cfg.nx) +
                                "&ny=" + String(cfg.ny) + "&authKey=" + cfg.kmaKey;
    String forecastBody = httpGetBody(forecastPath, 8192);
    bool forecastOk = false;
    if (forecastBody.length() > 0) {
        parseForecast(forecastBody);
        forecastOk = validForecastCount() > 0;
    } else {
        weather.status = "forecast fetch failed";
    }
    tm t{};
    if (localTime(t)) weather.updated = two(t.tm_hour) + ":" + two(t.tm_min);
    weather.valid = !isnan(weather.temp);
    if (!weather.valid) {
        weather.status = "no data";
    } else if (!forecastOk) {
        if (weather.status == "fetching") weather.status = "forecast no data";
    } else {
        weather.status = "ok";
    }
    weather.fetching = false;
    const bool got = weather.valid && forecastOk;
    if (got) lastWeatherMs = millis();
    weatherFailStreak = got ? 0 : static_cast<uint8_t>(weatherFailStreak < 7 ? weatherFailStreak + 1 : 7);
    return got;
}

// What to do when the device cannot join WiFi. Everything about it is on the
// panel, because the panel is the only thing left that works: the network it
// puts up, the address to open, and what to change once you are in. It used to
// show the last picture it had, which looks exactly like a clock that is fine.
void drawOfflineScreen() {
    // TFT_eSPI's own colours rather than the radar's: those are defined further
    // down the file, and this screen has to come before everything that can
    // fail to reach the network.
    tft.fillScreen(TFT_BLACK);
    drawCenteredText(16, F("WiFi \uc5f0\uacb0 \uc548\ub428"), 2, TFT_YELLOW, TFT_BLACK, 0, SCREEN_W);

    drawCenteredText(58, F("\uc544\ub798 \ubb34\uc120\ub9dd\uc5d0 \uc811\uc18d\ud55c \ub4a4"), 1,
                     TFT_LIGHTGREY, TFT_BLACK, 0, SCREEN_W);
    // AP_SSID is a pointer, not a literal, so it cannot go through F().
    drawCenteredText(80, String(AP_SSID), 2, TFT_WHITE, TFT_BLACK, 0, SCREEN_W);

    drawCenteredText(120, F("\ube0c\ub77c\uc6b0\uc800\uc5d0\uc11c \uc544\ub798 \uc8fc\uc18c\ub85c"), 1,
                     TFT_LIGHTGREY, TFT_BLACK, 0, SCREEN_W);
    drawCenteredText(142, F("http://192.168.4.1"), 2, TFT_WHITE, TFT_BLACK, 0, SCREEN_W);

    drawCenteredText(182, F("\ub4e4\uc5b4\uac00 WiFi \ub97c \ub2e4\uc2dc \uc815\ud558\uc138\uc694"), 1,
                     TFT_LIGHTGREY, TFT_BLACK, 0, SCREEN_W);
    drawCenteredText(208, F("\uc554\ud638 0000"), 1, TFT_DARKGREY, TFT_BLACK, 0, SCREEN_W);
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
    tft.drawString("AP  " + (apRunning ? WiFi.softAPIP().toString() : String("off")), 10, 124, 2);
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
        input.weather.currentWeatherCode = mapWeatherCode(weather.sky, weather.pty, weather.lgt);
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
            input.weather.forecast[i].weatherCode = mapWeatherCode(src.sky, src.pty, src.lgt);
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
        dashboardShowsIp = false;
        return;
    }

    const String ip = WiFi.localIP().toString() == "0.0.0.0" ? String("") : WiFi.localIP().toString();
    if (cacheIp != ip) {
        tft.fillRect(8, 4, 122, 25, LCD_BLACK);
        if (ip.length()) drawTextAt(8, 4, trimTextToWidth(ip, 1, 122), 1, rgb(36, 40, 44), LCD_BLACK);
        cacheIp = ip;
    }
    dashboardShowsIp = ip.length() > 0;

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
    const String path = String("/weather-icons/") + iconSlot(weather.sky, weather.pty, weather.lgt) + ".bmp";
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
// Sixteen was the right number when a photo cost 118 KB of raw pixels and the
// filesystem held little more than that. A JPEG costs about 16 KB, so sixteen
// left a megabyte unusable. Sixty-four puts the limit back where it belongs -
// on free space - for any photo above roughly 15 KB, and the list is no longer
// built in one heap block, so the count no longer drives peak memory.
constexpr uint8_t ALBUM_MAX = 64;
constexpr int16_t ALBUM_ROWS_PER_READ = 8;  // 3840 B, well inside the free heap

const char* const ALBUM_DIR = "/album";
const char* const ALBUM_INDEX = "/album/index.json";

struct AlbumEntry {
    String id;
    String name;
    bool on = true;
};

AlbumEntry albumEntries[ALBUM_MAX];
// Whether the last posted list had entries rejected by validation - the
// signal that the page and the filesystem disagree and nothing should be
// deleted on this pass.
bool albumPostDropped = false;
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
        wdtYield();   // one exists() per photo, at boot, with no yield of its own
        const String id = item["id"] | "";
        if (!albumIdOk(id)) continue;
        // A manifest entry without its pixels would draw a blank screen, so the
        // file is the authority on what exists. Both formats count: this line
        // asked only about .rgb after photos became JPEG, so every photo was
        // dropped from the list at boot. The empty list was then written back
        // over the index, and the sweep that used to run on the next post took
        // the files too. That is where the owner's photos went.
        if (!LittleFS.exists(albumPath(id, ".jpg")) &&
            !LittleFS.exists(albumPath(id, ".rgb"))) continue;
        albumEntries[albumCount].id = id;
        albumEntries[albumCount].name = item["name"] | id;
        albumEntries[albumCount].on = item["on"] | true;
        ++albumCount;
    }
}

// A photo file lives only while the index points at it - the same rule the
// radar's background images follow, learned the same way: an index replaced
// while the page could not show the old entries left eight orphaned JPEGs on
// a filesystem that had just been fought back from 96 percent. Names are
// collected before anything is removed, because deleting entries out of a
// directory while walking it is not something LittleFS promises anything
// about. A dozen per pass; the next save collects the rest.
// Names the files under /album that no index entry claims. It only reports;
// nothing here deletes. This used to run automatically at the end of every
// album post and delete what it found, which cost the owner seven photos: a
// page showing a stale list posts that list, every entry in it validates, and
// the sweep quietly removes the photos the stale list had never heard of. A
// reorder click should not be able to destroy a photo, so the deleting now
// lives behind its own endpoint that someone has to ask for.
// Every byte under /album, orphans included, in one directory pass. The page
// used to present the whole filesystem's usage on the album line, which read
// as though an empty album were occupying 900 KB of the device.
size_t albumBytes() {
    if (!fsMounted) return 0;
    size_t sum = 0;
    Dir dir = LittleFS.openDir(ALBUM_DIR);
    while (dir.next()) sum += dir.fileSize();
    return sum;
}

// Returns how many orphans were written to `out`; sets `more` when the walk
// stopped at the cap with files still unexamined, so a caller reporting "0
// left" after a delete is telling the truth rather than reporting its own
// buffer size back at itself.
uint8_t albumOrphans(String* out, uint8_t cap, bool* more) {
    if (more) *more = false;
    if (!fsMounted) return 0;
    uint8_t n = 0;
    Dir dir = LittleFS.openDir(ALBUM_DIR);
    while (dir.next()) {
        if (n >= cap) {
            if (more) *more = true;
            break;
        }
        String name = dir.fileName();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name == "index.json") continue;
        const int dot = name.lastIndexOf('.');
        if (dot <= 0) continue;
        if (albumFind(name.substring(0, dot)) >= 0) continue;
        out[n++] = name;
    }
    return n;
}

bool albumSaveIndex() {
    if (!fsMounted && !LittleFS.begin()) return false;
    fsMounted = true;
    if (!LittleFS.exists(ALBUM_DIR)) LittleFS.mkdir(ALBUM_DIR);
    File f = LittleFS.open(ALBUM_INDEX, "w");
    if (!f) return false;
    // One photo at a time through a small document rather than the whole list
    // through a large one. A name is arbitrary text from a filename, so it goes
    // through the serializer to be escaped rather than being pasted in.
    f.print("{\"photos\":[");
    for (uint8_t i = 0; i < albumCount; ++i) {
        if (i) f.print(',');
        JsonDocument item;
        item["id"] = albumEntries[i].id;
        item["name"] = albumEntries[i].name;
        item["on"] = albumEntries[i].on;
        serializeJson(item, f);
    }
    f.print("]}");
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
// TJpg_Decoder hands back one MCU block at a time - 16x16 host-order pixels -
// and this puts each straight on the panel. Everything else in this firmware
// runs the panel with setSwapBytes(false) and big-endian files, so the flag is
// raised for the duration of a decode and dropped after.
bool albumJpgBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= SCREEN_H) return false;   // past the panel: tell the decoder to stop
    tft.pushImage(x, y, w, h, bitmap);
    // The only place the watchdog can be fed while a photo decodes. drawFsJpg
    // blocks from first byte to last and calls this once per 16x16 block, so
    // this callback is the decode's only opening. The raw path has fed it per
    // row since it was written; the JPEG path was added later and did not, and
    // a decode that normally takes 120 ms took longer than eight seconds once
    // the radar had the heap fragmented - three times over, until the device
    // gave up and booted to safe mode.
    wdtYield();
    return true;
}

// Photos are stored as JPEG now - about 20 KB against the 115 KB of raw
// RGB565, which is what filled the filesystem to 96 percent. The decoder costs
// ~3.5 KB of heap while it runs and nothing while it does not. Raw .rgb photos
// from before the change still render through the old path below, so nothing
// already on a device is lost by updating.
bool albumRenderJpg(const String& id) {
    bootMarkDetail(static_cast<uint8_t>(albumCursor < 0 ? 0xFF : albumCursor));
    bootMarkWork(W_ALB_EXISTS);
    const String path = albumPath(id, ".jpg");
    if (!fsMounted || !LittleFS.exists(path)) return false;
    bootMarkWork(W_ALB_JPG);
    const uint32_t start = micros();
    wdtYield();   // enter the decode with a full budget
    tft.setSwapBytes(true);
    const JRESULT res = TJpgDec.drawFsJpg(0, 0, path.c_str(), LittleFS);
    tft.setSwapBytes(false);
    wdtYield();
    albumFrameUs = micros() - start;
    return res == JDR_OK;
}

bool albumRender(const String& id) {
    if (albumRenderJpg(id)) return true;
    bootMarkWork(W_ALB_RAW);
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
        bootMarkWork(W_ALB_BAND);
        analogBandEnd();  // hand the band memory back before allocating the row buffer
        albumDrawn = false;
    }

    // The decode hung the last boot, so it does not get a second try. Nothing
    // in TJpgDec can be interrupted once it starts - the callback that could
    // feed the watchdog is not reached until a whole block is decoded - so the
    // only place to stop is before entering it.
    if (workKilledLastBoot(W_ALB_JPG) || workKilledLastBoot(W_ALB_RAW)) {
        if (!albumDrawn) {
            albumShowMessage(F("Photo album"), F("skipped: hung last boot"));
            albumDrawn = true;
        }
        return;
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
        bootMarkWork(W_ALB_MSG);
        albumShowMessage(F("Photo album"), F("read failed"));
    }
    albumDrawn = true;
}

// ---------------------------------------------------------------------------
// The week ahead
//
// Forty three-hourly entries come back; six calendar days come out. Each day
// keeps the picture it will wear, the word for it, its average humidity and the
// warmest it is expected to feel - which is the number someone dressing for the
// day actually wants, not the mean of a night and an afternoon.
//
// The entry nearest three in the afternoon speaks for the day. A forecast at
// four in the morning is not what "Wednesday" means to anyone.
// ---------------------------------------------------------------------------

ForecastDay fcDays[FC_DAYS];

// What the nowcast said when it was last sampled. Held apart from fcDays so an
// OpenWeather fetch cannot overwrite it and so the two hours are counted from
// the sample rather than from whichever fetch happened to land last.
struct ForecastNow {
    bool valid = false;
    int sky = 1;
    int pty = 0;
    bool lgt = false;
    uint8_t humidity = 0;
    int16_t feels = 0;
};
ForecastNow fcNow;
uint32_t fcNowLastMs = 0;   // 0 until the first attempt, as fcLastMs is
uint32_t fcNowWaitMs = 0;
String fcStatus = "not fetched";
uint32_t fcLastMs = 0;
uint32_t fcNextMs = 0;   // when the next attempt is due; failure brings it forward
uint32_t fcFetchMs = 0;
// Bumped whenever the days change. The screen compares this rather than
// rebuilding a string of every field once a second to discover that nothing
// has moved.
uint32_t fcRevision = 0;
String fcCity;

const ForecastPreset& fcPreset() {
    const uint8_t i = cfg.fcPresetIdx < cfg.fcPresetCount ? cfg.fcPresetIdx : 0;
    return cfg.fcPresets[i];
}

uint8_t fcValidCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < FC_DAYS; ++i) if (fcDays[i].valid) ++n;
    return n;
}

// OpenWeather's own Korean is serviceable and not ours - it answers 온흐림 and
// 튼구름 where this device says 흐림. The icon code carries the same meaning
// without the vocabulary clash, and it is two characters instead of a string.
// The words match skyLook's, which is what the nowcast row draws: the two
// sources must not label the same sky differently on adjacent lines.
const char* fcWord(const char* icon) {
    if (icon == nullptr || icon[0] == 0) return "";
    if (!strncmp(icon, "01", 2)) return "맑음";
    if (!strncmp(icon, "02", 2)) return "구름";
    if (!strncmp(icon, "03", 2)) return "구름";
    if (!strncmp(icon, "04", 2)) return "흐림";
    if (!strncmp(icon, "09", 2) || !strncmp(icon, "10", 2)) return "비";
    if (!strncmp(icon, "11", 2)) return "뇌우";
    if (!strncmp(icon, "13", 2)) return "눈";
    if (!strncmp(icon, "50", 2)) return "안개";
    return "흐림";
}

// KMA's summer 체감온도, which needs only temperature and humidity - the winter
// half of the formula is wind chill and the six categories this firmware asks
// for do not include wind speed. Below 25℃ the reading is the temperature
// itself, which is what the agency publishes outside the summer months.
float fcFeelsLike(float tempC, int humidity) {
    if (isnan(tempC) || humidity < 0 || tempC < 25.0f) return tempC;
    const float rh = static_cast<float>(humidity);
    const float tw = tempC * atanf(0.151977f * sqrtf(rh + 8.313659f)) + atanf(tempC + rh) -
                     atanf(rh - 1.67633f) +
                     0.00391838f * powf(rh, 1.5f) * atanf(0.023101f * rh) - 4.686035f;
    return -0.2442f + 0.55399f * tw + 0.45535f * tempC - 0.0022f * tw * tw +
           0.00278f * tw * tempC + 3.0f;
}

// The same six bitmaps the dashboard draws. OpenWeather numbers its icons the
// way the manufacturer's did, so this is a rename rather than a translation.
const char* fcSlot(const char* icon) {
    if (icon == nullptr || icon[0] == 0) return "cloudy";
    if (!strncmp(icon, "01", 2)) return "clear";
    if (!strncmp(icon, "09", 2) || !strncmp(icon, "10", 2)) return "umbrella";
    if (!strncmp(icon, "11", 2)) return "storm";
    if (!strncmp(icon, "13", 2)) return "snow";
    if (!strncmp(icon, "50", 2)) return "fog";
    return "cloudy";
}

// Accumulated while the reply is read, one bucket per calendar day.
struct FcBucket {
    int32_t ymd = 0;        // yyyymmdd of the local day, 0 when unused
    uint32_t humSum = 0;
    uint16_t humCount = 0;
    float feelMax = -999.0f;
    int16_t bestGap = 32767;   // how far the best entry is from 15:00, in minutes
    char icon[4] = "";
    time_t stamp = 0;
};

// Reads straight off the socket rather than into a String. The reply is about
// seventeen kilobytes and the heap has twenty-six; buffering it whole and then
// parsing it needs both at once, which this chip does not have. The filter
// keeps four fields per entry and throws the rest away as it goes.
bool forecastFetch() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (cfg.owKey.length() == 0) {
        fcStatus = "no api key";
        return false;
    }
    const uint32_t block = ESP.getMaxFreeBlockSize();
    if (block < FC_MIN_BLOCK) {
        fcStatus = "heap too low: " + String(block);
        fcLastMs = millis();
        fcNextMs = fcLastMs + FC_RETRY_MS;
        return false;
    }
    const ForecastPreset& p = fcPreset();
    const uint32_t start = millis();
    radarBgRelease();
    bordRelease();

    String url = "http://";
    url += OW_HOST;
    url += "/data/2.5/forecast?units=metric&lat=";
    url += String(p.lat, 4);
    url += "&lon=";
    url += String(p.lon, 4);
    url += "&appid=";
    url += cfg.owKey;

    bool ok = false;
    {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(6000);
        http.setReuse(false);
        // Same pairing the other two feeds use: HTTP/1.0 has no chunked
        // encoding, and getStream() hands the parser the raw socket.
        http.useHTTP10(true);
        if (http.begin(client, url)) {
            http.addHeader("Accept", "application/json");
            http.addHeader("Accept-Encoding", "identity");
            const int code = http.GET();
            if (code == HTTP_CODE_OK) {
                JsonDocument filter;
                filter["city"]["timezone"] = true;
                filter["city"]["name"] = true;
                JsonObject item = filter["list"].add<JsonObject>();
                item["dt"] = true;
                item["main"]["humidity"] = true;
                item["main"]["feels_like"] = true;
                item["weather"][0]["icon"] = true;

                JsonDocument doc;
                const DeserializationError err =
                    deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
                if (err) {
                    fcStatus = String("parse: ") + err.c_str();
                } else {
                    const int32_t tz = doc["city"]["timezone"] | 32400;
                    fcCity = doc["city"]["name"] | "";
                    FcBucket bucket[FC_DAYS];
                    uint8_t used = 0;
                    for (JsonObjectConst it : doc["list"].as<JsonArrayConst>()) {
                        const time_t utc = static_cast<time_t>(it["dt"] | 0);
                        if (utc == 0) continue;
                        const time_t local = utc + tz;
                        tm t{};
                        gmtime_r(&local, &t);   // already shifted, so read it as UTC
                        const int32_t ymd = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
                        int8_t slot = -1;
                        for (uint8_t i = 0; i < used; ++i) if (bucket[i].ymd == ymd) slot = static_cast<int8_t>(i);
                        if (slot < 0) {
                            // Forty three-hourly entries span six or seven local
                            // days; the screen shows four and the rest are meant
                            // to be dropped here.
                            if (used >= FC_DAYS) continue;
                            slot = static_cast<int8_t>(used++);
                            bucket[slot].ymd = ymd;
                        }
                        FcBucket& b = bucket[slot];
                        b.humSum += static_cast<uint32_t>(it["main"]["humidity"] | 0);
                        ++b.humCount;
                        const float feel = it["main"]["feels_like"] | -999.0f;
                        if (feel > b.feelMax) b.feelMax = feel;
                        const int16_t gap = static_cast<int16_t>(abs((t.tm_hour * 60 + t.tm_min) - 900));
                        if (gap < b.bestGap) {
                            b.bestGap = gap;
                            strlcpy(b.icon, it["weather"][0]["icon"] | "", sizeof(b.icon));
                            b.stamp = local;
                        }
                    }
                    for (uint8_t i = 0; i < FC_DAYS; ++i) {
                        fcDays[i] = ForecastDay{};
                        // feelMax still at its sentinel means the reply carried
                        // humidity for this day but no feels_like, and the row
                        // would print -999 into a column sized for two digits.
                        if (i >= used || bucket[i].humCount == 0) continue;
                        if (bucket[i].feelMax < -900.0f) continue;
                        tm t{};
                        gmtime_r(&bucket[i].stamp, &t);
                        fcDays[i].valid = true;
                        fcDays[i].month = static_cast<uint8_t>(t.tm_mon + 1);
                        fcDays[i].day = static_cast<uint8_t>(t.tm_mday);
                        // tm_wday counts from Sunday; the labels start at Monday.
                        fcDays[i].weekday = static_cast<uint8_t>((t.tm_wday + 6) % 7);
                        strlcpy(fcDays[i].icon, bucket[i].icon, sizeof(fcDays[i].icon));
                        fcDays[i].humidity = static_cast<uint8_t>(bucket[i].humSum / bucket[i].humCount);
                        fcDays[i].feels = static_cast<int16_t>(lroundf(bucket[i].feelMax));
                    }
                    ok = fcValidCount() > 0;
                    fcStatus = ok ? String("ok") : String("no days");
                }
            } else {
                fcStatus = "http " + String(code);
            }
            http.end();
        } else {
            fcStatus = "begin failed";
        }
    }
    fcFetchMs = millis() - start;
    fcLastMs = millis();
    fcNextMs = fcLastMs + (ok ? FC_REFRESH_MS : FC_RETRY_MS);
    ++fcRevision;
    return ok;
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

// Sizes radarAc, radarDrawnCache and radarPrev between them - the three biggest
// things this firmware puts in static RAM, which is heap that never existed. It
// was 24, and the label declutter cannot place anywhere near that many: a 40 km
// ring, four times the usual setting, held ten. Sixteen keeps room for a busy
// day and hands about 830 bytes back.
constexpr uint8_t RADAR_MAX_AIRCRAFT = 16;
constexpr uint8_t RADAR_MAX_AIRPORTS = 6;
// Defined with the background machinery, after the screen code; the fetches
// sit earlier in the file and need to let the file go before dialling out.
void radarBgRelease();
bool radarBgActive();
void radarBgSweep();
void bordRelease();

constexpr const char* ADSB_HOST = "opendata.adsb.fi";
constexpr const char* ADSB_PATH = "/api/v3/lat/";
constexpr const char* ADSB_USER_AGENT = "Mozilla/5.0 (SmallTV)";
// Below this the handshake is not attempted. The original uses free heap; the
// largest contiguous block is the number that actually decides, because that is
// what the TLS buffers need.
//
// Measured, not guessed, and it is tighter than it looks. Fifty-nine polls over
// nineteen minutes on 2026-08-31 recorded the largest block before the fetch
// and again while the client still held its buffers: the difference ran 12,960
// to 17,896 bytes, the top of that at a 30 km radius with six aircraft in the
// reply. That leaves this floor 104 bytes of margin at its worst. It looked
// like double what was needed until it was measured at a radius wide enough to
// matter - do not lower it on the strength of a quiet sky.
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
// What a TLS fetch actually costs in contiguous heap, which is the only number
// that can say whether RADAR_MIN_BLOCK is set anywhere near right. The guard
// refuses without allocating, so a refusal teaches nothing; these are recorded
// on the attempts that go through. blockBefore is taken with the guard, and
// blockLow while the client is still alive and the buffers are still held.
uint32_t radarBlockBefore = 0;
uint32_t radarBlockLow = 0;
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
    // The feed counts in whole nautical miles, so the ring edge needs a margin
    // or a round-down leaves the fetch narrower than the dial. One mile over a
    // plain dial, where the beyond-ring traffic is only a dot; two over a map,
    // where it carries a full label and is worth seeing coming.
    const uint16_t margin = radarBgActive() ? 2 : 1;
    const uint16_t nm = static_cast<uint16_t>(lroundf(km / 1.852f)) + margin;
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
    radarBgRelease();
    bordRelease();
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
        bootMarkWork(W_RADAR_FETCH);
        // Not the last chance - the calls below feed it themselves. This is
        // here so the marker above is the last thing written before a fetch
        // that might not come back, which is what made the reset findable.
        wdtYield();
        const uint32_t block = ESP.getMaxFreeBlockSize();
        if (block < RADAR_MIN_BLOCK && radarBlockRefusals < RADAR_REFUSALS_MAX) {
            ++radarBlockRefusals;
            radarStatus = "heap too low: " + String(block);
            return false;
        }
        radarBlockRefusals = 0;
        radarBlockBefore = block;
        radarBlockLow = block;
        std::unique_ptr<BearSSL::WiFiClientSecure> holder(new BearSSL::WiFiClientSecure());
        BearSSL::WiFiClientSecure* client = holder.get();
        if (client == nullptr) {
            radarStatus = "no client";
            return false;
        }
        // Read-only public feed, and a trust store costs heap this chip does not
        // have to spare. The risk taken is a spoofed aircraft list.
        //
        // Three seconds for the socket and four for the request, which is short
        // - but not for the reason it was written down as. These were cut from
        // eight while hunting a hardware watchdog reset, on the belief that a
        // blocking handshake and GET could not feed the watchdog. They can.
        // ESP8266HTTPClient calls esp_yield() in its wait loops and
        // WiFiClientSecureBearSSL calls optimistic_yield() in its handshake and
        // read loops; both hand control back to the SDK, which is what feeds
        // it. The reset was the album's JPEG decode, which really does block
        // start to finish - see the callback in albumJpgBlock.
        //
        // The short budget stays anyway, for a reason nobody wrote down: the
        // watchdog is fed but the sweep is not. A poll that waits seven seconds
        // freezes the dial for seven seconds, and a poll that cannot finish in
        // that is worth losing. Lengthen these if a slow network starts costing
        // more polls than the pause is worth.
        client->setTimeout(3000);
        client->setInsecure();
        client->setBufferSizes(radarTlsRx, 512);
        // Held across fetches to let BearSSL resume instead of shaking hands
        // from scratch. It does not work, and the reason is worth writing down
        // so nobody spends another afternoon on it.
        //
        // The session is only captured inside WiFiClientSecureCtx::stop, under
        // an if (_handshake_done). This asks in HTTP/1.0, so the server closes
        // first; draining the reply tears the SSL context down and _freeSSL
        // clears that flag, so by the time anything calls stop there is nothing
        // left to save. Adding an explicit client->stop() after http.end() was
        // tried on 2026-08-31 and measured over 36 polls: the median GET stayed
        // at 823 ms against 813 ms without it. The servers are not the problem -
        // both opendata.adsb.fi and api.adsbdb.com resume happily under TLS 1.2,
        // checked from a PC.
        //
        // What it would be worth if someone did fix it: setInsecure() means
        // there is no certificate to verify, so the handshake is not the
        // expensive part it usually is. A resumed handshake measured 155-180 ms
        // against 404 ms for a full one, on a link whose round trip is already
        // 260-300 ms. Perhaps a fifth off a poll, and nothing off the heap -
        // the buffers below are allocated either way.
        static BearSSL::Session tlsSession;
        client->setSession(&tlsSession);

        HTTPClient http;
        // Was 8000. Cut while hunting a watchdog reset that turned out to be
        // the album's JPEG decode; the note that used to sit here said neither
        // call can feed the watchdog, and that is not true - both libraries
        // yield inside their wait loops. Kept short because the screen stops
        // while this blocks, not because the device would reset.
        http.setTimeout(4000);
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
            // Here, not later: the handshake is done and the client still holds
            // its buffers, so this is the low-water mark the guard is meant to
            // protect. After the scope closes they are gone and the reading
            // would say nothing.
            {
                const uint32_t now = ESP.getMaxFreeBlockSize();
                if (now < radarBlockLow) radarBlockLow = now;
            }
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
    radarBgRelease();
    bordRelease();
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
        // Was 8000. Cut while hunting a watchdog reset that turned out to be
        // the album's JPEG decode; the note that used to sit here said neither
        // call can feed the watchdog, and that is not true - both libraries
        // yield inside their wait loops. Kept short because the screen stops
        // while this blocks, not because the device would reset.
        http.setTimeout(4000);
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
// The week ahead - the screen
//
// One row a day, six of them, and the columns line up so the eye can run down
// humidity alone or down the feels-like alone. That is what a week is consulted
// for: finding the day that differs from the rest.
//
// Repainted only when the numbers change. A forecast moves every three hours
// and the fetch is half-hourly, so this screen is still for minutes at a time -
// there is no reason to redraw six bitmaps a second underneath it.
// ---------------------------------------------------------------------------

// Four rows across 240 px, and the arithmetic is tight enough that it has to be
// written down. Measured at size 2 - including the 1 px of tracking both font
// sets put between glyphs, which is easy to leave out of a width and cost this
// screen a revision when trimTextToWidth cut 흐림 down to an ellipsis:
//
//   weekday 18 + date 25 + icon 28 + word 37 + humidity 52 + feels-like 54 = 214
//
// Twenty-six left for two margins and five gaps. Two things follow. The word is
// capped at two syllables by fcWord, because 구름많음 is 72 px and there is no
// 35 to spare. And the per cent sign is drawn at size 1 beside a size-2 number:
// at size 2 it is 20 px against 13, and those seven are the whole margin.
//
// Vertically nothing is held back for the address badge - it covers the bottom
// fifteen pixels for the first three minutes of a boot and then goes for good.
// The rows are three pixels shorter than the spacing alone wants, because
// running them to the edge left the fourth day pressed against it: the gaps
// between rows read as generous and the panel still read as full. Those twelve
// pixels become the bottom margin, sixteen against fifteen at the top.
constexpr int16_t FC_HEAD_Y = 15;
// The 습도 and 체감 column labels stay at size 1 while the title next to them is
// size 2, so they are dropped six pixels to share its baseline.
constexpr int16_t FC_LABEL_Y = 21;
// Four pixels further from the title than the type alone would put it: the
// heading box ends on 38, and a rule three below that read as attached to the
// name rather than as the line under a header. The rows move with it rather
// than closing the gap they had, so the spacing below the rule is unchanged and
// the four pixels come out of the bottom margin - twelve now, against fifteen
// at the top.
constexpr int16_t FC_RULE_Y = 45;
constexpr int16_t FC_ROW_TOP = 49;
constexpr int16_t FC_ROW_H = 48;
// Text and icon sit near the top of their band rather than centred in it; the
// fourth band runs past the panel and only these two offsets keep it on screen.
constexpr int16_t FC_TEXT_DY = 9;
constexpr int16_t FC_ICON_DY = 7;
constexpr int16_t FC_DAY_X = 2;
constexpr int16_t FC_DATE_X = 24;
constexpr int16_t FC_ICON_X = 55;
constexpr int16_t FC_WORD_X = 89;
constexpr int16_t FC_HUM_R = 182;
constexpr int16_t FC_FEEL_R = 238;
// How wide those two columns get at their worst - 100% with the small sign, and
// -15℃. The headings are centred across exactly that span, so moving a right
// edge carries its heading with it rather than leaving it behind.
constexpr int16_t FC_HUM_W = 52;
constexpr int16_t FC_FEEL_W = 54;
// Wider than the 37 px a two-syllable word takes, so the guard only fires if
// the word table ever grows a longer entry - it is there to trim rather than
// overrun, not to trim what already fits.
constexpr int16_t FC_WORD_W = 42;

// What the panel is currently showing. Compared against fcRevision, which the
// fetch bumps - the screen used to rebuild a string of all six days once a
// second in order to find out that nothing had changed.
uint32_t fcDrawnRevision = 0;
uint8_t fcDrawnPreset = 0xFF;
int32_t fcDrawnToday = 0;

void drawForecastCentred(int16_t x0, int16_t x1, int16_t y, const String& text,
                         uint8_t size, uint16_t colour) {
    const int16_t w = measureText(text, size);
    drawTextAt(x0 + ((x1 - x0 - w) / 2), y, text, size, colour, LCD_BLACK);
}

void drawForecastRight(int16_t right, int16_t y, const String& text,
                       uint8_t size, uint16_t colour) {
    drawTextAt(right - measureText(text, size), y, text, size, colour, LCD_BLACK);
}

// A size-2 reading with its unit at size 1, right-aligned as one block. The
// sign sits on the number's baseline - the two font sets are 23 and 17 tall, so
// six pixels down.
void drawForecastUnit(int16_t right, int16_t y, const String& value, const String& unit,
                      uint16_t colour) {
    const int16_t uw = measureText(unit, 1);
    const int16_t vw = measureText(value, 2);
    const int16_t x = static_cast<int16_t>(right - uw - vw - 1);
    drawTextAt(x, y, value, 2, colour, LCD_BLACK);
    drawTextAt(static_cast<int16_t>(x + vw + 1), static_cast<int16_t>(y + 6), unit, 1,
               colour, LCD_BLACK);
}

// Whether the sample may still stand in for today. Asked when the value is
// used, not only when it was taken: a sample admitted one refresh before the
// nowcast went quiet would otherwise keep asserting current conditions for the
// two hours until the next attempt.
bool fcNowUsable() {
    return fcNow.valid && lastWeatherMs != 0 &&
           millis() - lastWeatherMs <= FC_NOW_STALE_MS;
}

// What a row actually shows. Today is a day that is already happening, so it
// comes off the nowcast rather than OpenWeather's three o'clock slot; the rest
// of the week can only be a forecast. Both the panel and /api/forecast go
// through here - reading the stored day directly is what made the API describe
// a screen that was showing something else.
struct ForecastCell {
    const char* slot;
    const char* word;
    uint8_t humidity;
    int16_t feels;
    bool live;
};

ForecastCell forecastCell(const ForecastDay& d, bool isToday) {
    if (isToday && fcNowUsable()) {
        const SkyLook look = skyLook(fcNow.sky, fcNow.pty, fcNow.lgt);
        return {look.slot, look.word, fcNow.humidity, fcNow.feels, true};
    }
    return {fcSlot(d.icon), fcWord(d.icon), d.humidity, d.feels, false};
}

// Copy what the nowcast is saying into today's row. fcRevision is what makes
// the screen notice; without the bump the panel would keep the values it drew
// two hours ago.
void forecastSampleNow() {
    // weather.valid is set from the temperature alone, so a reply carrying T1H
    // and no REH arrives valid with humidity still at its -1 sentinel. Clamping
    // that to zero drew a humidity the agency never published and left it there
    // for two hours, so the whole row goes back to OpenWeather instead.
    const bool fresh = weather.valid && weather.humidity >= 0 && lastWeatherMs != 0 &&
                       millis() - lastWeatherMs <= FC_NOW_STALE_MS;
    const float feels = fresh ? fcFeelsLike(weather.temp, weather.humidity) : NAN;
    fcNowLastMs = millis();
    if (isnan(feels)) {
        fcNowWaitMs = FC_NOW_RETRY_MS;
        // Nothing usable. Give the row back to OpenWeather, and say so on the
        // way past so the screen redraws instead of holding the old numbers.
        if (fcNow.valid) {
            fcNow.valid = false;
            ++fcRevision;
        }
        return;
    }
    fcNow.valid = true;
    fcNow.sky = weather.sky;
    fcNow.pty = weather.pty;
    fcNow.lgt = weather.lgt;
    fcNow.humidity = static_cast<uint8_t>(weather.humidity);
    fcNow.feels = static_cast<int16_t>(lroundf(feels));
    fcNowWaitMs = FC_NOW_MS;
    ++fcRevision;
}

// Today by the device's own clock, as yyyymmdd, or 0 before NTP. The first row
// is only "today" while the data behind it still is: fetches are half-hourly, so
// just after midnight the newest reply can still lead with yesterday.
int32_t fcTodayYmd() {
    const time_t now = time(nullptr);
    if (now < 1700000000) return 0;
    tm t{};
    localtime_r(&now, &t);
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

// Today as a row of its own, built from the device clock. The weather fields
// are left empty deliberately: forecastCell fills them from the nowcast, which
// is the only reason this row exists.
bool forecastTodayRow(ForecastDay& out) {
    const time_t now = time(nullptr);
    if (now < 1700000000) return false;
    tm t{};
    localtime_r(&now, &t);
    out = ForecastDay{};
    out.valid = true;
    out.month = static_cast<uint8_t>(t.tm_mon + 1);
    out.day = static_cast<uint8_t>(t.tm_mday);
    // tm_wday counts from Sunday; the labels start at Monday.
    out.weekday = static_cast<uint8_t>((t.tm_wday + 6) % 7);
    return true;
}

// The rows to draw, today first, assembled once so the panel and the API cannot
// disagree about which days exist.
//
// OpenWeather only returns three-hour slots that are still ahead, so from the
// evening onwards its earliest entry is already tomorrow and today has no
// bucket at all. Left alone, the row carrying the nowcast simply vanished for
// the last hours of every day - the screen went from four days to four days
// with today missing, and the reading the user actually wanted disappeared
// exactly when they were most likely to be looking at it. So when the reply has
// no bucket for today and the nowcast can still speak for it, the row is built
// from the clock instead. It also means the screen has something to say before
// the first OpenWeather fetch of a boot lands.
uint8_t forecastRows(ForecastDay* out, uint8_t max) {
    const int32_t today = fcTodayYmd();
    bool haveToday = false;
    for (uint8_t i = 0; i < FC_DAYS; ++i) {
        const ForecastDay& d = fcDays[i];
        if (!d.valid || today == 0) continue;
        if ((today / 10000) * 10000 + d.month * 100 + d.day == today) haveToday = true;
    }
    uint8_t n = 0;
    ForecastDay synth;
    if (!haveToday && fcNowUsable() && n < max && forecastTodayRow(synth)) out[n++] = synth;
    for (uint8_t i = 0; i < FC_DAYS && n < max; ++i) {
        if (fcDays[i].valid) out[n++] = fcDays[i];
    }
    return n;
}

bool drawForecast(bool force) {
    const int32_t today = fcTodayYmd();
    if (!force && fcDrawnRevision == fcRevision && fcDrawnPreset == cfg.fcPresetIdx &&
        fcDrawnToday == today) {
        return false;
    }
    fcDrawnRevision = fcRevision;
    fcDrawnPreset = cfg.fcPresetIdx;
    fcDrawnToday = today;

    tft.fillScreen(LCD_BLACK);

    // The struct allows eight syllables; without the trim a long name runs
    // under the 습도 heading.
    // 백석동 예보 is 100 px at this size and the 습도 label starts at 128; the trim
    // is what keeps a longer preset name out of it.
    drawTextAt(FC_DAY_X, FC_HEAD_Y, trimTextToWidth(String(fcPreset().name) + " 예보", 2, 124),
               2, rgb(226, 238, 244), LCD_BLACK);
    drawForecastCentred(FC_HUM_R - FC_HUM_W, FC_HUM_R, FC_LABEL_Y, String("습도"), 1,
                        rgb(96, 108, 118));
    drawForecastCentred(FC_FEEL_R - FC_FEEL_W, FC_FEEL_R, FC_LABEL_Y, String("체감"), 1,
                        rgb(96, 108, 118));
    tft.drawFastHLine(2, FC_RULE_Y, 236, rgb(30, 36, 44));

    ForecastDay rows[FC_DAYS];
    const uint8_t rowCount = forecastRows(rows, FC_DAYS);

    if (rowCount == 0) {
        // Size 1, not 2: 키 is in the small glyph set and not the large one,
        // and a single missing glyph sends the whole string to the built-in
        // font, where Hangul comes out as broken bytes.
        const String why = cfg.owKey.length() == 0 ? String("API 키 설정 필요") : fcStatus;
        drawForecastCentred(0, SCREEN_W, 108, why, 1, TFT_WHITE);
        return true;
    }

    static const char* const DOW[7] = {"월", "화", "수", "목", "금", "토", "일"};
    for (uint8_t row = 0; row < rowCount; ++row) {
        const ForecastDay& d = rows[row];
        const int16_t y = FC_ROW_TOP + row * FC_ROW_H;
        if (row) tft.drawFastHLine(2, y - 4, 236, rgb(30, 36, 44));
        // Where a line of size-2 text sits so the row reads as one band.
        const int16_t mid = static_cast<int16_t>(y + FC_TEXT_DY);

        // Only the row whose date really is today, checked against the clock
        // rather than against its position in the list.
        const int32_t rowYmd = today == 0 ? 0
            : (today / 10000) * 10000 + d.month * 100 + d.day;
        const bool isToday = today != 0 && rowYmd == today;
        const ForecastCell cell = forecastCell(d, isToday);

        if (isToday) {
            // A word rather than a weekday, so it takes the space the date
            // number would have had.
            drawTextAt(FC_DAY_X, mid, String("오늘"), 2, rgb(255, 220, 80), LCD_BLACK);
        } else {
            drawTextAt(FC_DAY_X, mid, String(DOW[d.weekday % 7]), 2, rgb(80, 225, 255), LCD_BLACK);
            drawTextAt(FC_DATE_X, mid, two(d.day), 2, rgb(96, 108, 118), LCD_BLACK);
        }

        const String path = sizedIconPath(cell.slot, 28);
        if (fsMounted && LittleFS.exists(path)) {
            drawBmpIcon(tft, path, FC_ICON_X, static_cast<int16_t>(y + FC_ICON_DY), 28);
        }

        drawTextAt(FC_WORD_X, mid, trimTextToWidth(String(cell.word), 2, FC_WORD_W),
                   2, rgb(226, 238, 244), LCD_BLACK);
        drawForecastUnit(FC_HUM_R, mid, String(cell.humidity), "%", rgb(128, 142, 152));
        // The degree sign this used was U+00B0, which is in neither glyph set -
        // so the whole string fell back to the built-in font and came out as
        // broken bytes. The rest of the device draws U+2103, and that is baked.
        drawForecastRight(FC_FEEL_R, mid, String(cell.feels) + String("℃"), 2,
                          rgb(255, 126, 54));
    }
    return true;
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
constexpr uint8_t RADAR_LABEL_SIZE = 2;    // the N marker
// The callsign is set at the same size as the airline above it and the
// altitude below. It was twice that, on the reasoning that it is the line you
// look for - but every label is three lines now, and the big middle one took
// so much of the dial that a crowd became unreadable, which is the opposite of
// standing out.
constexpr uint8_t RADAR_CALLSIGN_SIZE = 1;
// setTextSize only multiplies, so there is no 1.5 to ask for: size 1 is 7 px of
// ink and size 2 is 14. The UI font's small set sits between them at 10, and it
// carries every character the altitude needs, so the second line uses that
// instead of the built-in font.
constexpr int16_t RADAR_ALT_LINE = 17;     // the small set's line height
constexpr uint8_t RADAR_HEADER_SIZE = 2;   // range and count along the top
constexpr int16_t RADAR_LABEL_GAP = 4;     // between the two lines of a label

// Whose label gets first claim when boxes collide. The resolve pass used to
// run nearest-first every time, so of two aircraft flying close together the
// farther one shed its lines on every single poll and never got them back.
// Rotating the starting point one step per poll makes colliding neighbours
// take turns - full label this revolution, bare callsign the next. Aircraft
// with room around them are untouched: order only matters where boxes touch.
uint8_t radarLabelPhase = 0;

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
// An aircraft silhouette rather than a triangle: nose, swept wings and a
// tailplane, rotated to the track, so which way it is going reads at a glance
// without decoding an abstract shape. Built from seven filled triangles in a
// local frame where a is rightward and b is forward; everything stays inside
// the same 14 px marker box the hull already budgets for.
void radarPlaneTri(int16_t x, int16_t y, float trackDeg, uint16_t color) {
    const float th = trackDeg * PI / 180.0f;
    const float ct = cosf(th), st = sinf(th);
    struct P { float a, b; };
    auto sx = [&](P p) { return static_cast<int16_t>(x + lroundf(p.a * ct + p.b * st)); };
    auto sy = [&](P p) { return static_cast<int16_t>(y + lroundf(p.a * st - p.b * ct)); };
    auto tri = [&](P p1, P p2, P p3) {
        tft.fillTriangle(sx(p1), sy(p1), sx(p2), sy(p2), sx(p3), sy(p3), color);
    };
    tri({0, 12}, {-2, 8}, {2, 8});                        // nose
    tri({-2, 8}, {2, 8}, {2, -8});                        // fuselage
    tri({-2, 8}, {2, -8}, {-2, -8});
    tri({-2, 5}, {-11, -4}, {-2, -1});                    // wings, swept back
    tri({2, 5}, {11, -4}, {2, -1});
    tri({-1, -6}, {-6, -10}, {-1, -9});                   // tailplane
    tri({1, -6}, {6, -10}, {1, -9});
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

// What was on the panel when the last fetch went out: enough to recognise an
// aircraft again (the callsign - slots reorder, since the list is sorted by
// distance), to tell whether anything about it has changed (the signature),
// and to rub it out if it has (the extent).
struct RadarMark {
    char callsign[9];
    uint32_t sig;
    RadarLabel hull;
};
RadarMark radarPrev[RADAR_MAX_AIRCRAFT];
uint8_t radarPrevCount = 0;

// What the full pass settled on, per aircraft: the label box the ladder chose
// and the hull around everything. The plots are constant between fetches, yet
// the sweep repair was recomputing them for every aircraft on every one of the
// 240 steps a revolution - and a plot now means three PROGMEM table scans and
// the five-rung hull union. The step tests these instead, and rebuilds only
// the aircraft its radius actually crossed, which is one or two a frame.
// Refilled by every full pass, which runs after every fetch, so the cache can
// never describe positions the panel is not showing.
struct RadarDrawn {
    char callsign[9];
    uint32_t sig;
    RadarLabel hull;    // marker and label together: what to erase
    RadarLabel box;     // the label alone: where the text starts
    uint8_t level;      // which rung of the ladder survived the crowd
    bool labelled;
};
RadarDrawn radarDrawnCache[RADAR_MAX_AIRCRAFT];
uint8_t radarCacheCount = 0;

RadarLabel radarBoxUnion(const RadarLabel& a, const RadarLabel& b) {
    const int16_t x0 = a.x < b.x ? a.x : b.x;
    const int16_t y0 = a.y < b.y ? a.y : b.y;
    const int16_t x1a = static_cast<int16_t>(a.x + a.w), x1b = static_cast<int16_t>(b.x + b.w);
    const int16_t y1a = static_cast<int16_t>(a.y + a.h), y1b = static_cast<int16_t>(b.y + b.h);
    const int16_t x1 = x1a > x1b ? x1a : x1b;
    const int16_t y1 = y1a > y1b ? y1a : y1b;
    return {x0, y0, static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}

// Everything about an aircraft that shows, boiled to one number. Two frames
// agreeing on it agree on every pixel the aircraft is responsible for, so the
// second one has nothing to do.
uint32_t radarHashBytes(uint32_t h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t radarHashText(uint32_t h, const char* t) {
    return radarHashBytes(h, t, strlen(t) + 1);
}

void radarCacheStore(uint8_t i, const char* callsign, const RadarLabel& hull, const RadarLabel& box,
                     uint8_t level, bool labelled, uint32_t sig) {
    if (i >= RADAR_MAX_AIRCRAFT) return;
    RadarDrawn& d = radarDrawnCache[i];
    strlcpy(d.callsign, callsign, sizeof(d.callsign));
    d.hull = hull;
    d.box = box;
    d.level = level;
    d.labelled = labelled;
    d.sig = sig;
    if (static_cast<uint8_t>(i + 1) > radarCacheCount) radarCacheCount = static_cast<uint8_t>(i + 1);
}

int16_t radarPrevFind(const char* callsign) {
    for (uint8_t i = 0; i < radarPrevCount; ++i) {
        if (strcmp(radarPrev[i].callsign, callsign) == 0) return static_cast<int16_t>(i);
    }
    return -1;
}

int16_t radarDrawnFind(const char* callsign) {
    for (uint8_t i = 0; i < radarCacheCount; ++i) {
        if (strcmp(radarDrawnCache[i].callsign, callsign) == 0) return static_cast<int16_t>(i);
    }
    return -1;
}

// The two header strings, where they last landed. The UI font blends instead of
// painting a background, so unlike the old built-in text these do not rub
// themselves out - and going from "10 ac" to "1 ac" left the wider one showing
// through the narrower. Cleared from the recorded extents rather than a fixed
// box, which would bite into the ring behind them.
RadarLabel radarHdrLeft = {0, 0, 0, 0};
RadarLabel radarHdrRight = {0, 0, 0, 0};
char radarHdrLeftTxt[16] = "";
char radarHdrRightTxt[12] = "";

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
    // The far end of the speed vector. Worked out here rather than at draw
    // time because the hull has to cover it, and for a while it did not: at
    // 300 knots the line runs 24 px from the marker while the hull stopped at
    // 14, so the outer half of it was outside every rectangle meant to rub it
    // out. Over a map nothing ever took those pixels back and the dial filled
    // with magenta stubs pointing at where aircraft used to be; on the plain
    // dial the sweep blacked them and the repair, which tests the hull, did
    // not put them back.
    int16_t vx, vy;        // the same as (x, y) when there is no vector to draw
    bool vector;
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
    const int16_t csW = measureText(callsign, RADAR_CALLSIGN_SIZE);
    const int16_t flW = hasFl ? measureText(fl, 1) : 0;
    const int16_t alW = hasAirline ? measureText(airline, 1) : 0;
    int16_t lw = csW > flW ? csW : flW;
    if (alW > lw) lw = alW;
    // Taken from whichever set the callsign size selects, so the box follows
    // that constant instead of having to be changed alongside it.
    const int16_t csH = UiTextFont::fontSet(uiKind(RADAR_CALLSIGN_SIZE)).lineHeight;
    const int16_t lh = static_cast<int16_t>((hasAirline ? (RADAR_ALT_LINE + RADAR_LABEL_GAP) : 0) +
                                            csH + (hasFl ? (RADAR_LABEL_GAP + RADAR_ALT_LINE) : 0));
    // Right of the marker, or left when the right side runs out of panel. No
    // clamping beyond that: a label that still overflows is left where it
    // belongs and the panel clips it - every pixel goes through drawPixel,
    // which drops out-of-bounds coordinates. Shoving the box fully on-screen
    // was tried and put labels visibly away from their aircraft at the rim,
    // which reads worse than a truncated word.
    int16_t lx = static_cast<int16_t>(px + 9);
    if (lx + lw > SCREEN_W - 2) lx = static_cast<int16_t>(px - 9 - lw);
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
    // Traffic past the ring is put on the ring, at its bearing: the direction
    // is known and the distance is not. A dot fits at the very edge, but the
    // silhouette drawn there over a map does not - half of it would be off the
    // panel - so it is pulled in far enough to be drawn whole.
    const float rim = static_cast<float>(RADAR_RR) - (radarBgActive() ? 12.0f : 0.0f);
    radarPolar(p.beyond ? rim : (a.distKm / range * RADAR_RR),
               radarScreenDeg(a.bearingDeg), p.x, p.y);
    // Turned by the same amount as the position: a ground track is a compass
    // bearing and the dial may be sitting under it at any angle. Drawn only
    // where the marker itself is drawn - a rim dot on the plain dial gets no
    // vector, so the hull must not budget for one either.
    p.vx = p.x;
    p.vy = p.y;
    p.vector = !isnan(a.track) && !isnan(a.gs) && !(p.beyond && !radarBgActive());
    if (p.vector) {
        const float th = radarScreenDeg(a.track) * PI / 180.0f;
        const float len = constrain(a.gs * 0.08f, 6.0f, 24.0f);
        p.vx = static_cast<int16_t>(p.x + lroundf(sinf(th) * len));
        p.vy = static_cast<int16_t>(p.y - lroundf(cosf(th) * len));
    }
    // A helicopter almost never belongs to an airline, so the line that would
    // name one carries its type instead - which the feed gives directly, so
    // there is no table here to be wrong.
    p.airline[0] = 0;
    // Over a map the beyond-ring traffic is drawn as a full silhouette, so it
    // earns the full label too - airline, altitude, route, all of it. On the
    // plain dial it stays a bare dot and none of this is built for it.
    const bool labelled = !p.beyond || radarBgActive();
    if (labelled) {
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
    if (labelled) {
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

    // Just the marker here - about 14 px across whichever way it points. What
    // the label adds is settled by the draw, which knows which rung of the
    // ladder survived the crowd, and records the union of the two as this
    // aircraft's real extent.
    //
    // Taking the union of every rung instead, as this did, was a safe answer
    // to a question the draw can answer exactly: five variants spread across
    // nearly the whole panel, and erasing that over a map meant reading 25 KB
    // of ground the label never covered.
    p.hull = {static_cast<int16_t>(p.x - 14), static_cast<int16_t>(p.y - 14), 28, 28};
    // A segment lies inside the box its two ends span, and the marker box
    // already holds one of them, so unioning a few pixels round the far end
    // covers the whole line and nothing more.
    if (p.vector) {
        p.hull = radarBoxUnion(p.hull, RadarLabel{static_cast<int16_t>(p.vx - 2),
                                                  static_cast<int16_t>(p.vy - 2), 5, 5});
    }
    return p;
}

// Put aircraft i on the panel where the resolve pass decided it goes. The
// ladder is not re-run - the rung it chose is in the cache - so this and the
// resolve can never disagree about where the text belongs.
void radarPaintAircraft(uint8_t i) {
    if (i >= radarCacheCount) return;
    const Aircraft& a = radarAc[i];
    const RadarDrawn& d = radarDrawnCache[i];
    const RadarPlot p = radarPlotOf(i);

    if (p.beyond && !radarBgActive()) {
        // On the plain dial the ring is what gives a dot its meaning: this one
        // is outside, that way. Over a map there is no ring drawn, so a dot
        // sits on nothing and says less than the aircraft itself would.
        tft.fillCircle(p.x, p.y, radarScaleR(3), a.rotor ? RADAR_C_YELLOW : RADAR_C_RED);
        return;
    }
    // A track is a compass bearing, and so is the bearing that put the marker
    // where it is - so both need turning by the same amount when the dial is
    // turned. Only the position was, which left every aircraft on a rotated
    // dial pointing 20 degrees away from where it was actually going.
    const float heading = isnan(a.track) ? 0.0f : radarScreenDeg(a.track);
    // The endpoint comes from the plot, which is also what sized the hull. Two
    // copies of this arithmetic is exactly how the line came to be drawn
    // outside the box that was supposed to erase it.
    if (p.vector) tft.drawLine(p.x, p.y, p.vx, p.vy, RADAR_C_MAGENTA);
    const uint16_t ink = a.rotor ? RADAR_C_YELLOW : RADAR_C_RED;
    if (a.rotor) radarRotor(p.x, p.y, heading, ink);
    else if (!isnan(a.track)) radarPlaneTri(p.x, p.y, heading, ink);
    else tft.fillCircle(p.x, p.y, radarScaleR(4), ink);

    if (!d.labelled) return;
    const char* airlineLine = nullptr;
    const char* flLine = nullptr;
    radarLabelLines(p, d.level, &airlineLine, &flLine);
    int16_t ty = d.box.y;
    if (airlineLine[0] != 0) {
        drawTextAt(tft, d.box.x, ty, airlineLine, 1, RADAR_C_GRAY, TFT_BLACK);
        ty = static_cast<int16_t>(ty + RADAR_ALT_LINE + RADAR_LABEL_GAP);
    }
    drawTextAt(tft, d.box.x, ty, a.callsign, RADAR_CALLSIGN_SIZE, RADAR_C_GRAY, TFT_BLACK);
    if (flLine[0] != 0) {
        // No clear first: the sweep passes this text several times a revolution
        // and blanking it each time is what made it blink. Drawing the same
        // glyphs over themselves is harmless, because the reading only changes
        // on a fetch and a fetch always rubs the whole label out beforehand.
        const int16_t ay = static_cast<int16_t>(
            ty + UiTextFont::fontSet(uiKind(RADAR_CALLSIGN_SIZE)).lineHeight + RADAR_LABEL_GAP);
        drawTextAt(tft, d.box.x, ay, flLine, 1, RADAR_C_GRAY, TFT_BLACK);
    }
}

// One aircraft, marker and label, and the cache entries the sweep repair will
// read. Only the full pass calls this - the per-step repair goes through
// radarRedrawCrossed, which replays this function's cached verdict instead of
// re-deriving it.
// The state everything drawn for this aircraft depends on. Two frames that
// agree on this agree on every pixel it owns.
uint32_t radarSignature(const Aircraft& a, const RadarPlot& p, const RadarLabel& box,
                        const char* airline, const char* fl) {
    uint32_t h = 2166136261u;
    const int16_t nums[] = {p.x, p.y, box.x, box.y, box.w, box.h};
    h = radarHashBytes(h, nums, sizeof(nums));
    const uint8_t flags = static_cast<uint8_t>((a.rotor ? 1 : 0) | (p.beyond ? 2 : 0) |
                                              (isnan(a.track) ? 4 : 0) | (isnan(a.gs) ? 8 : 0));
    h = radarHashBytes(h, &flags, 1);
    // The marker turns with the track and the velocity line grows with the
    // speed, so both are part of the picture; a degree and a knot are finer
    // than the panel can show, which is enough resolution to compare at.
    const int16_t trk = isnan(a.track) ? 0 : static_cast<int16_t>(lroundf(a.track));
    const int16_t gs = isnan(a.gs) ? 0 : static_cast<int16_t>(lroundf(a.gs));
    h = radarHashBytes(h, &trk, sizeof(trk));
    h = radarHashBytes(h, &gs, sizeof(gs));
    h = radarHashText(h, a.callsign);
    h = radarHashText(h, airline);
    h = radarHashText(h, fl);
    return h;
}

// Decide where this aircraft's marker and label go and what the label says,
// and put nothing on the panel. Every aircraft goes through this, drawn or
// not: which rung of the ladder survives depends on every nearer one, so the
// placement has to run in full for the verdicts to come out the same.
void radarDrawAircraft(uint8_t i, RadarLabel* placed, uint8_t& placedCount) {
    const Aircraft& a = radarAc[i];
    const RadarPlot p = radarPlotOf(i);

    // A rim dot on the plain dial has nothing to label. Over a map the same
    // aircraft is a full silhouette and goes through the ladder like any other.
    if (p.beyond && !radarBgActive()) {
        radarCacheStore(i, a.callsign, p.hull, p.label, 0, false,
                        radarSignature(a, p, p.label, "", ""));
        return;
    }

    const int16_t x = p.x;
    const int16_t y = p.y;

    if (a.callsign[0] == 0) {
        // Nothing to label, but the marker is still its own to erase and put
        // back, so it still needs an entry.
        radarCacheStore(i, a.callsign, p.hull, p.label, 0, false,
                        radarSignature(a, p, p.label, "", ""));
        return;
    }

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
    uint8_t chosen = RADAR_LABEL_LEVELS - 1;
    radarLabelLines(p, chosen, &airlineLine, &flLine);
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
            chosen = v;
            break;
        }
    }

    if (placedCount < RADAR_MAX_AIRCRAFT) placed[placedCount++] = box;
    // The extent the sweep repairs and the next repaint erases: the marker and
    // the label that was actually chosen.
    radarCacheStore(i, a.callsign, radarBoxUnion(p.hull, box), box, chosen, true,
                    radarSignature(a, p, box, airlineLine, flLine));
}

void radarDrawHome() {
    // On the map the centre needs no marking - home is whatever the middle of
    // the picture shows. On the plain dial the dot is the only thing saying
    // where the observer stands, so it stays there.
    if (radarBgActive()) return;
    tft.fillCircle(RADAR_CX, RADAR_CY, radarScaleR(4), RADAR_C_GREEN);
}

// ---------------------------------------------------------------------------
// The background image. Same contract as an album photo - 240x240 RGB565,
// big-endian, decoded by the browser because no decoder fits in this flash -
// but the radar never repaints the whole screen, so everything that used to
// erase by painting black paints file pixels instead when an image is set.
//
// The file stays open across the polls: it is read a few hundred bytes at a
// time from inside the sweep, and reopening it every step would cost more
// than the reads. radarBgActive() rechecks the id, so changing the image in
// the web UI takes effect on the next repaint without a reboot.
// ---------------------------------------------------------------------------

// A map used to be 115,200 bytes: 240x240 of RGB565, one colour per pixel,
// because repainting reads back arbitrary rectangles and no compressed format
// can be opened at a rectangle. Raw was the right answer; two bytes a pixel
// was not. A map is flat fills, thin lines and lettering - a tile of Seoul
// measured 2,719 distinct colours - so 256 of them, chosen for the picture,
// cost 2.5/255 of mean channel error and half the file.
//
//   0    "SDP8"
//   4    256 colours, RGB565 big-endian, the palette this picture chose
//   516  240*240 index bytes
//
// The palette is dimmed once when the file is opened, so drawing is a lookup
// and nothing else - the per-pixel arithmetic the old path did on every read
// is gone. Files written before this stay 115,200 bytes and still draw; the
// two are told apart by size, and nothing on a device has to be converted.
constexpr uint32_t RADAR_BG8_HEAD = 4;
constexpr uint32_t RADAR_BG8_PAL = 512;
constexpr uint32_t RADAR_BG8_BASE = RADAR_BG8_HEAD + RADAR_BG8_PAL;
constexpr uint32_t RADAR_BG8_BYTES =
    RADAR_BG8_BASE + static_cast<uint32_t>(SCREEN_W) * SCREEN_H;

File radarBgFile;
String radarBgOpenId;
bool radarBgOk = false;
// Set when the open file is the indexed format. The palette holds host-order
// colours with the dim already applied.
bool radarBgIndexed = false;
uint16_t radarBgPal[256];

// The open file holds a LittleFS cache buffer, and where that lands in the
// heap matters more than its size: opened mid-life it sits in the middle and
// halves the largest free block, which is exactly the block TLS needs. So the
// file is let go before every fetch and reopened on demand by the next draw -
// the same manners the analog band sprite learned for the same reason.
void radarBgRelease() {
    if (radarBgFile) radarBgFile.close();
    radarBgOpenId = "";
    radarBgOk = false;
    radarBgIndexed = false;
}

// An image lives only while something points at it: a saved location, or the
// background in use right now - which is how one that has just been uploaded
// survives long enough to be attached to a location. Everything else under
// /radar is a leftover from a location that was deleted or an image that was
// replaced, and 115 KB is too much of this filesystem to leave lying about.
//
// Run after anything that changes what points where, so the rule holds without
// anyone having to remember it at the call site.
// What the last sweep found with nothing pointing at it. Reported rather than
// acted on - see the note at the end of radarBgSweep.
uint8_t radarBgOrphans = 0;
String radarBgOrphanIds[8];

void radarBgSweep() {
    // Never sweep on a guess. Every early return in loadConfig leaves the
    // defaults standing - no saved locations, no background - and a sweep run
    // against those would read as "nothing points at anything" and delete the
    // lot. A config that failed to parse is a reason to touch nothing.
    if (!fsMounted || !configLoaded) return;
    radarBgRelease();   // never unlink a file this still has open

    // Names first: removing entries from a directory while walking it is not
    // something LittleFS promises anything about.
    String doomed[8];
    uint8_t n = 0;
    Dir dir = LittleFS.openDir("/radar");
    while (dir.next() && n < 8) {
        wdtYield();   // a directory walk has no yield of its own
        String name = dir.fileName();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!name.endsWith(".rgb")) continue;
        const String id = name.substring(0, name.length() - 4);
        if (id == cfg.radarBg) continue;
        bool kept = false;
        for (uint8_t i = 0; i < cfg.radarPresetCount && !kept; ++i) {
            kept = (id == cfg.radarPresets[i].bg);
        }
        if (!kept) doomed[n++] = id;
    }
    // Counted, not deleted. This ran at every boot and erased any map no preset
    // pointed at, which is correct right up until a preset loses its reference
    // by accident - and then three 115 KB maps that took real effort to make are
    // gone on the next boot with nothing asking first. That is what happened.
    // A map costs 115 KB of a 2 MB filesystem; deleting one to reclaim that is
    // never urgent enough to do unasked. The count is reported so the web UI can
    // offer it, and /api/radar/sweep?delete=1 does it when told.
    radarBgOrphans = n;
    for (uint8_t i = 0; i < n && i < 8; ++i) radarBgOrphanIds[i] = doomed[i];
}

// Defined with the drawing below, where the constant it applies lives.
inline uint16_t radarBgDim(uint16_t v);

// Reads the header and the 256 colours, dimming each as it lands. A file that
// does not start with the magic is refused rather than drawn as noise: it is
// the same size as nothing else this firmware writes, so the only way to be
// here with the wrong bytes is a truncated or foreign file.
bool radarBgLoadPalette() {
    uint8_t buf[64];
    radarBgFile.seek(0);
    if (radarBgFile.read(buf, RADAR_BG8_HEAD) != static_cast<int>(RADAR_BG8_HEAD)) return false;
    if (buf[0] != 'S' || buf[1] != 'D' || buf[2] != 'P' || buf[3] != '8') return false;
    // Sixty-four bytes at a time rather than the whole 512. This runs at the
    // sweep's own call depth - the file is released before every fetch and
    // reopened by the next repaint - and the stack it is standing on is 4 KB.
    for (uint16_t i = 0; i < 256; i += 32) {
        if (radarBgFile.read(buf, sizeof(buf)) != static_cast<int>(sizeof(buf))) return false;
        for (uint16_t j = 0; j < 32; ++j) {
            const uint16_t c = static_cast<uint16_t>((buf[j * 2] << 8) | buf[j * 2 + 1]);
            radarBgPal[i + j] = radarBgDim(c);
        }
    }
    return true;
}

bool radarBgActive() {
    if (cfg.radarBg.length() == 0 || !albumIdOk(cfg.radarBg)) {
        if (radarBgFile) radarBgFile.close();
        radarBgOpenId = "";
        radarBgOk = false;
        radarBgIndexed = false;
        return false;
    }
    if (radarBgOk && radarBgFile && radarBgOpenId == cfg.radarBg) return true;
    if (radarBgFile) radarBgFile.close();
    radarBgOk = false;
    // Falls with the file, not with the next successful open: three of the
    // returns below leave without opening anything, and a flag saying "the
    // closed file was indexed" is a loaded gun pointed at whoever next calls
    // radarBgBlit without checking radarBgActive first.
    radarBgIndexed = false;
    radarBgOpenId = cfg.radarBg;
    if (!fsMounted) return false;
    const String path = "/radar/" + cfg.radarBg + ".rgb";
    if (!LittleFS.exists(path)) return false;
    radarBgFile = LittleFS.open(path, "r");
    if (!radarBgFile) return false;
    if (radarBgFile.size() >= RADAR_BG8_BYTES && radarBgFile.size() < ALBUM_BYTES) {
        if (!radarBgLoadPalette()) {
            radarBgFile.close();
            return false;
        }
        radarBgIndexed = true;
    } else if (radarBgFile.size() < ALBUM_BYTES) {
        radarBgFile.close();
        return false;
    }
    radarBgOk = true;
    return true;
}

// The map is shown at 35 percent brightness. Half was tried first and a
// daytime satellite tile still shouted over the markers; at 35 the terrain is
// context and nothing more. Per channel, x*45>>7 is 35.2 percent without a
// divide. Done here rather than at upload so any image already on the device
// dims too - and so the number can keep being argued about in one place.
inline uint16_t radarBgDim(uint16_t v) {
    const uint16_t r = static_cast<uint16_t>((((v >> 11) & 0x1F) * 45) >> 7);
    const uint16_t g = static_cast<uint16_t>((((v >> 5) & 0x3F) * 45) >> 7);
    const uint16_t b = static_cast<uint16_t>(((v & 0x1F) * 45) >> 7);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// One row at a time off a 240x240 RGB565 file, optionally dimmed in place. The
// file is big-endian for the same reason the album is: pushPixels memcpy's
// straight into the SPI FIFO, so the file's byte order IS the panel's.
//
// This is the pre-SDP8 path now: maps written before the palette format are
// still 115,200 bytes and still on devices, so the reader stays. The Borduhr's
// dial reads its own file the same way further down; the row buffer is 480
// bytes of stack, which is not something to keep two of at once.
void faceBlitRect(File& src, int16_t x, int16_t y, int16_t w, int16_t h, bool dim) {
    if (x < 0) { w = static_cast<int16_t>(w + x); x = 0; }
    if (y < 0) { h = static_cast<int16_t>(h + y); y = 0; }
    if (x + w > SCREEN_W) w = static_cast<int16_t>(SCREEN_W - x);
    if (y + h > SCREEN_H) h = static_cast<int16_t>(SCREEN_H - y);
    if (w <= 0 || h <= 0) return;
    uint8_t row[SCREEN_W * 2];
    const size_t want = static_cast<size_t>(w) * 2U;
    tft.setSwapBytes(false);
    for (int16_t r = 0; r < h; ++r) {
        src.seek((static_cast<uint32_t>(y + r) * SCREEN_W + x) * 2U);
        if (src.read(row, want) != static_cast<int>(want)) return;
        if (dim) {
            for (size_t i = 0; i + 1 < want; i += 2) {
                const uint16_t v = radarBgDim(static_cast<uint16_t>((row[i] << 8) | row[i + 1]));
                row[i] = static_cast<uint8_t>(v >> 8);
                row[i + 1] = static_cast<uint8_t>(v);
            }
        }
        tft.pushImage(x, static_cast<int16_t>(y + r), w, 1, reinterpret_cast<uint16_t*>(row));
    }
}

// One row of an indexed map. The indices are read into the far half of the
// same 480-byte buffer the colours are written into: the write for pixel i
// lands at 2i and the read for it comes from 240+i, so the writer only catches
// the reader on the very last pixel, after it has been read. Two buffers would
// be 240 more bytes of stack inside the sweep, which is not free here.
void radarBgBlitIndexed(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (x < 0) { w = static_cast<int16_t>(w + x); x = 0; }
    if (y < 0) { h = static_cast<int16_t>(h + y); y = 0; }
    if (x + w > SCREEN_W) w = static_cast<int16_t>(SCREEN_W - x);
    if (y + h > SCREEN_H) h = static_cast<int16_t>(SCREEN_H - y);
    if (w <= 0 || h <= 0) return;
    uint8_t row[SCREEN_W * 2];
    uint8_t* idx = row + SCREEN_W;
    tft.setSwapBytes(false);
    for (int16_t r = 0; r < h; ++r) {
        radarBgFile.seek(RADAR_BG8_BASE +
                         static_cast<uint32_t>(y + r) * SCREEN_W + x);
        if (radarBgFile.read(idx, static_cast<size_t>(w)) != w) return;
        for (int16_t i = 0; i < w; ++i) {
            const uint16_t c = radarBgPal[idx[i]];
            row[i * 2] = static_cast<uint8_t>(c >> 8);      // the panel's order
            row[i * 2 + 1] = static_cast<uint8_t>(c);
        }
        tft.pushImage(x, static_cast<int16_t>(y + r), w, 1, reinterpret_cast<uint16_t*>(row));
    }
}

// Both readers clip for themselves, so the rule has one owner each rather
// than a copy here that could drift from the one in faceBlitRect.
void radarBgBlit(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (radarBgIndexed) radarBgBlitIndexed(x, y, w, h);
    else faceBlitRect(radarBgFile, x, y, w, h, true);
}

uint16_t radarBgPixel(int16_t x, int16_t y) {
    if (radarBgIndexed) {
        uint8_t i = 0;
        radarBgFile.seek(RADAR_BG8_BASE + static_cast<uint32_t>(y) * SCREEN_W + x);
        if (radarBgFile.read(&i, 1) != 1) {
            // Say so once rather than painting black down the radius on every
            // revolution with nothing anywhere to say why.
            radarBgOk = false;
            return 0;
        }
        return radarBgPal[i];   // already host-order and already dimmed
    }
    uint8_t b[2] = {0, 0};
    radarBgFile.seek((static_cast<uint32_t>(y) * SCREEN_W + x) * 2U);
    radarBgFile.read(b, 2);
    // drawPixel wants a host-order colour; the file is big-endian.
    return radarBgDim(static_cast<uint16_t>((b[0] << 8) | b[1]));
}

void radarEraseRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (radarBgActive()) radarBgBlit(x, y, w, h);
    else tft.fillRect(x, y, w, h, TFT_BLACK);
}

// Erasing a radius means putting back whatever was under it. Bresenham, one
// file pixel per point - the offsets run monotonically along the line, which
// is the direction LittleFS seeks cheaply.
void radarEraseLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (!radarBgActive()) {
        tft.drawLine(x0, y0, x1, y1, TFT_BLACK);
        return;
    }
    const int16_t dx = static_cast<int16_t>(abs(x1 - x0));
    const int16_t dy = static_cast<int16_t>(-abs(y1 - y0));
    const int16_t sx = x0 < x1 ? 1 : -1;
    const int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = static_cast<int16_t>(dx + dy);
    while (true) {
        if (x0 >= 0 && x0 < SCREEN_W && y0 >= 0 && y0 < SCREEN_H) {
            tft.drawPixel(x0, y0, radarBgPixel(x0, y0));
        }
        if (x0 == x1 && y0 == y1) break;
        const int16_t e2 = static_cast<int16_t>(2 * err);
        if (e2 >= dy) { err = static_cast<int16_t>(err + dy); x0 = static_cast<int16_t>(x0 + sx); }
        if (e2 <= dx) { err = static_cast<int16_t>(err + dx); y0 = static_cast<int16_t>(y0 + sy); }
    }
}

// The rings sit every 5 km, because "half way out" answers no question anyone
// at the dial is asking, while "how many boxes of 5" reads at a glance. When
// the range is long enough that 5 km rings would touch, the step doubles up
// until they are legible again. The boundary ring is always there.
uint8_t radarRingRadii(int16_t* out, uint8_t max) {
    uint8_t n = 0;
    const float perKm = static_cast<float>(RADAR_RR) / static_cast<float>(cfg.radarRangeKm);
    float stepKm = 5.0f;
    while (stepKm * perKm < 12.0f) stepKm += 5.0f;
    for (float km = stepKm; km < static_cast<float>(cfg.radarRangeKm) - 0.01f && n + 1 < max; km += stepKm) {
        out[n++] = static_cast<int16_t>(lroundf(km * perKm));
    }
    out[n++] = RADAR_RR;
    return n;
}

void radarDrawRings() {
    // Over a map the dial furniture works against the picture: the map itself
    // says where things are, and rings, crosshair and a rotating sweep on top
    // of terrain read as clutter. Only the N marker stays - a rotated dial
    // still owes the viewer its bearings.
    if (!radarBgActive()) {
        int16_t radii[10];
        const uint8_t n = radarRingRadii(radii, sizeof(radii) / sizeof(radii[0]));
        for (uint8_t i = 0; i < n; ++i) tft.drawCircle(RADAR_CX, RADAR_CY, radii[i], RADAR_C_DGRAY);
        tft.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_RR, 2 * RADAR_RR, RADAR_C_DGRAY);
        tft.drawFastHLine(RADAR_CX - RADAR_RR, RADAR_CY, 2 * RADAR_RR, RADAR_C_DGRAY);
    }
}

// Split out because it is the one piece of furniture a map keeps, and so the
// only one an incremental repaint has to be able to put back by itself.
void radarDrawNorth() {
    const RadarLabel n = radarNorthBox();
    drawTextAt(tft, n.x, n.y, "N", RADAR_LABEL_SIZE, RADAR_C_GRAY, TFT_BLACK);
}

// Only a change needs the old text rubbed out, and rubbing out over a map is a
// read of the ground beneath it - that is the part worth avoiding. The text
// itself goes down every time regardless.
//
// Writing the same string over itself changes nothing: a glyph leaves its
// unlit pixels alone and blends the rest to a fixed colour. What it does do is
// put back anything an erase somewhere else has taken out of it - which is
// what kept happening, and why "15 km" spent its afternoons as "k". Noticing
// the damage and reacting to it was tried first; not needing to notice is
// better, and costs a few milliseconds of drawing against a map read.
void radarDrawHeader(RadarLabel& box, char* last, size_t lastSize, const char* text, bool onTheRight) {
    if (box.w > 0 && strcmp(last, text) != 0) radarEraseRect(box.x, box.y, box.w, box.h);
    const int16_t line = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    const int16_t w = measureText(text, RADAR_HEADER_SIZE);
    const int16_t x = onTheRight ? static_cast<int16_t>(SCREEN_W - w - 3) : 3;
    drawTextAt(tft, x, 1, text, RADAR_HEADER_SIZE, RADAR_C_GRAY, TFT_BLACK);
    box = {x, 1, w, line};
    strlcpy(last, text, lastSize);
}

// A header yields its corner. When an aircraft's label needs the spot, the
// header hides rather than overprinting it into a jumble, and comes back the
// poll after the corner is clear. Hiding means erasing, and an erase does not
// know whose pixels it is taking - the label that caused the hiding has just
// been painted through this very rectangle - so whatever the erase bit into
// is painted again, the same repair the incremental path already owes any
// erase it makes.
void radarHeaderYield(RadarLabel& box, char* last, size_t lastSize, const char* text,
                      bool onTheRight) {
    const int16_t w = measureText(text, RADAR_HEADER_SIZE);
    const int16_t lineH = UiTextFont::fontSet(UiTextFont::Kind::Large).lineHeight;
    const int16_t x = onTheRight ? static_cast<int16_t>(SCREEN_W - w - 3) : 3;
    const RadarLabel want = {x, 1, w, lineH};
    bool covered = false;
    for (uint8_t i = 0; i < radarCacheCount && !covered; ++i) {
        covered = radarDrawnCache[i].labelled && radarBoxHit(want, radarDrawnCache[i].box);
    }
    if (covered) {
        if (box.w > 0) {
            radarEraseRect(box.x, box.y, box.w, box.h);
            for (uint8_t i = 0; i < radarCacheCount; ++i) {
                if (radarBoxHit(box, radarDrawnCache[i].hull)) radarPaintAircraft(i);
            }
            box = RadarLabel{0, 0, 0, 0};
            last[0] = 0;
        }
        return;
    }
    radarDrawHeader(box, last, lastSize, text, onTheRight);
}

void radarDrawOverlays() {
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%d km", cfg.radarRangeKm);
    radarHeaderYield(radarHdrLeft, radarHdrLeftTxt, sizeof(radarHdrLeftTxt), hdr, false);

    char cnt[12];
    snprintf(cnt, sizeof(cnt), "%d ac", radarAcCount);
    radarHeaderYield(radarHdrRight, radarHdrRightTxt, sizeof(radarHdrRightTxt), cnt, true);

    if (radarErrorFlag) tft.fillCircle(6, SCREEN_H - 7, 4, RADAR_C_RED);
    else radarEraseRect(2, SCREEN_H - 11, 9, 9);
}

// `incremental` means the panel already holds the previous frame: work out
// what changed, rub out only that, and lay down only that. A full pass draws
// everything, which is what a cleared screen needs.
// `repaint` means the panel already holds the previous frame and its marks
// have to be rubbed out. Only over a map is that done selectively: the plain
// dial has rings and a crosshair beneath the markers, and those can be laid
// down before the aircraft only if every aircraft is being laid down too.
// Which costs nothing to insist on - erasing to black is a fillRect, where
// erasing to a map is a read of the ground.
void radarDrawContents(bool repaint = false) {
    const bool incremental = repaint && radarBgActive();

    // Place every aircraft first, drawing none of them. The verdicts depend on
    // each other, so they all have to be reached before any of them is acted on.
    // The order starts one further along each poll, so collision losers rotate.
    radarCacheCount = 0;
    RadarLabel placed[RADAR_MAX_AIRCRAFT];
    uint8_t placedCount = 0;
    for (uint8_t k = 0; k < radarAcCount; ++k) {
        radarDrawAircraft(static_cast<uint8_t>((k + radarLabelPhase) % radarAcCount),
                          placed, placedCount);
        wdtYield();
    }

    if (!incremental) {
        // Erase, then the furniture, then the aircraft over it. Drawing the
        // rings before the erase left a bite out of every one an aircraft
        // label had covered, with nothing to put it back.
        if (repaint) {
            for (uint8_t j = 0; j < radarPrevCount; ++j) {
                const RadarLabel& b = radarPrev[j].hull;
                radarEraseRect(b.x, b.y, b.w, b.h);
            }
            wdtYield();
        }
        radarDrawRings();
        radarDrawNorth();
        for (uint8_t i = 0; i < radarCacheCount; ++i) radarPaintAircraft(i);
        radarDrawHome();
        radarDrawOverlays();
        return;
    }

    // A mark is stale unless the same aircraft is still drawn exactly as it
    // was; an aircraft needs painting unless its mark says it is already there.
    bool stale[RADAR_MAX_AIRCRAFT];
    bool paint[RADAR_MAX_AIRCRAFT];
    for (uint8_t j = 0; j < radarPrevCount; ++j) {
        const int16_t at = radarDrawnFind(radarPrev[j].callsign);
        stale[j] = at < 0 || radarDrawnCache[at].sig != radarPrev[j].sig;
    }
    for (uint8_t i = 0; i < radarCacheCount; ++i) {
        const int16_t at = radarPrevFind(radarDrawnCache[i].callsign);
        paint[i] = at < 0 || radarPrev[at].sig != radarDrawnCache[i].sig;
    }

    for (uint8_t j = 0; j < radarPrevCount; ++j) {
        if (!stale[j]) continue;
        const RadarLabel& b = radarPrev[j].hull;
        radarEraseRect(b.x, b.y, b.w, b.h);
    }
    // The one piece of furniture a map keeps, written over itself for the same
    // reason as the headers: it costs a glyph and it can never be left broken.
    radarDrawNorth();
    wdtYield();

    // An erase does not know whose pixels it is taking. Anything it bit into
    // has to go down again even though nothing about it changed.
    for (uint8_t i = 0; i < radarCacheCount; ++i) {
        if (paint[i]) continue;
        for (uint8_t j = 0; j < radarPrevCount; ++j) {
            if (!stale[j] || !radarBoxHit(radarDrawnCache[i].hull, radarPrev[j].hull)) continue;
            paint[i] = true;
            break;
        }
    }
    // Neither does a draw. A silhouette is filled and a label is opaque ink, so
    // an aircraft going down again covers whatever it lands on - and the
    // neighbour it landed on, having changed nothing, would sit there with a
    // bite out of its label until it happened to move, which on a slow target
    // is minutes. The full pass never showed this because it lays the whole
    // dial down every poll; over a map only the changed ones are drawn.
    //
    // It spreads, so this closes rather than passing once: the neighbour now
    // being redrawn covers its own neighbour in turn. It ends, because the set
    // only ever grows and there are at most RADAR_MAX_AIRCRAFT of them.
    for (uint8_t round = 0; round < RADAR_MAX_AIRCRAFT; ++round) {
        bool added = false;
        for (uint8_t i = 0; i < radarCacheCount; ++i) {
            if (paint[i]) continue;
            for (uint8_t j = 0; j < radarCacheCount; ++j) {
                if (i == j || !paint[j]) continue;
                if (!radarBoxHit(radarDrawnCache[i].hull, radarDrawnCache[j].hull)) continue;
                paint[i] = true;
                added = true;
                break;
            }
        }
        if (!added) break;
    }

    for (uint8_t i = 0; i < radarCacheCount; ++i) {
        if (!paint[i]) continue;
        radarPaintAircraft(i);
        wdtYield();
    }
    radarDrawHome();
    radarDrawOverlays();
}

// The first draw, and only that: clearing the whole panel is right when there
// is nothing on it worth keeping.
void radarDrawScene() {
    if (radarBgActive()) radarBgBlit(0, 0, SCREEN_W, SCREEN_H);
    else tft.fillScreen(TFT_BLACK);
    // Nothing survives a cleared panel, so nothing may claim to.
    radarHdrLeft = radarHdrRight = RadarLabel{0, 0, 0, 0};
    radarHdrLeftTxt[0] = 0;
    radarHdrRightTxt[0] = 0;
    radarPrevCount = 0;
    radarDrawContents();
    radarSceneDrawn = true;
}

// Every revolution after the first. Clearing the panel first is what made the
// screen blink at twelve o'clock once a turn, so instead the few things that
// actually change are rubbed out: the radii the sweep has lit, and the boxes
// the previous aircraft occupied. The rings are thin enough to simply redraw.
void radarRepaint() {
    const uint32_t t0 = millis();
    if (!radarBgActive()) {
        for (uint8_t t = 0; t <= RADAR_TRAIL; ++t) {
            int16_t ex, ey;
            radarPolar(static_cast<float>(RADAR_RR),
                       radarStepDeg(static_cast<int32_t>(radarSweepStep) - t), ex, ey);
            radarEraseLine(RADAR_CX, RADAR_CY, ex, ey);
        }
    }
    wdtYield();
    const uint32_t t1 = millis();
    radarDrawContents(true);
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
    // No sweep is drawn over a map, so there is nothing of it to repair.
    if (radarBgActive()) return;
    int16_t ex, ey;
    radarPolar(static_cast<float>(RADAR_RR), deg, ex, ey);
    radarEraseLine(RADAR_CX, RADAR_CY, ex, ey);

    // Rings. A circle and a radius can share several pixels where they cross,
    // so the mend covers a short arc either side rather than the one or two
    // points the crossing nominally occupies - otherwise the sweep chews a
    // notch out of each ring as it goes by.
    int16_t radii[10];
    const uint8_t nr = radarRingRadii(radii, sizeof(radii) / sizeof(radii[0]));
    for (uint8_t i = 0; i < nr; ++i) {
        const int16_t r = radii[i];
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
        if (radarSegHitsBox(ex, ey, radarDrawnCache[i].hull)) radarPaintAircraft(i);
    }
    radarDrawHome();
}

void radarDrawSweep() {
    if (radarBgActive()) return;
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

// Says whether anything was actually put on the panel, which is what the IP
// badge needs to know: it sits in the bottom left where the sweep clips its
// right-hand end going past, and a repaint wipes it outright, so it has to
// follow every paint - but only those. Drawing it on the passes that painted
// nothing meant blending thirteen glyphs onto the panel as fast as the loop
// would go, for the whole three minutes it is up.
bool drawRadar(bool force) {
    if (force) {
        // TLS needs a big contiguous block and the band sprite is the biggest
        // thing standing in its way, so it goes back before the first fetch.
        analogBandEnd();
        radarSceneDrawn = false;
        radarNeedsRepaint = false;
        radarPrevCount = 0;
        radarSweepLastMs = 0;
        radarWantFetch = true;
    }

    if (cfg.radarLat == 0.0f && cfg.radarLon == 0.0f) {
        if (radarSceneDrawn) return false;
        tft.fillScreen(TFT_BLACK);
        drawCenteredText(104, F("Plane radar"), 2, RADAR_C_YELLOW, TFT_BLACK, 0, SCREEN_W);
        drawCenteredText(132, F("set home location"), 2, RADAR_C_GRAY, TFT_BLACK, 0, SCREEN_W);
        radarSceneDrawn = true;
        return true;
    }

    const uint32_t now = millis();
    if (!radarSceneDrawn) {
        radarDrawScene();
        radarNeedsRepaint = false;
        radarSweepStep = 0;
        radarSweepLastMs = now;
        return true;
    }
    if (radarNeedsRepaint) {
        radarRepaint();
        radarNeedsRepaint = false;
        radarSweepStep = 0;
        radarSweepLastMs = now;
        return true;
    }

    // One revolution per poll, so the sweep arrives back at north just as the
    // next set of positions does.
    const uint32_t periodMs = static_cast<uint32_t>(cfg.radarPollSec) * 1000UL;
    const uint32_t frameMs = periodMs / RADAR_STEPS;
    if (now - radarSweepLastMs < frameMs) return false;
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
    return true;
}

// Polls on its own clock, and only while the radar is the screen being shown:
// a TLS handshake is the most heap-hungry thing this firmware does and there is
// no reason to run it for a screen nobody is looking at.
void radarService() {
    if (activeScreen != SCREEN_RADAR) return;
    // Nothing is drawn onto a panel that still belongs to another screen. This
    // runs before drawRadar in the same pass, and it repairs sweep radii - so
    // arriving from the album with radarWantFetch left true from the previous
    // visit painted crosshairs and aircraft over the photograph, a frame before
    // the dial replaced it. The scene comes first; the fetch waits one pass.
    if (!radarSceneDrawn) return;
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
    // What is actually on the panel right now, which the cache has been
    // holding since the last full pass. Recomputing the plots here would give
    // the same positions - nothing has moved without a fetch - at the cost of
    // rebuilding every one of them.
    radarPrevCount = 0;
    for (uint8_t i = 0; i < radarCacheCount && radarPrevCount < RADAR_MAX_AIRCRAFT; ++i) {
        RadarMark& m = radarPrev[radarPrevCount++];
        strlcpy(m.callsign, radarDrawnCache[i].callsign, sizeof(m.callsign));
        m.sig = radarDrawnCache[i].sig;
        m.hull = radarDrawnCache[i].hull;
    }
    radarFetch();
    // After the positions, not before: which callsigns are in range is exactly
    // what the reply just told us.
    routeService();
    ++radarLabelPhase;          // colliding labels take turns, one poll each
    radarNeedsRepaint = true;   // new positions, drawn over the old without a blink
}


// ---------------------------------------------------------------------------
// Borduhr - the aircraft clock, as photographs
//
// Nothing on this screen is drawn. The dial is the photograph, at
// /faces/borduhr.rgb, in the same 240x240 RGB565 big-endian the album and the
// radar background use. The hands are photographs too - the watch's own, cut
// off their transparency and baked into display/BorduhrHands.h by
// scripts/gen/gen_borduhr.py - and the panel turns them and blends them over
// the dial with the alpha they were photographed with.
//
// Which means there is no erase step at all, and that is the point. A rectangle
// is rebuilt from the top down: read the dial's row out of flash, blend
// whichever hands cross that row into it, push the row. Whatever was there
// before is simply gone, so nothing can be left behind, and a hand can sweep
// across the numerals and the chapter ring without touching them.
//
// The rectangles are small. Every second only the seconds hand has moved, so
// only the ground it has left and the ground it has taken is rebuilt; the other
// three are redrawn as part of that, wherever they happen to cross it. On the
// minute the same is done for the others.
// ---------------------------------------------------------------------------

constexpr const char* BORD_PATH = "/faces/borduhr.rgb";

File bordFile;
bool bordFileOk = false;
bool bordDrawn = false;
int32_t bordLastSec = -1;
int32_t bordLastMin = -1;
int32_t bordLastHour = -1;

// The open file holds a LittleFS cache buffer, and where it lands in the heap
// matters more than its size - opened mid-life it sits in the middle and halves
// the largest free block, which is the block TLS wants. So it is let go before
// anything dials out, and the next draw opens it again.
void bordRelease() {
    if (bordFile) bordFile.close();
    bordFileOk = false;
    bordDrawn = false;
}

bool bordReady() {
    if (bordFileOk && bordFile) return true;
    if (bordFile) bordFile.close();
    bordFileOk = false;
    if (!fsMounted) return false;
    if (!LittleFS.exists(BORD_PATH)) return false;
    bordFile = LittleFS.open(BORD_PATH, "r");
    if (!bordFile) return false;
    if (bordFile.size() < ALBUM_BYTES) {
        bordFile.close();
        return false;
    }
    bordFileOk = true;
    return true;
}

struct BordRect {
    int16_t x0, y0, x1, y1;   // inclusive
};

// A hand, ready to be asked what it looks like at a given screen pixel. The
// rotation is held as its cosine and sine so the inner loop has no trigonometry
// in it at all - two multiplies and an add per pixel per hand.
struct BordHand {
    const BorduhrHands::Sprite* sp;
    float cx, cy;      // where it turns, on the panel
    float px, py;      // where it turns, within its own sprite
    float ct, st;
    BordRect box;
};

BordHand bordHands[4];
BordRect bordPrev[4];
bool bordPrevOk = false;

BordRect bordUnion(const BordRect& a, const BordRect& b) {
    return {static_cast<int16_t>(a.x0 < b.x0 ? a.x0 : b.x0),
            static_cast<int16_t>(a.y0 < b.y0 ? a.y0 : b.y0),
            static_cast<int16_t>(a.x1 > b.x1 ? a.x1 : b.x1),
            static_cast<int16_t>(a.y1 > b.y1 ? a.y1 : b.y1)};
}

// Turned clockwise from twelve, on a screen whose y grows downward. The corners
// of the sprite go round with it, and what they span is what has to be rebuilt.
void bordAim(BordHand& h, const BorduhrHands::Sprite& sp, float cx, float cy, float deg) {
    h.sp = &sp;
    h.cx = cx;
    h.cy = cy;
    h.px = sp.pivotX16 / 16.0f;
    h.py = sp.pivotY16 / 16.0f;
    const float th = deg * PI / 180.0f;
    h.ct = cosf(th);
    h.st = sinf(th);
    // The sprite is SPRITE_SCALE times panel resolution, so its extent on the
    // panel is its own extent divided down.
    const float inv = 1.0f / BorduhrHands::SPRITE_SCALE;
    const float ax = -h.px * inv, ay = -h.py * inv;
    const float bx = ax + (sp.w * inv), by = ay + (sp.h * inv);
    const float xs[4] = {ax, bx, bx, ax};
    const float ys[4] = {ay, ay, by, by};
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint8_t i = 0; i < 4; ++i) {
        const float X = cx + (xs[i] * h.ct) - (ys[i] * h.st);
        const float Y = cy + (xs[i] * h.st) + (ys[i] * h.ct);
        if (X < x0) x0 = X;
        if (X > x1) x1 = X;
        if (Y < y0) y0 = Y;
        if (Y > y1) y1 = Y;
    }
    h.box = {static_cast<int16_t>(floorf(x0) - 1), static_cast<int16_t>(floorf(y0) - 1),
             static_cast<int16_t>(ceilf(x1) + 1), static_cast<int16_t>(ceilf(y1) + 1)};
}

// Rebuild one rectangle: the dial as it is on flash, with every hand that
// crosses it blended back on top. Row by row, because that is how the file is
// laid out and how the panel wants to be fed.
void bordPaint(BordRect r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > SCREEN_W - 1) r.x1 = SCREEN_W - 1;
    if (r.y1 > SCREEN_H - 1) r.y1 = SCREEN_H - 1;
    const int16_t w = static_cast<int16_t>(r.x1 - r.x0 + 1);
    if (w <= 0 || r.y1 < r.y0) return;

    uint8_t row[SCREEN_W * 2];
    const size_t want = static_cast<size_t>(w) * 2U;
    tft.setSwapBytes(false);
    for (int16_t y = r.y0; y <= r.y1; ++y) {
        bordFile.seek((static_cast<uint32_t>(y) * SCREEN_W + r.x0) * 2U);
        if (bordFile.read(row, want) != static_cast<int>(want)) {
            // A short read means the file went away under us - released for a
            // fetch, or the filesystem hiccupped. The rows already pushed are
            // fine; the ones not reached would just stay stale, so the whole
            // scene is marked for a fresh start instead of trusting them.
            bordDrawn = false;
            return;
        }
        // The red index at twelve stands proud of the dial and the seconds hand
        // runs underneath it, as it does on the watch. Everything here is drawn
        // over the dial, so the index is set aside before the hands go on and
        // put back after: the picture wins, on this one rectangle.
        uint8_t keep[(BorduhrHands::MARK_X1 - BorduhrHands::MARK_X0 + 1) * 2];
        int16_t ka = -1, kb = -1;
        if (y >= BorduhrHands::MARK_Y0 && y <= BorduhrHands::MARK_Y1) {
            ka = BorduhrHands::MARK_X0 > r.x0 ? BorduhrHands::MARK_X0 : r.x0;
            kb = BorduhrHands::MARK_X1 < r.x1 ? BorduhrHands::MARK_X1 : r.x1;
            if (ka <= kb) memcpy(keep, &row[(ka - r.x0) * 2], static_cast<size_t>(kb - ka + 1) * 2U);
            else ka = -1;
        }
        for (uint8_t i = 0; i < 4; ++i) {
            const BordHand& h = bordHands[i];
            if (h.sp == nullptr || y < h.box.y0 || y > h.box.y1) continue;
            const int16_t xa = h.box.x0 > r.x0 ? h.box.x0 : r.x0;
            const int16_t xb = h.box.x1 < r.x1 ? h.box.x1 : r.x1;
            const float dy = static_cast<float>(y) - h.cy;
            for (int16_t x = xa; x <= xb; ++x) {
                const float dx = static_cast<float>(x) - h.cx;
                // The inverse turn, stepped up to sprite resolution: where on
                // the supersampled sprite this panel pixel looks.
                const float u = h.px + (((dx * h.ct) + (dy * h.st)) * BorduhrHands::SPRITE_SCALE);
                const float v = h.py + (((-dx * h.st) + (dy * h.ct)) * BorduhrHands::SPRITE_SCALE);
                if (u < 0.0f || v < 0.0f) continue;
                const int16_t su = static_cast<int16_t>(u);
                const int16_t sv = static_cast<int16_t>(v);
                if (su >= h.sp->w || sv >= h.sp->h) continue;
                // Bilinear over four texels, coverage and colour both. The
                // colour is premultiplied in the header, so a transparent
                // corner contributes nothing at all - averaging straight RGB
                // here is how a hand grows a fringe of whatever colour the
                // transparency was cut over. Taking the nearest texel instead
                // was the first attempt, and at eleven pixels across it turned
                // every hand into a saw blade: no rotation lands on whole
                // pixels, and each angle stepped the edge differently.
                const int16_t su1 = su + 1 < h.sp->w ? su + 1 : su;
                const int16_t sv1 = sv + 1 < h.sp->h ? sv + 1 : sv;
                const uint32_t fu = static_cast<uint32_t>((u - su) * 256.0f);
                const uint32_t fv = static_cast<uint32_t>((v - sv) * 256.0f);
                const uint32_t w00 = (256 - fu) * (256 - fv);
                const uint32_t w10 = fu * (256 - fv);
                const uint32_t w01 = (256 - fu) * fv;
                const uint32_t w11 = fu * fv;
                const uint32_t i00 = static_cast<uint32_t>(sv) * h.sp->w + su;
                const uint32_t i10 = static_cast<uint32_t>(sv) * h.sp->w + su1;
                const uint32_t i01 = static_cast<uint32_t>(sv1) * h.sp->w + su;
                const uint32_t i11 = static_cast<uint32_t>(sv1) * h.sp->w + su1;
                const uint32_t a = ((pgm_read_byte(&h.sp->alpha[i00]) * w00) +
                                    (pgm_read_byte(&h.sp->alpha[i10]) * w10) +
                                    (pgm_read_byte(&h.sp->alpha[i01]) * w01) +
                                    (pgm_read_byte(&h.sp->alpha[i11]) * w11)) >> 16;
                if (a == 0) continue;
                const uint16_t c00 = pgm_read_word(&h.sp->rgb[i00]);
                const uint16_t c10 = pgm_read_word(&h.sp->rgb[i10]);
                const uint16_t c01 = pgm_read_word(&h.sp->rgb[i01]);
                const uint16_t c11 = pgm_read_word(&h.sp->rgb[i11]);
                const uint32_t pr = ((((c00 >> 11) & 0x1F) * w00) + (((c10 >> 11) & 0x1F) * w10) +
                                     (((c01 >> 11) & 0x1F) * w01) + (((c11 >> 11) & 0x1F) * w11)) >> 16;
                const uint32_t pg = ((((c00 >> 5) & 0x3F) * w00) + (((c10 >> 5) & 0x3F) * w10) +
                                     (((c01 >> 5) & 0x3F) * w01) + (((c11 >> 5) & 0x3F) * w11)) >> 16;
                const uint32_t pb = (((c00 & 0x1F) * w00) + ((c10 & 0x1F) * w10) +
                                     ((c01 & 0x1F) * w01) + ((c11 & 0x1F) * w11)) >> 16;
                uint8_t* p = &row[(x - r.x0) * 2];
                const uint16_t dst = static_cast<uint16_t>((p[0] << 8) | p[1]);
                const uint32_t ia = 255 - a;
                uint32_t rr = pr + ((((dst >> 11) & 0x1F) * ia) + 127) / 255;
                uint32_t gg = pg + ((((dst >> 5) & 0x3F) * ia) + 127) / 255;
                uint32_t bb = pb + (((dst & 0x1F) * ia) + 127) / 255;
                if (rr > 31) rr = 31;
                if (gg > 63) gg = 63;
                if (bb > 31) bb = 31;
                const uint16_t out = static_cast<uint16_t>((rr << 11) | (gg << 5) | bb);
                p[0] = static_cast<uint8_t>(out >> 8);
                p[1] = static_cast<uint8_t>(out);
            }
        }
        if (ka >= 0) memcpy(&row[(ka - r.x0) * 2], keep, static_cast<size_t>(kb - ka + 1) * 2U);
        tft.pushImage(r.x0, y, w, 1, reinterpret_cast<uint16_t*>(row));
        if ((y & 0x0F) == 0) wdtYield();
    }
}

// Returns whether anything was put on the panel, which is what the IP badge
// needs to know - the same contract drawRadar keeps, for the same reason.
bool drawBorduhr(bool force) {
    if (force) bordDrawn = false;

    if (!bordReady()) {
        // The dial is a file, and a file can be missing - a firmware-only
        // update leaves the old filesystem in place. Say which file, because
        // "black screen" is not something anyone can act on.
        if (!bordDrawn) {
            tft.fillScreen(TFT_BLACK);
            drawCenteredText(96, F("Junghans Borduhr"), 2, TFT_YELLOW, TFT_BLACK, 0, SCREEN_W);
            drawCenteredText(124, F("dial file missing"), 2, TFT_LIGHTGREY, TFT_BLACK, 0, SCREEN_W);
            drawCenteredText(150, F("/faces/borduhr.rgb"), 1, TFT_DARKGREY, TFT_BLACK, 0, SCREEN_W);
            bordDrawn = true;
            return true;
        }
        return false;
    }

    const time_t now = time(nullptr);
    const bool validTime = now > 1700000000;
    tm t{};
    if (validTime) localtime_r(&now, &t);
    const int32_t sec = validTime ? t.tm_sec : 0;
    const int32_t minute = validTime ? t.tm_min : 0;
    if (bordDrawn && sec == bordLastSec) return false;

    const float degSec = static_cast<float>(sec) * 6.0f;
    // Whole minutes only - no seconds fraction. The fraction was a real bug,
    // not a nicety: the hands are re-aimed every tick but only the ground the
    // seconds hand crossed is repainted, so a minute hand that creeps 0.1
    // degrees a second got repainted at its new angle exactly where the
    // seconds hand happened to cross it and nowhere else - and the owner
    // watched it move in halves. Aim only at angles that are repainted whole.
    const float degMin = static_cast<float>(minute) * 6.0f;
    const float degHour = (static_cast<float>(validTime ? (t.tm_hour % 12) : 0) * 30.0f) +
                          (static_cast<float>(minute) * 0.5f);
    // The register is a chronograph's elapsed-minute counter, and on the
    // photographed watch it stands where its last run left it: between the 4
    // and the 6, nearer the 6. It is reproduced standing exactly there.
    // Driving it from the clock was tried and read as the wrong thing on the
    // dial - this counter never told the time of day.
    const float degSub = 64.0f;

    const float hx = BorduhrHands::HUB_X16 / 16.0f;
    const float hy = BorduhrHands::HUB_Y16 / 16.0f;
    const float rx = BorduhrHands::REG_X16 / 16.0f;
    const float ry = BorduhrHands::REG_Y16 / 16.0f;

    BordRect old[4];
    for (uint8_t i = 0; i < 4; ++i) old[i] = bordHands[i].box;
    const bool had = bordPrevOk;

    bordAim(bordHands[0], BorduhrHands::HOUR, hx, hy, degHour);
    bordAim(bordHands[1], BorduhrHands::MINUTE, hx, hy, degMin);
    bordAim(bordHands[2], BorduhrHands::REGISTER, rx, ry, degSub);
    bordAim(bordHands[3], BorduhrHands::SECONDS, hx, hy, degSec);
    bordPrevOk = true;

    if (!bordDrawn) {
        // Nothing on the panel worth keeping: lay the whole picture down.
        bordPaint(BordRect{0, 0, SCREEN_W - 1, SCREEN_H - 1});
        bordDrawn = true;
        bordLastSec = sec;
        bordLastMin = minute;
        bordLastHour = validTime ? t.tm_hour : 0;
        return true;
    }

    // The ground the seconds hand has left, and the ground it has taken.
    bordPaint(had ? bordUnion(old[3], bordHands[3].box) : bordHands[3].box);
    // The hour is checked in its own right, not inferred from the minute: an
    // NTP step or a timezone change can move the hour while landing on the
    // same minute number, and inferring left the hour hand standing on the
    // old hour for up to a minute.
    const int32_t hourNow = validTime ? t.tm_hour : 0;
    if (minute != bordLastMin || hourNow != bordLastHour) {
        for (uint8_t i = 0; i < 3; ++i) {
            bordPaint(had ? bordUnion(old[i], bordHands[i].box) : bordHands[i].box);
        }
    }

    bordLastSec = sec;
    bordLastMin = minute;
    bordLastHour = hourNow;
    return true;
}
// Whether anything was put on the panel. Three of these know: the forecast,
// the Borduhr and the radar all return it, and their answers are passed on
// rather than thrown away. The rest repaint something on every pass - a colon,
// a second hand, a photo - so true is the truth for them, not a placeholder.
bool drawActiveScreen(bool force) {
    if (activeScreen == SCREEN_FORECAST) {
        analogBandEnd();
        return drawForecast(force);
    } else if (activeScreen == SCREEN_BORDUHR) {
        return drawBorduhr(force);
    } else if (activeScreen == SCREEN_RADAR) {
        bootMarkWork(W_RADAR_DRAW);
        return drawRadar(force);
    } else if (activeScreen == SCREEN_ALBUM) {
        bootMarkWork(W_ALBUM);
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
    return true;
}

// What is on the panel right now, so the badge can be left alone when nothing
// underneath it has moved.
String ipBadgeDrawn;

// Small, bottom left, over whatever the screen is showing. Redrawn when the
// screen beneath it repainted - which would have rubbed it out - or when the
// address changed. It used to go down on every pass regardless, and on a screen
// that paints once every half hour that is a black box and grey glyphs written
// over themselves once a second, which reads as a flicker along the bottom.
void drawIpBadge(bool repainted) {
    if (ipBadgeText.length() == 0) {
        ipBadgeDrawn = "";
        return;
    }
    // Not on the dashboard once it is showing the address itself. The badge
    // exists to answer "where do I point a browser" in the first three minutes,
    // and on that screen the answer is already in the top left corner - so the
    // badge would be the same string twice, in the corner the sweep and the
    // forecast row both crowd. Every caller runs after the screen is painted,
    // so activeScreen and the flag are both settled by the time this reads
    // them.
    if (activeScreen == SCREEN_CLOCK_WEATHER && dashboardShowsIp) {
        ipBadgeDrawn = "";
        return;
    }
    if (!repainted && ipBadgeDrawn == ipBadgeText) return;
    drawTextAt(tft, 3, SCREEN_H - 15, ipBadgeText, 1, TFT_DARKGREY, TFT_BLACK);
    ipBadgeDrawn = ipBadgeText;
}

// Defined with the rest of the network code, far below; the offline branch
// here is the only caller of the first, and the recovery block of the second.
void staRetryBegin();
void wifiCardAdopt();

void updateDisplay(bool force = false) {
    uint32_t now = millis();

    // No network, and it has been that way long enough to mean it: say so, and
    // keep saying it until there is something better to show.
    const bool online = WiFi.status() == WL_CONNECTED;
    if (online) {
        staDownSinceMs = 0;
    } else if (staDownSinceMs == 0) {
        staDownSinceMs = now;
    }
    if (!online && now - staDownSinceMs >= OFFLINE_GRACE_MS) {
        // The access point is only raised at boot, so a drop that happens later
        // needs it brought up here - otherwise the screen advertises a network
        // that is not on the air.
        if (!apRunning) {
            // AP_STA for the same reason as at boot: the station has to stay on
            // the air or the block below, which is what takes this screen down
            // again, can never be reached.
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(AP_SSID);
            apRunning = true;
        }
        if (!offlineScreenDrawn) {
            drawOfflineScreen();
            offlineScreenDrawn = true;
        }
        static uint32_t staRetryAtMs = 0;
        if (staRetryAtMs == 0 || now - staRetryAtMs >= STA_RETRY_MS) {
            staRetryAtMs = now;
            staRetryBegin();
        }
        return;
    }
    if (offlineScreenDrawn) {
        // Back on the network. The AP was only ever a way in; take it down and
        // let the normal screens have the panel again.
        wifiCardAdopt();
        offlineScreenDrawn = false;
        if (apRunning) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            apRunning = false;
        }
        force = true;
        screenChromeDrawn = false;
        analogChromeDrawn = false;
        digitalLastStamp = -1;
        resetDisplayCache();
    }

    // The address shows for the first few minutes and then stops, which needs
    // the screen under it drawn again - the badge is text, and text does not
    // rub itself out.
    const bool wantBadge = online && now < IP_SHOW_MS;
    if (wantBadge) {
        ipBadgeText = WiFi.localIP().toString();
    } else if (ipBadgeShowing) {
        ipBadgeText = "";
        force = true;
        screenChromeDrawn = false;
        analogChromeDrawn = false;
        digitalLastStamp = -1;
        resetDisplayCache();
        radarSceneDrawn = false;
        albumDrawn = false;
    }
    ipBadgeShowing = wantBadge;

    if (enabledScreenCount() > 1) {
        const uint32_t intervalMs = static_cast<uint32_t>(cfg.themeIntervalSeconds) * 1000UL;
        if (now - lastScreenSwitchMs >= intervalMs) {
            activeScreen = nextEnabledScreen(activeScreen);
            lastScreenSwitchMs = now;
            screenChromeDrawn = false;
            analogChromeDrawn = false;
            digitalLastStamp = -1;
            resetDisplayCache();
            // The same two the badge path above resets. Their absence here was
            // survivable only because force reaches drawRadar and drawAlbum,
            // which clear them themselves - but anything reading them before
            // that saw a screen claiming to be drawn that no longer was.
            radarSceneDrawn = false;
            albumDrawn = false;
            force = true;
        }
    }

    // Ahead of the one-second gate: the colon has to flip twice a second, and
    // this repaints two dots rather than a screen.
    minuteFaceColonTick(false);
    if (activeScreen == SCREEN_BORDUHR) {
        // Its own gate: the hand has to land on the second, and the one-second
        // gate below is a "not more often than", which lets a tick slip past
        // and shows up as the needle jumping two marks at once.
        if (drawBorduhr(force)) drawIpBadge(true);
        if (force) lastDisplayMs = now;
        return;
    }
    if (activeScreen == SCREEN_RADAR) {
        // force has to survive: it is what hands the band sprite back, and a TLS
        // handshake needs those 11.5 KB. The sweep is what bypasses the
        // one-second gate, not the flag.
        radarService();
        // Only when the radar actually painted. Everything else on this screen
        // runs off the sweep's own clock, and the badge is a repair of what
        // that clock disturbed - not a frame of its own.
        if (drawRadar(force)) drawIpBadge(true);
        if (force) lastDisplayMs = now;
        return;
    }

    if (!force && now - lastDisplayMs < DISPLAY_INTERVAL_MS) return;
    lastDisplayMs = now;
    drawIpBadge(drawActiveScreen(force));
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
    // This path always knew the exact length and threw it away, starting the
    // update with the whole region and ending it with evenIfRemaining. It is
    // the recovery port - the one reached when the device is already in
    // trouble - so it should be the strictest, not the loosest.
    if (contentLength == 0 || contentLength > capacity) {
        rawReply(client, 500, "Too Big",
                 String("refused ") + contentLength + " bytes, " + capacity + " available\n");
        return;
    }
    if (mode == U_FS) {
        LittleFS.end();
        fsMounted = false;
        delay(50);
    }
    if (!Update.begin(contentLength, mode)) {
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
        if (total == 0 && mode == U_FLASH && buf[0] != 0xE9) {
            failed = true;
            error = "not an esp8266 image";
            break;
        }
        if (Update.write(buf, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
            failed = true;
            error = Update.getErrorString();
            break;
        }
        total += got;
    }
    if (!failed && total != contentLength) {
        failed = true;
        error = String("truncated: got ") + total + " of " + contentLength;
    }
    // Exact: begin was given the real length, so anything short fails here.
    if (!failed && !Update.end(false)) {
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
    doc["ap_ip"] = apRunning ? WiFi.softAPIP().toString() : String("off");
    doc["fs_mounted"] = fsMounted;
    doc["last"] = lastStatus;
    doc["weather_status"] = weather.status;
    doc["night_mode_active"] = isNightModeActive();
    doc["effective_brightness"] = effectiveBrightness();
    // largest contiguous block matters more than the total: a sprite needs one
    doc["free_heap"] = ESP.getFreeHeap();
    doc["max_free_block"] = ESP.getMaxFreeBlockSize();
    doc["heap_frag_pct"] = ESP.getHeapFragmentation();
    // For the memory gauges on the web page: RAM against the post-boot heap,
    // firmware against its OTA slot, and the filesystem against itself. The
    // filesystem numbers come from a real block walk, so they are cached and
    // refreshed at most every 30 seconds - the page polls this endpoint.
    doc["heap_boot"] = heapAtBoot;
    doc["safe_mode"] = bootSafeMode;
    doc["boot_fails"] = bootMark.fails;
    // Where the previous boot stopped: 2 fs, 3 config, 4 wifi, 5 routes,
    // 6 radar sweep, 7 album, 8 ready, 9 settled, 20 safe mode.
    doc["prev_boot_phase"] = bootMark.prevPhase;
    doc["min_heap"] = bootMark.minHeap;
    doc["work"] = bootMark.work;
    doc["detail"] = bootMark.detail;
    doc["album_skipped"] = workKilledLastBoot(W_ALB_JPG) || workKilledLastBoot(W_ALB_RAW);
    // The last eight boots, newest first: how far each got, what ended it, and
    // the least heap it ever had. reason follows the SDK: 0 power-on,
    // 1 hardware watchdog, 2 exception, 3 software watchdog, 4 software
    // restart, 5 deep-sleep wake, 6 external reset.
    {
        JsonArray hist = doc["boot_history"].to<JsonArray>();
        for (uint8_t i = 0; i < BOOT_HISTORY; ++i) {
            if (bootMark.histPhase[i] == 0 && bootMark.histReason[i] == 0 &&
                bootMark.histMinHeap[i] == 0) {
                continue;   // never filled
            }
            JsonObject b = hist.add<JsonObject>();
            b["phase"] = bootMark.histPhase[i];
            b["reason"] = bootMark.histReason[i];
            b["work"] = bootMark.histWork[i];
            b["detail"] = bootMark.histDetail[i];
            b["min_heap"] = bootMark.histMinHeap[i] == 0xFFFF ? 0 : bootMark.histMinHeap[i];
        }
    }
    doc["fw_used"] = ESP.getSketchSize();
    // Against the linker's program region, not the OTA slot. getFreeSketchSpace
    // says 1.2 MB and reads as plenty, but the build fails at 1044 KB - the
    // IROM segment eagle.flash.4m2m.ld lays out - and that is the wall that
    // matters. The constant is that ld script's, and PlatformIO prints the
    // same figure after every build as the Flash percentage.
    doc["fw_total"] = 1044464;
    {
        size_t total = 0, used = 0;
        albumFsInfo(total, used);
        doc["fs_used"] = used;
        doc["fs_total"] = total;
    }
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
    doc["ap_active"] = apRunning;
    doc["location"] = cfg.location;
    doc["kma_key"] = cfg.kmaKey;
    doc["kma_key_set"] = cfg.kmaKey.length() > 0;
    doc["web_password_is_default"] = cfg.webPassword == AUTH_DEFAULT_PASSWORD;
    {
        // Names only. This endpoint answers without authentication, and even
        // authenticated it has no business handing WiFi passwords to a page
        // that only needs to list which networks are on file.
        JsonArray arr = doc["wifi_profiles"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg.wifiProfileCount; ++i) {
            arr.add<JsonObject>()["ssid"] = cfg.wifiProfiles[i].ssid;
        }
    }
    {
        const String live = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
        doc["ssid_live"] = live;
        doc["pass_set"] = wifiPasswordKnown(live);
        doc["card_ready"] = cfg.ssid.length() > 0 && cfg.pass.length() > 0;
    doc["ow_key"] = cfg.owKey;
    doc["ow_key_set"] = cfg.owKey.length() > 0;
    doc["fc_preset_idx"] = cfg.fcPresetIdx;
    emitFcPresets(doc);
        doc["wifi_channel"] = cfg.wifiChannel;
        doc["wifi_bssid"] = bssidText(cfg.wifiBssid);
    }
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
    doc["wifi_channel"] = cfg.wifiChannel;
    doc["wifi_bssid"] = bssidText(cfg.wifiBssid);
    doc["radar_lat"] = cfg.radarLat;
    doc["radar_lon"] = cfg.radarLon;
    doc["radar_range_km"] = cfg.radarRangeKm;
    doc["radar_poll_sec"] = cfg.radarPollSec;
    doc["radar_min_alt_ft"] = cfg.radarMinAltFt;
    doc["radar_up_deg"] = cfg.radarUpDeg;
    doc["radar_routes"] = cfg.radarRoutes;
    doc["radar_bg"] = cfg.radarBg;
    doc["radar_preset"] = cfg.radarPreset;
    emitWeatherPresets(doc);
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
    loadWifiProfiles(doc["wifi_profiles"], true);
    cfg.location = doc["location"] | cfg.location;
    cfg.kmaKey = doc["kma_key"] | cfg.kmaKey;
    cfg.nx = doc["nx"] | cfg.nx;
    cfg.ny = doc["ny"] | cfg.ny;
    cfg.timezoneOffsetMinutes = doc["timezone_offset_minutes"] | cfg.timezoneOffsetMinutes;
    cfg.weatherEnabled = doc["weather_enabled"] | cfg.weatherEnabled;
    cfg.clock24h = doc["clock_24h"] | cfg.clock24h;
    // Only when something was actually typed. The page shows a mask for a key
    // it can never read back, and letting that mask through would store the
    // mask as the key.
    if (doc["ow_key"].is<const char*>()) {
        cfg.owKey = doc["ow_key"].as<const char*>();
    }
    if (doc["fc_preset_idx"].is<unsigned int>()) {
        const uint8_t i = static_cast<uint8_t>(doc["fc_preset_idx"].as<unsigned int>());
        if (i < cfg.fcPresetCount) {
            cfg.fcPresetIdx = i;
            fcLastMs = 0;   // another place means the week on screen is the wrong one
            fcNextMs = 0;
        }
    }
    loadFcPresets(doc["fc_presets"]);
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
    if (doc["radar_bg"].is<const char*>()) {
        // The id becomes a filename, so it passes the same gate the album ids
        // do - and an empty string is the way back to the plain dial.
        const char* bg = doc["radar_bg"].as<const char*>();
        cfg.radarBg = albumIdOk(bg) ? bg : "";
    }
    if (doc["radar_preset"].is<const char*>()) {
        cfg.radarPreset = String(doc["radar_preset"].as<const char*>()).substring(0, 24);
    }
    loadWeatherPresets(doc["weather_presets"]);
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

    // Whatever this request did to the saved locations or to the background,
    // what is on disk follows from it now - so an image nothing points at any
    // more goes, and one just uploaded is kept because radar_bg points at it.
    radarBgSweep();
    bool ok = saveConfig();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com");
    applyBrightness();
    // Only dirty bands are repainted, so a colour change needs the whole face
    // redrawn or the old colour survives everywhere the second hand has not been.
    if (coloursChanged) analogChromeDrawn = false;
    if (screensChanged) applyScreenSelection();
    refreshWeather();
    updateDisplay(true);
    // Safe mode gets its own answer. "save failed" would read as a filesystem
    // problem, when the truth is that the settings in memory are blanks and
    // writing them would destroy the file - which is exactly what happened once.
    if (!ok && bootSafeMode) {
        sendText(503, F("safe mode: settings not loaded, refusing to save over them - restart first\n"));
        return;
    }
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
    doc["lgt"] = weather.lgt;
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

// Walks into the directories rather than stopping at them. The flat listing
// answered "album 0" for a folder holding megabytes, which is precisely the
// question this endpoint gets asked: what is actually taking up the space, and
// is anything in there that the album's index has forgotten about.
void fsListInto(String& body, const String& prefix, uint8_t depth) {
    Dir dir = LittleFS.openDir(prefix.length() ? prefix : String("/"));
    while (dir.next()) {
        const String name = prefix + dir.fileName();
        if (dir.isDirectory()) {
            body += name + "/\t<dir>\n";
            if (depth > 0) fsListInto(body, name + "/", static_cast<uint8_t>(depth - 1));
        } else {
            body += name + "\t" + String(dir.fileSize()) + "\n";
        }
    }
}

bool fsInfoStale = true;

bool fsRemove(const String& path) {
    const bool ok = LittleFS.remove(path);
    if (ok) fsInfoStale = true;
    return ok;
}

void handleFsList() {
    if (!fsMounted && !LittleFS.begin()) {
        sendText(500, F("LittleFS mount failed\n"));
        return;
    }
    fsMounted = true;
    String body;
    fsListInto(body, "/", 2);
    sendText(200, body.length() ? body : String(F("(empty)\n")));
}

void handleFormat() {
    LittleFS.end();
    fsMounted = false;
    bool ok = LittleFS.format();
    fsInfoStale = true;
    fsMounted = ok && LittleFS.begin();
    sendText(ok ? 200 : 500, ok ? "format ok\n" : "format failed\n");
}

void handleRestart() {
    sendText(200, F("restarting\n"));
    delay(300);
    ESP.restart();
}

// A truncated image that still arrives as a tidy end-of-upload is the way this
// firmware can brick a device, and it has. Update.begin was handed the whole
// free region rather than the image's real length, so Update.end(true) had to
// accept whatever had turned up - half an image included - and the device
// rebooted into a sketch that does not boot. There is no USB-serial chip on
// this board, so the only way back is the UART pads inside the case.
//
// The cure is to be told what to expect. A caller that sends ?size= and ?md5=
// gets the strict path: the exact length goes to Update.begin, the hash goes to
// Update.setMD5, and Update.end() refuses anything short or altered. A caller
// that sends neither still gets the checks that need no cooperation - the
// image has to start with an ESP8266 header and cannot be implausibly small.
uint32_t otaExpected = 0;    // bytes the caller promised, 0 if it did not say
uint32_t otaWritten = 0;     // bytes actually taken in
bool otaSawFirst = false;
String otaRefusal;           // set when the request was turned away before Update started
constexpr uint32_t OTA_MIN_FLASH_BYTES = 200000;   // a real build is ~885 KB

void otaStart(const String& filename, int mode) {
    lastStatus = String("ota start ") + filename;
    drawSystemScreen();
    otaExpected = static_cast<uint32_t>(server.arg(F("size")).toInt());
    otaWritten = 0;
    otaSawFirst = false;
    otaRefusal = "";
    const String md5 = server.arg(F("md5"));

    const size_t capacity =
        mode == U_FS ? static_cast<size_t>(FS_PHYS_SIZE) : ((ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U);
    if (otaExpected > capacity) {
        // Said before a byte is written, which is the whole point of asking.
        otaRefusal = String("ota too big: ") + otaExpected + " bytes, " + capacity + " available";
        lastStatus = otaRefusal;
        drawSystemScreen();
        return;   // Update never starts, so otaWrite writes nothing
    }
    if (mode == U_FS) {
        LittleFS.end();
        fsMounted = false;
        delay(50);
    }
    if (!Update.begin(otaExpected > 0 ? otaExpected : capacity, mode)) {
        lastStatus = String("begin failed ") + Update.getErrorString();
        return;
    }
    if (md5.length() == 32) Update.setMD5(md5.c_str());
}

void otaWrite(HTTPUpload& upload, int mode) {
    if (Update.hasError() || !Update.isRunning()) return;
    if (!otaSawFirst && upload.currentSize > 0) {
        otaSawFirst = true;
        // Every ESP8266 sketch image opens with 0xE9. A filesystem image or a
        // truncated download that begins mid-stream does not.
        if (mode == U_FLASH && upload.buf[0] != 0xE9) {
            // Kept as the refusal so the reply says this rather than whatever
            // the Updater makes of being stopped early ("No data supplied").
            otaRefusal = F("ota rejected: not an esp8266 image");
            lastStatus = otaRefusal;
            Update.end();
            return;
        }
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        lastStatus = String("write failed ") + Update.getErrorString();
        return;
    }
    otaWritten += upload.currentSize;
    wdtYield();
}

void otaFail(const String& why) {
    Update.end();
    lastStatus = why;
    drawSystemScreen();
    server.send(500, F("text/plain"), why + "\n");
}

void otaEnd(int mode) {
    if (otaRefusal.length()) {
        otaFail(otaRefusal);
        return;
    }
    if (Update.hasError()) {
        otaFail(String("ota failed ") + Update.getErrorString());
        return;
    }
    if (otaExpected > 0 && otaWritten != otaExpected) {
        otaFail(String("ota truncated: got ") + otaWritten + " of " + otaExpected + " bytes");
        return;
    }
    if (mode == U_FLASH && otaWritten < OTA_MIN_FLASH_BYTES) {
        otaFail(String("ota too small: ") + otaWritten + " bytes is not a firmware image");
        return;
    }
    // Exact only when a length was given, because only then was Update started
    // with the real size and can have nothing remaining. A caller that sent a
    // hash but no length still gets that hash checked inside end() - but end()
    // must be allowed to finish, since it was handed the whole region and will
    // always have bytes to spare. This distinction is why md5 alone used to
    // fail every upload it was meant to protect.
    if (!Update.end(otaExpected == 0)) {
        otaFail(String("ota failed ") + Update.getErrorString());
        return;
    }
    // Without a promised length there is no way to know an image was cut short
    // - the bytes that arrived are all the evidence there is - so the reply
    // says as much rather than letting a bare "OK" be read as a guarantee. It
    // is not: an OK only ever meant the upload finished.
    if (otaExpected == 0) {
        lastStatus = String("ota ok (unverified) ") + otaWritten + " bytes";
        server.send(200, F("text/plain"),
                    String(F("OK unverified: no size given, ")) + otaWritten + F(" bytes written\n"));
    } else {
        lastStatus = String("ota ok ") + otaWritten + " bytes";
        server.send(200, F("text/plain"), F("OK\n"));
    }
    delay(800);
    ESP.restart();
}

void handleMultipartOta(int mode) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) otaStart(upload.filename, mode);
    else if (upload.status == UPLOAD_FILE_WRITE) otaWrite(upload, mode);
    else if (upload.status == UPLOAD_FILE_END) otaEnd(mode);
    else if (upload.status == UPLOAD_FILE_ABORTED) Update.end();
}

bool fileUploadFailed = false;   // set by handleFileUpload, read by handleFileDone

// Room is checked before the first byte, not discovered when a write comes up
// short. A caller that sends ?size= is answered before it uploads anything;
// without it the only honest thing left is to notice the failure on the way
// past. The filesystem is measured for real here rather than through the
// thirty-second cache the gauges use - two uploads in a row would otherwise be
// weighed against a figure taken before the first one landed.
void handleFileUpload() {
    static File file;
    bool& failed = fileUploadFailed;
    static String path;
    static uint32_t promised = 0;
    static uint32_t written = 0;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        path = server.arg(F("path"));
        failed = false;
        written = 0;
        promised = static_cast<uint32_t>(server.arg(F("size")).toInt());
        if (!validFsPath(path) || (!fsMounted && !LittleFS.begin())) {
            failed = true;
            return;
        }
        fsMounted = true;
        if (promised > 0) {
            FSInfo info{};
            if (LittleFS.info(info)) {
                // What the file being replaced gives back, so overwriting a
                // photo with one the same size is not refused on a full disk.
                uint32_t reclaimed = 0;
                if (LittleFS.exists(path)) {
                    File old = LittleFS.open(path, "r");
                    if (old) {
                        reclaimed = old.size();
                        old.close();
                    }
                }
                // LittleFS needs slack for its own metadata; a filesystem run
                // to the last byte starts failing writes in ways that are much
                // harder to explain than a refusal here.
                const uint32_t free = info.totalBytes - info.usedBytes + reclaimed;
                if (promised + 8192U > free) {
                    lastStatus = String("file too big: ") + promised + " bytes, " + free + " free";
                    failed = true;
                    return;
                }
            }
        }
        ensureParentDirs(path);
        file = LittleFS.open(path, "w");
        if (!file) failed = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!failed && file) {
            if (file.write(upload.buf, upload.currentSize) != upload.currentSize) failed = true;
            else written += upload.currentSize;
        }
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (file) file.close();
        if (!failed && upload.status == UPLOAD_FILE_ABORTED) {
            lastStatus = F("file write failed: upload aborted");
            fsRemove(path);   // a half file is worse than no file
        } else if (!failed && promised > 0 && written != promised) {
            lastStatus = String("file write failed: got ") + written + " of " + promised + " bytes";
            fsRemove(path);
        } else if (failed) {
            lastStatus = lastStatus.startsWith("file too big") ? lastStatus : String(F("file write failed"));
        } else {
            lastStatus = String("file ok ") + path;
            fsInfoStale = true;
        }
    }
}

// Reads the flag the upload actually set. Searching lastStatus for the word
// "failed" answered 200 to "file too big: 2000000 bytes, 991232 free", and the
// page reads a 200 as a stored file - so a refused upload looked like a saved
// one. Whether a write happened is not something to infer from prose.
void handleFileDone() {
    sendText(fileUploadFailed ? 500 : 200, lastStatus + "\n");
}

// A saved profile is redundant only when the WiFi Password card above holds
// exactly the same credentials, because that attempt has already been made.
// Matching on the SSID alone was the bug behind "WiFi 접속 실패" with good
// profiles saved: loadConfig fills cfg.ssid from the last-joined network, so
// the field is never empty, and with its password blank no attempt happens at
// all - yet every profile naming that same network was passed over as though
// one had. A wrong password in that field caused the same silent skip.
// Nothing is redundant. This used to skip a profile holding the same
// credentials as the WiFi card, on the reasoning that the attempt had already
// been made - and that reasoning cost a device its network. A wireless
// association fails for reasons that have nothing to do with the credentials
// being wrong: a busy channel, a router still coming up, a moment of bad
// signal. When the card's attempt failed for one of those, the profile naming
// the same network was skipped as a duplicate, the only remaining profile named
// a network in another building, and the device went to its access point while
// the right network sat there waiting.
//
// A second attempt at the same credentials costs seconds. Not making it costs
// the network until somebody walks over. The function is kept as the one place
// this decision is written down, and it now says what it should have said all
// along.
bool saveConfigAllowed() {
    return !bootSafeMode;
}

bool wifiProfileRedundant(const WifiProfile&) {
    return false;
}

// One candidate per call, round robin, and not waited on.
//
// connectSta blocks fifteen seconds a candidate, which is fine in setup and
// impossible from the draw loop. WiFi.begin returns at once and the SDK
// finishes in the background, so this costs a frame and the next pass through
// the offline branch reads the result off WiFi.status().
//
// One at a time because begin() is asynchronous: firing every candidate in a
// row would just overwrite the previous request before the SDK could answer it.
//
// persistent is turned off around the call. WiFi.persistent(true) is set at
// boot so the SDK keeps credentials for its own reconnects, but under it every
// begin() writes them to flash - and a retry every twenty seconds, forever,
// would be writing the same bytes to the same sector for as long as the
// network stayed down.
uint8_t staRetryIdx = 0;

void staRetryBegin() {
    // The same order setupNetwork uses at boot - the WiFi card, then the saved
    // profiles, then whatever the SDK kept. Two different orders for the same
    // job would be one more thing to hold in your head when this misbehaves.
    const bool card = cfg.ssid.length() > 0 && cfg.pass.length() > 0;
    const uint8_t lead = card ? 1 : 0;
    const uint8_t candidates = static_cast<uint8_t>(lead + cfg.wifiProfileCount + 1);
    const uint8_t i = staRetryIdx % candidates;
    staRetryIdx = static_cast<uint8_t>((staRetryIdx + 1) % candidates);
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
    WiFi.persistent(false);
    if (card && i == 0) {
        WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    } else {
        const uint8_t p = static_cast<uint8_t>(i - lead);
        if (p < cfg.wifiProfileCount && cfg.wifiProfiles[p].ssid[0] != 0) {
            WiFi.begin(cfg.wifiProfiles[p].ssid, cfg.wifiProfiles[p].pass);
        } else {
            WiFi.begin();   // whatever the SDK still has in its own flash
        }
    }
    WiFi.persistent(true);
}

// channel and bssid, when given, name exactly which radio on which channel to
// talk to, and the SDK stops scanning for the access point - that search is
// most of what a reconnect spends its time on. A hint that has gone stale (the
// router moved channel, or steered the device to a different radio) fails
// rather than falling back on its own, which is why the caller gives it a short
// timeout and then tries again without it.
bool connectSta(const char* ssid, const char* pass, bool stored,
                uint8_t channel = 0, const uint8_t* bssid = nullptr,
                uint32_t timeoutMs = STA_TIMEOUT_MS) {
    WiFi.mode(WIFI_STA);
    if (stored) {
        WiFi.begin();
    } else if (channel > 0 && bssid != nullptr) {
        WiFi.begin(ssid, pass, channel, bssid, true);
    } else {
        WiFi.begin(ssid, pass);
    }
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        wdtYield();
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

// Write down whatever actually worked.
//
// The card is the first thing setupNetwork tries, ahead of the scan and every
// profile. Leaving it empty meant the quickest way in was never taken: a device
// that had been on this network five minutes ago would still scan for two
// seconds and then walk the profiles to reach it. Worse, loadConfig fills the
// card from WiFi.SSID() and then lets the file's empty string overwrite it, so
// the card could not fill itself in even by accident.
//
// The password comes from the profile that names the same network - there is
// nowhere else to get it, since the one the radio is using is inside the SDK
// and cannot be read back. A connection made from the SDK's own stored
// credentials therefore teaches nothing, and the card is left alone rather than
// filled with a network and no way in.
//
// Saved only when it would change something. This runs on every boot and every
// recovery, and config.json lives in flash.
void wifiCardAdopt() {
    if (WiFi.status() != WL_CONNECTED) return;
    const String live = WiFi.SSID();
    if (live.length() == 0) return;

    // Where this network was found, so the next boot can be told instead of
    // having to look. Kept beside the card because that is what uses it.
    const uint8_t ch = static_cast<uint8_t>(WiFi.channel());
    const uint8_t* bs = WiFi.BSSID();
    bool moved = ch != cfg.wifiChannel;
    if (bs != nullptr) {
        for (uint8_t i = 0; i < 6 && !moved; ++i) moved = bs[i] != cfg.wifiBssid[i];
    }

    // The password can only come from a profile naming the same network. The one
    // the radio is using lives inside the SDK and cannot be read back, so a join
    // made from its stored credentials teaches nothing about how to repeat it -
    // but it still says where the access point was, and that is worth keeping.
    bool cardChanged = false;
    for (uint8_t i = 0; i < cfg.wifiProfileCount; ++i) {
        if (live != cfg.wifiProfiles[i].ssid) continue;
        if (cfg.wifiProfiles[i].pass[0] == 0) break;
        if (cfg.ssid != live || cfg.pass != cfg.wifiProfiles[i].pass) {
            cfg.ssid = live;
            cfg.pass = cfg.wifiProfiles[i].pass;
            cardChanged = true;
        }
        break;
    }

    // The hint belongs to the card, so it is only worth keeping when the card
    // names the network that was actually joined.
    const bool keepHint = moved && cfg.ssid == live && bs != nullptr;
    if (keepHint) {
        cfg.wifiChannel = ch;
        memcpy(cfg.wifiBssid, bs, sizeof(cfg.wifiBssid));
    }
    // Nothing changed, nothing written. This runs on every boot and on every
    // recovery, and saveConfig rewrites config.json in flash each time.
    if (cardChanged || keepHint) saveConfig();
}

void setupNetwork() {
    WiFi.persistent(true);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    bool staOk = false;
    if (cfg.ssid.length() > 0 && cfg.pass.length() > 0) {
        // Where it was last found, if that is known. Six seconds, because a
        // hinted join either lands almost at once or the hint is wrong.
        if (cfg.wifiChannel > 0) {
            staOk = connectSta(cfg.ssid.c_str(), cfg.pass.c_str(), false,
                               cfg.wifiChannel, cfg.wifiBssid, STA_HINT_MS);
            // Wrong hint. Forget it rather than spend six seconds on it every
            // boot from here on; a successful join writes a fresh one.
            if (!staOk) {
                cfg.wifiChannel = 0;
                memset(cfg.wifiBssid, 0, sizeof(cfg.wifiBssid));
            }
        }
        if (!staOk) staOk = connectSta(cfg.ssid.c_str(), cfg.pass.c_str(), false);
    }
    // The saved profiles, so the device can be carried between places and just
    // plugged in. A scan first: fifteen seconds is the price of one blind
    // attempt, a scan costs about two and says which profiles are actually on
    // the air. Profiles the scan did not see get their turn afterwards anyway -
    // a hidden SSID never appears in a scan and is still worth trying.
    if (!staOk && cfg.wifiProfileCount > 0) {
        WiFi.mode(WIFI_STA);
        wdtYield();
        const int8_t seen = WiFi.scanNetworks();   // blocking, seconds long
        wdtYield();
        bool tried[WIFI_PROFILE_MAX] = {false};
        for (uint8_t pass = 0; pass < 2 && !staOk; ++pass) {
            for (uint8_t i = 0; i < cfg.wifiProfileCount && !staOk; ++i) {
                const WifiProfile& p = cfg.wifiProfiles[i];
                if (tried[i] || wifiProfileRedundant(p)) continue;
                bool visible = false;
                for (int8_t n = 0; n < seen && !visible; ++n) {
                    visible = WiFi.SSID(n) == p.ssid;
                }
                if ((pass == 0) != visible) continue;
                tried[i] = true;
                staOk = connectSta(p.ssid, p.pass, false);
            }
        }
        WiFi.scanDelete();
    }
    if (!staOk) staOk = connectSta(nullptr, nullptr, true);
    // The compiled-in fallback earns a try only when it names a network. It is
    // empty in the published tree, and fifteen seconds were being spent on
    // every failed boot asking to join "".
    if (!staOk && FALLBACK_STA_SSID[0] != 0) staOk = connectSta(FALLBACK_STA_SSID, FALLBACK_STA_PASS, false);

    // Up only when it is the way in: the station failed, or someone asked for
    // it to stay up. Losing WiFi later leaves no AP until the next boot, and
    // that is the right trade - a boot with no network brings it back on its
    // own, while an open AP carrying an unauthenticated OTA has no business
    // being on the air for months at a time because of one bad afternoon.
    wifiCardAdopt();
    apRunning = !staOk;
    if (apRunning) {
        // AP_STA, not AP. The ternary that used to be here could only ever pick
        // the second branch - apRunning is !staOk, so staOk is false by the
        // time it is read - and WIFI_AP takes the station radio down. That made
        // the recovery condition unreachable: nothing can set WL_CONNECTED with
        // no station, so a device that missed its window at boot could not come
        // back however long the network was healthy again.
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_SSID);
    } else {
        // Asking for station mode does not take an access point off the air.
        // WiFi.persistent keeps the last AP configuration in flash and the SDK
        // raises it during boot, before any of this runs - so the first attempt
        // reported "off" while SDP-Recovery was still being broadcast at 95 per
        // cent, which nothing but a scan for it would have caught.
        // softAPdisconnect(true) stops the radio and clears that stored
        // configuration, so it stays down across the next boot too.
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
    }
}

// --- album API -------------------------------------------------------------
// Photo pixels go up through the existing /file route, which already streams an
// upload into LittleFS. What is left is the manifest, the thumbnails and the
// storage figures the card needs to tell the user how much room is left.

// Cached, and shared with /status: LittleFS.info walks every block of a 2 MB
// filesystem and it is the most expensive thing this web server can be asked
// to do. Once per thirty seconds is plenty for a pair of gauge needles.
void albumFsInfo(size_t& total, size_t& used) {
    static FSInfo info{};
    static uint32_t atMs = 0;
    if (fsMounted && (fsInfoStale || atMs == 0 || millis() - atMs > 30000UL)) {
        LittleFS.info(info);
        atMs = millis();
        fsInfoStale = false;
    }
    if (fsMounted) {
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

    // Sent in chunks. Sixty-four photos serialised into one String wanted an
    // 8 KB contiguous block on a heap with about 30 KB left in it, and the
    // count of photos should not be what decides whether this reply fits.
    // Every field in the head is a number, so none of it needs escaping; the
    // photo objects go through the serializer because a name is arbitrary
    // text taken from a filename.
    String head;
    head.reserve(256);
    head += F("{@interval_seconds@:");   head += cfg.albumIntervalSeconds;
    head += F(",@max_photos@:");         head += ALBUM_MAX;
    // What one more photo is likely to cost, used only until a real photo
    // exists to measure. JPEG sizes vary; 30 KB is a conservative ceiling for
    // a 240x240 at the quality the page encodes.
    head += F(",@slot_bytes@:30720");
    head += F(",@width@:");              head += ALBUM_W;
    head += F(",@height@:");             head += ALBUM_H;
    head += F(",@thumb@:");              head += ALBUM_THUMB;
    head += F(",@fs_total@:");           head += total;
    head += F(",@fs_used@:");            head += used;
    head += F(",@fs_free@:");            head += (total > used ? (total - used) : 0);
    // What the album itself occupies, as opposed to the device as a whole.
    head += F(",@album_bytes@:");        head += albumBytes();
    head += F(",@frame_us@:");           head += albumFrameUs;
    head += F(",@photos@:[");
    head.replace('@', '"');

    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, F("application/json"), "");
    server.sendContent(head);
    for (uint8_t i = 0; i < albumCount; ++i) {
        JsonDocument item;
        item["id"] = albumEntries[i].id;
        item["name"] = albumEntries[i].name;
        item["on"] = albumEntries[i].on;
        // Which file backs it, so the page knows whether the browser can show
        // the photo directly (jpg) or has to unpack a raw thumb (rgb).
        item["fmt"] = LittleFS.exists(albumPath(albumEntries[i].id, ".jpg")) ? "jpg" : "rgb";
        String one;
        serializeJson(item, one);
        if (i) one = "," + one;
        server.sendContent(one);
    }
    server.sendContent(F("]}"));
    server.sendContent("");   // ends the chunked reply
}

// Takes the whole list at once: order is the array order, so a reorder and a
// toggle are the same request. Entries whose pixels are missing are dropped
// rather than kept as a promise the renderer cannot honour.
void handleAlbumPost() {
    // Cleared per request. It is a global so the reply at the bottom can see
    // it, and leaving last request's value standing made a post that only
    // changed the interval report a staleness warning it had no evidence for.
    albumPostDropped = false;
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
        uint8_t offered = 0;
        for (JsonObject item : doc["photos"].as<JsonArray>()) {
            // Counted before the cap check, so a list longer than the device
            // can hold is reported rather than silently trimmed.
            ++offered;
            if (n >= ALBUM_MAX) continue;
            const String id = item["id"] | "";
            if (!albumIdOk(id)) continue;
            if (!LittleFS.exists(albumPath(id, ".jpg")) &&
                !LittleFS.exists(albumPath(id, ".rgb"))) continue;
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
        albumPostDropped = offered != n;
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
    // A dropped entry means the list that was posted named a photo this device
    // does not have - the page was working from a stale view. Say so instead of
    // answering "ok", because the stale view is the thing worth knowing about.
    if (albumPostDropped) {
        sendText(200, F("saved, but some entries named photos this device does not have - reload the page\n"));
        return;
    }
    sendText(200, F("ok\n"));
}

void handleAlbumDelete() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id\n"));
        return;
    }
    fsRemove(albumPath(id, ".jpg"));
    fsRemove(albumPath(id, ".rgb"));
    fsRemove(albumPath(id, ".thm"));
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
// The stored background, raw, for the web UI to draw a preview from. /file is
// upload-only, and there is no encoder here to turn 115 KB of RGB565 into
// anything smaller, so the browser gets the pixels and does the shrinking -
// the same division of labour that put the image here in the first place.
void handleRadarBg() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id"));
        return;
    }
    const String path = "/radar/" + id + ".rgb";
    if (!fsMounted || !LittleFS.exists(path)) {
        sendText(404, F("no image"));
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendText(500, F("open failed"));
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "application/octet-stream");
    f.close();
}

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

// The photo as stored: the JPEG when there is one - which a browser renders
// natively, so the grid needs no thumbnail files any more - or the raw RGB565
// otherwise, which is how the old photos get off the device to be converted.
void handleAlbumPhoto() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id\n"));
        return;
    }
    String path = albumPath(id, ".jpg");
    const char* mime = "image/jpeg";
    if (!fsMounted || !LittleFS.exists(path)) {
        path = albumPath(id, ".rgb");
        mime = "application/octet-stream";
        if (!fsMounted || !LittleFS.exists(path)) {
            sendText(404, F("no photo\n"));
            return;
        }
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendText(500, F("open failed\n"));
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, mime);
    f.close();
}

// Undo half a conversion: drop a photo's JPEG and fall back to its raw - but
// only while the raw is still there to fall back to, so this can never orphan
// a photo. What it is for: an upload that died mid-write leaves a truncated
// .jpg on the filesystem, and from that moment the photo endpoint serves the
// truncation instead of the good raw - the migration cannot even re-download
// what it needs to retry. This is the way back out.
void handleAlbumUnjpg() {
    const String id = server.arg("id");
    if (!albumIdOk(id)) {
        sendText(400, F("bad id\n"));
        return;
    }
    if (!LittleFS.exists(albumPath(id, ".rgb"))) {
        sendText(409, F("no raw to fall back to\n"));
        return;
    }
    fsRemove(albumPath(id, ".jpg"));
    sendText(200, F("ok\n"));
}

// Deliberate, not automatic: for every indexed photo that has a JPEG, drop the
// raw original and its thumbnail. Run after the conversions are verified -
// deleting the raw the moment a .jpg appears would trade the only good copy
// for a file nobody has proven decodable yet.
void handleAlbumCompact() {
    uint32_t freed = 0;
    uint8_t photos = 0;
    for (uint8_t i = 0; i < albumCount; ++i) {
        if (!LittleFS.exists(albumPath(albumEntries[i].id, ".jpg"))) continue;
        bool any = false;
        const char* exts[2] = {".rgb", ".thm"};
        for (uint8_t e = 0; e < 2; ++e) {
            const String victim = albumPath(albumEntries[i].id, exts[e]);
            if (!LittleFS.exists(victim)) continue;
            File f = LittleFS.open(victim, "r");
            if (f) {
                freed += f.size();
                f.close();
            }
            fsRemove(victim);
            any = true;
        }
        if (any) ++photos;
    }
    String out = "compacted ";
    out += photos;
    out += " photos, freed ";
    out += freed;
    out += " bytes\n";
    sendText(200, out);
}

// What the next boot would do with the saved WiFi settings, answered while the
// device is still online. The alternative was to prove the profile path by
// blanking the password and rebooting, which stakes the only way back to the
// device on credentials nobody has verified. This calls the very same
// wifiProfileRedundant the boot path calls, so the answer cannot drift from
// the behaviour it describes. The scan is opt-in (?scan=1) because scanning
// briefly stalls the connection this reply travels over.
void handleWifiPlan() {
    JsonDocument doc;
    doc["ssid"] = cfg.ssid;
    const bool primary = cfg.ssid.length() > 0 && cfg.pass.length() > 0;
    doc["primary_attempted"] = primary;
    if (!primary) doc["primary_skipped"] = cfg.ssid.length() ? "no password saved" : "no ssid saved";

    int8_t seen = -1;
    if (server.arg("scan") == "1") seen = WiFi.scanNetworks();

    JsonArray arr = doc["profiles"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.wifiProfileCount; ++i) {
        const WifiProfile& p = cfg.wifiProfiles[i];
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = p.ssid;
        o["has_pass"] = strlen(p.pass) > 0;
        const bool skip = wifiProfileRedundant(p);
        o["would_try"] = !skip;
        if (skip) o["skipped"] = "already tried";
        // Said plainly, because this is the field that revealed the fault:
        // a profile naming the same network as the card is tried anyway.
        o["same_as_card"] = (cfg.ssid == p.ssid);
        if (seen >= 0) {
            bool visible = false;
            for (int8_t n = 0; n < seen && !visible; ++n) visible = WiFi.SSID(n) == p.ssid;
            o["visible"] = visible;
        }
    }
    if (seen >= 0) WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    sendJson(200, out);
}

// Lists the files under /album with no index entry, and removes them only when
// asked with ?delete=1. Touches nothing outside /album - not /radar, not
// /faces, not the web files - and never the index itself.
void handleAlbumSweep() {
    String names[16];
    bool more = false;
    const uint8_t n = albumOrphans(names, 16, &more);
    const bool doIt = server.arg("delete") == "1";
    JsonDocument doc;
    doc["orphans"] = n;
    doc["more"] = more;   // true means run it again to see the rest
    doc["deleted"] = doIt;
    JsonArray arr = doc["files"].to<JsonArray>();
    for (uint8_t i = 0; i < n; ++i) {
        arr.add(names[i]);
        if (doIt) fsRemove(String(ALBUM_DIR) + "/" + names[i]);
    }
    String out;
    serializeJson(doc, out);
    sendJson(200, out);
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
        doc["block_before"] = radarBlockBefore;
        doc["block_low"] = radarBlockLow;
        doc["tls_block_cost"] = radarBlockBefore > radarBlockLow ? radarBlockBefore - radarBlockLow : 0;
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
    // Ask now rather than waiting out the half hour, which is what anyone does
    // after typing a key in for the first time.
    server.on(F("/api/forecast"), HTTP_GET, []() {
        JsonDocument doc;
        doc["status"] = fcStatus;
        doc["city"] = fcCity;
        doc["place"] = fcPreset().name;
        doc["fetch_ms"] = fcFetchMs;
        doc["key_set"] = cfg.owKey.length() > 0;
        doc["now_valid"] = fcNowUsable();
        const int32_t today = fcTodayYmd();
        ForecastDay rows[FC_DAYS];
        const uint8_t rowCount = forecastRows(rows, FC_DAYS);
        JsonArray arr = doc["days"].to<JsonArray>();
        for (uint8_t i = 0; i < rowCount; ++i) {
            const ForecastDay& d = rows[i];
            const bool isToday = today != 0 &&
                (today / 10000) * 10000 + d.month * 100 + d.day == today;
            const ForecastCell cell = forecastCell(d, isToday);
            JsonObject o = arr.add<JsonObject>();
            o["month"] = d.month;
            o["day"] = d.day;
            o["weekday"] = d.weekday;
            o["icon"] = d.icon;
            o["slot"] = cell.slot;
            o["word"] = cell.word;
            o["humidity"] = cell.humidity;
            o["feels"] = cell.feels;
            o["live"] = cell.live;
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });
    server.on(F("/api/forecast/fetch"), HTTP_POST, []() {
        const bool ok = forecastFetch();
        fcDrawnRevision = fcRevision - 1;   // make the screen redraw with whatever came back
        sendText(ok ? 200 : 500, fcStatus + " (" + String(fcFetchMs) + " ms)" + String(static_cast<char>(10)));
    });
    server.on(F("/api/radar/fetch"), HTTP_POST, []() {
        const bool ok = radarFetch();
        sendText(ok ? 200 : 500, radarStatus + " (" + String(radarFetchMs) + " ms)" + String(static_cast<char>(10)));
    });
    server.on(F("/api/wifi/plan"), HTTP_GET, handleWifiPlan);
    // Lists the radar maps no preset points at, and removes them only when asked
    // with ?delete=1. The boot sweep used to do this by itself.
    server.on(F("/api/radar/sweep"), HTTP_POST, []() {
        // Rescan rather than answer from the boot-time count, which is stale the
        // moment a map is uploaded or a preset edited. Safe to call now that the
        // sweep only counts.
        radarBgSweep();
        const bool doIt = server.arg("delete") == "1";
        String out = "{\"orphans\":" + String(radarBgOrphans) +
                     ",\"deleted\":" + String(doIt ? "true" : "false") + ",\"files\":[";
        for (uint8_t i = 0; i < radarBgOrphans && i < 8; ++i) {
            if (i) out += ',';
            out += '"' + radarBgOrphanIds[i] + '"';
            if (doIt) fsRemove("/radar/" + radarBgOrphanIds[i] + ".rgb");
        }
        out += "]}";
        if (doIt) radarBgOrphans = 0;
        sendJson(200, out);
    });
    server.on(F("/api/album"), HTTP_GET, handleAlbumGet);
    server.on(F("/api/album"), HTTP_POST, handleAlbumPost);
    server.on(F("/api/album/delete"), HTTP_POST, handleAlbumDelete);
    server.on(F("/api/album/thumb"), HTTP_GET, handleAlbumThumb);
    server.on(F("/api/album/photo"), HTTP_GET, handleAlbumPhoto);
    server.on(F("/api/album/compact"), HTTP_POST, handleAlbumCompact);
    server.on(F("/api/album/sweep"), HTTP_POST, handleAlbumSweep);
    server.on(F("/api/album/unjpg"), HTTP_POST, handleAlbumUnjpg);
    server.on(F("/api/radar/bg"), HTTP_GET, handleRadarBg);
    server.on(F("/format"), HTTP_POST, handleFormat);
    server.on(F("/restart"), HTTP_ANY, handleRestart);
    server.on(F("/update_ota"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fw"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FLASH); });
    server.on(F("/api/ota/fs"), HTTP_POST, []() {}, []() { handleMultipartOta(U_FS); });
    server.on(F("/file"), HTTP_POST, handleFileDone, handleFileUpload);
    // Deleting one file by path. The filesystem could be filled but never
    // tidied: uploading a map three times left three maps, and the only way to
    // reclaim the space was to rewrite the whole 2 MB image and lose the
    // settings and photos with it. Refuses a directory and anything outside the
    // filesystem, and says what it removed.
    server.on(F("/file"), HTTP_DELETE, []() {
        const String path = server.arg("path");
        if (!validFsPath(path)) {
            sendText(400, F("bad path\n"));
            return;
        }
        if (!fsMounted || !LittleFS.exists(path)) {
            sendText(404, F("not found\n"));
            return;
        }
        uint32_t bytes = 0;
        File f = LittleFS.open(path, "r");
        if (f) {
            if (f.isDirectory()) {
                f.close();
                sendText(400, F("that is a directory\n"));
                return;
            }
            bytes = f.size();
            f.close();
        }
        if (!fsRemove(path)) {
            sendText(500, F("remove failed\n"));
            return;
        }
        lastStatus = String("deleted ") + path;
        sendText(200, String("deleted ") + path + " (" + bytes + " bytes)\n");
    });
    server.onNotFound(handleStatic);
    server.begin();
    rawServer.begin();
    rawServer.setNoDelay(true);
}

}  // namespace

// Nothing here touches the filesystem, because the filesystem is the leading
// suspect whenever this runs. An access point and the routes, and that is all:
// enough to take an OTA and enough to be found.
void setupSafeMode() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("SAFE MODE", 10, 10, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("boot failed 3x", 10, 36, 2);
    tft.drawString("join", 10, 74, 2);
    tft.drawString(AP_SSID, 10, 96, 2);
    tft.drawString("http://192.168.4.1", 10, 130, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("POST /api/ota/fw", 10, 158, 2);
    tft.drawString("power off to retry", 10, 180, 2);

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    apRunning = true;
    // The access point alone would mean a device in safe mode can only be
    // reached by someone standing next to it with a phone joined to
    // SDP-Recovery. So the saved credentials get a try as well - but the call
    // is made and not waited on. Waiting is the thing that is dangerous here,
    // not asking: WiFi.begin() returns immediately and the SDK finishes in the
    // background, so this costs no time at all and the routes are already up
    // either way. If it joins, the device can be recovered over the house
    // network; if it does not, the access point is still there.
    WiFi.begin();
    lastStatus = "safe mode: boot failed 3x";
    bootMarkPhase(PH_SAFE);
    setupRoutes();
}

// Defined below; setup() needs it before the first paint.
void setupAlbumDecoder();

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

    // Auto-format OFF, once, before any mount anywhere in this firmware can
    // happen: setConfig is global, and there are seven LittleFS.begin() calls
    // in this file. The default formats a volume it cannot mount, which on a
    // device carrying the stock weather icons and fonts would erase them on
    // the first boot that failed to mount - silently, with no way back. A
    // volume that will not mount is a thing to report, not to erase.
    LittleFS.setConfig(LittleFSConfig(false));

    bootMarkBegin();
    makeAuthToken();
    if (bootSafeMode) {
        setupSafeMode();
        return;
    }

    // Fed on both sides of the mount and the config read. A filesystem with
    // work outstanding can spend seconds here, and there is no yield inside it.
    bootMarkPhase(PH_FS);
    wdtYield();
    fsMounted = LittleFS.begin();
    wdtYield();
    bootMarkPhase(PH_CONFIG);
    loadConfig();
    wdtYield();
    bootMarkPhase(PH_WIFI);
    // Once at boot as well, which is what clears out images left behind by a
    // firmware that had no idea it was supposed to tidy up after itself.
    setupNetwork();

    // The routes come up here, before the sweeps and the index reads, not
    // after them. This is the ordering the last firmware had wrong: every way
    // back into this device - /status, the OTA endpoints, the raw port - was
    // opened at the very end of setup(), so anything that stalled the work
    // above it reset the device before a single route existed, and the next
    // boot did the same thing. Whatever goes wrong below this line now goes
    // wrong on a device that can still be reached.
    bootMarkPhase(PH_ROUTES);
    setupRoutes();
    bootMarkPhase(PH_SWEEP);
    radarBgSweep();
    // After the network wait, snap the active screen onto the enabled set and
    // start the rotation clock here. Leaving lastScreenSwitchMs at 0 would make
    // the very first interval expire instantly, and leaving activeScreen at its
    // default would render a screen the user had switched off.
    bootMarkPhase(PH_ALBUM);
    albumLoadIndex();
    applyScreenSelection();
    configTime(cfg.timezoneOffsetMinutes * 60, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    // Before the first paint, because that paint can be the album and the
    // decoder cannot run without its callback. It used to be set on the first
    // pass of loop(), which is after this line: a device whose active screen
    // was the album entered TJpgDec with no callback registered and hung there
    // until the hardware watchdog reset it - every boot, forever, on any photo.
    // The boot history said READY every time, which is exactly this line.
    setupAlbumDecoder();

    bootMarkPhase(PH_READY);
    lastStatus = "ready";
    updateDisplay(true);
    // No fetch here: NTP has not answered yet. The loop runs it as soon as the
    // clock is set, which is within a few seconds of the network coming up.
}

void setupAlbumDecoder() {
    // 0 is 1:1. The scale is a power-of-two divisor, so 1 meant half size and
    // a 240x240 photo was being drawn into 120x120.
    TJpgDec.setJpgScale(0);
    TJpgDec.setCallback(albumJpgBlock);
}

void setupHeapMark() {
    heapAtBoot = ESP.getFreeHeap();
}

void loop() {
    if (heapAtBoot == 0) setupHeapMark();   // first pass after setup
    bootMarkWork(W_HTTP);
    server.handleClient();
    handleRawServerClient();
    // Shut the boot grace window, once, and for this boot only. Nothing is
    // reported when it closes: lastStatus is a diagnostic of the last real
    // operation, and the window is not something to advertise either.
    if (authGraceOpen && millis() >= AUTH_GRACE_MS) authGraceOpen = false;
    bootMarkHeap();

    // A boot counts as good once it has stayed up long enough to be worth
    // keeping - not at the end of setup(), which says nothing about a device
    // that dies a second later. Ten seconds is past the watchdog, past the
    // first screen paint, and past the first pass of everything below.
    //
    // This has to happen BEFORE safe mode returns, and it did not: a device
    // that reached safe mode and sat there perfectly healthy never cleared its
    // counter, so every restart pushed it deeper and the only way out was to
    // pull the power. Safe mode is a place to be rescued from, not a trap, and
    // ten seconds of serving requests there is exactly as good a sign of
    // health as ten seconds anywhere else.
    if (!bootMarkCleared && millis() > BOOT_SETTLED_MS) {
        bootMarkCleared = true;
        bootMark.fails = 0;
        bootMark.phase = bootSafeMode ? PH_SAFE : PH_SETTLED;
        bootMarkWrite();
    }

    // Safe mode serves requests and does nothing else. Everything below reaches
    // the filesystem or the network sooner or later, and not doing those is the
    // entire reason for being here.
    if (bootSafeMode) return;
    // Half-hourly, and only when the screen is in the rotation at all - there
    // is no sense spending a fetch on a week nobody is going to look at.
    if ((cfg.screens & (1U << SCREEN_FORECAST)) && WiFi.status() == WL_CONNECTED &&
        cfg.owKey.length() > 0 && (fcLastMs == 0 || millis() >= fcNextMs)) {
        bootMarkWork(W_HTTP);
        forecastFetch();
        bootMarkWork(W_IDLE);
    }
    // And today's row off the nowcast, every two hours. No fetch of its own -
    // the dashboard already pulls this half-hourly, so this only decides how
    // often the panel is allowed to move.
    // Either the two hours are up, or the sample went stale under us - the
    // nowcast falling silent is exactly when the row has to stop claiming to be
    // current, and waiting out the rest of the two hours would be the wrong way
    // round.
    if ((cfg.screens & (1U << SCREEN_FORECAST)) &&
        ((fcNowLastMs == 0 || millis() - fcNowLastMs >= fcNowWaitMs) ||
         (fcNow.valid && !fcNowUsable()))) {
        forecastSampleNow();
    }
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
        const bool cooled = now - lastWeatherTryMs >= weatherRetryDelay();
        if (clockReady && (!weatherBootDone || (due && cooled))) {
            weatherBootDone = true;
            bootMarkWork(W_WEATHER);
            refreshWeather();
        }
    }
    applyBrightness();
    bootMarkWork(W_DISPLAY);
    updateDisplay(false);
    bootMarkWork(W_IDLE);
    wdtYield();
}
