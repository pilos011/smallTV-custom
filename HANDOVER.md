# SmallTV Custom Handover

## 목적

이 저장소는 GeekMagic SmallTV-Ultra 및 SD_PRO 형태의 ESP8266 ESP-12F 기반 240x240 LCD 장치에 커스텀 화면과 관리 UI를 올리기 위한 작업 기준입니다.

현재 안정 기준은 `v1.0.2`입니다. `main`은 안정 버전만 유지하고, SOS 화면, 사진액자, 추가 화면 전환 구조 같은 기능은 새 브랜치에서 진행합니다.

## 저장소 구조

- `firmware/sdpro-clock-weather/`: PlatformIO 펌웨어 프로젝트
- `firmware/sdpro-clock-weather/src/`: ESP8266 펌웨어 소스
- `firmware/sdpro-clock-weather/src/display/`: 원본 시계/날씨 화면 기반 렌더링 자산
- `firmware/sdpro-clock-weather/data/web/`: LittleFS에 올라가는 웹 관리 UI
- `firmware/sdpro-clock-weather/data/weather-icons/`: BMP 날씨 아이콘
- `release/`: 공개 안정 릴리스 바이너리만 추적
- `scripts/`: 빌드/릴리스 보조 스크립트

## 하드웨어 기준

PlatformIO 보드는 `esp12e`입니다. 현재 확인된 SD_PRO 호환 핀 설정은 다음과 같습니다.

| 기능 | GPIO |
| --- | --- |
| TFT MOSI | GPIO13 |
| TFT SCLK | GPIO14 |
| TFT DC | GPIO0 |
| TFT RST | GPIO2 |
| TFT BL | GPIO5 |
| TFT CS | 사용 안 함 |

백라이트는 active-low로 취급합니다. UI에서 밝기 `100`은 최대 밝기, `0`은 최소 밝기입니다.

## 빌드

Windows PowerShell에서:

```powershell
cd D:\Personal\SmallTV-Custom\firmware\sdpro-clock-weather
py -m platformio run
py -m platformio run --target buildfs
```

릴리스 산출물 생성:

```powershell
cd D:\Personal\SmallTV-Custom
.\scripts\build_release.ps1 -Version v1.0.2
```

이 명령은 펌웨어와 LittleFS를 빌드하고, `release/`에 바이너리 두 개를, `dist/`에 전체 소스 zip을 만든 뒤 `CHANGELOG.md`에 넣을 SHA256을 출력합니다. GitHub에는 아무것도 올리지 않으므로 반복 실행해도 안전합니다.

태그와 GitHub Release까지 게시하려면 `-Publish`를 붙입니다.

```powershell
.\scripts\build_release.ps1 -Version v1.0.2 -Publish
```

`-Publish`는 게시 전에 다음을 확인하고, 하나라도 어긋나면 중단합니다.

- `dist/release-notes-<버전>.md`가 존재할 것
- `gh` CLI가 설치되어 있고 인증되어 있을 것
- 작업 트리가 clean할 것
- 현재 브랜치가 `main`이고 `origin/main`과 같을 것

보조 옵션:

| 옵션 | 용도 |
| --- | --- |
| `-SkipBuild` | 기존 `.pio` 산출물을 재사용합니다 |
| `-NotesFile` | 릴리스 노트 경로를 직접 지정합니다 |
| `-Repo` | 대상 저장소를 `owner/name`으로 지정합니다 |

## 업로드와 복구 경로

정상 동작 중 웹 UI:

- `http://<device-ip>/`
- `http://<device-ip>/status`
- `http://<device-ip>/api/config`
- `http://<device-ip>/weather/status`

OTA:

- 펌웨어: `POST /update_ota`, multipart field `update`
- 펌웨어 대체 API: `POST /api/ota/fw`, multipart field `update`
- LittleFS: `POST /api/ota/fs`, multipart field `fs`

raw fallback:

- `http://<device-ip>:8080/status`
- `POST http://<device-ip>:8080/rawfw`
- `POST http://<device-ip>:8080/rawfs`

파일 단위 LittleFS 쓰기:

- `POST /file?path=/web/index.html`
- `POST /file?path=/web/app.js`
- `POST /file?path=/web/style.css`
- `POST /file?path=/weather-icons/<name>.bmp`

전체 LittleFS 업로드는 설정 파일을 덮을 수 있습니다. 설정을 보존해야 할 때는 `/file` API로 필요한 파일만 갱신합니다.

## 설정

웹 UI의 System 메뉴에서 WiFi SSID/password와 밝기를 설정합니다. Weather 메뉴에서는 기상청 APIHub key, 위치명, Grid X/Y, 시간대, 24시간 표시 여부를 설정합니다.

System 메뉴의 Auto Night Mode는 지정한 시간대에 백라이트를 야간 밝기로 낮춥니다. 시작 시각이 종료 시각보다 늦으면 자정을 넘어가는 구간으로 처리합니다. 시각 판정은 장치 로컬 시간 기준이므로 NTP 동기화 전에는 야간 모드가 적용되지 않습니다.

| 설정 키 | 의미 | 기본값 |
| --- | --- | --- |
| `night_mode_enabled` | 자동 야간 모드 사용 여부 | `false` |
| `night_brightness` | 야간 구간 밝기 `0`~`100` | `20` |
| `night_start_minutes` | 야간 시작, 자정 기준 분 | `1380` (23:00) |
| `night_stop_minutes` | 야간 종료, 자정 기준 분 | `420` (07:00) |

