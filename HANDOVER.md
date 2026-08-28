# SmallTV Custom Handover

## 목적

이 저장소는 GeekMagic SmallTV-Ultra 및 SD_PRO 형태의 ESP8266 ESP-12F 기반 240x240 LCD 장치에 커스텀 화면과 관리 UI를 올리기 위한 작업 기준입니다.

현재 안정 기준은 `v1.0.18`입니다. `main`은 안정 버전만 유지하고, SOS 화면 같은 신규 기능은 새 브랜치에서 진행합니다.

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
.\scripts\build_release.ps1 -Version v1.0.4
```

이 명령은 펌웨어와 LittleFS를 빌드하고, `release/`에 바이너리 두 개를, `dist/`에 전체 소스 zip을 만든 뒤 `CHANGELOG.md`에 넣을 SHA256을 출력합니다. GitHub에는 아무것도 올리지 않으므로 반복 실행해도 안전합니다.

태그와 GitHub Release까지 게시하려면 `-Publish`를 붙입니다.

```powershell
.\scripts\build_release.ps1 -Version v1.0.4 -Publish
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

System 메뉴의 Display 항목에서 쓸 화면을 고르고 전환 간격을 정합니다. 두 개 이상 고르면 간격마다 번갈아 표시하고, 하나만 고르면 그 화면만 유지합니다.

| 설정 키 | 의미 | 기본값 |
| --- | --- | --- |
| `screens` | 화면 비트마스크. 아래 표 참조 | `1` |
| `theme_interval_seconds` | 전환 간격 `3`~`3600` | `10` |

비트 순서는 `main.cpp`의 `SCREEN_*` 상수 순서와 같고, `/api/config`가 `screen_count`로
펌웨어가 실제로 가진 화면 수를 알려줍니다.

| 비트 | 화면 | 내용 |
| --- | --- | --- |
| bit0 | 시계/날씨 | 원본 대시보드 |
| bit1 | Analog | 야광 눈금과 숫자를 쓰는 아날로그 시계 |
| bit2 | Mondaine | 스위스 철도 시계, 검은 문자판 |
| bit3 | Mondaine White | 같은 시계의 흰 문자판 |
| bit4 | Digital | 시를 좌상단, 분을 우하단에 크게 |
| bit5 | Weather Digital | 날씨 아이콘, 기온, 한글 날씨 이름과 시각 |
| bit6 | Date Digital | 시각과 한글 요일, 년월일 |
| bit7 | Photo Album | 올린 사진을 순서대로 넘김 |
| bit8 | Plane Radar | 반경 안의 항공기를 PPI 레이더로 |

비트마스크는 **어떤** 화면을 쓸지만 말할 수 있고 순서는 담지 못하므로,
돌아가는 순서는 `screen_order` 배열이 따로 가집니다. 웹 UI에서 행을 끌어
옮기면 이 배열이 바뀝니다.

```json
"screens": 225, "screen_order": [5, 6, 7, 0, 1, 2, 3, 4]
```

받은 순서는 항상 **온전한 순열로 정리**됩니다. 범위를 벗어나거나 중복된
값은 버리고 빠진 화면은 뒤에 붙입니다. 순서가 잘못 와도 접근하지 못하는
화면이 생기지 않게 하려는 것입니다.

최소 한 화면은 켜져 있어야 합니다. 웹 UI가 마지막 하나의 체크 해제를 거부하고,
펌웨어도 빈 마스크를 받으면 시계/날씨로 되돌립니다.

### 사진 액자

상단 Album 탭입니다. 끌어 놓거나 붙여넣기로 여러 장을 한번에 올리고,
썸네일 그리드에서 순서를 바꾸거나 개별로 끄고 지울 수 있습니다.

**디코딩은 브라우저가 합니다.** 장치에는 이미지 디코더를 넣을 플래시도,
디코딩된 프레임을 들 힙도 없습니다 — 240×240×2가 115KB인데 전체 RAM이 80KB입니다.
그래서 웹 UI가 canvas로 자르고 줄여 RGB565로 포장해 올리고, 장치는 파일을
그대로 SPI로 밀기만 합니다(측정 62ms).

**바이트 순서는 파일의 것이지 CPU의 것이 아닙니다.** ESP8266의 `pushPixels`가
버퍼를 SPI FIFO로 `memcpy`하므로 파일 바이트가 그대로 패널로 갑니다. 파일은
빅엔디안으로 쓰며, 어느 쪽에서도 스왝이 일어나지 않습니다.

