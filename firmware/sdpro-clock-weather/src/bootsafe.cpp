// SDP Bootsafe - a firmware whose only job is to be reachable.
//
// It exists because the full firmware could not be put on a new device safely.
// The stock firmware is 488 KB on a 4m3m layout, which leaves 547 KB of staging
// room for an OTA; the full build is 872 KB and simply does not fit. Worse, the
// full build was carved out of a firmware that opens its recovery routes at the
// very end of setup(), after mounting the filesystem, reading config, sweeping
// directories and connecting WiFi - so anything that stalls that work past the
// eight-second watchdog resets the device before a single route exists, and the
// next boot does the same thing. That cost this project a device.
//
// So this one inverts the order. The access point and the routes come up first,
// on nothing but RAM, and the filesystem and the station connection happen
// afterwards where their failure cannot take the way in with them. A counter in
// RTC memory notices boots that never settled and skips even that much.
//
// It is also built for the stock 4m3m layout on purpose: that makes the stock
// filesystem mountable, which is how the weather icons in /ico come off the
// device at all - there is no download route in the stock web UI.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <Updater.h>

namespace {

constexpr const char* FW_NAME = "SDP Bootsafe";
constexpr const char* FW_VERSION = "bootsafe-1";
constexpr const char* AP_SSID = "SDP-Recovery";

// Short on purpose. This firmware never needs to wait long for a network: the
// access point is already up, so a station that will not answer costs nothing
// but the seconds spent asking.
constexpr uint32_t STA_TIMEOUT_MS = 8000;

// A boot counts as settled once it has been up this long. The end of setup()
// proves nothing about a device that dies a second later.
constexpr uint32_t BOOT_SETTLED_MS = 10000;
// Three boots may fail; the fourth is the one that gives up and goes to safe
// mode. Named for what it counts rather than what it compares against, because
// "= 3" beside "> 3" reads like an off-by-one when it is not.
constexpr uint32_t BOOT_FAILS_ALLOWED = 3;
constexpr uint32_t BOOT_MARK_MAGIC = 0x53445002;

TFT_eSPI tft;
ESP8266WebServer server(80);

struct BootMark {
    uint32_t magic;
    uint32_t fails;
};

BootMark bootMark{};
bool safeMode = false;
bool markCleared = false;
bool fsMounted = false;
String lastStatus = "booting";
String bootPhase = "start";

void wdtYield() {
    ESP.wdtFeed();
    delay(0);
}

// ---------------------------------------------------------------- boot mark

void markWrite() {
    ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&bootMark), sizeof(bootMark));
}

// Counted and written before anything else is attempted. A count saved on the
// way out is never saved on the boot that actually needed it. RTC memory
// survives a reset and is cleared by pulling the power, so unplugging the
// device is itself a way back to a normal boot.
void markBegin() {
    ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&bootMark), sizeof(bootMark));
    if (bootMark.magic != BOOT_MARK_MAGIC) {
        bootMark.magic = BOOT_MARK_MAGIC;
        bootMark.fails = 0;
    }
    ++bootMark.fails;
    markWrite();
    safeMode = bootMark.fails > BOOT_FAILS_ALLOWED;
}

// ------------------------------------------------------------------- screen

void screenLine(int16_t y, const String& text, uint16_t colour) {
    tft.setTextColor(colour, TFT_BLACK);
    tft.drawString(text, 8, y, 2);
}

// Repainting the whole panel is 57,600 pixels over SPI, so it happens when
// something it shows has actually changed - not on a timer. A boot that just
// sits there should look still, not flicker.
String screenShown;

