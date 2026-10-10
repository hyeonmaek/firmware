// 1단계: 두 보드가 블루투스로 서로 찾기
// - 모든 보드에 같은 코드를 올린다
// - 각 보드는 "나 여기 있어"(광고)를 방송하면서 동시에 동료 신호를 듣는다(스캔)
// - 동료 신호 세기(RSSI)를 LED 개수로 표시: 가까울수록 많이 켜짐

#include <bluefruit.h>

// 우리 장치끼리만 알아보기 위한 표식 (제조사 데이터)
const uint16_t COMPANY_ID = 0xFFFF;  // 테스트용 ID
const uint8_t MAGIC_0 = 'S';
const uint8_t MAGIC_1 = 'Q';

// 신호 세기 → LED 개수 기준 (dBm, 0에 가까울수록 강함). 현장에서 조정 예정
const int RSSI_4_LEDS = -55;
const int RSSI_3_LEDS = -65;
const int RSSI_2_LEDS = -75;

// 이 시간 동안 신호가 없으면 "두절"
const unsigned long LOST_TIMEOUT_MS = 3000;

const uint8_t LED_PINS[] = {PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4};

uint8_t myId;
volatile float smoothedRssi = -100;
volatile unsigned long lastSeenMs = 0;
volatile bool everSeen = false;
volatile int lastRawRssi = 0;

void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t data[8];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, data, sizeof(data));

  // [회사ID 2바이트][S][Q][보낸 보드 ID]
  bool isPeer = len >= 5 &&
                data[0] == (COMPANY_ID & 0xFF) && data[1] == (COMPANY_ID >> 8) &&
                data[2] == MAGIC_0 && data[3] == MAGIC_1 &&
                data[4] != myId;

  if (isPeer) {
    lastRawRssi = report->rssi;
    // 신호 세기는 들쭉날쭉하므로 이전 값과 섞어서 부드럽게
    if (!everSeen) {
      smoothedRssi = report->rssi;
      everSeen = true;
    } else {
      smoothedRssi = smoothedRssi * 0.8f + report->rssi * 0.2f;
    }
    lastSeenMs = millis();
  }

  Bluefruit.Scanner.resume();  // 다음 신호 계속 듣기
}

void startAdvertising() {
  uint8_t payload[] = {
      (uint8_t)(COMPANY_ID & 0xFF), (uint8_t)(COMPANY_ID >> 8),
      MAGIC_0, MAGIC_1, myId};

  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addManufacturerData(payload, sizeof(payload));
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(160, 160);  // 160 x 0.625ms = 100ms마다 방송
  Bluefruit.Advertising.start(0);               // 0 = 계속 방송
}

void startScanning() {
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.setInterval(160, 80);  // 100ms 중 50ms 듣기
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.start(0);              // 0 = 계속 듣기
}

void showBars(uint8_t count) {
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(LED_PINS[i], i < count ? LED_STATE_ON : !LED_STATE_ON);
  }
}

void setup() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(LED_PINS[i], OUTPUT);
  }
  showBars(0);

  Serial.begin(115200);  // 노트북에서 신호 세기 숫자 확인용

  // 칩마다 고유한 번호에서 보드 ID를 뽑는다 → 보드마다 코드를 바꿀 필요 없음
  myId = (uint8_t)(NRF_FICR->DEVICEID[0] & 0xFF);

  Bluefruit.begin(1, 1);
  Bluefruit.setTxPower(8);  // 송신 세기 최대 (+8 dBm): 안테나가 약한 방향이어도 닿도록
  Bluefruit.autoConnLed(false);

  startAdvertising();
  startScanning();
}

void loop() {
  bool lost = !everSeen || (millis() - lastSeenMs > LOST_TIMEOUT_MS);

  if (lost) {
    // 동료를 못 찾음: LED 1만 짧게 깜빡 (프로그램은 살아 있다는 표시)
    showBars((millis() % 1000) < 100 ? 1 : 0);
  } else {
    int rssi = (int)smoothedRssi;
    uint8_t bars = rssi >= RSSI_4_LEDS ? 4
                 : rssi >= RSSI_3_LEDS ? 3
                 : rssi >= RSSI_2_LEDS ? 2
                 : 1;
    showBars(bars);
  }

  // 0.25초마다 측정값을 노트북으로 출력
  static unsigned long lastPrintMs = 0;
  if (millis() - lastPrintMs >= 250) {
    lastPrintMs = millis();
    if (lost) {
      Serial.println("LOST");
    } else {
      Serial.print("raw=");
      Serial.print(lastRawRssi);
      Serial.print(" avg=");
      Serial.println((int)smoothedRssi);
    }
  }

  delay(20);
}