| 항목 | 값 |
| --- | --- |
| 사진 파일 | `/album/<id>.rgb` — 115,200 B (240×240 RGB565) |
| 썸네일 | `/album/<id>.thm` — 3,200 B (40×40) |
| 목록 | `/album/index.json` — 순서와 on/off |
| 최대 | 16장 (`ALBUM_MAX`). 실제는 파일시스템이 먼저 찹니다 |
| 주기 | `album_interval_seconds`, 기본 10초 |

순서와 on/off를 파일명이 아니라 목록 파일에 둔 이유는 간단합니다 — 순서를
바꿀 때 115KB 파일을 옮기는 대신 200바이트만 다시 씁니다. 목록보다
파일이 우선입니다: 픽셀이 없는 항목은 불러올 때 버려집니다.

API:

| 경로 | 동작 |
| --- | --- |
| `GET /api/album` | 목록, 주기, 저장공간, 마지막 프레임 시간 |
| `POST /api/album` | 목록 전체를 한번에 교체(배열 순서 = 표시 순서) |
| `POST /api/album/delete?id=` | 사진 파일 둘과 목록 항목 삭제 |
| `GET /api/album/thumb?id=` | 썸네일 원시 RGB565 |

사진 픽셀은 기존 `/file?path=/album/<id>.rgb`로 올립니다. id는 파일명이
되므로 영숫자·`-`·`_`만 받아 `/album` 밖으로 빠져나가지 못하게 합니다.

### 항공기 레이더

상단 Radar 탭입니다. 기준 좌표를 중심으로 반경 안의 항공기를 PPI 레이더처럼
그립니다. **중계할 PC 에이전트가 없습니다** — 장치가 직접 조회합니다.

| 설정 키 | 의미 | 기본값 |
| --- | --- | --- |
| `radar_lat`, `radar_lon` | 화면 중심이 되는 내 좌표 | `0` |
| `radar_range_km` | 바깥 원의 반경 | `10` |
| `radar_poll_sec` | 조회 주기이자 안테나 한 바퀴 | `10` |
| `radar_min_alt_ft` | 이 고도 아래는 버림 | `0` |
| `radar_up_deg` | 화면 위쪽이 가리킬 방위 | `0` (정북) |
| `radar_routes` | 경로 조회 사용 여부 | `true` |
| `radar_presets` | 이름 붙인 위치 목록, 최대 6개 | `[]` |

**저장된 위치.** 장치를 집과 회사로 들고 다니면 좌표를 매번 다시 넣어야 해서,
장소에 따라 달라지는 것 전부 — 좌표, 반경, 화면 방위, 최저 고도 — 를 이름 하나에
묶어 둡니다. 폴링 주기만 빠지는데, 그건 장소가 아니라 피드의 속성입니다.
목록은 `screen_order`처럼 **통째로 교체**합니다. 추가/삭제 동사 둘을 두면 경합을
심판할 코드가 필요해집니다. `0,0` 좌표는 레이더 끄기 스위치라 저장을 거부합니다.

```json
"radar_presets": [ { "name": "회사", "lat": 37.49, "lon": 127.01, "km": 15, "up": 340, "min_alt": 0 } ]
```

`radar_up_deg`는 장치를 책상 위 실제 방향대로 놓기 위한 것입니다. 340이면
화면 위가 북북서를 가리키고, 화면의 `N` 글자도 같이 돌아갑니다. 방위에서
화면 각도로 가는 변환은 `radarScreenDeg()` 한 곳에만 있습니다.

**갱신은 스윕이 합니다.** 화면 전체를 다시 칠하면 한 바퀴마다 깜빡이므로,
안테나가 지나간 반경만 지우고 그 자리의 내용을 다시 그립니다(`radarRepaint()`).
조회는 스윕이 12시를 지날 때 걸어, 조회에 드는 0.4~0.9초가 늘 같은 각도에서
멈추는 것처럼 보이지 않게 합니다. 스윕은 부동소수 누산이 아니라 정수 스텝으로
돌립니다 — 360도에서 되감을 때 지우는 선이 1픽셀씩 어긋났습니다.

