// NU-87 TinyDK 상황판 게이트웨이
// 대원 보드(NU-40, Tap_Talk)의 블루투스 방송을 듣고, 와이파이로 JSON을 제공한다.
//   GET /data -> {"uptime":ms,"peers":[{"id":"E9","rssi":-63,"seq":87,"event":1,"battery":80,"bearing":-1,"ack":-1,"age":120}],
//                 "msg":{"id":3,"to":"FF","active":true,"age":1200}}
//   GET /msg?to=FF&t=<UTF-16LE 16진수> -> 대원에게 자유 문장 방송 (to: 대상 보드 ID 2자리, FF=전체)
//   (event 0 없음 / 1 OK / 2 ALERT / 3 ENEMY, battery % 또는 -1, bearing 적 방향 0~359 또는 -1, age = 마지막으로 들은 뒤 지난 ms)
// 방송 형식(암호화): 제조사 데이터 [0xFF 0xFF][버전 0x02][보낸이ID][카운터 3][AES-128 암호문 6][인증태그 4]
//   - 내용(이벤트·배터리·방향·확인번호)은 암호문. 게이트웨이도 분대 공용 열쇠로 풀어 사건을 판단.
//   - 태그 불일치=위조 버림, 카운터 역행=재생 버림 (forged/replay 수를 /data로 노출)
// USE_AP_MODE 1: NU-87이 직접 와이파이(SQUAD-C2)를 만든다(발표/오프라인용, 주소 192.168.1.1)
// USE_AP_MODE 0: secrets.h의 와이파이에 접속한다(개발용, 노트북 인터넷 유지)
#include <WiFi.h>
#include "wifi_drv.h"
#include "lwip/netif.h"
extern struct netif xnetif[];   // 와이파이 드라이버의 네트워크 인터페이스(wifi_drv.cpp)
#include "BLEDevice.h"
#include "secrets.h"
extern "C" {
#include "wifi_conf.h"
#include "wifi_ind.h"
#include "gap_adv.h"
#include "secure.h"   // 대원 보드와 똑같은 방송 암호(복호화·검증). Tap_Talk/secure.h 심링크
}

#define USE_AP_MODE 0
#define ENABLE_BLE 1   // 0이면 블루투스를 끈다(와이파이 속도 진단용)
char apSsid[] = "SQUAD-C2";
char apPass[] = "squad1234";
char apChannel[] = "6";

const uint16_t COMPANY_ID = 0xFFFF;
const int MAX_PEERS = 8;

struct Peer {
  uint8_t id;
  float rssi;           // 흔들림 완화한 신호 세기
  uint8_t seq;
  uint8_t event;
  int battery;          // 배터리 잔량(%), 모르면 -1
  int bearing;          // 적 발견(event 3) 때 보낸 방향 0~359, 모르면 -1
  int ack;              // 대원이 마지막으로 수신 확인한 메시지 번호, 모르면 -1
  int32_t latE7, lonE7; // 대원 폰 GPS(×1e7), 0이면 모름
  uint8_t reportedSeq;  // 중앙에 이미 올린 보고의 순번(중복 업로드 방지), 255=없음
  unsigned long lastSeenMs;
  unsigned long packets;
  bool used;
};

Peer peers[MAX_PEERS];
WiFiServer server(80);
volatile unsigned long advTotal = 0;  // 진단: 받은 블루투스 방송 전체 개수
volatile unsigned long advSquad = 0;  // 진단: 그중 대원 보드 방송 개수

// 중앙 지휘소 서버 (노트북). 분대 보고를 올릴 때만 접속(지금은 와이파이 상시 연결, mTLS는 배치 시).
IPAddress CENTRAL_IP(192, 168, 0, 252);
const uint16_t CENTRAL_PORT = 8090;
// 행사장에서 DHCP 주소(.51)를 다른 팀 NU-87과 겹쳐 받아 통신이 섞였다 → 비어 있는 고정 주소 사용
IPAddress MY_IP(192, 168, 0, 233), MY_GW(192, 168, 0, 1), MY_MASK(255, 255, 255, 0);
// 이벤트 코드(단말과 동일): 2=SOS, 3=적발견 사격개시, 6=정정 → 이 셋만 중앙 업로드
volatile bool uploadPending = false;
volatile uint8_t upFrom = 0, upEvent = 0;
volatile uint16_t upBearing = 0;
volatile int32_t upLatE7 = 0, upLonE7 = 0;   // 보고자 폰 GPS(×1e7), 0이면 모름

