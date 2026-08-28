#if defined (ESP32) || defined (ARDUINO_ARCH_ESP8266) || defined (ARDUINO_ARCH_RP2040)
  #define TJPGD_LOAD_FFS
#endif

// Vendored copy, one change: the SD library stays out. Its global object
// has a constructor, which defeats --gc-sections, and it was dragging 57 KB
// of flash and 4.5 KB of RAM into a build that reads JPEGs from LittleFS.
// #define TJPGD_LOAD_SD_LIBRARY
