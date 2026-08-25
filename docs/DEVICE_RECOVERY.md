# Device Recovery Notes

## 우선순위

1. 정상 웹 UI가 열리면 Recovery 메뉴의 OTA를 사용합니다.
2. 설정이나 웹 파일만 바꿀 때는 `/file?path=/...`를 사용합니다.
3. 웹 UI가 깨졌지만 HTTP가 살아 있으면 fallback HTML 또는 API를 사용합니다.
4. 포트 8080이 살아 있으면 raw fallback을 사용합니다.
5. OTA가 반복 재부팅되면 UART/TTL 플래싱으로 복구합니다.

## UART/TTL 기준

- ESP8266은 3.3V TTL입니다. 5V TTL을 RX/TX에 넣지 않습니다.
- 어댑터 TX는 ESP8266 RX에, 어댑터 RX는 ESP8266 TX에 교차 연결합니다.
- GND는 반드시 공통으로 연결합니다.
- flash mode 진입은 GPIO0을 GND에 둔 상태로 리셋/전원 인가합니다.

## esptool 예시

```powershell
py -m pip install esptool
py -m esptool --port COM6 flash_id
py -m esptool --port COM6 read_flash 0x00000 0x400000 backup.bin
py -m esptool --port COM6 write_flash 0x00000 release\SDP_ClockWeather_v1.0.0.bin
```

실제 COM 포트와 flash size는 장치에서 확인한 값으로 바꿉니다.