### 화면 선택과 전환

Display Settings 카드에서 쓸 화면을 고르고 전환 간격을 정합니다. 두 개 이상 고르면 간격마다 번갈아 표시하고, 하나만 고르면 그 화면만 유지합니다.

| 설정 키 | 의미 | 기본값 |
| --- | --- | --- |
| `screens` | 화면 비트마스크. bit0 = 시계/날씨, bit1 = 아날로그 | `1` |
| `theme_interval_seconds` | 전환 간격 `3`~`3600` | `10` |

### 아날로그 시계 색

같은 카드에서 색 네 가지를 지정합니다. 값은 24비트 RGB 정수로 저장하고 그릴 때 RGB565로 낮춥니다. 바늘 윤곽선은 바늘 색에서 자동으로 파생되므로 따로 설정하지 않습니다.

| 설정 키 | 의미 | 기본값 |
| --- | --- | --- |
| `analog_dial_rgb` | 문자판 | `0` (`#000000`) |
| `analog_case_rgb` | 원 바깥 테두리 | `8` (`#000008`) |
| `analog_lume_rgb` | 숫자와 시간바 | `61695` (`#00F0FF`) |
| `analog_hand_rgb` | 시침·분침·초침, 중심축 | `16711680` (`#FF0000`) |

패널이 16비트라 어두운 쪽에서는 표현 단계가 매우 적습니다. 검정 바로 위 값은 `0x0001`, `0x0002`, `0x0020`, `0x0021` 넷뿐이고, 휘도의 대부분을 녹색이 차지하므로 파랑만 남긴 값이 가장 어두운 테두리가 됩니다. 서로 다른 색을 골라도 같은 화면 색으로 합쳐질 수 있어서, 웹 UI가 실제 표시될 RGB565 값을 함께 보여줍니다.

예시 API:

```powershell
$body = @{
  ssid = "YOUR_2G_WIFI_SSID"
  pass = "YOUR_2G_WIFI_PASSWORD"
  location = "Seoul"
  kma_key = "YOUR_KMA_APIHUB_KEY"
  nx = 61
  ny = 125
  timezone_offset_minutes = 540
  weather_enabled = $true
  clock_24h = $true
  brightness = 88
  night_mode_enabled = $true
  night_brightness = 20
  night_start_minutes = 1380
  night_stop_minutes = 420
} | ConvertTo-Json

Invoke-RestMethod -Method Post -Uri "http://192.168.4.1/api/config" -ContentType "application/json" -Body $body
```

## 웹 UI 비밀번호

`/login`에서 비밀번호를 받고, 부팅할 때마다 새로 만드는 세션 토큰을 쿠키로 내려줍니다. 재부팅하면 모두 로그아웃됩니다. 비밀번호는 `main.cpp`의 `AUTH_PASSWORD`에 있습니다.

`/login`과 `/logout`을 뺀 모든 경로에 인증이 걸립니다. OTA와 `/file`은 업로드가 시작되는 시점에 세션을 확인해 거부하므로, 인증 없는 요청은 한 바이트도 쓰지 못합니다.

**raw 8080 서버는 의도적으로 인증 없이 열어 둡니다.** 웹 UI가 망가졌을 때 돌아올 유일한 길이기 때문입니다.

배포 스크립트를 쓸 때는 펌웨어 OTA **전에** 로그인해야 합니다. 이 순서를 놓치면 업로드가 401로 거부됩니다.

## 보안 원칙

- `INFO.md`는 로컬 전용이며 커밋하지 않습니다.
- WiFi 비밀번호, API key, GitHub token, 실제 운영 IP가 들어간 설정 백업은 커밋하지 않습니다.
- 공개 예시는 placeholder만 사용합니다.
- 한국어 문서는 UTF-8로 저장합니다.

## 릴리스 정책

1. 기능 작업은 브랜치에서 진행합니다.
2. 소스의 `FW_VERSION`과 문서의 버전 표기를 새 버전으로 맞춥니다.
3. `.\scripts\build_release.ps1 -Version <버전>`으로 빌드와 패키징을 통과시킵니다.
4. 하드코딩 secret이 없는지 검색합니다.
5. `CHANGELOG.md`에 버전 항목을 추가하고, 스크립트가 출력한 SHA256을 채웁니다.
6. `dist/release-notes-<버전>.md`에 릴리스 노트를 작성합니다.
7. 안정 산출물만 `.gitignore` 화이트리스트에 추가해 `release/` 예외로 추적합니다.
8. 변경을 커밋하고 `main`에 병합한 뒤 push합니다.
9. `.\scripts\build_release.ps1 -Version <버전> -Publish`로 태그와 GitHub Release를 생성합니다.

GitHub Release에는 항상 에셋 세 개를 올립니다. 펌웨어 bin, LittleFS bin, 그리고 전체 소스 zip입니다.

## 현재 한계와 주의

- `v1.0.2`는 원본 시계/날씨 화면 자산을 포함하지만 LCD 드라이버 glue는 `TFT_eSPI` 기반입니다.
- OTA가 불안정한 장치에서는 업로드 중 재부팅될 수 있으므로 UART/TTL 복구 수단을 확보한 뒤 큰 변경을 적용합니다.
- 새 기능을 넣을 때도 `/status`, `/api/config`, `/file`, raw 8080 복구 경로는 유지합니다.