| 출처 | 쓰는 것 |
| --- | --- |
| `opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}` | 위치, 편명, 고도, 기종, 카테고리 |
| `api.adsbdb.com/v0/callsign/{cs}` | 목적지 IATA 세 글자 |

둘 다 응답이 커서 통째로 파싱하지 않고 ArduinoJson 스트리밍에
`DeserializationOption::Filter`를 씌워 필요한 필드만 꺼냅니다. 가까운 순으로
최대 `RADAR_MAX_AIRCRAFT`(24)대만 남깁니다.

**목적지는 배급제입니다.** ADS-B에는 항로가 없어 편명으로 따로 물어야 하는데,
그것이 TLS 핸드셰이크 한 번입니다. 그래서 한 바퀴에 한 대만, 아직 모르는 것 중
가장 가까운 것만 조회하고 답을 기억합니다(`ROUTE_CACHE` 24칸, LRU).
답이 "항로 없음"이어도 기억합니다 — 그래야 매 바퀴 다시 묻지 않습니다.
실패해도 기록합니다: 기록하지 않으면 같은 편명이 큐 맨 앞에 영원히 남아
뒤의 항공기가 전부 굶습니다.

**항공사·기종·목적지 이름.** 생성 테이블 셋이 코드를 한글로 옮깁니다.

| 파일 | 옮기는 것 | 개수 |
| --- | --- | --- |
| `display/Airlines.h` | 호출부호 세 글자 → 항공사명 | 125 |
| `display/Rotorcraft.h` | ICAO 기종 코드 → 기체 이름 | 44 |
| `display/Airports.h` | 공항 IATA → 한글 공항·도시명 | 210 |

셋 다 생성물이고, 표에 없으면 원래 문자열을 그대로 씁니다 — 틀린 이름보다
코드가 낫습니다. 생성기와 **원본 목록은 `scripts/gen/`에 있습니다**
(`data/airports.py` 등이 진짜 원본이고 헤더는 산출물입니다). 표를 고치면
`py scripts/gen/gen_extra_glyphs.py`로 한글을 먼저 굽고 해당 생성기를 돌립니다. 공항·항공사 표의 뼈대는 소유자가 정리한 한국 직항 공항표와
취항 항공사표(인천공항 공식 취항정보 기준)이고, 그 위에 실제 상공 표본 감사
(`audit_airports.py`)로 구멍을 메웠습니다. 생성기는 256개째에서 거부합니다 —
`COUNT`가 `uint8_t`라 넘치면 0으로 감겨 모든 라벨이 조용히 코드로 돌아갑니다.

회전익은 ADS-B 카테고리 `A7` **또는 기종 표가 아는 코드**로 가려냅니다.
카테고리는 트랜스폰더 설정값이라 Bell 206이 `A1`로 날아오기도 합니다.
표식을 달리 그리고 항공사 자리에 기종 이름을 넣습니다.

**편명은 영숫자가 하나는 있어야 편명입니다.** 미설정 트랜스폰더는 편명 칸을
공백 여덟 개나 `@` 여덟 개(빈 Mode S 레지스터 판독값)로 방송하는데, ArduinoJson의
`|` 폴백은 키가 없을 때만 작동해 둘 다 통과했습니다. 쓸 만한 편명이 없으면
hex를 이름으로 씁니다.

**목적지 캐시는 이름이 아니라 IATA 코드를 담습니다.** 4바이트 대 22바이트이기도
하고, 표를 다시 뽑아도 캐시가 낡지 않기 때문입니다. 한글로 옮기는 것은 그릴 때입니다.

공항 이름은 **가장 짧은 자연스러운 형태**로 둡니다. 목적지는 고도와 한 줄을
나눠 쓰는데 문자판이 240픽셀이라, 길면 라벨이 화면 밖으로 밀려납니다.

세 표 모두 이름을 **PROGMEM 문자열로 두고 `strncpy_P`로 꺼냅니다.** PROGMEM
배열에 평범한 리터럴의 포인터를 담으면 리터럴 자체는 DRAM에 남습니다 —
이 표를 그렇게 썼다가 정적 RAM 784바이트를 그냥 버리고 있었습니다.

번들 UI 폰트에 없는 한글은 `display/ExtraGlyphs.h`에 같은 4비트 포장으로
구워 넣고, 조회가 빗나갈 때만 참조합니다. 메트릭은 짐작하지 않고 폰트가 이미
가진 글자들에 맞춰 포인트 크기와 베이스라인을 훑어 맞춥니다.

