# Changelog

이 프로젝트는 안정 릴리스를 `main` 브랜치에 유지하고, 기능 추가는 별도 브랜치에서 검증한 뒤 병합합니다.

## [v1.0.1] - 2026-08-25

### Added

- 자동 야간 모드를 추가했습니다. 지정한 시간대에 백라이트를 야간 밝기로 낮추고, 시작 시각이 종료 시각보다 늦으면 자정을 넘어가는 구간으로 처리합니다.
- 웹 UI System 메뉴에 Auto Night Mode 사용 여부, 야간 밝기, 시작/종료 시각 입력을 배치했습니다.
- `/api/config`에 `night_mode_enabled`, `night_brightness`, `night_start_minutes`, `night_stop_minutes` 항목을 추가했습니다.

### Changed

- 시계 화면의 날짜 타이포그래피 정렬을 조정했습니다.
- 시계/날씨 화면의 세로 간격을 조정했습니다.
- 시계/날씨 화면 텍스트 대비를 높여 가독성을 개선했습니다.

### Build

- 펌웨어 바이너리: `release/SDP_ClockWeather_v1.0.1.bin`
  - SHA256: `DF3713B2DE7C3A7A580E1A852003F1C89E3858192F9CF8F1E53BF19B895FB8A2`
- LittleFS 이미지: `release/littlefs-clock-weather-v1.0.1.bin`
  - SHA256: `7DD727A15DE90D00E0953A44457319699A54878B30F9B4087C9998F7515401A8`

### Known Limitations

- 야간 모드는 장치 로컬 시간 기준이므로 NTP 동기화 전에는 적용되지 않습니다.
- LCD 드라이버 연결부는 계속 `TFT_eSPI` 기반 어댑터입니다.

## [v1.0.0] - 2026-08-25

### Added

- SD_PRO / SmallTV 호환 ESP8266용 `SDP Clock Weather` 안정 펌웨어를 추가했습니다.
- 원본 `wonjj6768/smalltv-ultra-korean-custom-firmware`의 시계/날씨 화면 구성 요소를 프로젝트에 포함했습니다.
  - `ClockDashboardScene`
  - `ClockDigitFont`
  - `UiTextFont`
  - LittleFS BMP 날씨 아이콘
- 기상청 APIHub 기반 날씨 조회와 위치 격자 설정을 추가했습니다.
- 웹 UI를 Status, Weather, System, Recovery 탭으로 구성했습니다.
- System 메뉴에 WiFi SSID/password, 밝기, 재시작, LittleFS 목록 기능을 배치했습니다.
- Recovery 메뉴에 펌웨어 OTA, LittleFS OTA, raw 8080 복구 경로 안내를 배치했습니다.
- `/file?path=/...` 파일 단위 업로드 API를 추가해 전체 LittleFS를 덮지 않고 웹 파일/아이콘을 교체할 수 있게 했습니다.

### Fixed

- 복구 AP는 비밀번호 없이 열리도록 정리했습니다.
- ST7789 백라이트가 active-low인 보드에서 UI 밝기 값과 실제 밝기가 반대로 동작하던 문제를 보정했습니다.
- 공개 배포 빌드에서 하드코딩 WiFi SSID/password를 제거했습니다.

### Build

- 펌웨어 바이너리: `release/SDP_ClockWeather_v1.0.0.bin`
  - SHA256: `E95E72E217064864D487DE62818A69CA248B8DEAFA3CB3409F890F2210DCF111`
- LittleFS 이미지: `release/littlefs-clock-weather-v1.0.0.bin`
  - SHA256: `48812BA15D8E62001853040D1608C5C1BF4C1A4CE36139778721C1F8B627ECFF`

### Known Limitations

- 화면 구성 요소와 폰트/아이콘은 원본 자산을 사용하지만, LCD 드라이버 연결부는 현재 `TFT_eSPI` 기반 어댑터입니다.
- OTA가 불안정한 장치는 UART/TTL 복구 경로를 확보한 뒤 펌웨어와 LittleFS를 갱신해야 합니다.
