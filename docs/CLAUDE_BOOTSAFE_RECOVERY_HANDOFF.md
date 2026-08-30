# Claude Handoff: OTA-First Bootsafe Recovery for SDP / SmallTV ESP8266

작성일: 2026-08-30  
작성자: 코선생  
대상 저장소: `D:\Personal\SmallTV-Custom`  
대상 펌웨어: `firmware/sdpro-clock-weather`  
핵심 전제: 새 SDP 장치는 UART 납땜 복구를 현실적인 안전망으로 보지 않는다. OTA만으로 최대한 길을 잃지 않는 구조를 먼저 만든다.

## 결론

이번 작업의 1차 목표는 시계/날씨/사진 기능이 아니다. 목표는 **OTA로 올려도 다시 OTA로 되돌아올 수 있는 최소 본 펌웨어**를 만드는 것이다.

임시 recovery firmware를 먼저 올리는 방식은 충분하지 않다. ESP8266 Arduino OTA 기본 구조에서는 이후 본 firmware를 올리는 순간 임시 recovery firmware가 앱 영역에서 사라진다. 따라서 복구 기능은 본 firmware 내부에 있어야 한다.

단, 본 firmware 내부 safe mode도 완전한 보증은 아니다. 앱이 부팅 첫 단계도 실행하지 못하면 safe mode도 실행되지 않는다. 그래서 OTA-only 전략에서는 기능을 줄이고, 부팅 초반 코드를 극단적으로 단순하게 만들고, 검증 없는 큰 펌웨어를 올리지 않는 방식으로 위험을 낮춘다.

## 절대 원칙

클로드는 아래 조건을 어기면 안 된다.

1. 새 기기에 기능이 많은 큰 firmware를 바로 OTA하지 않는다.
2. `.pio/build/.../firmware.bin`을 즉석 산출물 그대로 올리지 않는다.
3. `curl OK`를 배포 성공으로 해석하지 않는다.
4. 본 firmware와 LittleFS full image를 연속으로 올리지 않는다.
5. boot safety 변경과 UI/화면/색상/사진/날씨 기능 변경을 같은 release에 섞지 않는다.
6. 실패 원인을 LittleFS 손상으로 단정하지 않는다.
7. UART 복구를 전제로 위험한 OTA 실험을 하지 않는다.

## 현재 로컬 상태

- repo: `D:\Personal\SmallTV-Custom`
- public remote: `https://github.com/pilos011/SmallTV-Custom`
- 현재 HEAD: `0ffff85 Keep a way in when setup cannot finish`
- 현재 HEAD는 `origin/main`보다 1 commit 앞서 있다.
- 해당 commit은 다음을 포함한다.
  - `firmware/sdpro-clock-weather/src/main.cpp`
  - `release/SDP_ClockWeather_v1.0.22-bootsafe.bin`
- 이 구현은 아직 새 SDP 실기에서 검증된 안정 release로 취급하면 안 된다.

## 과거 사고의 핵심 원인

1. 앱 내부 recovery route가 너무 늦게 열렸다.
   - `/status`, `/update_ota`, `/api/ota/fw`, `/api/ota/fs`, raw 8080 route가 `setup()` 후반에 열리면, 그 전에 WDT reset이 나면 복구 경로가 없다.

2. `setup()` 초반에 너무 많은 heavy work가 있었다.
   - `LittleFS.begin()`
   - config load
   - radar/background sweep
   - WiFi scan/connect
   - 화면 렌더링 준비

3. WiFi 연결 시도는 WDT보다 길 수 있다.
   - `STA_TIMEOUT_MS = 15000`
   - WDT는 `WDTO_8S`
   - scan/connect 순서가 부팅 초반에 있으면 위험하다.

4. OTA `OK`는 단지 업로드 핸들러가 응답했다는 뜻이다.
   - 재부팅 후 expected `/status`가 확인되어야 성공이다.

## OTA-only 목표 구조

본 firmware는 다음 순서를 가져야 한다.