// 대원 → 중앙 메시지: 대원 보드가 [FF FF]['S']['M'][번호(0xC0|순번)][보낸이][조각<<4|조각수][UTF-16LE ×≤11]로
// 20초 반복 방송한다. 조각을 모아 다 되면 중앙에 type=message로 올린다.
const int UP_MAX_UNITS = 66;
volatile uint8_t mrxFrom = 0, mrxId = 0, mrxCount = 0, mrxLastUnits = 0;
volatile uint16_t mrxMask = 0;
uint16_t mrxUnits[UP_MAX_UNITS];
volatile uint16_t mDoneKeys[4] = {0};   // 이미 올린 메시지((보낸이<<8)|번호)
volatile int mDoneHead = 0;
volatile bool mUpPending = false;
volatile uint8_t mUpFrom = 0;
volatile int mUpLen = 0;
uint16_t mUpUnits[UP_MAX_UNITS];

// 방송 복호화: 게이트웨이도 분대 공용 열쇠를 가진다(어떤 사건인지 알아야 중앙에 올릴지 판단).
SecCtx secCtx;
SecReplay secReplay;
volatile unsigned long advForged = 0;   // 진단: 위조 방송(태그 불일치) 수
volatile unsigned long advReplay = 0;   // 진단: 재생 방송(옛 카운터) 수

// ---------------- 블루투스 ----------------
Peer* findPeer(uint8_t id) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && peers[i].id == id) return &peers[i];
  }
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) {
      peers[i] = {id, 0, 0, 0, -1, -1, -1, 0, 0, 255, 0, 0, true};
      return &peers[i];
    }
  }
  return nullptr;
}

