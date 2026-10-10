// NU-87 TinyDK: 대원 보드(NU-40, Tap_Talk)의 블루투스 방송을 듣는 시험
// 방송 형식: 제조사 데이터 [0xFF 0xFF]['S']['Q'][보드ID][순번][이벤트]
// 1초마다 들린 보드별로 "ID 신호세기 횟수 이벤트"를 시리얼(115200)에 출력
#include "BLEDevice.h"

const uint16_t COMPANY_ID = 0xFFFF;
const int MAX_PEERS = 8;

struct Peer {
  uint8_t id;
  int rssiSum;
  int count;       // 이번 1초 동안 받은 횟수
  uint8_t seq;
  uint8_t event;
  bool used;
};

Peer peers[MAX_PEERS];
BLEAdvertData adv;

Peer* findPeer(uint8_t id) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && peers[i].id == id) return &peers[i];
  }
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) {
      peers[i] = {id, 0, 0, 0, 0, true};
      return &peers[i];
    }
  }
  return nullptr;
}

void scanCallback(T_LE_CB_DATA* p_data) {
  adv.parseScanInfo(p_data);
  if (adv.getManufacturer() != COMPANY_ID) return;
  uint8_t len = adv.getManufacturerDataLength();
  uint8_t* d = adv.getManufacturerData();
  if (len < 5 || d[0] != 'S' || d[1] != 'Q') return;

  Peer* p = findPeer(d[2]);
  if (!p) return;
  p->rssiSum += adv.getRSSI();
  p->count++;
  p->seq = d[3];
  p->event = d[4];
}

const char* eventName(uint8_t e) {
  return e == 1 ? "OK" : e == 2 ? "ALERT" : "-";
}

void setup() {
  Serial.begin(115200);
  BLE.init();
  BLE.configScan()->setScanMode(GAP_SCAN_MODE_PASSIVE);
  BLE.configScan()->setScanInterval(100);
  BLE.configScan()->setScanWindow(100);  // 쉬지 않고 계속 듣기
  BLE.configScan()->updateScanParams();
  BLE.setScanCallback(scanCallback);
  BLE.beginCentral(0);
  BLE.configScan()->startScan();
  Serial.println("NU87 LISTEN START");
}

void loop() {
  delay(1000);
  int heard = 0;
  char line[64];
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer& p = peers[i];
    if (!p.used) continue;
    if (p.count > 0) {
      snprintf(line, sizeof(line), "PEER %02X rssi=%d n=%d seq=%d ev=%s",
               p.id, p.rssiSum / p.count, p.count, p.seq, eventName(p.event));
      Serial.println(line);
      heard++;
    } else {
      snprintf(line, sizeof(line), "PEER %02X LOST", p.id);
      Serial.println(line);
    }
    p.rssiSum = 0;
    p.count = 0;
  }
  if (heard == 0) Serial.println("NO PEER");
}
