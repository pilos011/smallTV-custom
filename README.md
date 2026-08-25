# SmallTV Custom

GeekMagic SmallTV-Ultra / SD_PRO 계열 ESP8266 장치용 커스텀 펌웨어 작업 저장소입니다.

현재 `main` 브랜치는 안정 릴리스만 유지합니다. 새 화면, 기능, 복구 실험은 별도 브랜치에서 진행한 뒤 검증된 산출물만 `main`으로 병합합니다.

## 현재 안정 버전

- 버전: `v1.0.2`
- 펌웨어: `release/SDP_ClockWeather_v1.0.2.bin`
- 파일시스템: `release/littlefs-clock-weather-v1.0.2.bin`
- 대상: ESP8266 ESP-12F 기반 SD_PRO / SmallTV 호환 보드

## 포함 기능

- 원본 `wonjj6768/smalltv-ultra-korean-custom-firmware`의 시계/날씨 화면 자산 기반 렌더링
- 기상청 APIHub 초단기예보 조회
- 웹 관리 UI: Status, Weather, System, Recovery
- System 메뉴의 WiFi, 밝기, 재시작, LittleFS 목록
- 지정한 시간대에 화면을 자동으로 어둡게 하는 자동 야간 모드
- 아날로그 시계 화면. 문자판, 테두리, 숫자·눈금, 바늘 색을 웹 UI에서 지정합니다
- 사용할 화면을 골라 정해진 간격으로 번갈아 띄우는 화면 전환
- 웹 UI 비밀번호 로그인
- 복구용 OTA 경로와 파일 단위 LittleFS 쓰기 API

상세 인수인계, 빌드, 업로드, 복구 절차는 [HANDOVER.md](HANDOVER.md)를 기준으로 봅니다.