**구울 목록은 손으로 적지 않습니다.** 생성기가 세 표가 쓰는 음절 전체에서 번들
폰트가 가진 것을 빼서 구합니다(현재 Small 91자). 표는 자주 늘어나는데 옆에 둔
목록은 조용히 낡고, 그러면 이름 한 글자가 화면에서 그냥 사라집니다.

라벨은 텍스트 크기 1로 그리므로 `uiKind()`가 **Small** 세트를 고릅니다. Large를
검사하면 아무것도 보증하지 못합니다.

**두 피드 모두 HTTP/1.0으로 요청합니다.** 본문을 JSON 파서에 그대로 흘려 넣는데
`http.getStream()`은 청크를 풀지 않습니다 — 디청킹은 `writeToStream()` 안에만
있습니다. 그래서 chunked 응답은 파서에 `60d\r\n{"ac":[...` 로 도착하고, 파서가
청크 길이를 문서로 읽고 멈춥니다. adsb.fi가 2026-08-27에 chunked로 바꾸면서
하늘이 비었든 가득 찼든 모든 조회가 한꺼번에 죽었습니다.

HTTP/1.0을 켜면 HTTPClient가 자기 `Accept-Encoding` 헤더를 **같이 뺍니다**
(`!_useHTTP10` 안에서만 붙습니다). Accept-Encoding이 없는 요청은 서버가 아무
인코딩이나 골라도 되는 요청이므로, `identity`를 직접 붙여 줍니다. `httpGetBody()`가
기상청에 이미 쓰던 짝입니다.

**힙 가드는 물러설 줄 알아야 합니다.** `RADAR_MIN_BLOCK`은 끝낼 수 없는 구멍에서
핸드셰이크를 시작하지 않게 막습니다. 그런데 그것만으로는 일방통행 문입니다 —
웹 파일을 올리면 힙이 갈라져 최대 블록이 17KB 남짓, 가드 바로 아래로 떨어지는
일이 있고, 그때부터 모든 조회가 거부되는데 **거부된 조회는 아무것도 할당하지
않으므로 기다리는 그 단편화가 영원히 바뀌지 않습니다.** 재부팅 전까지 레이더가
죽어 있었습니다. 그래서 연속 `RADAR_REFUSALS_MAX`(6)번 거부하면 한 번은 비켜
줍니다. 정말 부족하면 아래 할당이 실패하고 널 검사가 돌려보내므로, 잃는 것은
기능 전체가 아니라 폴링 한 번입니다.

진단은 `GET /api/radar`입니다. 조회·리페인트 시간, 조회 중 힙, 목적지 캐시
상태, 그리고 현재 목록을 그대로 돌려줍니다.

### 시계 화면 색

상단 Clock 탭에서 지정합니다. 화면마다 색 다섯 가지를 따로 갖고, 값은 24비트 RGB 정수로 저장한 뒤 그릴 때 RGB565로 낮춥니다. 바늘 윤곽선은 바늘 색에서 자동으로 파생되므로 따로 설정하지 않습니다.

설정은 `analog_faces` 배열입니다. 각 항목의 키는 `dial`, `case`, `lume`, `hand`, `accent`입니다.

```json
"analog_faces": [ { "dial": 0, "case": 8, "lume": 61695, "hand": 16711680, "accent": 0 } ]
```

다섯 채널이 무엇을 칠하는지는 화면마다 다릅니다. 웹 UI가 고른 화면에 맞춰 라벨을 바꾸고, 그 화면이 쓰지 않는 채널은 숨깁니다.

| 화면 | `dial` | `case` | `lume` | `hand` | `accent` |
| --- | --- | --- | --- | --- | --- |
| Analog | 문자판 | 테두리 | 숫자와 시간바 | 바늘 | — |
| Mondaine, Mondaine White | 문자판 | 테두리 | 눈금과 시·분침 | 초침 | — |
| Digital | 배경 | — | 시 | 분 | — |
| Weather Digital | 배경 | — | 시·구분자·기온 | 분·날씨 이름 | — |
| Date Digital | 배경 | 년월일 | 시·구분자·요일 글자 | 분 | 요일 배경 |