// 행사장에는 초당 수백 개의 방송이 있어서, 전체 해석(parseScanInfo) 없이
// 원본 바이트에서 우리 표시([0xFF][회사ID 2][버전 0x02])만 빠르게 찾아 복호화한다
void scanCallback(T_LE_CB_DATA* p_data) {
  advTotal++;
  T_LE_SCAN_INFO* info = p_data->p_le_scan_info;
  const uint8_t* buf = info->data;
  uint8_t pos = 0;
  while (pos + 1 < info->data_len) {
    uint8_t fieldLen = buf[pos];          // 타입 1바이트 + 내용
    if (fieldLen == 0 || pos + 1 + fieldLen > info->data_len) return;
    const uint8_t* f = buf + pos + 1;     // f[0] = 타입
    // 대원 → 중앙 메시지 조각
    if (f[0] == 0xFF && fieldLen - 1 >= 7 && f[1] == (COMPANY_ID & 0xFF) && f[2] == (COMPANY_ID >> 8) &&
        f[3] == 'S' && f[4] == 'M' && (f[5] & 0xC0) == 0xC0) {
      uint8_t id = f[5], from = f[6], idx = f[7] >> 4, count = f[7] & 0x0F;
      uint8_t units = (fieldLen - 1 - 7) / 2;
      uint16_t key = ((uint16_t)from << 8) | id;
      bool done = false;
      for (int i = 0; i < 4; i++) if (mDoneKeys[i] == key) done = true;
      if (done || count == 0 || idx >= count || count * 11 > UP_MAX_UNITS) return;
      if (id != mrxId || from != mrxFrom || count != mrxCount) { mrxId = id; mrxFrom = from; mrxCount = count; mrxMask = 0; }
      for (uint8_t i = 0; i < units && i < 11; i++) mrxUnits[idx * 11 + i] = f[8 + i * 2] | (f[9 + i * 2] << 8);
      if (idx == count - 1) mrxLastUnits = units;
      mrxMask |= (1 << idx);
      if (mrxMask == (1 << count) - 1 && !mUpPending) {
        mUpLen = (count - 1) * 11 + mrxLastUnits;
        for (int i = 0; i < mUpLen; i++) mUpUnits[i] = mrxUnits[i];
        mUpFrom = from;
        mUpPending = true;
        mDoneKeys[mDoneHead] = key; mDoneHead = (mDoneHead + 1) % 4;
      }
      return;
    }
    // 암호화된 대원 방송: [0xFF][회사ID 2][버전 0x02][보낸이][카운터 3][암호문 6][태그 4]
    if (f[0] == 0xFF && fieldLen - 1 >= SEC_FRAME &&
        f[1] == (COMPANY_ID & 0xFF) && f[2] == (COMPANY_ID >> 8) && f[3] == SEC_VER) {
      uint8_t sid; uint32_t sctr; uint8_t content[SEC_CONTENT];
      int res = sec_open(&secCtx, &secReplay, COMPANY_ID, f + 1, fieldLen - 1, &sid, &sctr, content);
      (void)sctr;
      if (res == SEC_FORGED) { advForged++; return; }   // 열쇠 없는 가짜 → 버림
      if (res == SEC_REPLAY) { advReplay++; return; }    // 옛 방송 재생 → 버림
      if (res != SEC_OK && res != SEC_DUP) return;
      advSquad++;
      Peer* p = findPeer(sid);
      if (!p) return;
      int8_t rssi = info->rssi;
      p->rssi = p->packets == 0 ? rssi : p->rssi * 0.7f + rssi * 0.3f;
      p->lastSeenMs = millis();
      p->packets++;
      if (res == SEC_DUP) return;                        // 반복 방송: 거리만 갱신, 내용 처리는 생략
      uint8_t ev = content[0];
      uint8_t evSeq = content[1];
      p->seq = evSeq;
      p->event = ev;
      p->battery = content[2];
      p->bearing = content[3] | (content[4] << 8);
      p->ack = content[5];
      p->latE7 = (int32_t)((uint32_t)content[6] | ((uint32_t)content[7] << 8) | ((uint32_t)content[8] << 16) | ((uint32_t)content[9] << 24));
      p->lonE7 = (int32_t)((uint32_t)content[10] | ((uint32_t)content[11] << 8) | ((uint32_t)content[12] << 16) | ((uint32_t)content[13] << 24));
      // 기지국이 켜진 뒤 처음 듣는 대원이면, 그 대원이 계속 방송 중인 지난 사건은 올리지 않는다(재부팅 때 가짜 보고 방지)
      if (p->reportedSeq == 255) p->reportedSeq = evSeq;
      // 교전(사격개시)·구조·정정이고 새 사건 번호면 중앙 업로드 요청(HTTP는 loop에서)
      if ((ev == 2 || ev == 3 || ev == 6) && evSeq != p->reportedSeq && !uploadPending) {
        p->reportedSeq = evSeq;
        upFrom = sid;
        upEvent = ev;
        upBearing = p->bearing;
        upLatE7 = (int32_t)((uint32_t)content[6] | ((uint32_t)content[7] << 8) | ((uint32_t)content[8] << 16) | ((uint32_t)content[9] << 24));
        upLonE7 = (int32_t)((uint32_t)content[10] | ((uint32_t)content[11] << 8) | ((uint32_t)content[12] << 16) | ((uint32_t)content[13] << 24));
        uploadPending = true;
      }
      return;
    }
    pos += fieldLen + 1;
  }
}

// ---------------- 중앙 → 대원 메시지 (자유 문장) ----------------
// 상황판이 GET /msg?to=FF&t=<UTF-16LE 16진수> 를 보내면, 문장을 11글자씩 조각내
// 무연결 BLE 방송으로 조각을 돌아가며 내보낸다. 대원 보드는 조각을 모아 화면에 띄운다.
// 방송 데이터(제조사 데이터): [FF FF]['S']['M'][메시지번호][대상ID(FF=전체)][조각번호<<4 | 조각수][글자 UTF-16LE ×최대 11]
// 메시지가 없을 때는 방송하지 않는다(전파 방출 최소화).
const int MSG_MAX_UNITS = 66;              // 최대 66글자 (6조각)
const int MSG_UNITS_PER_CHUNK = 11;
const unsigned long MSG_ACTIVE_MS = 60000; // 60초 동안 반복 방송
const unsigned long MSG_CHUNK_MS = 80;     // 조각 바꾸는 간격

