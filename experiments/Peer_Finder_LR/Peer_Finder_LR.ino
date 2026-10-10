// 1단계 (장거리 모드 버전): 두 보드가 블루투스로 서로 찾기
// - Peer_Finder와 동작은 같고, 무선 방식만 BLE 장거리 모드(Coded PHY, 125kbps)로 바꿈
//   → 감도 -95dBm에서 -103dBm으로 좋아져 더 멀리, 잡음에 더 강하게 통신
// - Bluefruit 라이브러리는 장거리 모드를 지원하지 않아서 광고/스캔은 SoftDevice API를 직접 호출
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

// SoftDevice 호출 결과 (0이면 성공). 시리얼로 출력해서 문제 확인용
uint32_t advErr = 0;
uint32_t scanErr = 0;

// 장거리 모드 스캔은 확장 광고를 받아야 해서 버퍼가 최소 255바이트 필요
uint8_t scanBuffer[BLE_GAP_SCAN_BUFFER_EXTENDED_MIN];
ble_data_t scanData = {scanBuffer, sizeof(scanBuffer)};

// SoftDevice가 광고 데이터를 계속 참조하므로 전역으로 유지
uint8_t advPayload[31];
ble_gap_adv_data_t advData;
uint8_t advHandle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;

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

  // 다음 신호 계속 듣기 (라이브러리의 31바이트 버퍼 대신 우리 버퍼로 재개)
  scanData.len = sizeof(scanBuffer);
  sd_ble_gap_scan_start(NULL, &scanData);
}

void startAdvertising() {
  uint8_t len = 0;
  // 확장 광고에는 Flags를 넣지 않는다. [길이][타입=제조사 데이터][회사ID 2바이트][S][Q][보드 ID]
  advPayload[len++] = 1 + 5;
  advPayload[len++] = BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
  advPayload[len++] = (uint8_t)(COMPANY_ID & 0xFF);
  advPayload[len++] = (uint8_t)(COMPANY_ID >> 8);
  advPayload[len++] = MAGIC_0;
  advPayload[len++] = MAGIC_1;
  advPayload[len++] = myId;

  advData.adv_data.p_data = advPayload;
  advData.adv_data.len = len;
  advData.scan_rsp_data.p_data = NULL;
  advData.scan_rsp_data.len = 0;

  ble_gap_adv_params_t params;
  memset(&params, 0, sizeof(params));
  params.properties.type = BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  params.interval = 160;                     // 160 x 0.625ms = 100ms마다 방송
  params.duration = 0;                       // 0 = 계속 방송
  params.filter_policy = BLE_GAP_ADV_FP_ANY;
  params.primary_phy = BLE_GAP_PHY_CODED;    // 장거리 모드
  params.secondary_phy = BLE_GAP_PHY_CODED;

  advErr = sd_ble_gap_adv_set_configure(&advHandle, &advData, &params);
  if (advErr == NRF_SUCCESS) {
    advErr = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, advHandle, 8);
  }
  if (advErr == NRF_SUCCESS) {
    advErr = sd_ble_gap_adv_start(advHandle, BLE_CONN_CFG_TAG_DEFAULT);
  }
}

void startScanning() {
  // 수신 처리는 라이브러리 이벤트 → scanCallback 경로를 그대로 사용 (필터 없음, 판별은 콜백에서)
  Bluefruit.Scanner.setRxCallback(scanCallback);

  ble_gap_scan_params_t params;
  memset(&params, 0, sizeof(params));
  params.extended = 1;                      // 확장 광고 받기 (장거리 모드에 필수)
  params.active = 0;
  params.interval = 160;                    // 100ms 중
  params.window = 80;                       // 50ms 듣기
  params.timeout = 0;                       // 0 = 계속 듣기
  params.scan_phys = BLE_GAP_PHY_CODED;     // 장거리 모드로 듣기
  params.filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL;

  scanErr = sd_ble_gap_scan_start(&params, &scanData);
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
    if (advErr || scanErr) {
      Serial.print("ERR adv=");
      Serial.print(advErr);
      Serial.print(" scan=");
      Serial.println(scanErr);
    } else if (lost) {
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