```text
setup()
  Serial.begin
  WDT enable
  최소 display init
  boot fail counter 증가 및 즉시 저장

  if bootSafeMode:
    최소 AP + 최소 OTA/status route 시작
    return

  최소 AP + 최소 OTA/status route 먼저 시작
  이후 LittleFS/config/WiFi/weather/display/photo/radar 초기화
  정상 생존 시간 통과 후 boot fail counter clear
```

중요한 차이는 이것이다.

```text
나쁜 구조:
heavy init -> setupRoutes()

좋은 구조:
boot counter -> recovery-capable routes -> heavy init
```

safe mode는 3회 실패 후 들어가는 보조 장치이고, 그 이전에도 최소 route는 가능한 한 먼저 열려 있어야 한다.

## Safe Mode 요구사항

safe mode는 다음만 해야 한다.

- `WiFi.mode(WIFI_AP)`
- `WiFi.softAP(AP_SSID)`
- `/status`
- `/update_ota`
- `/api/ota/fw`
- `/api/ota/fs`
- 가능하면 `/file?path=/...`
- raw 8080 `/status`, `/rawfw`, `/rawfs`가 이미 있다면 유지
- LCD에는 `SAFE MODE`, AP SSID, 접속 URL만 표시

safe mode에서 금지:

- `LittleFS.begin()`
- `LittleFS.info()`
- config read/write
- directory walk
- radar sweep
- album scan
- WiFi STA scan/connect
- NTP/weather/radar HTTP request
- 사진/JPEG decode
- 화면 rotation

## 현재 bootsafe 구현에서 반드시 리뷰할 부분

현재 `src/main.cpp`에는 다음 구현이 들어간 것으로 보인다.

- RTC user memory 기반 `BootMark`
- `BOOT_FAILS_TO_SAFE = 3`
- `BOOT_SETTLED_MS = 10000`
- `bootMarkBegin()`
- `setupSafeMode()`
- `/status`에 `safe_mode`, `boot_fails` 노출
- `loop()`에서 safe mode early return

하지만 클로드는 이것을 그대로 신뢰하면 안 된다. 아래를 직접 확인해야 한다.

1. `bootMarkBegin()`이 heavy work보다 먼저 실행되는가.
2. safe mode 분기 전에 filesystem mount가 없는가.
3. safe mode 분기 전에 WiFi scan/connect가 없는가.
4. safe mode에서 `setupRoutes()`가 실제로 호출되는가.
5. `loop()`가 safe mode에서 server/raw handling 외 작업을 하지 않는가.
6. boot counter clear가 `setup()` 끝이 아니라 일정 시간 생존 후 수행되는가.
7. 조건이 `fails > 3`인지 `fails >= 3`인지 문구와 동작이 일치하는가.

## 더 안전한 1차 배포물 정의

새 장치에 처음 올릴 배포물은 `full feature`가 아니라 다음 성격이어야 한다.

```text
SDP_ClockWeather_bootsafe_minimal.bin
```

특성:

- 시계/날씨는 있어도 되지만, boot path에서는 지연 실행
- 사진/레이더/색상 튜닝/대형 asset 기능은 비활성 또는 지연
- LittleFS가 없어도 fallback HTML로 OTA 가능
- 설정 파일이 깨져도 AP + OTA 가능
- WiFi 접속 실패해도 AP + OTA 가능
- `/status`가 부팅 단계와 safe mode 상태를 보여줌

권장 `/status` 필드:

```json
{
  "name": "SDP Clock Weather",
  "version": "...",
  "boot_phase": "routes_ready|fs_mount|config|wifi|ready|safe_mode",
  "safe_mode": false,
  "boot_fails": 0,
  "fs_mounted": true,
  "ap_ip": "off or 192.168.4.1",
  "ip": "...",
  "last": "..."
}
```

## OTA 검증 요구사항

OTA upload endpoint는 다음을 만족해야 한다.

