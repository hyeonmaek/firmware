# 현맥 (HYEONMAEK, 玄脈) — 단말·기지국 펌웨어

> 분대를 끊김 없이 잇는 검고 깊은 줄기. *The dark vein that keeps a squad connected.*

D4D × EDTH SEOUL 2026 해커톤 출품작의 하드웨어 펌웨어. 분대원 단말(NU-40 DK)끼리 블루투스 방송으로 사건을 주고받고, 기지국(NU-87)이 이를 받아 와이파이로 C2 서버에 올린다.

## 구성

| 폴더 | 내용 |
|---|---|
| `Tap_Talk/` | 분대원 단말(nRF52840 NU-40 DK + OLED + IMU) |
| `NU87_Gateway/` | 기지국(Realtek AmebaD NU-87): 단말 방송 수신 → C2 업로드, C2 명령 → 단말 방송 |
| `tools/` | 업로드 스크립트, 시리얼 모니터, 암호 시험, 한글 글꼴 생성 |
| `docs/` | 하드웨어 구성, 제품 설계서, 글꼴 라이선스 |
| `experiments/` | 개발 중 쓴 하드웨어 시험 스케치 |

## 단말 기능

- 버튼: S1 홈(길게 3초 = 진북 기준 맞추기), S2 받은 메시지 보기, S3 긴급구호, S4 적 발견(1번 사격개시 / 2번 사격금지)
  - 첫 누름부터 3초 안에만 취소(S3 2번, S4 3번). 3초 뒤 자동 전송
- 적 발견 수신: 동료 화면에 적 방향 화살표 + 거리. 수신자가 몸을 돌리면 화살표도 따라 돈다(IMU 자이로)
- 시계 화면: 호출부호, 요일·날짜, 24시간 시각, 배터리, 알림 벨
- 폰 연결(BLE GATT, 6자리 번호 페어링): 폰 GPS를 받아 보고에 싣고, 폰 메시지를 분대 또는 중앙으로 방송, 받은 메시지를 폰에 전달

## 보안

- 단말 방송은 AES-128 CTR 암호화 + CBC-MAC 인증 태그 + 카운터 재생 방지 (`Tap_Talk/secure.h`, NIST 시험값으로 검증: `tools/crypto_test.c`)
- 카운터는 플래시에 저장해 재부팅 후에도 뒤로 가지 않는다

## 빌드

1. 열쇠·와이파이 파일 만들기 (git에 올리지 않음)
   ```sh
   cp Tap_Talk/squad_key.h.example Tap_Talk/squad_key.h        # 분대 공용 열쇠 16바이트
   cp NU87_Gateway/secrets.h.example NU87_Gateway/secrets.h    # 와이파이 이름·비밀번호
   ```
   `NU87_Gateway.ino`의 `CENTRAL_IP`를 C2 서버 주소로 바꾼다.
2. 업로드 (arduino-cli)
   ```sh
   arduino-cli compile --fqbn nucode:nrf52:nu40dk Tap_Talk
   tools/upload_nu40.sh <단말 USB 일련번호>
   tools/upload_nu87.sh NU87_Gateway
   ```
3. 암호 시험: `cc -I Tap_Talk -o /tmp/crypto_test tools/crypto_test.c && /tmp/crypto_test`

## 관련 저장소

- 아이폰 앱: [hyeonmaek/app](https://github.com/hyeonmaek/app)
- C2 웹(지휘소 화면·서버): [hyeonmaek/web](https://github.com/hyeonmaek/web)

한글 글꼴: Galmuri11 (SIL Open Font License 1.1, `docs/licenses/`)