struct Message {
  uint8_t id;
  uint8_t target;
  uint16_t units[MSG_MAX_UNITS];
  int len;
  int chunks;
  int curChunk;
  bool active;
  unsigned long startMs;
  unsigned long lastChunkMs;
};
Message msg = {0, 0xFF, {0}, 0, 0, 0, false, 0, 0};
bool advRunning = false;

void setAdvChunk(int idx) {
  uint8_t adv[31];
  int start = idx * MSG_UNITS_PER_CHUNK;
  int n = msg.len - start;
  if (n > MSG_UNITS_PER_CHUNK) n = MSG_UNITS_PER_CHUNK;
  if (n < 0) n = 0;
  int pos = 0;
  adv[pos++] = 1 + 7 + n * 2;              // AD 길이(타입 포함)
  adv[pos++] = 0xFF;                       // 제조사 데이터
  adv[pos++] = COMPANY_ID & 0xFF;
  adv[pos++] = COMPANY_ID >> 8;
  adv[pos++] = 'S';
  adv[pos++] = 'M';
  adv[pos++] = msg.id;
  adv[pos++] = msg.target;
  adv[pos++] = (uint8_t)((idx << 4) | (msg.chunks & 0x0F));
  for (int i = 0; i < n; i++) {
    adv[pos++] = msg.units[start + i] & 0xFF;
    adv[pos++] = msg.units[start + i] >> 8;
  }
  le_adv_set_param(GAP_PARAM_ADV_DATA, pos, adv);
}

void startMsgAdvertising() {
  uint8_t advType = GAP_ADTYPE_ADV_NONCONN_IND;
  uint16_t intMin = 32, intMax = 48;       // 20~30ms
  le_adv_set_param(GAP_PARAM_ADV_EVENT_TYPE, sizeof(advType), &advType);
  le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MIN, sizeof(intMin), &intMin);
  le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MAX, sizeof(intMax), &intMax);
  setAdvChunk(0);
  T_GAP_CAUSE c = le_adv_start();
  advRunning = (c == GAP_CAUSE_SUCCESS);
  Serial.print("MSG ADV START cause=");
  Serial.println((int)c);
}

void stopMsgAdvertising() {
  if (advRunning) le_adv_stop();
  advRunning = false;
}