- firmware는 첫 byte `0xE9` 확인
- `size`가 전달되면 `otaWritten == size` 확인
- `md5`가 전달되면 `Update.setMD5()` 사용
- size/md5 없이 성공한 경우 `OK`가 아니라 `OK unverified`처럼 분리
- 가능하면 새 장치 배포 스크립트는 size/md5 없는 OTA를 거부

배포 성공 판정:

```text
성공 아님:
curl returned OK

성공:
재부팅 후 /status에서 expected version 확인
safe_mode, boot_fails, fs_mounted, ip/ap_ip 확인
```

## 새 SDP 장치 OTA 순서

1. 순정 상태 read-only 확인
   - 가능한 endpoint:
     - `/`
     - `/status`
     - `/api/v1/system/version`
     - `/update_ota`
   - 응답 내용과 시간만 기록한다.
   - 아직 upload 금지.

2. artifact 확정
   - git commit hash
   - firmware path
   - byte size
   - SHA256
   - flash layout
   - build log

3. 첫 OTA는 bootsafe minimal만 허용
   - full feature firmware 금지
   - LittleFS upload 금지

4. 재부팅 후 검증
   - `/status` expected version
   - `safe_mode=false`
   - `boot_fails`가 10초 이후 0으로 clear되는지 확인
   - AP 또는 STA 접근 경로가 살아있는지 확인

5. 의도적 장애 테스트
   - 잘못된 WiFi 설정 또는 config 오류를 통해 safe mode 진입 가능성 확인
   - 단, LittleFS full overwrite 테스트는 금지

6. 그 뒤에만 기능 추가
   - 기능은 작은 commit/release 단위
   - 각 release마다 rollback 가능한 이전 bin 보존
   - UI/icon 변경은 가능하면 `/file?path=/...`

## 클로드 작업 산출물 양식

클로드는 구현 또는 리뷰 후 아래 양식으로 보고해야 한다.

```text
Purpose:
Git commit:
Firmware artifact:
Firmware size:
Firmware sha256:
LittleFS artifact used: yes/no
Flash layout:
Build result:
Boot route order:
Routes started before LittleFS: yes/no
Routes started before WiFi scan: yes/no
Safe mode avoids LittleFS: yes/no
Safe mode avoids STA scan/connect: yes/no
OTA size/md5 validation:
Read-only device checks:
Deployment performed: yes/no
Post-reboot /status:
Rollback artifact:
Residual risks:
```

## UART에 대한 위치

UART/TTL은 문서상 비상 참고일 뿐, 이번 전략의 전제가 아니다. 사용자는 납땜이 어렵고 핀이 작아 UART를 실전 복구 수단으로 삼기 어렵다.

따라서 클로드는 "문제 생기면 UART로 복구"라는 전제로 위험한 배포를 하면 안 된다.

참고 안전 기준:

- ESP8266은 3.3V TTL이다.
- TTL VCC/3.3V/5V는 USB-C 전원 사용 중 ESP에 연결하지 않는다.
- GND 공통, TX/RX 교차, GPIO0-GND는 flash mode 때만.
- `flash_id` 성공 전 `write_flash` 금지.

## 클로드에게 주는 직접 지시

새 SDP 기기에는 아직 기능 펌웨어를 올리지 말 것. 먼저 OTA-only bootsafe minimal firmware를 리뷰/정리하고, artifact/hash/size를 확정한 뒤, 사용자와 코선생에게 배포 전 보고할 것.

특히 현재 `release/SDP_ClockWeather_v1.0.22-bootsafe.bin`이 있어도 이것을 자동으로 안전하다고 가정하지 말 것. 새 기기에 올리기 전에는 위 검증 게이트를 통과해야 한다.

작업 우선순위:

```text
1. boot path audit
2. routes-before-heavy-init 보장
3. OTA validation 강화
4. status evidence 강화
5. bootsafe minimal artifact 생성
6. 새 장치 read-only 확인
7. 배포 승인 요청
```

그 전까지 시계/날씨/사진/색상/UI 개선은 보류한다.
