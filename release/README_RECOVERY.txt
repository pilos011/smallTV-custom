SDP Custom Recovery Firmware
Generated: 2026-08-24T20:00:07

File: C:\Users\kimsu\Documents\Codex\2026-08-15\geekmagic-smalltv-ultrageekmagic-smalltv-ultra\work\sdpro-custom-recovery\release\SDP_CustomRecovery_20260824_1.bin
Size: 359728 bytes
SHA256: BF7DDED97D28C93C0A600F0B861601F155E8FA57239B10AAF8FFFD6A4E9B1275

Purpose:
- Recovery-first custom base firmware for SD_PRO compatible ESP8266 device.
- Embedded web UI, no LittleFS dependency for recovery page.
- Tries stored WiFi first, then Home2G/123456789K, and always starts AP SDP-Recovery/123456789K.

Recovery paths provided after boot:
- GET  /status or /config
- POST /update_ota multipart field update: firmware OTA, also accepts official SD_PRO firmware for rollback.
- POST /api/ota/fw multipart field update: firmware OTA alias.
- POST /api/ota/fs multipart field fs: LittleFS image OTA.
- POST /rawfw raw body: firmware OTA on port 80.
- POST /rawfs raw body: LittleFS OTA on port 80.
- POST http://IP:8080/rawfw raw body: independent firmware raw server.
- POST http://IP:8080/rawfs raw body: independent LittleFS raw server.
- POST /file?path=/target/path multipart field file: write one file into LittleFS.
- POST /format: format LittleFS.
- /restart: reboot.

Known hardware assumptions:
- ESP8266 ESP-12F.
- ST7789 240x240 compatible LCD.
- Pins: MOSI GPIO13, SCLK GPIO14, DC GPIO0, RST GPIO2, BL GPIO5, CS disabled.

Rollback package:
- Official restore script remains at ..\sdpro-ota-safe\restore-sdpro-v106.ps1.
- Official firmware remains at ..\sdpro-ota-safe\SDPro_V1.0.6_20260525_174828.bin.