void updateMessage() {
  if (!msg.active) return;
  unsigned long now = millis();
  if (now - msg.startMs > MSG_ACTIVE_MS) {
    msg.active = false;
    stopMsgAdvertising();
    Serial.println("MSG DONE");
    return;
  }
  if (now - msg.lastChunkMs < MSG_CHUNK_MS) return;
  msg.lastChunkMs = now;
  msg.curChunk = (msg.curChunk + 1) % msg.chunks;
  setAdvChunk(msg.curChunk);
  le_adv_update_param();
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 쿼리에서 key=값 꺼내기 (값은 공백·&·끝까지)
String queryParam(const String& req, const char* key) {
  String k = String(key) + "=";
  int i = req.indexOf("?" + k);
  if (i < 0) i = req.indexOf("&" + k);
  if (i < 0) return "";
  i += k.length() + 1;
  int e = i;
  while (e < (int)req.length() && req[e] != '&' && req[e] != ' ') e++;
  return req.substring(i, e);
}

// /msg 처리: 성공하면 메시지 번호, 실패하면 -1
int setMessageHex(uint8_t target, const String& t) {
  if (t.length() < 4 || t.length() % 4 != 0) return -1;
  int n = t.length() / 4;
  if (n > MSG_MAX_UNITS) n = MSG_MAX_UNITS;
  for (int i = 0; i < n; i++) {
    int lo1 = hexVal(t[i * 4]), lo2 = hexVal(t[i * 4 + 1]), hi1 = hexVal(t[i * 4 + 2]), hi2 = hexVal(t[i * 4 + 3]);
    if (lo1 < 0 || lo2 < 0 || hi1 < 0 || hi2 < 0) return -1;
    msg.units[i] = (uint16_t)((lo1 << 4 | lo2) | ((hi1 << 4 | hi2) << 8));
  }
  msg.len = n;
  msg.chunks = (n + MSG_UNITS_PER_CHUNK - 1) / MSG_UNITS_PER_CHUNK;
  msg.target = target;
  msg.id = msg.id >= 127 ? 1 : msg.id + 1;   // 1~127만(128 이상은 대원 보드가 보낸 메시지 번호)
  msg.curChunk = 0;
  msg.startMs = millis();
  msg.lastChunkMs = millis();
  msg.active = true;
  if (advRunning) { setAdvChunk(0); le_adv_update_param(); }
  else startMsgAdvertising();
  Serial.print("MSG id="); Serial.print(msg.id);
  Serial.print(" to="); Serial.print(msg.target, HEX);
  Serial.print(" units="); Serial.print(msg.len);
  Serial.print(" chunks="); Serial.println(msg.chunks);
  return msg.id;
}

int startMessage(const String& req) {
  String to = queryParam(req, "to");
  String t = queryParam(req, "t");
  uint8_t target = 0xFF;
  if (to.length() == 2) target = (uint8_t)(hexVal(to[0]) << 4 | hexVal(to[1]));
  return setMessageHex(target, t);
}

int startMessageOLD(const String& req) {
  String to = queryParam(req, "to");
  String t = queryParam(req, "t");
  if (t.length() < 4 || t.length() % 4 != 0) return -1;
  int n = t.length() / 4;
  if (n > MSG_MAX_UNITS) n = MSG_MAX_UNITS;
  for (int i = 0; i < n; i++) {
    int lo1 = hexVal(t[i * 4]), lo2 = hexVal(t[i * 4 + 1]), hi1 = hexVal(t[i * 4 + 2]), hi2 = hexVal(t[i * 4 + 3]);
    if (lo1 < 0 || lo2 < 0 || hi1 < 0 || hi2 < 0) return -1;
    msg.units[i] = (uint16_t)((lo1 << 4 | lo2) | ((hi1 << 4 | hi2) << 8));
  }
  msg.len = n;
  msg.chunks = (n + MSG_UNITS_PER_CHUNK - 1) / MSG_UNITS_PER_CHUNK;
  msg.target = 0xFF;
  if (to.length() == 2) msg.target = (uint8_t)(hexVal(to[0]) << 4 | hexVal(to[1]));
  msg.id = msg.id == 255 ? 1 : msg.id + 1;
  msg.curChunk = 0;
  msg.startMs = millis();
  msg.lastChunkMs = millis();
  msg.active = true;
  if (advRunning) {
    setAdvChunk(0);
    le_adv_update_param();
  } else {
    startMsgAdvertising();
  }
  Serial.print("MSG id=");
  Serial.print(msg.id);
  Serial.print(" to=");
  Serial.print(msg.target, HEX);
  Serial.print(" units=");
  Serial.print(msg.len);
  Serial.print(" chunks=");
  Serial.println(msg.chunks);
  return msg.id;
}

void startBle() {
  BLE.init();
  BLE.configScan()->setScanMode(GAP_SCAN_MODE_PASSIVE);
  BLE.configScan()->setScanInterval(100);
  BLE.configScan()->setScanWindow(30);   // 안테나를 와이파이와 나눠 쓰므로 30%만 듣는다(100%면 와이파이 응답이 수 초 지연)
  BLE.configScan()->setScanDuplicateFilter(false);  // 같은 방송도 매번 받아야 "살아 있음"을 안다
  BLE.configScan()->updateScanParams();
  BLE.setScanCallback(scanCallback);
  BLE.beginCentral(0);
  BLE.configScan()->startScan();
}

// ---------------- LED ----------------
void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

// ---------------- 와이파이 ----------------
void enable5GHz() {
  // 누코드 시작 가이드 5장: 5GHz 사용을 위한 채널 플랜 0x7F
  uint8_t plan;
  if (wifi_get_channel_plan(&plan) == RTW_SUCCESS && plan != 0x7F) {
    wifi_set_channel_plan(0x7F);
  }
}

void printNearbyNetworks() {
  Serial.println("WIFI SCAN...");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.print("  ");
    Serial.print(WiFi.SSID(i));
    Serial.print("  ");
    Serial.print(WiFi.RSSI(i));
    Serial.println(" dBm");
  }
  Serial.print("WIFI SCAN DONE: ");
  Serial.println(n);
}

