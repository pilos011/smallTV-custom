# 순정 SD_PRO 기기를 커스텀 펌웨어로 옮기기

2026-09-02에 네 대를 이 순서로 옮겼습니다. **UART는 쓰지 않았습니다.** 납땜이 어렵고
핀이 작아 실전 복구 수단이 못 되므로, UART를 전제로 삼은 절차는 절차가 아닙니다.

## 왜 순서를 지켜야 하는가

**본 펌웨어를 순정에 바로 OTA할 수 없습니다.** 물리적으로 안 들어갑니다.

ESP8266은 새 이미지를 **파일시스템 시작 주소 아래**에 임시로 씁니다. 그래서 스테이징
상한은 `FS 시작 − 현재 펌웨어 크기`입니다.

| | FS 시작 | 현재 펌웨어 | 상한 |
| --- | --- | --- | --- |
| 순정 4m3m | 0x100000 | 488 KB | **547 KB** |
| 우리 4m2m | 0x200000 | 916 KB | 1.7 MB |

본 펌웨어는 916 KB입니다. 547 KB에 들어가지 않습니다. **중간 단계를 몇 번 거쳐도 4m3m
에서는 상한이 커지지 않습니다.** 350 KB짜리 bootsafe를 먼저 올려 FS를 0x200000으로
내리는 것이 유일한 길입니다.

## 순서

### 0. 순정 read-only 확인 — 아직 아무것도 올리지 않습니다

```bash
curl http://<IP>/                 # 200, "Smart Weather Clock"
curl http://<IP>/version          # 예: SD EN V1.1.7
curl http://<IP>/status           # 404  (우리 펌웨어가 아님을 확인)
curl http://<IP>/photo/list       # 200
```

**OTA 경로는 기기 자신의 코드에서 확인합니다.** 추측하지 않습니다.

```bash
curl -s http://<IP>/javascript.js | grep -o 'open("POST", "/[a-z_]*"'
# open("POST", "/update_ota"
```

순정은 `POST /update_ota`, multipart 필드 `update`, 그리고 **파일명이 `SDP`로 시작해야**
받습니다(`name.startsWith("SDP")`). GET으로 찔러보면 404가 나오는데, POST 전용 라우트라
그런 것이지 없는 것이 아닙니다.

### 1. bootsafe-4m2m

```powershell
curl.exe --fail --http1.0 --connect-timeout 5 --max-time 240 `
  -F "update=@<path>\firmware.bin;filename=SDP_Bootsafe_4m2m.bin" `
  "http://<IP>/update_ota"
```

`.pio/build/sdpro-bootsafe-4m2m/firmware.bin`, 350,864 B. 순정 상한 547 KB 안에
들어갑니다. **순정 `/update_ota`는 크기도 MD5도 검증하지 않으므로**, 올리기 전에 첫
바이트가 `0xE9`인지와 크기를 직접 확인합니다.

재부팅 후 `/status`로 판정합니다.

- `version` 이 `bootsafe-1`
- `safe_mode=false`, `fs_mounted=false`(정상 — 아래 참고)
- `ip` 와 `ap_ip` 가 둘 다 있음 — 집 망과 `SDP-Recovery`(암호 `123456789K`) 양쪽
- `boot_fails` 가 **10초 뒤 0으로 지워지는지** 확인. 순정에서 넘어오며 리셋 사유가
  크래시로 읽혀 1로 시작하는 경우가 있습니다
- `free_sketch_space` 가 1,744,896 으로 넓어졌는지

### 2. 본 펌웨어

```powershell
.\scripts\ota-upload.ps1 -Device <IP> -Bin .\release\SDP_ClockWeather_vX.Y.Z.bin
```

여기서부터는 **크기와 MD5를 붙여 보낼 수 있습니다.** bootsafe의 `/api/ota/fw` 가
`otaExpected` 와 `Update.setMD5()` 로 검증하고 용량 초과를 거부합니다.

재부팅 후 `version`, `safe_mode=false`, `boot_fails=0/0`, `ip` 확인.

### 3. LittleFS 이미지

```powershell
.\scripts\ota-upload.ps1 -Device <IP> -Bin .\release\littlefs-clock-weather-vX.Y.Z.bin -Fs
```

재부팅 후 `fs_mounted=true`, `fs_used` 가 598,016 부근인지 확인.

### 4. 설정 복원

```bash
py restore.py <IP>
```

**WiFi를 먼저 넣고 복원을 나중에 합니다.** 자세한 이유는 아래.

## 순정 파일시스템은 언제 사라지는가

순정 FS(`0x100000~0x400000`)와 우리 4m2m FS(`0x200000~0x400000`)는 겹칩니다. 다만 이
펌웨어는 `LittleFS.setConfig(LittleFSConfig(false))` 로 **자동 포맷을 꺼두므로**, 마운트에
실패해도 지우지 않고 보고만 합니다. 1·2단계에서 `fs_mounted=false` 가 나오는 것이 그것이고,
**순정 데이터는 3단계 전까지 살아 있습니다.**

꺼낼 것이 있으면 그 전에 **4m3m** 빌드(`sdpro-bootsafe`)를 올려 마운트하십시오. 순정
웹 UI에는 다운로드 경로가 아예 없어 그것이 유일한 방법입니다. 들어 있는 것: 아이콘 세
세트(`/ico` 65×46, `/ico2` 45×32, `/weather` 240×142), 한글이 든 `NotoSansKR18.vlw`
포함 폰트 5종, FlipClock 숫자 GIF 10개, 테마 GIF 5개, 출하 사진.

## WiFi를 먼저, 복원을 나중에

**백업은 WiFi 비밀번호를 담을 수 없습니다.** `/api/config` 는 `pass_set: true` 와
`{"ssid": ...}` 까지만 내보냅니다. 그래서 `restore.py` 는 `ssid`, `pass`,
`wifi_profiles` 를 **보내지 않습니다**(v1.0.35~).

그 전에는 보냈고, 2026-09-02에 기기 하나가 그것 때문에 망에서 떨어졌습니다. 이름만 있는
네트워크를 빈 비밀번호로 시도하다가 SDK가 보관하던 작동하던 자격증명까지 덮었습니다.
펌웨어도 함께 고쳐 이제 빈 자격증명으로는 시도하지 않고 시도가 저장된 것을 건드리지도
않지만, **순서는 여전히 WiFi 먼저입니다.**

3단계 직후 기기는 순정 시절 SDK에 남은 자격증명으로 접속을 유지합니다. 설정에는 없는
상태이므로 웹 UI의 WiFi 카드에 직접 넣어 두십시오.

## 판정 기준

**`curl` 이 `OK` 를 돌려준 것은 업로드가 끝났다는 뜻일 뿐입니다.** 매 단계 재부팅 후
`/status` 로 판정합니다 — `version`, `safe_mode`, `boot_fails`, `fs_mounted`, `ip`/`ap_ip`.

`curl exit 56`(연결 리셋)이 성공한 OTA에서 나올 수 있습니다. 응답이 재시작에 밀린 것이니
**다시 올리기 전에 `/status` 의 `fw_used` 가 올린 크기와 같은지 확인**하십시오.