void screenPaint() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    screenLine(8, safeMode ? "SAFE MODE" : FW_NAME, safeMode ? TFT_YELLOW : TFT_GREEN);
    screenLine(30, FW_VERSION, TFT_DARKGREY);
    screenLine(60, String("AP  ") + AP_SSID, TFT_WHITE);
    screenLine(82, "    192.168.4.1", TFT_WHITE);
    const String sta = WiFi.isConnected() ? WiFi.localIP().toString() : String("not joined");
    screenLine(112, String("STA ") + sta, TFT_WHITE);
    screenLine(142, String("FS  ") + (fsMounted ? "mounted" : "not mounted"), TFT_WHITE);
    screenLine(172, String("boot fails ") + bootMark.fails, TFT_DARKGREY);
    screenLine(200, "POST /api/ota/fw", TFT_DARKGREY);
    screenShown = String(WiFi.isConnected() ? WiFi.localIP().toString() : String("-")) +
                  "|" + (fsMounted ? "1" : "0") + "|" + bootPhase;
}

// ------------------------------------------------------------------- routes

void sendText(int code, const String& body) {
    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.send(code, F("text/plain"), body);
}

String jsonEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<uint8_t>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// Written by hand rather than through a JSON library: this firmware carries no
// dependency it does not need, and every field here is a number or a string
// this file produced.
void handleStatus() {
    String b = "{";
    b += "\"name\":\"" + String(FW_NAME) + "\"";
    b += ",\"version\":\"" + String(FW_VERSION) + "\"";
    b += ",\"boot_phase\":\"" + bootPhase + "\"";
    b += ",\"safe_mode\":" + String(safeMode ? "true" : "false");
    b += ",\"boot_fails\":" + String(bootMark.fails);
    b += ",\"fs_mounted\":" + String(fsMounted ? "true" : "false");
    b += ",\"ip\":\"" + (WiFi.isConnected() ? WiFi.localIP().toString() : String("off")) + "\"";
    b += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
    b += ",\"sketch_size\":" + String(ESP.getSketchSize());
    b += ",\"free_sketch_space\":" + String(ESP.getFreeSketchSpace());
    b += ",\"flash_chip_real_size\":" + String(ESP.getFlashChipRealSize());
    b += ",\"free_heap\":" + String(ESP.getFreeHeap());
    if (fsMounted) {
        FSInfo info{};
        if (LittleFS.info(info)) {
            b += ",\"fs_total\":" + String(info.totalBytes);
            b += ",\"fs_used\":" + String(info.usedBytes);
        }
    }
    b += ",\"last\":\"" + jsonEscape(lastStatus) + "\"";
    b += "}";
    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.send(200, F("application/json"), b);
}

// Walks into the directories, because a flat listing that answers "ico 0" for a
// folder full of icons is no listing at all.
// Capped: this reads a filesystem written by unknown firmware, and a listing
// that grows without limit would be answered by running out of heap rather
// than by an error. Truncation is reported instead.
constexpr size_t FS_LIST_MAX = 6000;

void fsListInto(String& body, const String& prefix, uint8_t depth) {
    Dir dir = LittleFS.openDir(prefix.length() ? prefix : String("/"));
    while (dir.next()) {
        if (body.length() > FS_LIST_MAX) return;
        wdtYield();
        const String name = prefix + dir.fileName();
        if (dir.isDirectory()) {
            body += name + "/\t<dir>\n";
            if (depth > 0) fsListInto(body, name + "/", static_cast<uint8_t>(depth - 1));
        } else {
            body += name + "\t" + String(dir.fileSize()) + "\n";
        }
    }
}

void handleFsList() {
    if (!fsMounted) {
        sendText(503, F("filesystem not mounted\n"));
        return;
    }
    String body;
    body.reserve(1024);
    fsListInto(body, "/", 3);
    if (body.length() > FS_LIST_MAX) body += F("... truncated\n");
    sendText(200, body.length() ? body : String(F("(empty)\n")));
}

bool validFsPath(const String& p) {
    return p.startsWith("/") && p.indexOf("..") < 0 && p.indexOf('\\') < 0 &&
           p.length() >= 2 && p.length() <= 96;
}