void startWifi() {
  setLed(true, false, false);
#if USE_AP_MODE
  WiFi.apbegin(apSsid, apPass, apChannel);
  Serial.print("AP MODE SSID=");
  Serial.println(apSsid);
#else
  char ssid[] = WIFI_SSID;
  char pass[] = WIFI_PASS;
  // 행사장에 같은 NU-87이 여러 대라 공장 기본 MAC(00:e0:4c:87:00:00)이 겹친다 → 공유기가 1분마다 끊음(10/11).
  // 와이파이를 켜자마자(검색·접속 전) RAM에서만 우리 고유 MAC으로 바꾼다(칩 efuse에는 안 씀).
  WiFiDrv::wifiDriverInit();   // 와이파이 켜기(접속 전)
  u8 myMac[6] = {0x02, 0x4D, 0x41, 0x45, 0x4B, 0x01};   // "MAEK" 01, 로컬 관리 주소
  int macRet = wifi_change_mac_address_from_ram(0, myMac);
  if (macRet == RTW_SUCCESS) memcpy(xnetif[0].hwaddr, myMac, 6);   // ARP 응답도 같은 주소로
  Serial.print("MAC CHANGE ret="); Serial.println(macRet);
  WiFi.config(MY_IP, MY_GW, MY_GW, MY_MASK);
  int status = WL_IDLE_STATUS;
  for (int attempt = 1; status != WL_CONNECTED; attempt++) {
    Serial.print("WIFI CONNECT TRY ");
    Serial.println(attempt);
    status = WiFi.begin(ssid, pass);
    if (status != WL_CONNECTED) delay(3000);
  }
#endif
  WiFi.disablePowerSave();   // 절전 중에는 요청 응답이 늦어진다
#if !USE_AP_MODE
  wifi_reg_event_handler(WIFI_EVENT_DISCONNECT, onWifiDisconnect, NULL);
#endif
  server.begin();
  setLed(false, true, false);
  Serial.print("WIFI OK IP=");
  Serial.print(WiFi.localIP());
  Serial.print(" AP_RSSI=");
  Serial.println(WiFi.RSSI());
}

// 행사장 공유기가 가끔 연결을 끊는다(18:10 disassoc reason 8). 다시 붙어도 웹서버 소켓은
// 죽은 채로 남아 "Connection refused"가 나므로, 끊김을 감지하면 재접속 후 웹서버를 다시 연다.
volatile bool wifiDropped = false;
volatile bool reconnecting = false;

void onWifiDisconnect(char* buf, int len, int flags, void* userData) {
  if (!reconnecting) wifiDropped = true;
}

void checkWifi() {
#if !USE_AP_MODE
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  if (!wifiDropped && WiFi.status() == WL_CONNECTED) return;

  Serial.println("WIFI DROPPED -> RECONNECT");
  reconnecting = true;
  setLed(true, false, false);
  server.end();
  char ssid[] = WIFI_SSID;
  char pass[] = WIFI_PASS;
  WiFi.config(MY_IP, MY_GW, MY_GW, MY_MASK);
  while (WiFi.begin(ssid, pass) != WL_CONNECTED) delay(3000);
  WiFi.disablePowerSave();
  server.begin();
  wifiDropped = false;
  reconnecting = false;
  setLed(false, true, false);
  Serial.print("WIFI RECOVERED IP=");
  Serial.println(WiFi.localIP());
  helloCentral();
#endif
}