`/api/config`는 `analog_face_count`로 **펌웨어가 실제로 그릴 수 있는 화면 수**를 알려주고, 웹 UI는 그 수만큼 선택 목록을 만듭니다. 화면을 추가할 때는 `main.cpp`의 `ANALOG_FACE_COUNT`만 올리면 되고 페이지는 손대지 않아도 됩니다. 저장 공간은 `ANALOG_FACE_MAX`(현재 6)까지 확보되어 있습니다.

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

## 네트워크와 복구 AP

**복구용 AP는 상시 켜져 있지 않습니다.** `SDP-Recovery`는 비밀번호가 없는 개방망이고
그 뒤로 OTA·`/file`·`/format`·raw 8080이 무인증으로 열려 있습니다. 들어올 길이
필요할 때만 올립니다.

- 부팅 시 STA 접속 실패 → 즉시
- 돌던 중 끊김 → 12초(`OFFLINE_GRACE_MS`) 뒤
- WiFi 복귀 → AP와 안내 화면 모두 내림

**`WiFi.mode(WIFI_STA)`만으로는 AP가 안 내려갑니다.** `WiFi.persistent`가 직전 AP
설정을 플래시에 남기고 SDK가 부팅 중 `setupNetwork()`보다 먼저 띄웁니다. 실제로
`/status`가 `ap_ip: "off"`라고 보고하는 동안 `SDP-Recovery`가 95%로 전파되고
있었고, **SSID 스캔 말고는 잡을 방법이 없었습니다.** `WiFi.softAPdisconnect(true)`가
전파를 끄고 저장된 설정까지 지웁니다. 상태 줄은 라디오가 아니라 의도를 보고합니다.

확인법: BSSID가 장치 STA MAC의 첫 바이트 `+0x02`면 같은 장치입니다(ESP8266 규칙).

**WiFi가 없으면 화면이 말합니다.** `drawOfflineScreen()`이 무선망 이름·주소·다음
할 일을 띄웁니다. 예전에는 마지막 화면을 그대로 두어 잘 도는 시계와 구별되지
않았습니다. 이 문구의 한글은 `scripts/gen/gen_extra_glyphs.py`의 `SCREEN_TEXT`가
글리프 출처로 갖고 있습니다 — 문구를 바꾸면 거기도 같이 고칩니다.

**재시작 후 3분간 좌하단에 IP**를 모든 화면에 그립니다(`drawIpBadge`). 헤더와 같은
방식으로 매 프레임 다시 씁니다 — 글리프가 안 켜진 픽셀을 건드리지 않으므로 덮어써도
화면은 그대로이고, 밑에서 화면이 다시 그려져도 반쯤 지워진 채 남지 않습니다.

## 날씨 조회에서 물린 것

**호출이 둘이면 성공도 둘이어야 합니다.** 관측값만 성공해도 전체를 성공으로 쳐서
`status`를 `ok`로 덮고 `lastWeatherMs`를 찍었습니다. 예보가 비어도 30분간 잠깁니다.
유효한 예보가 하나도 없으면 성공으로 치지 않습니다.

**`httpGetBody`의 예약 크기가 응답과 맞아야 합니다.** 매번 14KB를 예약했는데 실측은
관측 1.5KB, 예보 5.5KB(40행)입니다. 두 본문이 동시에 살아 있으면 28KB가 묶여,
**30KB가 비어 있는데도** 예보 파서가 `NoMemory`를 냅니다. 호출마다 예상 크기를
넘기고, 관측 버퍼와 레이더 배경 파일을 예보 요청 전에 반납합니다.

**재시도는 백오프해야 합니다.** 실패 시 `lastWeatherMs`가 안 움직이므로 `due`가 계속
참이고, 남는 것은 쿨다운뿐입니다. 15초 고정이면 장애 동안 KMA를 하루 11,000번
부릅니다. `weatherFailStreak`으로 대기를 두 배씩 늘려 정규 주기까지 갑니다.

## 웹 UI 비밀번호

`/login`에서 비밀번호를 받고, 부팅할 때마다 새로 만드는 세션 토큰을 쿠키로 내려줍니다. 재부팅하면 모두 로그아웃됩니다.

**비밀번호는 소스가 아니라 설정에 있습니다.** 초기값은 `0000`이고, 설정 키는
`web_password`입니다. 예전에는 `main.cpp`의 `AUTH_PASSWORD` 상수였는데, 그러면 빌드된
이미지가 특정 소유자 전용이 되어 배포물로 쓸 수 없습니다.

