# Changelog

이 프로젝트는 안정 릴리스를 `main` 브랜치에 유지하고, 기능 추가는 별도 브랜치에서 검증한 뒤 병합합니다.

## [v1.0.2] - 2026-08-25

### Added

- 아날로그 시계 화면을 추가했습니다. 검은 문자판에 시간바 8개와 12·3·6·9 숫자, 끝으로 갈수록 좁아지는 바늘 세 개로 구성했습니다.
- 숫자는 시계/날씨 화면의 `ClockDigitFont` Main 글리프를 그대로 씁니다. 원본이 55픽셀 고정이고 확대·축소 기능이 없어 28픽셀로 박스 필터 축소해 넣었습니다.
- 화면 선택과 전환을 추가했습니다. 웹 UI에서 쓸 화면을 고르고 전환 간격을 지정합니다.
- 아날로그 시계의 문자판, 테두리, 숫자·시간바, 바늘 색을 웹 UI에서 지정할 수 있게 했습니다. 실제 표시될 RGB565 값을 함께 보여줍니다.
- 웹 UI에 비밀번호 로그인을 추가했습니다. 세션 토큰은 부팅할 때마다 새로 만듭니다.
- `/status`에 힙과 아날로그 프레임 시간을 노출해 성능을 장치에서 직접 확인할 수 있게 했습니다.

### Changed

- 웹 UI 제목을 `선일의 다목적 모니터링 디스플레이`로 바꿨습니다.
- CPU를 160MHz로 올렸습니다. 아날로그 화면 합성이 CPU에 묶여 있었습니다.
- `scripts/build_release.ps1`이 전체 소스 zip 생성과 GitHub Release 게시까지 처리합니다. 게시는 `-Publish`를 붙일 때만 합니다.

### Fixed

- 아날로그 화면을 오프스크린 스프라이트에 띠 단위로 합성한 뒤 밀어 넣도록 바꿔 깜빡임을 없앴습니다. 화면에는 완성된 픽셀만 나타납니다.
- 매초 바뀌는 띠만 다시 그립니다. 한 프레임이 250ms에서 60~130ms로 줄었습니다.
- `drawWedgeLine`이 선의 바운딩 박스 전체를 스캔하던 낭비를 없앴습니다. 획을 띠 안쪽 구간으로 먼저 자른 뒤 그립니다.
- 부분 갱신에서 분침이 두 위치에 동시에 그려지던 문제를 고쳤습니다. 느린 바늘은 화면이 확정한 각도로만 그립니다.
- 부팅 시 활성 화면을 설정에 맞춰 정하도록 고쳤습니다. 시계/날씨를 꺼둔 채 재부팅해도 그 화면이 나오던 문제입니다.
- OTA와 `/file`이 업로드 시작 시점에 인증을 확인하도록 했습니다. 완료 시점 확인은 이미 기록이 끝난 뒤라 의미가 없었습니다.
- 전체 화면을 덮는 시스템 화면을 그린 뒤 chrome 플래그를 지우도록 했습니다. OTA 실패 후 화면이 깨진 채 남던 문제입니다.

### Build

- 펌웨어 바이너리: `release/SDP_ClockWeather_v1.0.2.bin`
  - SHA256: `90C0A65E6E4B199D7F0E3A06005C9966C8810C7ADB5D3BF6E2DAD443A9E229DF`
- LittleFS 이미지: `release/littlefs-clock-weather-v1.0.2.bin`
  - SHA256: `76AD052D63659578BAD59BCD0BB21B7438B0C8519192C1D847E2DD108C9AD093`

### Notes

- LittleFS 이미지는 빌드마다 바이트가 달라집니다. 펌웨어 bin은 재현되지만 `mklittlefs` 산출물은 그렇지 않으므로, 위 SHA256은 실제로 릴리스에 올라간 빌드의 값이어야 합니다. 릴리스에 쓸 빌드에서 뽑은 해시를 그대로 넣으십시오.

### Known Limitations

- 패널이 16비트라 어두운 영역의 표현 단계가 매우 적습니다. 검정 바로 위 값은 네 개뿐이라 문자판과 테두리를 모두 어둡게 두면 서로 구분되지 않을 수 있습니다.
- 아날로그 화면은 초 단위로 갱신합니다. 부드러운 초침은 지원하지 않습니다.

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