// ---------------- 웹 ----------------
String buildJson() {
  unsigned long now = millis();
  String s = "{\"uptime\":";
  s += now;
  s += ",\"peers\":[";
  bool first = true;
  for (int i = 0; i < MAX_PEERS; i++) {
    Peer& p = peers[i];
    if (!p.used) continue;
    char item[144];
    snprintf(item, sizeof(item), "%s{\"id\":\"%02X\",\"rssi\":%d,\"seq\":%d,\"event\":%d,\"battery\":%d,\"bearing\":%d,\"ack\":%d,\"age\":%lu}",
             first ? "" : ",", p.id, (int)lroundf(p.rssi), p.seq, p.event, p.battery, p.bearing, p.ack, now - p.lastSeenMs);
    s += item;
    first = false;
  }
  s += "],\"forged\":";
  s += (unsigned long)advForged;       // 들은 위조 방송 누적(열쇠 없는 가짜)
  s += ",\"replay\":";
  s += (unsigned long)advReplay;       // 들은 재생 방송 누적(옛 방송 다시 틀기)
  s += ",\"msg\":{\"id\":";
  s += msg.id;
  s += ",\"to\":\"";
  char t[3];
  snprintf(t, sizeof(t), "%02X", msg.target);
  s += t;
  s += "\",\"active\":";
  s += msg.active ? "true" : "false";
  s += ",\"age\":";
  s += msg.active ? (now - msg.startMs) : 0;
  s += "}}";
  return s;
}

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  // 요청 첫 줄(GET /data HTTP/1.1)만 보고, 나머지 헤더는 빈 줄까지 버린다
  String requestLine = "";
  String line = "";
  unsigned long deadline = millis() + 3000;
  while (client.connected() && millis() < deadline) {
    if (!client.available()) { delay(1); continue; }
    char c = client.read();
    if (c == '\r') continue;
    if (c != '\n') { line += c; continue; }
    if (requestLine.length() == 0) requestLine = line;
    if (line.length() == 0) break;
    line = "";
  }

  if (requestLine.length() == 0) {   // 요청 줄을 못 받았으면 엉뚱한 응답 대신 그냥 닫는다
    client.stop();
    return;
  }

  String body;
  String type;
  if (requestLine.startsWith("GET /msg")) {
    int id = startMessage(requestLine);
    body = id < 0 ? String("{\"ok\":false}") : String("{\"ok\":true,\"id\":") + id + ",\"chunks\":" + msg.chunks + "}";
    type = "application/json";
  } else if (requestLine.startsWith("GET /data")) {
    body = buildJson();
    type = "application/json";
  } else {
    body = "SQUAD-C2 gateway OK. Data: /data\n";
    type = "text/plain";
  }
  // 헤더와 본문을 한 번에 보낸다(여러 번 나눠 보내면 조각마다 지연이 생길 수 있음)
  String response = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
                    "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                    String(body.length()) + "\r\n\r\n" + body;
  client.write((const uint8_t*)response.c_str(), response.length());
  delay(1);
  client.stop();
}

// ---------------- 시리얼 요약 ----------------
void printSummary() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint < 2000) return;
  lastPrint = millis();
  Serial.print("ADV total=");
  Serial.print(advTotal);
  Serial.print(" squad=");
  Serial.print(advSquad);
  advTotal = 0;
  advSquad = 0;
  Serial.print(" WIFI=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "down");
  Serial.print(" IP=");
  Serial.print(WiFi.localIP());
  Serial.print(" ");
  Serial.println(buildJson());
}

// 부팅해서 와이파이에 붙으면 TOC에 한 번 인사(TOC가 기지국 주소를 알고 상태를 확인하게)
void helloCentral() {
  WiFiClient c;
  if (!c.connect(CENTRAL_IP, CENTRAL_PORT)) { Serial.println("HELLO FAIL"); return; }
  c.print("POST /hello HTTP/1.1\r\nHost: central\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 2\r\n\r\n{}");
  c.flush();
  delay(200);
  c.stop();
  Serial.println("HELLO sent");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  Serial.println("NU87 GATEWAY START");
  sec_init(&secCtx, SEC_MASTER_KEY);   // 분대 공용 열쇠로 복호화·검증 준비
  sec_replay_reset(&secReplay);
  enable5GHz();
  startWifi();
#if ENABLE_BLE
  startBle();
#endif
  helloCentral();
}