// The reason this firmware can rescue the stock icons: the stock web UI has no
// download route at all, so the only way to read a file off that filesystem is
// to be the firmware mounting it.
void handleFileGet() {
    const String path = server.arg("path");
    if (!validFsPath(path)) {
        sendText(400, F("bad path\n"));
        return;
    }
    if (!fsMounted || !LittleFS.exists(path)) {
        sendText(404, F("not found\n"));
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendText(500, F("open failed\n"));
        return;
    }
    server.sendHeader(F("Access-Control-Allow-Origin"), F("*"));
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.streamFile(f, "application/octet-stream");
    f.close();
}

// ---------------------------------------------------------------------- OTA

uint32_t otaExpected = 0;
uint32_t otaWritten = 0;
bool otaSawFirst = false;
String otaRefusal;

constexpr uint32_t OTA_MIN_FLASH_BYTES = 100000;

void otaStart(int mode) {
    otaExpected = static_cast<uint32_t>(server.arg(F("size")).toInt());
    otaWritten = 0;
    otaSawFirst = false;
    otaRefusal = "";
    const String md5 = server.arg(F("md5"));

    const size_t capacity = (ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U;
    if (mode == U_FLASH && otaExpected > capacity) {
        // Said before a byte is written, which is the entire point of asking.
        otaRefusal = String("ota too big: ") + otaExpected + " bytes, " + capacity + " available";
        lastStatus = otaRefusal;
        return;
    }
    if (mode == U_FS) {
        LittleFS.end();
        fsMounted = false;
        delay(50);
    }
    const size_t want = otaExpected > 0 ? otaExpected : capacity;
    if (!Update.begin(mode == U_FS ? static_cast<size_t>(FS_PHYS_SIZE) : want, mode)) {
        otaRefusal = String("begin failed ") + Update.getErrorString();
        lastStatus = otaRefusal;
        return;
    }
    if (md5.length() == 32) Update.setMD5(md5.c_str());
    lastStatus = "ota running";
}

void otaWrite(HTTPUpload& upload, int mode) {
    if (Update.hasError() || !Update.isRunning()) return;
    if (!otaSawFirst && upload.currentSize > 0) {
        otaSawFirst = true;
        // Every ESP8266 sketch image opens with 0xE9.
        if (mode == U_FLASH && upload.buf[0] != 0xE9) {
            otaRefusal = F("ota rejected: not an esp8266 image");
            lastStatus = otaRefusal;
            Update.end();
            return;
        }
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        otaRefusal = String("write failed ") + Update.getErrorString();
        lastStatus = otaRefusal;
        return;
    }
    otaWritten += upload.currentSize;
    wdtYield();
}

void otaFail(const String& why) {
    Update.end();
    lastStatus = why;
    sendText(500, why + "\n");
    screenPaint();
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
        otaFail(String("ota truncated: got ") + otaWritten + " of " + otaExpected);
        return;
    }
    if (mode == U_FLASH && otaWritten < OTA_MIN_FLASH_BYTES) {
        otaFail(String("ota too small: ") + otaWritten + " bytes");
        return;
    }
    // Exact only when a length was given, because only then was Update started
    // with the real size and can have nothing left over.
    if (!Update.end(otaExpected == 0)) {
        otaFail(String("ota failed ") + Update.getErrorString());
        return;
    }
    if (otaExpected == 0) {
        // Without a promised length there is no way to know an image arrived
        // whole, so the reply says so rather than letting OK be read as proof.
        sendText(200, String(F("OK unverified: no size given, ")) + otaWritten + F(" bytes\n"));
    } else {
        sendText(200, F("OK\n"));
    }
    delay(800);
    ESP.restart();
}

void handleOta(int mode) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) otaStart(mode);
    else if (upload.status == UPLOAD_FILE_WRITE) otaWrite(upload, mode);
    else if (upload.status == UPLOAD_FILE_END) otaEnd(mode);
    else if (upload.status == UPLOAD_FILE_ABORTED) Update.end();
}