System 메뉴 아래쪽에서 바꿉니다. `POST /api/password`가 `{"current": "...", "new": "..."}`를
받고, 새 값은 1~32자여야 합니다. 성공하면 세션 토큰을 새로 발급하므로 **다른 브라우저는
모두 로그아웃**되고, 방금 바꾼 브라우저만 응답에 실린 새 쿠키로 유지됩니다.
설정 저장이 실패하면 옛 값으로 되돌리고 500을 돌려줍니다 — 재부팅하면 사라질 비밀번호를
바뀐 것처럼 보고하지 않기 위해서입니다.

**부팅 후 2분 동안은 메뉴가 잠기지 않습니다.** 비밀번호를 잊었을 때 돌아오는 길입니다.
전원을 껐다 켜고 그 사이에 새로 정하면 됩니다. 이 구간에서는 `/api/password`가
`current`를 검사하지 않습니다. UI에는 일부러 표시하지 않습니다.

이 창은 `millis()` 비교가 아니라 **래치**여야 합니다. `millis() < AUTH_GRACE_MS`로 쓰면
약 49일마다 카운터가 되감길 때 창이 저절로 2분씩 열립니다. `loop()`가 한 번 닫고,
진짜 재부팅 전에는 다시 열리지 않습니다.

`/api/config`는 비밀번호를 돌려주지 않습니다. 이 엔드포인트는 무인증이므로
`web_password_is_default` 불리언만 내보냅니다. 무인증인 `/api/config` **POST도
`web_password`를 받지 않습니다** — 받으면 누구나 옛 비밀번호 없이 바꿀 수 있습니다.
`loadConfig()`와 이 핸들러는 대입문이 글자 단위로 같으니 고칠 때 주의합니다.

다만 `config.json` 자체는 무인증 `/file?path=/config.json`으로 읽힙니다. WiFi 비밀번호와
기상청 키가 이미 그런 것과 같은 조건이고, 복구 경로를 열어 두기로 한 결정의 결과입니다.

**인증은 웹 메뉴에만 걸립니다.** 즉 `/`로 서빙되는 페이지와 `/app.js`, `/style.css`뿐입니다.

| 경로 | 인증 |
| --- | --- |
| `/`, `/app.js`, `/style.css` | 필요 |
| `/status`, `/api/config`, `/weather/*`, `/fs/list`, `/restart`, `/format` | 불필요 |
| `/update_ota`, `/api/ota/*`, `/file` | 불필요 |
| raw 8080 | 불필요 |

기존 도구와 복구 경로가 로그인 없이 그대로 동작하도록 한 선택입니다. 장치에 접근할 수 있으면 HTTP로 여전히 전부 조작할 수 있고, 비밀번호는 메뉴가 무심코 열리는 것만 막습니다.

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
7. `release/`는 통째로 추적합니다. 예전의 `.gitignore` 화이트리스트는 v1.0.4에서 걷어냈습니다.
8. 변경을 커밋하고 `main`에 병합한 뒤 push합니다.
9. `.\scripts\build_release.ps1 -Version <버전> -Publish`로 태그와 GitHub Release를 생성합니다.

GitHub Release에는 항상 에셋 세 개를 올립니다. 펌웨어 bin, LittleFS bin, 그리고 전체 소스 zip입니다.

## 현재 한계와 주의

- `v1.0.5`는 원본 시계/날씨 화면 자산을 포함하지만 LCD 드라이버 glue는 `TFT_eSPI` 기반입니다.
- **순정 상태의 새 장치에는 복구 펌웨어를 먼저 올립니다.** `release/SDP_CustomRecovery_*.bin`을
  UART로 넣어 raw 8080 복구 포트를 확보한 뒤에 본 펌웨어를 OTA합니다. 이 순서를
  건너뛰면 OTA가 실패했을 때 남는 경로가 없어 UART 재플래싱뿐입니다 —
  2026-08-26에 실제로 한 대를 그렇게 벽돌로 만들었습니다.
- OTA가 불안정한 장치에서는 업로드 중 재부팅될 수 있으므로 UART/TTL 복구 수단을 확보한 뒤 큰 변경을 적용합니다.
- 새 기능을 넣을 때도 `/status`, `/api/config`, `/file`, raw 8080 복구 경로는 유지합니다.
