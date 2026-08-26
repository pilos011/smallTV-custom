SDP Clock Weather build
=======================

Target device:
  SD_PRO / ESP8266 ESP-12F / ST7789 240x240

Files:
  SDP_ClockWeather_20260825_1.bin    Firmware OTA image
  littlefs-clock-weather-20260825-1.bin
                                      Web UI and weather icon LittleFS image

Default network:
  STA: Home2G / 123456789K
  AP fallback: SDP-Recovery / 123456789K

Default weather:
  Location: Baekseok
  KMA grid: nx=57, ny=128
  Timezone: UTC+9

Upload order from current SDP Custom Recovery:
  1. upload-clock-weather-fw.ps1
  2. Wait about 15 seconds.
  3. Open http://192.168.10.72/status and confirm name is "SDP Clock Weather".
  4. upload-clock-weather-fs.ps1
  5. Wait about 15 seconds.
  6. Open http://192.168.10.72/
  7. configure-baekseok-weather.ps1

Fallback FS upload:
  If /api/ota/fs fails after the new firmware is running, use:
    raw-upload-clock-weather-fs-8080.ps1

Important endpoints:
  http://192.168.10.72/              Web UI
  http://192.168.10.72/status        System status
  http://192.168.10.72/api/config    GET/POST config JSON
  http://192.168.10.72/weather/status
  http://192.168.10.72/update_ota    Firmware multipart field "update"
  http://192.168.10.72/api/ota/fs    LittleFS multipart field "fs"
  http://192.168.10.72:8080/rawfw    Raw firmware upload
  http://192.168.10.72:8080/rawfs    Raw LittleFS upload

Design note:
  This build preserves recovery OTA paths and adds a modular web UI shell for
  Status, Weather, System, and Recovery. The clock/weather screen follows the
  original wonjj6768 KMA clock-weather behavior and endpoints, adapted to the
  SD_PRO TFT_eSPI hardware/recovery base.

2026.08.24.2 display note:
  The LCD no longer rotates to the system screen automatically. Clock/weather is
  fixed on the panel, system information is available from the web UI/API, and
  the dashboard redraws only changed regions. Digits use the original
  ClockDigitFont bitmap glyphs and weather icons are loaded from LittleFS BMPs.

2026.08.24.3 display note:
  The clock/weather screen now uses the original wonjj6768 ClockDashboardScene,
  ClockDigitFont, UiTextFont, and weather icon assets. The SD_PRO-specific code
  is limited to TFT_eSPI drawing, KMA data adaptation, WiFi, config, and OTA.

2026.08.25.1 system note:
  SDP-Recovery AP is open with no password. WiFi SSID/password controls are in
  the System tab. Brightness is also in the System tab and is inverted for the
  active-low backlight PWM, so UI 100 means maximum brightness.