// 응답 JSON에서 첫 번째 "key":"..." 값을 꺼낸다(간이 파서)
String jsonStr(const String& src, const char* key) {
  String k = String("\"") + key + "\":\"";
  int i = src.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int e = src.indexOf('"', i);
  if (e < 0) return "";
  return src.substring(i, e);
}

// 교전/구조/정정을 중앙 서버에 POST, 답장의 명령(u16)을 분대에 방송
void uploadToCentral() {
  if (!uploadPending) return;
  uploadPending = false;
  uint8_t from = upFrom, ev = upEvent;
  uint16_t brg = upBearing;
  int32_t la = upLatE7, lo = upLonE7;

  char fromHex[3];
  snprintf(fromHex, sizeof(fromHex), "%02X", from);
  // 폰 GPS가 있으면 위경도 포함(소수 7자리). 없으면 생략.
  char gps[48] = "";
  if (la != 0 || lo != 0)
    snprintf(gps, sizeof(gps), ",\"lat\":%ld.%07ld,\"lon\":%ld.%07ld",
             (long)(la / 10000000), (long)labs(la % 10000000), (long)(lo / 10000000), (long)labs(lo % 10000000));
  char body[220];
  if (ev == 3)
    snprintf(body, sizeof(body), "{\"type\":\"engagement\",\"from\":\"%s\",\"bearing\":%u,\"fire\":\"open\"%s}", fromHex, brg, gps);
  else if (ev == 2)
    snprintf(body, sizeof(body), "{\"type\":\"sos\",\"from\":\"%s\"%s}", fromHex, gps);
  else
    snprintf(body, sizeof(body), "{\"type\":\"correction\",\"from\":\"%s\",\"bearing\":%u%s}", fromHex, brg, gps);

  postReport(body);
}

void postReport(const char* body) {
  Serial.print("UPLOAD -> central: ");
  Serial.println(body);

  WiFiClient c;
  if (!c.connect(CENTRAL_IP, CENTRAL_PORT)) {
    Serial.println("UPLOAD FAIL: no connect");
    return;
  }
  c.print("POST /report HTTP/1.1\r\nHost: central\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
  c.print(strlen(body));
  c.print("\r\n\r\n");
  c.print(body);
  c.flush();
  delay(300);          // 전송 완료 여유. 응답은 안 읽는다(중앙이 대기 명령을 NU-87 /msg로 되밀어줌).
  c.stop();
  Serial.println("UPLOAD done");
}

// 대원이 폰으로 '중앙'에 보낸 메시지를 올린다. 글자는 UTF-16LE 16진수(t)로, 위치는 그 대원 방송의 GPS.
void uploadMessageToCentral() {
  if (!mUpPending) return;
  static char body[420];
  uint8_t from = mUpFrom;
  int n = mUpLen;
  int k = snprintf(body, sizeof(body), "{\"type\":\"message\",\"from\":\"%02X\",\"t\":\"", from);
  for (int i = 0; i < n && k < (int)sizeof(body) - 80; i++)
    k += snprintf(body + k, sizeof(body) - k, "%02x%02x", mUpUnits[i] & 0xFF, mUpUnits[i] >> 8);
  mUpPending = false;
  k += snprintf(body + k, sizeof(body) - k, "\"");
  Peer* p = nullptr;
  for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used && peers[i].id == from) p = &peers[i];
  if (p && (p->latE7 != 0 || p->lonE7 != 0))
    k += snprintf(body + k, sizeof(body) - k, ",\"lat\":%ld.%07ld,\"lon\":%ld.%07ld",
                  (long)(p->latE7 / 10000000), (long)labs(p->latE7 % 10000000), (long)(p->lonE7 / 10000000), (long)labs(p->lonE7 % 10000000));
  snprintf(body + k, sizeof(body) - k, "}");
  postReport(body);
}

void loop() {
  checkWifi();
  uploadToCentral();
  uploadMessageToCentral();
  updateMessage();
  handleClient();
  printSummary();
  delay(2);
}