void handleRoot() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>SDP Bootsafe</title><h1>SDP Bootsafe</h1>"
                 "<p><a href='/status'>status</a> &middot; <a href='/fs/list'>files</a></p>"
                 "<form method=post action='/api/ota/fw' enctype='multipart/form-data'>"
                 "<input type=file name=update><button>Firmware</button></form>"
                 "<form method=post action='/api/ota/fs' enctype='multipart/form-data'>"
                 "<input type=file name=fs><button>Filesystem</button></form>");
    server.send(200, F("text/html"), h);
}

void setupRoutes() {
    server.on(F("/"), HTTP_GET, handleRoot);
    server.on(F("/status"), HTTP_GET, handleStatus);
    server.on(F("/fs/list"), HTTP_GET, handleFsList);
    server.on(F("/file"), HTTP_GET, handleFileGet);
    server.on(F("/update_ota"), HTTP_POST, []() {}, []() { handleOta(U_FLASH); });
    server.on(F("/api/ota/fw"), HTTP_POST, []() {}, []() { handleOta(U_FLASH); });
    server.on(F("/api/ota/fs"), HTTP_POST, []() {}, []() { handleOta(U_FS); });
    server.on(F("/restart"), HTTP_POST, []() {
        sendText(200, F("restarting\n"));
        delay(300);
        ESP.restart();
    });
    server.begin();
}

}  // namespace

void setup() {
    Serial.begin(115200);
    ESP.wdtEnable(WDTO_8S);

    tft.init();
    tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT);
    analogWriteRange(1023);
    // Active low: 0 is full brightness on this panel.
    analogWrite(TFT_BL, 200);
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    screenLine(8, "BOOTSAFE", TFT_GREEN);

    markBegin();
    bootPhase = "counted";

    // The access point and the routes come up on nothing but RAM, before the
    // filesystem is touched and before any station is attempted. This is the
    // whole design: whatever goes wrong below, there is still a way in.
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    setupRoutes();
    bootPhase = "routes_ready";
    screenPaint();

    if (safeMode) {
        lastStatus = "safe mode: boot failed 3x";
        bootPhase = "safe_mode";
        screenPaint();
        return;
    }

    bootPhase = "fs_mount";
    // Auto-format OFF, and this is the single most important line in the file.
    // LittleFS::begin() formats the volume when it cannot mount it, and that
    // default is on. This firmware exists to read a filesystem written by
    // somebody else's firmware - if their layout does not mount, the right
    // answer is to report that and leave it alone, not to erase the icons this
    // whole exercise is meant to rescue. A failed mount costs a diagnostic; a
    // format costs the data.
    LittleFS.setConfig(LittleFSConfig(false));
    wdtYield();
    fsMounted = LittleFS.begin();
    wdtYield();
    if (!fsMounted) lastStatus = "filesystem did not mount (not formatted)";

    // The credentials the SDK already holds - whatever firmware stored them.
    // Nothing is read from a config file here, and nothing is compiled in.
    bootPhase = "wifi";
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin();
    const uint32_t started = millis();
    while (!WiFi.isConnected() && millis() - started < STA_TIMEOUT_MS) {
        wdtYield();
        delay(100);
    }

    bootPhase = "ready";
    lastStatus = "ready";
    screenPaint();
}

void loop() {
    server.handleClient();

    if (!markCleared && millis() > BOOT_SETTLED_MS) {
        markCleared = true;
        if (bootMark.fails != 0) {
            bootMark.fails = 0;
            markWrite();
        }
    }

    // Safe mode serves requests and does nothing else at all.
    if (safeMode) return;

    // A slow repaint, only to keep the station address on screen once it
    // arrives. Nothing here touches the filesystem.
    static uint32_t lastLook = 0;
    if (millis() - lastLook > 2000) {
        lastLook = millis();
        const String now = String(WiFi.isConnected() ? WiFi.localIP().toString() : String("-")) +
                           "|" + (fsMounted ? "1" : "0") + "|" + bootPhase;
        if (now != screenShown) {
            screenShown = now;
            screenPaint();
        }
    }
}
