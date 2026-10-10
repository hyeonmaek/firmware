// 3단계: 탭 신호 (Tap Talk)
// - 보드를 톡(1번) 치면 "OK", 톡톡(2번) 치면 "ALERT"를 블루투스로 동료에게 보낸다
// - 받은 보드는 OLED에 표시하고 LED로 알린다 (OK: LED 한 바퀴, ALERT: LED 전체 깜빡)
// - IMU(NU-IMU LSM6DS3TR-C)에 내장된 탭 감지 기능을 사용
// - 버튼 S1 = OK, S2 = ALERT (탭 대신 쓸 수 있는 예비 입력)
// - 모든 보드에 같은 코드를 올린다. IMU나 OLED가 없어도 나머지 기능은 동작
//
// 방송 데이터: [회사ID 2바이트][S][Q][보드 ID][순번][이벤트]
// - 순번이 바뀌면 새 이벤트로 판단 (같은 방송을 여러 번 받아도 한 번만 처리)

#include <bluefruit.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "hangul_font.h"   // Galmuri11 한글 글꼴 (tools/make_font.py로 생성)
#include <InternalFileSystem.h>
#include "secure.h"         // 방송 암호화(AES-128)+인증 태그+재생 방지 (맥에서 NIST 시험값 검증)

// 페어링 열쇠를 저장하지 않게(본딩 끔) 하려고 라이브러리의 private 보안 파라미터에 접근.
// → 연결할 때마다 단말 화면의 6자리를 다시 입력해야 한다.
template <typename Tag, typename Tag::type M>
struct SecParamRob { friend typename Tag::type secParamPtr(Tag) { return M; } };
struct SecParamTag { typedef ble_gap_sec_params_t BLESecurity::* type; friend type secParamPtr(SecParamTag); };
template struct SecParamRob<SecParamTag, &BLESecurity::_sec_param>;

// ---------------- 설정 ----------------
const uint16_t COMPANY_ID = 0xFFFF;  // 테스트용 ID
const uint8_t MAGIC_0 = 'S';
const uint8_t MAGIC_1 = 'Q';

const uint8_t EVENT_NONE = 0;
const uint8_t EVENT_OK = 1;              // 이상 없음
const uint8_t EVENT_SOS = 2;             // 긴급 구조 (중앙 업로드)
const uint8_t EVENT_ENEMY_FIRE = 3;      // 적 발견·사격 개시 (중앙 업로드), 겨눈 방향 포함
const uint8_t EVENT_ACK = 4;             // 메시지 수신 확인
const uint8_t EVENT_ENEMY_HOLD = 5;      // 적 발견·사격 금지 (분대만, 은밀 유지), 겨눈 방향 포함
const uint8_t EVENT_CORRECTION = 6;      // 정정: 잘못 봤음 (직전 보고 취소)
inline bool isEnemy(uint8_t e)  { return e == EVENT_ENEMY_FIRE || e == EVENT_ENEMY_HOLD; }
inline bool isUrgent(uint8_t e) { return e == EVENT_SOS || isEnemy(e); }
inline bool goesCentral(uint8_t e) { return e == EVENT_ENEMY_FIRE || e == EVENT_SOS || e == EVENT_CORRECTION; }
const unsigned long ENEMY_SHOW_MS = 60000;      // 받은 적 방향 화살표를 보여주는 시간

const unsigned long LOST_TIMEOUT_MS = 5000;     // 이 시간 동안 신호가 없으면 두절 (순간 끊김은 무시, 상황판과 같은 기준)
const bool TAP_SEND_ENABLED = false;            // 살짝 움직여도 탭으로 인식돼서 끔 → S1=OK, S2=ALERT 버튼만 사용
const unsigned long DOUBLE_TAP_WAIT_MS = 400;   // 1번 탭 후 2번째 탭을 기다리는 시간
const unsigned long EVENT_SHOW_MS = 4000;       // 받은 이벤트를 화면에 보여주는 시간

// 배터리: VBAT -[470k]- P0.02(A0) -[1M]- GND (NU-40 DK 회로도 3쪽 R13/R14)
const float BATTERY_DIVIDER = 1.47f;
const unsigned long BATTERY_READ_MS = 10000;

// 신호 세기 → 거리 (임시 기준값, 상황판 Dashboard와 같은 값. 안테나 도착 후 실측으로 교체)
const float RSSI_AT_1M = -60.0f;
const float PATH_LOSS_N = 2.5f;

const uint8_t LED_PINS[] = {PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4};

// OLED: 0.96인치 128x64, I2C 0x3C
const uint8_t OLED_ADDR = 0x3C;
const int SCREEN_WIDTH = 128;
Adafruit_SSD1306 display(SCREEN_WIDTH, 64, &Wire, -1);
bool hasDisplay = false;

// IMU: LSM6DS3TR-C, I2C 0x6A (보드 충전 칩과 주소가 같지만 0x0F 이상 레지스터만 쓰므로 문제없음)
const uint8_t IMU_ADDR = 0x6A;
const uint8_t REG_WHO_AM_I = 0x0F;
const uint8_t REG_CTRL1_XL = 0x10;
const uint8_t REG_TAP_SRC = 0x1C;
const uint8_t REG_TAP_CFG = 0x58;
const uint8_t REG_TAP_THS_6D = 0x59;
const uint8_t REG_INT_DUR2 = 0x5A;
const uint8_t REG_WAKE_UP_THS = 0x5B;
const uint8_t TAP_SRC_SINGLE = 0x20;
const uint8_t TAP_SRC_DOUBLE = 0x10;
const uint8_t REG_CTRL2_G = 0x11;
const uint8_t REG_OUTX_L_G = 0x22;   // 자이로 X,Y,Z → 이어서 0x28부터 가속도 X,Y,Z (12바이트 연속)
bool hasImu = false;

// ---------------- 상태 ----------------
uint8_t myId;
uint8_t mySeq = 0;

// 방송 보안 상태
SecCtx secCtx;                       // AES 열쇠(파생)
SecReplay secReplay;                 // 보낸이별 마지막 카운터(재생 방지)
uint32_t myCounter = 0;              // 내 방송 카운터(매 송신 증가, 재생 방지 논스)
// 카운터를 플래시에 '여기까지 써도 됨'으로 1000씩 미리 적어 둔다. 껐다 켜도 카운터가 뒤로 가지 않아야
// 동료·기지국이 내 방송을 재생 공격으로 오인해 버리지 않는다(10/11 재부팅 후 기지국 수신 끊김 사고).
uint32_t ctrReserved = 0;
const uint32_t CTR_STEP = 1000;
const uint32_t CTR_FIRST = 1000000;  // 저장 기록이 없을 때 시작값(이전 펌웨어가 쓴 값보다 확실히 큼)
void reserveCounter() {
  using namespace Adafruit_LittleFS_Namespace;
  ctrReserved = myCounter + CTR_STEP;
  InternalFS.remove("/ctr");
  File f(InternalFS);
  if (f.open("/ctr", FILE_O_WRITE)) { f.write((uint8_t*)&ctrReserved, 4); f.close(); }
}
void loadCounter() {
  using namespace Adafruit_LittleFS_Namespace;
  InternalFS.begin();
  uint32_t v = 0;
  File f(InternalFS);
  if (f.open("/ctr", FILE_O_READ)) { f.read((uint8_t*)&v, 4); f.close(); }
  myCounter = v ? v : CTR_FIRST;
  reserveCounter();
  Serial.print("CTR START "); Serial.println(myCounter);
}
uint8_t myEventSeq = 0;              // 내 이벤트 순번(새 사건일 때만 증가, 중복 처리 방지)
volatile uint32_t forgedCount = 0;   // 받은 위조 방송 수(열쇠 없는 가짜)
volatile uint32_t replayCount = 0;   // 받은 재생 방송 수(옛 방송 다시 틀기)
volatile unsigned long attackMs = 0; // 마지막 공격 감지 시각(화면 경보용)
// 공격 시연용(같은 펌웨어, 시리얼 z/x로): 내 방송을 녹음했다가 위조/재생으로 내보낸다
uint8_t attackFrame[SEC_FRAME];
bool attackActive = false;
unsigned long attackUntilMs = 0;
uint8_t replaySnap[SEC_FRAME];
bool replayArmed = false;
unsigned long replayAtMs = 0;

// ---- 중앙 → 대원 메시지 수신 (NU-87이 조각으로 방송: [FF FF]['S']['M'][번호][대상][조각<<4|조각수][UTF-16LE…]) ----
const int MSG_MAX_UNITS = 66;
const int MSG_UNITS_PER_CHUNK = 11;
const uint8_t MSG_MAGIC_1 = 'M';
volatile uint8_t rxMsgId = 0;            // 모으는 중인 메시지 번호
volatile uint8_t rxFrom = 0;             // 모으는 중인 메시지의 보낸이(0=중앙, 그 외 보드 ID)
// 이미 받은 메시지 표시((보낸이<<8)|번호). 같은 메시지가 20~60초 반복 방송되므로 한 번만 넣는다.
#define SEEN_N 8
volatile uint16_t seenKeys[SEEN_N] = {0};
int seenHead = 0;
bool seenKey(uint16_t k) { for (int i = 0; i < SEEN_N; i++) if (seenKeys[i] == k) return true; return false; }
volatile uint8_t rxChunkCount = 0;
volatile uint16_t rxMask = 0;            // 받은 조각 표시
volatile uint8_t rxLastChunkUnits = 0;
uint16_t rxUnits[MSG_MAX_UNITS];
volatile bool msgReadyPending = false;   // 콜백 → loop: 다 모였음
uint8_t ackedMsgId = 0;                  // 수신 확인한 메시지 번호 (방송에 실음)
uint8_t shownMsgId = 0;
uint16_t shownUnits[MSG_MAX_UNITS];
int shownLen = 0;
bool msgOpen = false;                    // 메시지 보기 창이 열려 있는가(S2로 열고 S1로 수신확인)
unsigned long msgShownMs = 0;

// 중앙 메시지 수신함(최근 몇 개). 도착해도 화면을 덮지 않고 안 읽음 표시만.
#define CENTRAL_RING 4
uint16_t cUnits[CENTRAL_RING][MSG_MAX_UNITS];
int cLen[CENTRAL_RING] = {0};
uint8_t cId[CENTRAL_RING] = {0};
uint8_t cFrom[CENTRAL_RING] = {0};     // 보낸이: 0=중앙, 그 외 동료 보드 ID
uint32_t cTime[CENTRAL_RING] = {0};    // 도착 시각(하루 안 초)
uint8_t shownFrom = 0;
uint32_t shownTime = 0;
int cHead = 0, cCount = 0;
uint8_t lastCentralId = 0;
bool centralUnread = false;
unsigned long msgArriveMs = 0;      // 새 메시지 도착 LED 깜빡임용
int viewIdx = -1;                   // -1=안 봄, 0=최신

// 적 발견/구조 3초 카운트다운 상태 (S4·S3 공통). 3번 누르면 취소(전송 후면 정정).
uint8_t pendBtn = 0;                // 0 없음, 4=S4(적발견), 3=S3(구조)
uint8_t pendCount = 0;
bool pendSent = false;
unsigned long pendLastMs = 0;
unsigned long pendStartMs = 0;      // 첫 누름 시각. 취소는 여기서 3초 안에만 된다
uint8_t myEvent = EVENT_NONE;

volatile float smoothedRssi = -100;
volatile unsigned long lastSeenMs = 0;
volatile bool everSeen = false;
volatile bool everSeenEvent = false;     // 동료의 이벤트 순번을 한 번이라도 봤는가(켤 때 기존 사건 무시)
volatile uint8_t peerSeq = 0;
volatile uint8_t peerEvent = EVENT_NONE;
volatile bool peerEventPending = false;  // 스캔 콜백 → loop로 넘기는 표시
volatile uint16_t peerBearing = 0;       // 상대가 보낸 적 방향(0~359)
volatile uint8_t peerId = 0;

// 여러 대 추적: 분대원 각각의 신호 세기와 마지막으로 들은 시각(연결 수·거리 표시용)
#define MAX_PEERS_TRACK 6
struct TrackPeer { uint8_t id; float rssi; unsigned long lastSeen; bool used; };
volatile TrackPeer tpeers[MAX_PEERS_TRACK];
bool peerUnread = false;                 // 동료가 보낸 알림(상태 ping 등)을 아직 안 봄

// 시계: 보드엔 RTC가 없어 millis로 센다. 시각을 넣어 주면(시리얼 T 또는 폰) 그때부터 유지.
uint32_t clockSetSec = 0;                // 설정된 기준 시각(하루 안 초 0~86399)
unsigned long clockSetMs = 0;            // 설정 당시 millis
bool clockSet = false;
uint8_t clockWeekday = 0;                // 기준 요일 0=SU..6=SA
uint8_t clockDate = 1;                   // 기준 날짜(일)

// 폰↔단말 GATT (웹앱이 크롬 Web Bluetooth로 연결). UUID 베이스 5155xxxx-4252-414E-4753-414E32303236
// (iOS 앱 BLEProtocol과 동일). 아래는 그 128bit UUID를 Bluefruit용 리틀엔디안 16바이트로.
uint8_t UUID_SVC[16]    = {0x36,0x32,0x30,0x32,0x4E,0x41,0x53,0x47,0x4E,0x41,0x52,0x42,0x01,0x00,0x55,0x51};
uint8_t UUID_GPS[16]    = {0x36,0x32,0x30,0x32,0x4E,0x41,0x53,0x47,0x4E,0x41,0x52,0x42,0x02,0x00,0x55,0x51};
uint8_t UUID_MSG[16]    = {0x36,0x32,0x30,0x32,0x4E,0x41,0x53,0x47,0x4E,0x41,0x52,0x42,0x03,0x00,0x55,0x51};
uint8_t UUID_STATUS[16] = {0x36,0x32,0x30,0x32,0x4E,0x41,0x53,0x47,0x4E,0x41,0x52,0x42,0x04,0x00,0x55,0x51};
BLEService squadSvc(UUID_SVC);
BLECharacteristic chGps(UUID_GPS);       // 폰 → 단말: [1][위도E7 4][경도E7 4][정확도][나이3]
BLECharacteristic chMsg(UUID_MSG);       // 폰 → 단말: [대상1바이트][UTF-8 텍스트]
BLECharacteristic chStatus(UUID_STATUS); // 단말 → 폰: [배터리%][연결수][최근이벤트][플래그]
volatile bool phoneConnected = false;
volatile int32_t myLatE7 = 0, myLonE7 = 0;   // 폰이 넣어준 최신 위치(위도·경도 ×1e7). 0이면 모름
volatile unsigned long gpsRxMs = 0;
char pairCode[7] = "";                       // 화면에 띄울 6자리 페어링 번호
volatile bool pairShowing = false;
volatile unsigned long pairShowMs = 0;
volatile int8_t pairResult = -1;             // 직전 페어링 결과: 1 성공, 0 실패, -1 없음
volatile unsigned long pairResultMs = 0;
uint16_t phoneUnits[MSG_MAX_UNITS];
volatile int phoneLen = 0;
volatile bool phonePending = false;
volatile uint8_t phoneTarget = 0;         // 0=동료(분대), 1=중앙
// 폰이 보낸 메시지를 분대에 조각 방송: [회사ID 2]['S']['M'][번호][대상FF][조각<<4|조각수][UTF-16LE ×≤11]
uint16_t outUnits[MSG_MAX_UNITS];
int outLen = 0, outChunks = 0, outSlot = 0;
uint8_t outId = 0x80;                  // 번호: 0x80=보드가 보냄, 0x40=중앙행, 아래 6비트=순번
uint8_t outCtr = 0;
bool outActive = false;
unsigned long outUntilMs = 0, outSlotMs = 0;

uint16_t myEnemyBearing = 0;             // 내가 보낸 적 방향
uint16_t enemyBearing = 0;               // 화면에 화살표로 보여줄 적 방향
uint8_t enemyFrom = 0;
bool enemyFire = false;          // 받은 적 발견이 사격 개시(true)/금지(false)
unsigned long enemyMs = 0;               // 0이면 표시 안 함
unsigned long needHeadingMsgMs = 0;      // S3 안 누르고 S4 눌렀을 때 안내

uint8_t shownEvent = EVENT_NONE;         // 화면에 보여주는 받은 이벤트
unsigned long shownEventMs = 0;
uint8_t lastSentEvent = EVENT_NONE;      // 화면에 보여주는 보낸 이벤트
unsigned long lastSentMs = 0;

unsigned long pendingSingleMs = 0;       // 1번 탭 후 2번째 탭 대기 중이면 그 시각

// LED 애니메이션
uint8_t ledAnim = EVENT_NONE;
unsigned long ledAnimStartMs = 0;

// ---------------- I2C 도우미 ----------------
bool readReg(uint8_t addr, uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  *value = Wire.read();
  return true;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// ST 응용 노트(AN5130)의 1번/2번 탭 감지 예시 설정
// ---------------- 방향(헤딩): 자이로로 "수평으로 몇 도 돌았는지" ----------------
// 보드를 어떻게 들든, 가속도로 중력(위쪽) 방향을 알고 자이로를 그 축으로 투영해서
// 몸이 수평으로 돈 각도만 쌓는다. 시계 방향(위에서 볼 때)이 + 방향.
//
// 자이로는 가만히 있어도 조금 돈다고 읽는 치우침(bias)이 있고, 온도 등으로 변한다.
// 치우침은 "연속한 두 1초 창의 평균이 거의 같고 흔들림(표준편차)도 작다"로 가만히 있음을 판단해 자동으로 배운다.
//  - 값 크기(|값-치우침|)로 판단하면 치우침이 한 번 크게 틀어졌을 때 영영 정지로 인정되지 않아 드리프트가 갇힌다.
//  - 최대-최소 폭으로 판단하면 실측 잡음·진동(1초에 1.4~14°/s 폭)을 못 견딘다. 진동은 평균이 0이라 평균으로 본다.
// 가만히 있던 1초 동안 쌓인 각도는 드리프트이므로 되돌린다.
const float GYRO_DPS_PER_LSB = 0.0175f;     // ±500dps 설정
const unsigned long STILL_WINDOW_MS = 1000;
const float STILL_STD_DPS = 2.0f;           // 1초 창의 축별 표준편차가 이보다 작고
const float STILL_MEAN_DIFF_DPS = 0.4f;     // 직전 창 평균과의 차이도 이보다 작으면 가만히 있음
const uint8_t REG_CTRL3_C = 0x12;

float gyroBias[3] = {0, 0, 0};
bool biasLearned = false;                   // 치우침을 한 번이라도 배웠는지
float gravity[3] = {0, 0, 1};
float heading = 0;                          // 0~360, S3 누른 순간이 0
bool headingReady = false;
unsigned long zeroDoneMs = 0;              // 방향 기준을 맞춘 시각(확인 화면용)
bool zeroPending = false;                   // S3 눌렀는데 치우침을 아직 못 배워서 기다리는 중

float winSum[3], winSumSq[3];               // 1초 창의 통계
float prevWinMean[3];
bool havePrevWinMean = false;
int winCount = 0;
unsigned long winStartMs = 0;
float winHeadingDelta = 0;                  // 이번 창 동안 쌓인 각도 변화
float lastWinStd[3] = {0, 0, 0};            // 진단용: 직전 창의 축별 표준편차
float lastWinMeanDiff[3] = {0, 0, 0};       // 진단용: 직전 창과 평균 차이
int lastWinCount = 0;

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void setupGyro() {
  writeReg(IMU_ADDR, REG_CTRL3_C, 0x44);    // BDU(상·하위 바이트가 다른 측정값으로 섞이지 않게) + 주소 자동 증가
  writeReg(IMU_ADDR, REG_CTRL2_G, 0x44);    // 자이로 104Hz, ±500dps
}

void resetStillWindow() {
  for (int i = 0; i < 3; i++) {
    winSum[i] = 0;
    winSumSq[i] = 0;
  }
  winCount = 0;
  winStartMs = millis();
  winHeadingDelta = 0;
}

// 방향 기억: 가만히 있을 때 지금 방향을 플래시에 적어 둔다. 껐다 켜거나 펌웨어를 새로 올려도
// 그동안 단말을 돌리지 않았다면 켜자마자 그 방향 그대로 이어서 쓴다(진북 다시 맞출 필요 없음).
float savedHeading = -1000;
unsigned long headingSaveMs = 0;
void saveHeading() {
  using namespace Adafruit_LittleFS_Namespace;
  float h = fmodf(heading + 3600.0f, 360.0f);
  InternalFS.remove("/hdg");
  File f(InternalFS);
  if (f.open("/hdg", FILE_O_WRITE)) { f.write((uint8_t*)&h, 4); f.close(); }
  savedHeading = heading;
  headingSaveMs = millis();
}
void loadHeading() {
  using namespace Adafruit_LittleFS_Namespace;
  float h;
  File f(InternalFS);
  if (f.open("/hdg", FILE_O_READ)) {
    if (f.read((uint8_t*)&h, 4) == 4 && h >= 0 && h < 360) {
      heading = h; savedHeading = h; headingReady = true;
      Serial.print("HEADING RESTORED "); Serial.println(h);
    }
    f.close();
  }
}

void zeroHeading() {
  heading = 0;
  headingReady = true;
  zeroPending = false;
  zeroDoneMs = millis();
  resetStillWindow();
  saveHeading();
  Serial.println("HEADING ZERO");
}

// S3: 지금 방향을 0°로. 치우침을 아직 못 배웠으면 가만히 1초 있을 때까지 기다렸다가 0°로.
void requestHeadingZero() {
  if (biasLearned) zeroHeading();
  else zeroPending = true;
}

// 진단: 노트북에서 'r'을 보내면 자이로·가속도 원본(LSB) 60개를 그대로 출력
void dumpRawImu() {
  Serial.println("RAW gx gy gz ax ay az");
  for (int n = 0; n < 60; n++) {
    uint8_t b[12];
    if (readRegs(IMU_ADDR, REG_OUTX_L_G, b, 12)) {
      Serial.print("RAW");
      for (int i = 0; i < 6; i++) {
        Serial.print(" ");
        Serial.print((int16_t)(b[i * 2] | (b[i * 2 + 1] << 8)));
      }
      Serial.println();
    } else {
      Serial.println("RAW READ FAIL");
    }
    delay(10);
  }
}

// IMU 자가 복구: 2초마다 확인해서, 센서가 안 보이거나(선 접촉) 센서가 혼자 리셋돼 자이로가 꺼져 있으면
// 다시 설정한다. (10/11 E-2: 재부팅 직후 화살표가 안 돌던 현상 — 다시 켜니 정상)
void checkImu() {
  static unsigned long lastMs = 0;
  if (millis() - lastMs < 2000) return;
  lastMs = millis();
  uint8_t g = 0;
  bool ok = readReg(IMU_ADDR, REG_CTRL2_G, &g) && g == 0x44;
  if (ok && hasImu) return;
  bool was = hasImu;
  hasImu = setupImuTap();
  Serial.print("IMU RECOVER "); Serial.print(was ? "reconfig " : "found ");
  Serial.println(hasImu ? "ok" : "fail");
}

void updateHeading() {
  checkImu();
  if (!hasImu) return;
  static unsigned long lastUs = 0;
  uint8_t b[12];
  if (!readRegs(IMU_ADDR, REG_OUTX_L_G, b, 12)) return;
  float g[3], a[3];
  for (int i = 0; i < 3; i++) {
    g[i] = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8)) * GYRO_DPS_PER_LSB;
    a[i] = (int16_t)(b[6 + i * 2] | (b[6 + i * 2 + 1] << 8));
  }
  unsigned long nowUs = micros();
  float dt = lastUs == 0 ? 0 : (nowUs - lastUs) / 1e6f;
  lastUs = nowUs;
  if (winStartMs == 0) resetStillWindow();

  // 중력 방향은 천천히 따라간다(흔들림에 덜 민감하게)
  float norm = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (norm > 1) {
    for (int i = 0; i < 3; i++) gravity[i] = gravity[i] * 0.95f + (a[i] / norm) * 0.05f;
  }
  float gn = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);

  // 1초 창 통계
  for (int i = 0; i < 3; i++) {
    winSum[i] += g[i];
    winSumSq[i] += g[i] * g[i];
  }
  winCount++;

  // 각도 적분 (치우침을 배운 뒤부터)
  if (biasLearned && gn > 0.5f && dt > 0 && dt < 0.2f) {
    float yawRate = 0;   // 위쪽 축 기준 회전 속도 (반시계 +)
    for (int i = 0; i < 3; i++) yawRate += (g[i] - gyroBias[i]) * gravity[i] / gn;
    float delta = -yawRate * dt;   // 시계 방향을 +로
    heading += delta;
    winHeadingDelta += delta;
  }

  // 창이 끝나면: 가만히 있었는지 판단 → 치우침 학습 + 그동안 쌓인 드리프트 되돌리기
  if (millis() - winStartMs >= STILL_WINDOW_MS) {
    bool enough = winCount >= 20;
    bool still = enough && havePrevWinMean;
    float mean[3];
    lastWinCount = winCount;
    for (int i = 0; i < 3; i++) {
      if (!enough) break;
      mean[i] = winSum[i] / winCount;
      float var = winSumSq[i] / winCount - mean[i] * mean[i];
      lastWinStd[i] = sqrtf(var > 0 ? var : 0);
      lastWinMeanDiff[i] = havePrevWinMean ? fabsf(mean[i] - prevWinMean[i]) : 99;
      if (lastWinStd[i] > STILL_STD_DPS || lastWinMeanDiff[i] > STILL_MEAN_DIFF_DPS) still = false;
    }
    if (enough) {
      for (int i = 0; i < 3; i++) prevWinMean[i] = mean[i];
      havePrevWinMean = true;
    }
    if (still) {
      for (int i = 0; i < 3; i++) {
        gyroBias[i] = biasLearned ? gyroBias[i] * 0.7f + mean[i] * 0.3f : mean[i];
      }
      biasLearned = true;
      heading -= winHeadingDelta;   // 가만히 있었으니 이 1초 동안의 변화는 전부 드리프트
      // 가만히 있고 방향이 3° 넘게 바뀌었으면 기억(플래시 닳지 않게 최소 5초 간격)
      if (headingReady && fabsf(heading - savedHeading) > 3 && millis() - headingSaveMs > 5000) saveHeading();
    }
    resetStillWindow();
    if (zeroPending && biasLearned) zeroHeading();
  }

  while (heading < 0) heading += 360;
  while (heading >= 360) heading -= 360;
}

bool setupImuTap() {
  uint8_t who = 0;
  if (!readReg(IMU_ADDR, REG_WHO_AM_I, &who) || who != 0x6A) return false;
  writeReg(IMU_ADDR, REG_CTRL1_XL, 0x60);     // 가속도 416Hz, ±2g
  writeReg(IMU_ADDR, REG_TAP_CFG, 0x8F);      // 탭 감지 켜기, X/Y/Z 축, 결과 유지(읽으면 지워짐)
  writeReg(IMU_ADDR, REG_TAP_THS_6D, 0x8C);   // 탭 세기 기준
  writeReg(IMU_ADDR, REG_INT_DUR2, 0x7F);     // 탭 길이, 조용한 시간, 2번 탭 간격
  writeReg(IMU_ADDR, REG_WAKE_UP_THS, 0x80);  // 1번 탭 + 2번 탭 둘 다 감지
  setupGyro();
  return true;
}

// ---------------- 블루투스 ----------------
void scanCallback(ble_gap_evt_adv_report_t* report) {
  uint8_t data[31];
  uint8_t len = Bluefruit.Scanner.parseReportByType(
      report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, data, sizeof(data));

  // 중앙 메시지 조각
  if (len >= 9 && data[0] == (COMPANY_ID & 0xFF) && data[1] == (COMPANY_ID >> 8) &&
      data[2] == MAGIC_0 && data[3] == MSG_MAGIC_1) {
    // 번호 0x80 이상 = 동료 보드가 보낸 것(5번째 바이트 = 보낸이), 그중 0x40이 서 있으면 중앙행(기지국만 받음).
    // 0x80 미만 = 기지국(중앙)이 보낸 것(5번째 바이트 = 대상, FF=전체).
    uint8_t id = data[4], b5 = data[5];
    uint8_t idx = data[6] >> 4, count = data[6] & 0x0F;
    uint8_t units = (len - 7) / 2;
    bool fromBoard = id & 0x80;
    uint8_t from = fromBoard ? b5 : 0;
    bool forMe = fromBoard ? (b5 != myId && !(id & 0x40)) : (b5 == 0xFF || b5 == myId);
    if (forMe && count > 0 && idx < count && count * MSG_UNITS_PER_CHUNK <= MSG_MAX_UNITS &&
        !seenKey(((uint16_t)from << 8) | id)) {
      if (id != rxMsgId || from != rxFrom || count != rxChunkCount) {   // 새 메시지면 처음부터 모은다
        rxMsgId = id;
        rxFrom = from;
        rxChunkCount = count;
        rxMask = 0;
      }
      for (uint8_t i = 0; i < units && i < MSG_UNITS_PER_CHUNK; i++) {
        rxUnits[idx * MSG_UNITS_PER_CHUNK + i] = data[7 + i * 2] | (data[8 + i * 2] << 8);
      }
      if (idx == count - 1) rxLastChunkUnits = units;
      rxMask |= (1 << idx);
      if (rxMask == (1 << count) - 1) msgReadyPending = true;
    }
    Bluefruit.Scanner.resume();
    return;
  }

  // 암호화된 동료 방송: [회사ID 2][버전 0x02][보낸이][카운터 3][암호문 6][태그 4]
  if (len >= SEC_FRAME && data[0] == (COMPANY_ID & 0xFF) &&
      data[1] == (COMPANY_ID >> 8) && data[2] == SEC_VER) {
    uint8_t sid; uint32_t sctr; uint8_t content[SEC_CONTENT];
    int res = sec_open(&secCtx, &secReplay, COMPANY_ID, data, len, &sid, &sctr, content);

    if (res == SEC_FORGED) { forgedCount++; attackMs = millis(); }       // 열쇠 없는 가짜
    else if (res == SEC_REPLAY) { replayCount++; attackMs = millis(); }  // 옛 방송 재생
    else if ((res == SEC_OK || res == SEC_DUP) && sid != myId) {
      // 정상(OK) 또는 같은 방송의 반복(DUP) → 둘 다 진짜 동료 신호: 거리·연결 갱신
      if (!everSeen) { smoothedRssi = report->rssi; everSeen = true; }
      else smoothedRssi = smoothedRssi * 0.8f + report->rssi * 0.2f;
      lastSeenMs = millis();
      peerId = sid;
      int ti = findTrackPeer(sid);            // 이 분대원의 신호 세기·생존 갱신
      if (ti >= 0) {
        tpeers[ti].rssi = tpeers[ti].lastSeen == 0 ? report->rssi
                        : tpeers[ti].rssi * 0.8f + report->rssi * 0.2f;
        tpeers[ti].lastSeen = millis();
      }
      if (res == SEC_OK) {
        uint8_t event = content[0];
        uint8_t evSeq = content[1];
        peerBearing = content[3] | (content[4] << 8);
        if (!everSeenEvent) { peerSeq = evSeq; everSeenEvent = true; }  // 켜질 때 이미 방송 중이던 사건은 무시
        else if (evSeq != peerSeq && event != EVENT_NONE) {
          peerSeq = evSeq;
          peerEvent = event;
          peerEventPending = true;
        }
      }
    }
    Bluefruit.Scanner.resume();
    return;
  }

  Bluefruit.Scanner.resume();
}

uint8_t batteryPercent = 0;   // 방송에 실어 보내는 배터리 잔량(%)
float batteryVolts = 0;

float readBatteryVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(A0);
  return (sum / 8.0f) * 3.0f / 4095.0f * BATTERY_DIVIDER;
}

// 리튬폴리머 전압 → 잔량(%) 대략값
uint8_t voltsToPercent(float v) {
  const float volts[] = {3.30f, 3.50f, 3.65f, 3.75f, 3.85f, 4.00f, 4.20f};
  const float pct[]   = {0,     5,     20,    40,    60,    80,    100};
  if (v <= volts[0]) return 0;
  for (int i = 1; i < 7; i++) {
    if (v <= volts[i]) {
      return (uint8_t)(pct[i - 1] + (v - volts[i - 1]) / (volts[i] - volts[i - 1]) * (pct[i] - pct[i - 1]));
    }
  }
  return 100;
}

// 제조사 데이터로 임의 바이트를 방송(정상 암호 프레임·공격 프레임 공용)
void advertiseBytes(const uint8_t* buf, uint8_t len, bool withFlags = true) {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  if (withFlags) Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addManufacturerData(buf, len);
  // 폰이 안 붙어 있으면 연결형(폰이 이 광고로 연결), 붙어 있으면 연결 안 받는 방송만 계속
  // → 폰과 1:1 연결 중에도 분대 방송(1:N)은 끊기지 않는다.
  // (라이브러리가 스캔응답을 항상 같이 싣기 때문에 '스캔 안 받는' 형식은 시작 자체가 실패함 → 스캔은 받는 형식)
  Bluefruit.Advertising.setType(phoneConnected ? BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED
                                               : BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(48, 48);    // 30ms마다 방송 (행사장이 붐벼서 NU-87이 들을 기회를 늘림)
  if (!Bluefruit.Advertising.start(0)) Serial.println("ADV START FAIL");
}

void updateAdvertising() {
  if (attackActive || outActive) return;    // 공격 시연·메시지 조각 방송 중에는 덮지 않는다
  // 평문 내용 14바이트: [이벤트][순번][배터리][방향lo][방향hi][확인번호][위도E7 4][경도E7 4]
  int32_t la = myLatE7, lo = myLonE7;   // 폰 GPS(없으면 0)
  uint8_t content[SEC_CONTENT] = {
      myEvent, myEventSeq, batteryPercent,
      (uint8_t)(myEnemyBearing & 0xFF), (uint8_t)(myEnemyBearing >> 8), ackedMsgId,
      (uint8_t)la, (uint8_t)(la >> 8), (uint8_t)(la >> 16), (uint8_t)(la >> 24),
      (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24)};
  myCounter++;                 // 매 송신마다 카운터 증가(키스트림 재사용 금지·재생 방지)
  if (myCounter >= ctrReserved) reserveCounter();
  uint8_t frame[SEC_FRAME];
  sec_seal(&secCtx, COMPANY_ID, myId, myCounter, content, frame);
  memcpy(attackFrame, frame, SEC_FRAME);   // 시연용: 방금 내 방송을 기억(재생 공격 재료)
  advertiseBytes(frame, SEC_FRAME);
}

// 시연: 위조 방송. 열쇠 없는 공격자가 'OPEN FIRE'를 꾸며 보낸 척 — 내용·태그를 아무 값이나 넣는다.
// 받는 보드는 태그가 안 맞아 "위조 거부"로 센다. (2초 뒤 정상 방송 복귀)
void startForgeAttack() {
  uint8_t f[SEC_FRAME];
  f[0] = COMPANY_ID & 0xFF; f[1] = COMPANY_ID >> 8; f[2] = SEC_VER; f[3] = myId;
  uint32_t fake = millis();
  f[4] = fake; f[5] = fake >> 8; f[6] = fake >> 16;
  for (int i = 7; i < SEC_FRAME; i++) f[i] = (uint8_t)(fake * (i + 7) + 0x5A);  // 가짜 내용+태그
  memcpy(attackFrame, f, SEC_FRAME);
  attackActive = true;
  attackUntilMs = millis() + 2000;
  advertiseBytes(attackFrame, SEC_FRAME);
  Serial.println("[공격] 위조 방송 2초 (열쇠 없이 가짜 명령) → 상대 보드: 위조 거부");
}

// 시연: 재생 공격. 지금 내 진짜 방송을 녹음하고, 5초 뒤 그대로 다시 내보낸다.
// 그 사이 카운터가 올라가므로, 받는 보드는 "옛 방송"으로 보고 "재생 거부"로 센다.
void armReplayAttack() {
  memcpy(replaySnap, attackFrame, SEC_FRAME);   // 지금 방송 스냅샷
  replayArmed = true;
  replayAtMs = millis() + 5000;
  Serial.println("[공격] 지금 방송 녹음 완료 → 5초 뒤 재생합니다 (상대 보드: 재생 거부)");
}

void startScanning() {
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.filterMSD(COMPANY_ID);
  Bluefruit.Scanner.setInterval(160, 160);   // 쉬지 않고 듣기 (NU-40은 와이파이가 없어서 손해 없음)
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.start(0);
}

void sendEvent(uint8_t event) {
  myEventSeq++;              // 새 사건 번호(수신자가 같은 사건을 한 번만 처리). 암호 카운터는 updateAdvertising에서 따로 증가
  myEvent = event;
  if (isEnemy(event)) myEnemyBearing = (uint16_t)heading % 360;
  updateAdvertising();
  lastSentEvent = event;
  lastSentMs = millis();
  Serial.print("TX ");
  Serial.print(eventName(event));
  if (isEnemy(event)) {
    Serial.print(" BRG=");
    Serial.print(myEnemyBearing);
  }
  if (goesCentral(event)) Serial.print(" ->CENTRAL");
  Serial.println();
}

// ---------------- 표시 ----------------
const char* eventName(uint8_t event) {
  switch (event) {
    case EVENT_ENEMY_FIRE: return "E-FIRE";
    case EVENT_ENEMY_HOLD: return "E-HOLD";
    case EVENT_SOS:        return "SOS";
    case EVENT_CORRECTION: return "CANCEL";
    case EVENT_ACK:        return "ACK";
    default:               return "OK";
  }
}

void setLeds(uint8_t mask) {
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(LED_PINS[i], (mask >> i) & 1 ? LED_STATE_ON : !LED_STATE_ON);
  }
}

void updateLeds(bool lost) {
  unsigned long t = millis() - ledAnimStartMs;

  if (ledAnim == EVENT_OK && t < 800) {
    setLeds(1 << (t / 200));                 // LED 1→2→3→4 한 바퀴
    return;
  }
  if (isUrgent(ledAnim) && t < 1800) {
    setLeds((t / 150) % 2 == 0 ? 0x0F : 0);  // 전체 빠르게 깜빡
    return;
  }
  ledAnim = EVENT_NONE;

  if (msgArriveMs != 0 && millis() - msgArriveMs < 1500) {   // 새 메시지 도착: LED4 빠르게 깜빡
    setLeds((millis() % 300) < 150 ? 0x08 : 0);
    return;
  }
  if (msgOpen) {
    setLeds((millis() % 1000) < 500 ? 0x08 : 0);   // 메시지 수신 확인 기다리는 중: LED4 깜빡
    return;
  }

  if (pairResult == 0 && millis() - pairResultMs < 1500) {   // 페어링 실패: 전체 빠르게 깜빡
    setLeds((millis() / 120) % 2 ? 0x0F : 0);
    return;
  }
  if (millis() - lastSentMs < 150) {
    setLeds(0x01);                           // 보냈다는 짧은 표시
  } else if (lost) {
    setLeds((millis() % 1000) < 100 ? 0x01 : 0);  // 동료 못 찾음: LED1 짧게 깜빡
  } else {
    setLeds(0);
  }
}

// 화면 구석에 보여 줄 내 이름 (보드 ID → 호출부호, 상황판의 독수리-1/2와 같은 번호)
const char* callsignOf(uint8_t id) {
  static char buf[8];
  if (id == 0xAD) return "E-1";
  if (id == 0xFA) return "E-2";
  snprintf(buf, sizeof(buf), "ID%02X", id);
  return buf;
}

const char* myCallsign() {
  return callsignOf(myId);
}

// 적 발견 수신 화면: 딱 3가지 — 사격개시/사격금지, 적 방향 화살표, 적까지 거리.
// 화살표 위쪽 = 내가 겨누는 앞. 내가 돌면 (적각도 - 내각도)만큼 돌아 적을 계속 가리킨다.
void drawEnemy() {
  // 윗줄 가운데: 사격개시 / 사격금지 (한글)
  static const uint16_t fire[] = {0xC0AC, 0xACA9, 0xAC1C, 0xC2DC};  // 사격개시
  static const uint16_t hold[] = {0xC0AC, 0xACA9, 0xAE08, 0xC9C0};  // 사격금지
  const uint16_t* lab = enemyFire ? fire : hold;      // 왼쪽 위
  int lx = 2;
  for (int i = 0; i < 4; i++) lx += drawGlyph(lx, 11, lab[i]);

  // 오른쪽: 큰 화살표. 내가 돌면 자이로 heading이 바뀌어 화살표가 적을 계속 가리킨다.
  // (영점 전에도 heading은 돌아가므로 항상 heading 사용. S1 길게는 절대 기준 보정용)
  const int cx = 98, cy = 37, r = 23;
  float h = heading;
  float rel = (enemyBearing - h) * (float)M_PI / 180.0f;
  float sx = sinf(rel), cyv = cosf(rel);
  int tipX = cx + (int)(r * sx), tipY = cy - (int)(r * cyv);
  int tailX = cx - (int)(r * 0.85f * sx), tailY = cy + (int)(r * 0.85f * cyv);
  for (int o = -2; o <= 2; o++) {                     // 굵은 몸통
    display.drawLine(tailX + o, tailY, tipX + o, tipY, SSD1306_WHITE);
    display.drawLine(tailX, tailY + o, tipX, tipY + o, SSD1306_WHITE);
  }
  float bx = tipX - 11 * sx, by = tipY + 11 * cyv;    // 큰 화살촉
  display.fillTriangle(tipX, tipY,
                       (int)(bx + 8 * cyv), (int)(by + 8 * sx),
                       (int)(bx - 8 * cyv), (int)(by - 8 * sx), SSD1306_WHITE);
  display.drawCircle(cx, cy, r + 3, SSD1306_WHITE);

  // 왼쪽: 적 발견한 동료까지의 대략 거리 (숫자 크게)
  float dm = peerMeters(enemyFrom);
  if (dm > 0) {
    char num[8];
    if (dm < 10.0f) snprintf(num, sizeof(num), "%.1f", dm);
    else snprintf(num, sizeof(num), "%d", (int)(dm + 0.5f));
    display.setTextSize(3);
    display.setCursor(2, 28);
    display.print(num);
    int mx = 2 + (int)strlen(num) * 18;               // 'm'은 작게 뒤에
    display.setTextSize(2);
    display.setCursor(mx + 2, 35);
    display.print("m");
  }
}

float rssiToMeters(float rssi) {
  return powf(10.0f, (RSSI_AT_1M - rssi) / (10.0f * PATH_LOSS_N));
}

// 여러 대 추적: id의 칸 번호를 찾거나(없으면 새로 만들어) 반환, 자리 없으면 -1
// (포인터 대신 번호 반환 — 아두이노 자동 프로토타입이 사용자 구조체 반환형을 못 다루는 문제 회피)
int findTrackPeer(uint8_t id) {
  for (int i = 0; i < MAX_PEERS_TRACK; i++)
    if (tpeers[i].used && tpeers[i].id == id) return i;
  for (int i = 0; i < MAX_PEERS_TRACK; i++)
    if (!tpeers[i].used) { tpeers[i].used = true; tpeers[i].id = id; tpeers[i].rssi = -100; tpeers[i].lastSeen = 0; return i; }
  return -1;
}

// 최근에 들은(살아 있는) 분대원 수
int connectedCount() {
  int n = 0;
  for (int i = 0; i < MAX_PEERS_TRACK; i++)
    if (tpeers[i].used && millis() - tpeers[i].lastSeen < LOST_TIMEOUT_MS) n++;
  return n;
}

// 특정 분대원까지의 대략 거리(m). 모르면 -1
float peerMeters(uint8_t id) {
  for (int i = 0; i < MAX_PEERS_TRACK; i++)
    if (tpeers[i].used && tpeers[i].id == id && millis() - tpeers[i].lastSeen < LOST_TIMEOUT_MS)
      return rssiToMeters(tpeers[i].rssi);
  return -1;
}

// 기준 시각부터 흐른 총 초(날짜 계산용). 설정 전엔 켠 뒤 경과 시간.
uint32_t totalSeconds() {
  return (clockSet ? clockSetSec : 0) + (millis() - clockSetMs) / 1000;
}
uint32_t nowSeconds() { return totalSeconds() % 86400UL; }   // 하루 안 초

void drawDistance() {
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("DISTANCE (approx)");

  // 표시 전용으로 한 번 더 천천히 누른다(숫자가 가만히 있을 때 덜 출렁이게)
  static float dispM = -1;
  float m = rssiToMeters(smoothedRssi);
  dispM = (dispM < 0) ? m : dispM * 0.85f + m * 0.15f;

  char text[12];
  if (dispM < 10.0f) snprintf(text, sizeof(text), "%.1f m", dispM);
  else snprintf(text, sizeof(text), "%d m", (int)(dispM + 0.5f));
  display.setTextSize(3);
  display.setCursor(0, 26);
  display.print(text);
}

// 진단: 노트북에서 "m<UTF-16LE 16진수>\n"을 보내면 NU-87 없이 메시지 화면을 시험한다 (번호 250)
void injectTestMessage() {
  String hex = Serial.readStringUntil('\n');
  hex.trim();
  int n = hex.length() / 4;
  if (n > MSG_MAX_UNITS) n = MSG_MAX_UNITS;
  for (int i = 0; i < n; i++) {
    uint16_t lo = strtol(hex.substring(i * 4, i * 4 + 2).c_str(), NULL, 16);
    uint16_t hi = strtol(hex.substring(i * 4 + 2, i * 4 + 4).c_str(), NULL, 16);
    shownUnits[i] = lo | (hi << 8);
  }
  shownLen = n;
  shownMsgId = 250;
  shownFrom = 0;
  shownTime = totalSeconds() % 86400UL;
  msgOpen = true;
  msgShownMs = millis();
  Serial.print("MSG TEST units=");
  Serial.println(n);
}

// ---------------- 한글 메시지 그리기 ----------------
const int MSG_TOP = 13;          // 첫 줄 위쪽(그 위는 보낸이 줄)
const int MSG_LINE_H = 13;       // 줄 간격 (글자 11 + 여유 2)
const int MSG_LINES = 4;         // 화면에 보이는 줄 수
const unsigned long MSG_SCROLL_MS = 2500;

int findGlyph(uint16_t cp) {
  int lo = 0, hi = HFONT_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (HFONT_CODES[mid] == cp) return mid;
    if (HFONT_CODES[mid] < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

int glyphAdvance(uint16_t cp) {
  int g = findGlyph(cp);
  if (g < 0) g = findGlyph('?');
  return g < 0 ? 6 : HFONT_GLYPHS[g].dwidth;
}

// (x, 기준선)에 글자 하나를 그리고 다음 글자까지의 너비를 돌려준다
int drawGlyph(int x, int baseline, uint16_t cp) {
  int g = findGlyph(cp);
  if (g < 0) g = findGlyph('?');
  if (g < 0) return 6;
  const HGlyph& gl = HFONT_GLYPHS[g];
  const uint8_t* bits = HFONT_BITMAP + gl.offset;
  int rowBytes = (gl.w + 7) / 8;
  int top = baseline - gl.yoff - gl.h;
  for (int r = 0; r < gl.h; r++) {
    for (int c = 0; c < gl.w; c++) {
      if (bits[r * rowBytes + c / 8] & (0x80 >> (c % 8))) {
        display.drawPixel(x + gl.xoff + c, top + r, SSD1306_WHITE);
      }
    }
  }
  return gl.dwidth;
}

// 코드포인트 배열을 한 줄로 그린다(한글·숫자 섞어 쓰기용)
void drawCps(int x, int baseline, const uint16_t* cps, int n) {
  for (int i = 0; i < n; i++) x += drawGlyph(x, baseline, cps[i]);
}

// 공격 감지 경보: 최근에 위조·재생을 받으면 크게 띄운다(발표 하이라이트)
void drawAttackBanner() {
  display.fillRect(0, 13, SCREEN_WIDTH, 24, SSD1306_WHITE);        // 반전 배너
  display.setTextColor(SSD1306_BLACK);
  const uint16_t title[] = {0xACF5, 0xACA9, 0xAC10, 0xC9C0};       // "공격감지"
  drawCps(4, 29, title, 4);
  char buf[24];
  snprintf(buf, sizeof(buf), "F%lu R%lu", (unsigned long)forgedCount, (unsigned long)replayCount);  // 위조·재생 누적
  display.setTextSize(1);
  display.setCursor(80, 22);
  display.print(buf);
  display.setTextColor(SSD1306_WHITE);
}

// 화면 너비(128)에 맞춰 줄을 나눈다. lineStart[i] = i번째 줄이 시작하는 글자 위치
// 가능하면 띄어쓰기에서 줄을 바꾸고, 한 단어가 한 줄보다 길면 글자 단위로 자른다.
int wrapMessage(int* lineStart, int maxLines) {
  int lines = 0, x = 0, lastSpace = -1;
  int i = 0;
  while (i < shownLen) {
    if (x == 0) {
      if (shownUnits[i] == ' ' && lines > 0) { i++; continue; }   // 줄 맨 앞 공백은 건너뜀
      if (lines >= maxLines) break;
      lineStart[lines++] = i;
      lastSpace = -1;
    }
    int w = glyphAdvance(shownUnits[i]);
    if (x + w > SCREEN_WIDTH) {
      if (lastSpace > lineStart[lines - 1]) i = lastSpace + 1;    // 마지막 띄어쓰기 뒤부터 다음 줄
      x = 0;
      continue;
    }
    if (shownUnits[i] == ' ') lastSpace = i;
    x += w;
    i++;
  }
  return lines;
}

void drawMessage() {
  // 윗줄: 보낸이(중앙 / E-1·E-2) + 도착 시각
  if (shownFrom == 0) {
    const uint16_t c2[] = {0xC911, 0xC559};   // "중앙"
    drawCps(0, 11, c2, 2);
  } else {
    const char* cs = callsignOf(shownFrom);
    int x = 0;
    for (int i = 0; cs[i]; i++) x += drawGlyph(x, 11, (uint8_t)cs[i]);
  }
  char tm[6];
  snprintf(tm, sizeof(tm), "%02lu:%02lu", (unsigned long)(shownTime / 3600), (unsigned long)((shownTime % 3600) / 60));
  display.setTextSize(1);
  display.setCursor(SCREEN_WIDTH - 5 * 6, 2);
  display.print(tm);
  display.drawLine(0, 13, SCREEN_WIDTH, 13, SSD1306_WHITE);

  int lineStart[MSG_MAX_UNITS];
  int lines = wrapMessage(lineStart, MSG_MAX_UNITS);
  // 4줄보다 길면 2.5초마다 한 줄씩 올린다(끝까지 가면 처음으로)
  int first = 0;
  if (lines > MSG_LINES) {
    int steps = lines - MSG_LINES + 2;   // 끝에서 한 번 더 머무름
    first = ((millis() - msgShownMs) / MSG_SCROLL_MS) % steps;
    if (first > lines - MSG_LINES) first = lines - MSG_LINES;
  }
  for (int l = 0; l < MSG_LINES && first + l < lines; l++) {
    int start = lineStart[first + l];
    int end = (first + l + 1 < lines) ? lineStart[first + l + 1] : shownLen;
    int x = 0, baseline = MSG_TOP + l * MSG_LINE_H + 11;
    for (int i = start; i < end; i++) x += drawGlyph(x, baseline, shownUnits[i]);
  }
}

// 신호 세기 → 거리 구간 (숫자는 15dB 이상 흔들려 못 믿으므로 구간으로)
void drawRange() {
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("RANGE");
  int r = (int)smoothedRssi;
  const char* b;
  if (r > -48)      b = "CONTACT";    // 거의 붙음
  else if (r > -62) b = "NEAR";       // 1m 안팎
  else if (r > -74) b = "MID";        // 수 m
  else if (r > -84) b = "FAR";        // 멀다
  else              b = "VERY FAR";
  display.setTextSize(2);
  display.setCursor(0, 28);
  display.print(b);
}

// 작은 알림 벨(외부/중앙 알림 도착 표시). 왼쪽위 (x,y)에 6x7 정도로 그린다.
void drawBell(int x, int y) {
  display.drawLine(x + 1, y, x + 4, y, SSD1306_WHITE);       // 윗머리
  display.drawPixel(x, y + 1, SSD1306_WHITE);
  display.drawPixel(x + 5, y + 1, SSD1306_WHITE);
  display.drawLine(x, y + 2, x, y + 4, SSD1306_WHITE);       // 몸통 양옆
  display.drawLine(x + 5, y + 2, x + 5, y + 4, SSD1306_WHITE);
  display.drawLine(x - 1, y + 5, x + 6, y + 5, SSD1306_WHITE); // 아랫단
  display.drawPixel(x + 2, y + 6, SSD1306_WHITE);            // 추
  display.drawPixel(x + 3, y + 6, SSD1306_WHITE);
}

// 평소 워치페이스: 카시오 F-91W 느낌. 윗줄 AM/PM·알림벨·요일·날짜, 아랫줄 큰 시:분 + 작은 초.
void drawWatchFace(bool lost) {
  (void)lost;
  uint32_t total = totalSeconds();
  uint32_t s = total % 86400UL;
  uint32_t days = total / 86400UL;
  int hh24 = s / 3600, mm = (s % 3600) / 60, ss = s % 60;
  static const char* WD[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  int wd = (clockWeekday + days) % 7;
  int date = clockDate + (int)days;      // 데모용(월 넘김은 다루지 않음)

  // 윗줄(작은 글씨): 기기 이름 · 알림벨 · 요일 · 날짜
  display.setTextSize(1);
  display.setCursor(2, 1);
  display.print(myCallsign());
  if (centralUnread) drawBell(34, 1);    // 외부(중앙) 알림 도착
  display.setCursor((SCREEN_WIDTH - 2 * 6) / 2, 1);   // 요일 가운데 정렬
  display.print(WD[wd]);
  char ds[4];
  snprintf(ds, sizeof(ds), "%02d", date);
  display.setCursor(SCREEN_WIDTH - 2 * 6 - 2, 1);
  display.print(ds);
  display.drawLine(0, 11, SCREEN_WIDTH, 11, SSD1306_WHITE);  // 구분선

  // 아랫줄: 큰 시:분(24시간제) + 작은 초
  char hm[6];
  snprintf(hm, sizeof(hm), "%02d:%02d", hh24, mm);
  display.setTextSize(3);
  display.setCursor(4, 26);
  display.print(hm);
  char se[3];
  snprintf(se, sizeof(se), "%02d", ss);
  display.setTextSize(2);
  display.setCursor(99, 40);
  display.print(se);

  // 왼쪽 아래: 배터리 아이콘 + 잔량(%)
  display.drawRect(2, 56, 12, 7, SSD1306_WHITE);
  display.fillRect(14, 58, 2, 3, SSD1306_WHITE);              // 배터리 꼭지
  int fillW = (int)batteryPercent * 10 / 100;
  if (fillW > 0) display.fillRect(3, 57, fillW, 5, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(20, 56);
  display.print(batteryPercent);
  display.print("%");
}

// 적 발견/구조 3초 카운트다운 표시
void drawCountdown() {
  const char* label = pendBtn == 3 ? "SOS"
                    : (pendCount == 1 ? "OPEN FIRE" : "HOLD FIRE");
  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(label);
  display.setTextSize(1);
  display.setCursor(0, 42);
  if (!pendSent) {
    int remain = 3 - (int)((millis() - pendStartMs) / 1000);
    if (remain < 1) remain = 1;
    display.print("SEND ");
    display.print(remain);
    display.print(pendBtn == 3 ? "s  x2=CANCEL" : "s  x3=CANCEL");
  } else {
    display.print("SENT  PRESS=HOME");
  }
}

void drawScreen(bool lost) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (zeroPending || (zeroDoneMs && millis() - zeroDoneMs < 2500)) {   // 방향 기준 맞추기 피드백
    static const uint16_t title[] = {0xBC29, 0xD5A5, ' ', 0xAE30, 0xC900};           // 방향 기준
    static const uint16_t wait[]  = {0xAC00, 0xB9CC, 0xD788, ' ', 0xB4E4, 0xACE0, ' ', 0xC788, 0xAE30};  // 가만히 들고 있기
    static const uint16_t done[]  = {0xBD81, 0xCABD, ' ', 0xAE30, 0xC900, ' ', 0xC124, 0xC815};          // 북쪽 기준 설정
    int w = 0; for (int i = 0; i < 5; i++) w += glyphAdvance(title[i]);
    drawCps((SCREEN_WIDTH - w) / 2, 12, title, 5);
    if (zeroPending) {
      display.drawCircle(64, 34, 10 + (millis() / 150) % 8, SSD1306_WHITE);   // 기다리는 동안 맥박
      int w2 = 0; for (int i = 0; i < 9; i++) w2 += glyphAdvance(wait[i]);
      drawCps((SCREEN_WIDTH - w2) / 2, 62, wait, 9);
    } else {
      display.setTextSize(3);
      display.setCursor(28, 22);
      display.print("N 0");
      display.setTextSize(1);
      display.print((char)247);
      int w2 = 0; for (int i = 0; i < 8; i++) w2 += glyphAdvance(done[i]);
      drawCps((SCREEN_WIDTH - w2) / 2, 62, done, 8);
    }
    display.display();
    return;
  }
  if (pairResult >= 0 && millis() - pairResultMs < 3000) {   // 페어링 결과 3초 표시
    bool ok = pairResult == 1;
    if (ok) display.fillRect(0, 0, SCREEN_WIDTH, 64, SSD1306_WHITE);
    else    display.drawRect(0, 0, SCREEN_WIDTH, 64, SSD1306_WHITE);
    static const uint16_t okTxt[]   = {0xD398, 0xC5B4, 0xB9C1, ' ', 0xC644, 0xB8CC};   // 페어링 완료
    static const uint16_t failTxt[] = {0xBC88, 0xD638, ' ', 0xD2C0, 0xB9BC};           // 번호 틀림
    const uint16_t* t = ok ? okTxt : failTxt;
    int n = ok ? 6 : 5, w = 0;
    for (int i = 0; i < n; i++) w += glyphAdvance(t[i]);
    if (ok) {   // 반전 바탕 위 검은 글씨: 글자를 흰색으로 그린 뒤 반전 영역에서 지워 표현
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(2);
      display.setCursor(40, 16);
      display.print("OK");
      display.setTextColor(SSD1306_WHITE);
      display.fillRect(0, 38, SCREEN_WIDTH, 26, SSD1306_BLACK);
      drawCps((SCREEN_WIDTH - w) / 2, 58, t, n);
    } else {
      display.setTextSize(2);
      display.setCursor(52, 12);
      display.print("X");
      drawCps((SCREEN_WIDTH - w) / 2, 50, t, n);
    }
    display.display();
    return;
  }
  if (pairShowing && millis() - pairShowMs < 60000) {   // 페어링 번호(폰에 입력)
    display.setTextSize(1);
    display.setCursor(0, 2);
    display.print("PAIRING CODE");
    display.setTextSize(3);
    display.setCursor(10, 24);
    display.print(pairCode);
    display.display();
    return;
  }

  if (msgOpen) {          // 중앙 메시지는 수신 확인(S1)할 때까지 화면을 차지한다
    drawMessage();
    display.display();
    return;
  }

  // 평소(특별한 상황 없음)엔 워치페이스: 시각 + 분대원 수 + 알림
  bool attackShow = attackMs != 0 && millis() - attackMs < 6000;
  bool eventShow = shownEvent != EVENT_NONE && millis() - shownEventMs < EVENT_SHOW_MS;
  bool enemyShow = enemyMs != 0 && millis() - enemyMs < ENEMY_SHOW_MS;
  if (!attackShow && !pendBtn && !eventShow && !enemyShow) {
    drawWatchFace(lost);
    display.display();
    return;
  }

  // 적 발견: 깔끔한 전체 화면(사격개시/금지 + 화살표 + 거리만)
  if (enemyShow && !attackShow && !pendBtn && !eventShow) {
    drawEnemy();
    display.display();
    return;
  }

  // 맨 윗줄: 연결 상태와 신호 세기
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (lost) {
    display.print(everSeen ? "PEER LOST" : "SEARCHING...");
  } else {
    display.print("LINK ");
    display.print((int)smoothedRssi);
    display.print("dBm");
  }
  display.setCursor(104, 0);
  display.print(myCallsign());
  if (centralUnread) display.fillRect(123, 0, 5, 5, SSD1306_WHITE);  // 안 읽은 중앙 메시지

  // 가운데(우선순위): 공격 경보 > 카운트다운 > 받은 이벤트 > 적 화살표 > 거리
  if (attackMs != 0 && millis() - attackMs < 6000) {
    drawAttackBanner();
  } else if (pendBtn) {
    drawCountdown();
  } else if (shownEvent != EVENT_NONE && millis() - shownEventMs < EVENT_SHOW_MS) {
    display.setTextSize(1);
    display.setCursor(0, 16);
    display.print("FROM PEER:");
    display.setTextSize(2);
    display.setCursor(0, 28);
    display.print(eventName(shownEvent));
  } else if (enemyMs != 0 && millis() - enemyMs < ENEMY_SHOW_MS) {
    drawEnemy();
  }

  // 맨 아랫줄: 내가 보낸 이벤트
  display.setTextSize(1);
  display.setCursor(0, 54);
  if (lastSentEvent != EVENT_NONE && millis() - lastSentMs < EVENT_SHOW_MS) {
    display.print("SENT: ");
    display.print(eventName(lastSentEvent));
  }

  // 아랫줄 오른쪽: 돈 각도 (S3으로 영점). 적 화면에선 그 자리에 거리를 쓰므로 생략.
  if (!enemyShow) {
    display.setTextSize(1);
    display.setCursor(98, 54);
    if (zeroPending) display.print("HOLD");
    else if (headingReady) {
      char h[6];
      snprintf(h, sizeof(h), "%3d", (int)heading);
      display.print(h);
      display.print((char)247);   // ° 기호
    }
  }

  display.display();
}

// ---------------- 입력 ----------------
void checkTap() {
  if (!TAP_SEND_ENABLED) return;
  if (!hasImu) return;

  uint8_t src = 0;
  if (!readReg(IMU_ADDR, REG_TAP_SRC, &src)) return;

  if (src & TAP_SRC_DOUBLE) {
    pendingSingleMs = 0;
    sendEvent(EVENT_SOS);
  } else if ((src & TAP_SRC_SINGLE) && pendingSingleMs == 0) {
    pendingSingleMs = millis();  // 2번째 탭이 올지 잠깐 기다림
  }

  if (pendingSingleMs && millis() - pendingSingleMs > DOUBLE_TAP_WAIT_MS) {
    pendingSingleMs = 0;
    sendEvent(EVENT_OK);
  }
}

// S4/S3를 누를 때마다 호출. 첫 누름부터 3초 안에만 바꾸거나 취소할 수 있다.
//   S3 긴급구호: 3초 안에 한 번 더 = 취소
//   S4 적 발견: 1번 = 사격개시, 3초 안에 2번 = 사격금지, 3번 = 취소
//   3초가 지나 전송된 뒤 같은 버튼을 또 누르면 취소가 아니라 시계 화면으로 돌아간다.
void pressPending(uint8_t b) {
  if (pendBtn != b) { pendBtn = b; pendCount = 1; pendSent = false; pendStartMs = millis(); }
  else if (pendSent) { pendBtn = 0; }              // 이미 전송됨 → 시계로
  else {
    pendCount++;
    if (pendCount >= (b == 3 ? 2 : 3)) pendBtn = 0; // 3초 안 취소(전송 안 함)
  }
  pendLastMs = millis();
}

// 첫 누름부터 3초 지나면 확정 전송. 전송 화면은 15초 뒤 저절로 닫힌다.
void updatePending() {
  if (!pendBtn) return;
  if (!pendSent && millis() - pendStartMs > 3000) {
    if (pendBtn == 4) sendEvent(pendCount == 1 ? EVENT_ENEMY_FIRE : EVENT_ENEMY_HOLD);
    else sendEvent(EVENT_SOS);
    pendSent = true;
    pendLastMs = millis();
  }
  if (pendSent && millis() - pendLastMs > 15000) pendBtn = 0;
}

// S2: 받은 메시지를 최신→이전으로 한 개씩 보여주고, 끝까지 보면 닫는다.
void viewMessageStep() {
  if (cCount == 0) return;
  viewIdx++;
  if (viewIdx >= cCount) { viewIdx = -1; msgOpen = false; return; }
  int slot = (cHead - 1 - viewIdx + CENTRAL_RING * 4) % CENTRAL_RING;
  shownLen = cLen[slot];
  shownMsgId = cId[slot];
  shownFrom = cFrom[slot];
  shownTime = cTime[slot];
  for (int i = 0; i < shownLen && i < MSG_MAX_UNITS; i++) shownUnits[i] = cUnits[slot][i];
  msgOpen = true;
  centralUnread = false;
  msgShownMs = millis();
}

void checkButtons() {
  static bool p1 = false, p2 = false, p3 = false, p4 = false;
  static unsigned long s1DownMs = 0;
  bool n1 = digitalRead(PIN_BUTTON1) == LOW;
  bool n2 = digitalRead(PIN_BUTTON2) == LOW;
  bool n3 = digitalRead(PIN_BUTTON3) == LOW;
  bool n4 = digitalRead(PIN_BUTTON4) == LOW;

  if (n4 && !p4) pressPending(4);          // S4 적 발견: 1=사격개시 2=사격금지 3=취소 (3초 안, 전송 후 누르면 시계)
  if (n3 && !p3) pressPending(3);          // S3 긴급 구조: 3초 안에 2번=취소 (전송 후 누르면 시계)
  if (n2 && !p2) viewMessageStep();        // S2 메시지 보기

  if (n1 && !p1) s1DownMs = millis();      // S1 누름 시작
  if (!n1 && p1) {                          // S1 뗌 = 홈(워치 화면으로)
    if (millis() - s1DownMs >= 3000) {
      requestHeadingZero();                 // 길게 3초: 방향 기준 맞추기(선택)
    } else {
      if (msgOpen) { ackedMsgId = shownMsgId; sendEvent(EVENT_ACK); }  // 보던 메시지 수신확인
      msgOpen = false; viewIdx = -1;        // 메시지 창 닫기
      shownEvent = EVENT_NONE; shownEventMs = 0;   // 받은 이벤트 표시 지움
      enemyMs = 0;                          // 적 화살표 지움 → 워치로
    }
  }
  p1 = n1; p2 = n2; p3 = n3; p4 = n4;
}

// ---------------- 메인 ----------------
// I2C 선(SDA P0.26, SCL P0.27)이 막혀 있으면(부품 고장·잘못된 연결로 선을 LOW로 붙잡음)
// Wire가 영원히 기다리며 보드 전체가 멈춘다. 그래서 시작 전에 선 상태를 보고,
// 막혀 있으면 SCL을 9번 흔들어 풀어 보고, 그래도 막혀 있으면 화면·IMU 없이 블루투스만 동작한다.
bool i2cBusOk = true;
int i2cSdaLevel = -1, i2cSclLevel = -1;   // 진단용: 시작 때 읽은 선 상태

bool i2cLinesHigh() {
  pinMode(26, INPUT_PULLUP);
  pinMode(27, INPUT_PULLUP);
  delay(2);
  i2cSdaLevel = digitalRead(26);
  i2cSclLevel = digitalRead(27);
  return i2cSdaLevel == HIGH && i2cSclLevel == HIGH;
}

// I2C 선은 "LOW로 당기기" 또는 "놓아서 풀업이 HIGH로" 두 상태만 쓴다(HIGH로 밀면 상대 칩과 부딪침)
void i2cLine(uint8_t pin, bool high) {
  if (high) {
    pinMode(pin, INPUT_PULLUP);
  } else {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  delayMicroseconds(10);
}

// I2C 표준 버스 복구: 리셋 순간 IMU·화면이 데이터 주고받는 도중이었으면 SDA를 붙잡고 다음 클럭을 기다린다.
// SDA가 풀릴 때까지 SCL을 최대 18번 흔들고, 정식 STOP 신호로 대화를 끝낸다. 3번까지 재시도.
bool checkI2cBus() {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (i2cLinesHigh()) return true;

    for (int i = 0; i < 18; i++) {
      i2cLine(27, false);
      i2cLine(27, true);
      if (i >= 1 && digitalRead(26) == HIGH) break;
    }
    // STOP: SCL LOW 상태에서 SDA LOW → SCL HIGH → SDA HIGH
    i2cLine(27, false);
    i2cLine(26, false);
    i2cLine(27, true);
    i2cLine(26, true);
    delay(5);
  }
  return i2cLinesHigh();
}

// ---------------- 폰↔단말 GATT ----------------
// UTF-8 바이트 → 코드포인트(uint16_t, BMP). 보드 글꼴은 코드포인트로 글자를 찾는다.
int utf8ToUnits(const uint8_t* b, int n, uint16_t* out, int maxOut) {
  int o = 0, i = 0;
  while (i < n && o < maxOut) {
    uint8_t c = b[i];
    uint32_t cp;
    if (c < 0x80) { cp = c; i += 1; }
    else if ((c >> 5) == 0x6 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (b[i+1] & 0x3F); i += 2; }
    else if ((c >> 4) == 0xE && i + 2 < n) { cp = ((uint32_t)(c & 0x0F) << 12) | ((b[i+1] & 0x3F) << 6) | (b[i+2] & 0x3F); i += 3; }
    else { i += 1; continue; }
    out[o++] = (uint16_t)cp;
  }
  return o;
}

// 폰에서 온 메시지: 화면을 덮지 않고 수신함에 넣고 알림(벨)만 띄운다. S2로 꺼내 본다.
void showMessageUnits(const uint16_t* u, int n) {
  if (n > MSG_MAX_UNITS) n = MSG_MAX_UNITS;
  for (int i = 0; i < n; i++) cUnits[cHead][i] = u[i];
  cLen[cHead] = n;
  cId[cHead] = 251;                       // 폰 메시지 식별용
  cHead = (cHead + 1) % CENTRAL_RING;
  if (cCount < CENTRAL_RING) cCount++;
  centralUnread = true;                   // 워치 화면에 벨 표시
  msgArriveMs = millis();
}

// 폰 → 단말 GPS write 콜백: [버전1][위도E7 LE4][경도E7 LE4][정확도][나이3]
void onGpsWrite(uint16_t conn, BLECharacteristic* chr, uint8_t* d, uint16_t len) {
  (void)conn; (void)chr;
  if (len < 9 || d[0] != 1) return;
  myLatE7 = (int32_t)((uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24));
  myLonE7 = (int32_t)((uint32_t)d[5] | ((uint32_t)d[6] << 8) | ((uint32_t)d[7] << 16) | ((uint32_t)d[8] << 24));
  gpsRxMs = millis();
}

// 폰 → 단말 메시지 write 콜백 (iOS/웹 앱 공통 형식):
// [버전=1][대상(0=동료,1=중앙)][글자수][UTF-16LE 유닛 × 글자수]. 무거운 작업은 loop에서.
void onMsgWrite(uint16_t conn, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn; (void)chr;
  if (len < 3 || data[0] != 1) return;
  int count = data[2];
  int avail = (len - 3) / 2;
  if (count > avail) count = avail;
  if (count > MSG_MAX_UNITS) count = MSG_MAX_UNITS;
  for (int i = 0; i < count; i++) phoneUnits[i] = data[3 + i * 2] | (data[4 + i * 2] << 8);
  phoneLen = count;
  phoneTarget = data[1];
  phonePending = true;
}

void onPhoneConnect(uint16_t conn) { (void)conn; phoneConnected = true; Serial.println("PHONE CONNECTED"); }
void onPhoneDisconnect(uint16_t conn, uint8_t reason) {
  (void)conn;
  phoneConnected = false;
  if (pairShowing) { pairShowing = false; pairResult = 0; pairResultMs = millis(); }   // 번호 입력 중 끊김 = 실패
  Serial.print("PHONE DISCONNECTED reason="); Serial.println(reason);
}

// 페어링: 보드가 6자리 번호를 화면에 띄우고, 폰에서 그 번호를 입력해야 연결(LESC + MITM).
// 콜백은 BLE 문맥이라 화면은 loop에서 그린다.
bool onPasskey(uint16_t conn, uint8_t const passkey[6], bool match_request) {
  (void)conn; (void)match_request;
  memcpy(pairCode, passkey, 6); pairCode[6] = 0;
  pairShowing = true; pairShowMs = millis();
  Serial.print("PAIR CODE "); Serial.println(pairCode);
  return true;
}
void onPairDone(uint16_t conn, uint8_t status) {
  (void)conn;
  pairShowing = false;
  pairResult = (status == 0) ? 1 : 0;       // 0이면 성공, 그 외(번호 틀림·취소)는 실패
  pairResultMs = millis();
  Serial.print("PAIR DONE status="); Serial.println(status);
}

void setupGatt() {
  squadSvc.begin();
  chGps.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  chGps.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  chGps.setMaxLen(20);
  chGps.setWriteCallback(onGpsWrite);
  chGps.begin();
  chMsg.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  chMsg.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  chMsg.setMaxLen(200);
  chMsg.setWriteCallback(onMsgWrite);
  chMsg.begin();
  chStatus.setProperties(CHR_PROPS_NOTIFY | CHR_PROPS_READ);
  chStatus.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_NO_ACCESS);
  chStatus.setMaxLen(20);
  chStatus.begin();
  Bluefruit.Periph.setConnectCallback(onPhoneConnect);
  Bluefruit.Periph.setDisconnectCallback(onPhoneDisconnect);
}

// 단말 → 폰 상태 알림 (iOS/웹 앱 형식): 동료 이벤트를 올려 보낸다.
// [버전=1][종류=3(event)][보낸이][이벤트][0][방향 lo][방향 hi]
void notifyEventToPhone(uint8_t fromId, uint8_t ev, uint16_t bearing) {
  if (!phoneConnected) return;
  uint8_t st[7] = {1, 3, fromId, ev, 0, (uint8_t)(bearing & 0xFF), (uint8_t)(bearing >> 8)};
  chStatus.notify(st, sizeof(st));
}

// 단말 → 폰 받은 메시지(중앙·동료): 20바이트 알림 여러 개로 나눠 보낸다.
// [버전=1][종류 2=중앙,1=동료][보낸이(0=중앙)][메시지번호][조각<<4|조각수][UTF-16LE ×≤7]
void notifyInboxToPhone(uint8_t from, uint8_t id, const uint16_t* u, int n) {
  if (!phoneConnected) return;
  const int PER = 7;
  int parts = (n + PER - 1) / PER;
  if (parts < 1) parts = 1;
  for (int p = 0; p < parts; p++) {
    uint8_t f[5 + PER * 2];
    int k = 0;
    f[k++] = 1; f[k++] = from == 0 ? 2 : 1; f[k++] = from; f[k++] = id;
    f[k++] = (uint8_t)((p << 4) | parts);
    for (int i = p * PER; i < n && i < (p + 1) * PER; i++) { f[k++] = u[i] & 0xFF; f[k++] = u[i] >> 8; }
    chStatus.notify(f, k);
  }
  Serial.print("PHONE <- MSG parts="); Serial.println(parts);
}

void setup() {
  for (uint8_t i = 0; i < 4; i++) pinMode(LED_PINS[i], OUTPUT);
  setLeds(0);
  pinMode(PIN_BUTTON1, INPUT_PULLUP);
  pinMode(PIN_BUTTON2, INPUT_PULLUP);
  pinMode(PIN_BUTTON3, INPUT_PULLUP);
  pinMode(PIN_BUTTON4, INPUT_PULLUP);

  Serial.begin(115200);

  i2cBusOk = checkI2cBus();
  if (i2cBusOk) {
    Wire.setPins(26, 27);
    Wire.begin();

    Wire.beginTransmission(OLED_ADDR);
    bool oledFound = (Wire.endTransmission() == 0);
    hasDisplay = oledFound && display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
    Wire.setClock(400000);

    hasImu = setupImuTap();
  }

  myId = (uint8_t)(NRF_FICR->DEVICEID[0] & 0xFF);

  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  batteryVolts = readBatteryVolts();
  batteryPercent = voltsToPercent(batteryVolts);

  Bluefruit.begin(1, 1);
  Bluefruit.setTxPower(8);
  Bluefruit.autoConnLed(false);
  char nm[12];
  snprintf(nm, sizeof(nm), "SQUAD-%s", myCallsign());   // 웹앱이 이름으로 찾음(SQUAD-E1 등)
  Bluefruit.setName(nm);

  loadCounter();
  loadHeading();
  sec_init(&secCtx, SEC_MASTER_KEY);   // 분대 공용 열쇠로 암호·인증 열쇠 파생(실전: 폰이 QR로 장전)
  sec_replay_reset(&secReplay);

  Bluefruit.Security.setIOCaps(true, false, false);   // 화면만 있음 → 폰이 번호 입력
  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setPairPasskeyCallback(onPasskey);
  Bluefruit.Security.setPairCompleteCallback(onPairDone);
  (Bluefruit.Security.*secParamPtr(SecParamTag())).bond = 0;   // 열쇠 저장 안 함 → 매번 번호
  Bluefruit.Periph.clearBonds();                                // 예전에 저장된 열쇠도 지움
  setupGatt();                                   // 폰↔단말 GATT 서비스
  Bluefruit.ScanResponse.addService(squadSvc);   // 아이폰 앱은 서비스 UUID로 찾음(스캔응답에 실음)
  Bluefruit.ScanResponse.addName();              // 이름도(SQUAD-E1). 광고 본문은 방송 프레임이 차지
  Bluefruit.Advertising.restartOnDisconnect(true);

  updateAdvertising();
  startScanning();
}

void loop() {
  bool lost = connectedCount() == 0;   // 살아 있는 분대원이 하나도 없으면 두절

  checkTap();
  checkButtons();
  updatePending();
  updateHeading();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' && hasImu) dumpRawImu();
    if (c == 'm') injectTestMessage();
    if (c == 'f') sendEvent(EVENT_ENEMY_FIRE);   // 시험: 사격개시 교전
    if (c == 'h') sendEvent(EVENT_ENEMY_HOLD);   // 시험: 사격금지
    if (c == 'g') sendEvent(EVENT_SOS);          // 시험: 긴급 구조
    if (c == 'c') sendEvent(EVENT_CORRECTION);   // 시험: 정정
    if (c == 'z') startForgeAttack();            // 시연: 위조 방송(열쇠 없는 가짜) 2초
    if (c == 'x') armReplayAttack();             // 시연: 지금 방송을 녹음→5초 뒤 재생
    if (c == 'P') { phoneConnected = !phoneConnected; Serial.print("SIM PHONE "); Serial.println(phoneConnected ? "ON" : "OFF"); updateAdvertising(); }  // 시험: 폰 연결 흉내
    if (c == 'B') {                              // 시험: 폰에서 "시험 메시지"가 온 것처럼
      const uint16_t t[] = {0xC2DC, 0xD5D8, ' ', 0xBA54, 0xC2DC, 0xC9C0};   // "시험 메시지"
      for (int i = 0; i < 6; i++) phoneUnits[i] = t[i];
      phoneLen = 6; phoneTarget = 0; phonePending = true;
    }
    if (c == 'S') { Bluefruit.Scanner.stop(); startScanning(); Serial.println("SCAN RESTART"); }  // 수신 복구(리부트 없이)
    if (c == 'R') { Serial.println("REBOOT"); delay(30); NVIC_SystemReset(); }                     // 소프트 리셋(플래시 없이 재부팅)
    if (c == 'T') {                              // 시각 설정: T 뒤 HHMMSS[+요일1자리0=SU+날짜2자리] (폰 연동 전 임시)
      char g[9]; int k = 0;
      unsigned long dl = millis() + 500;
      while (k < 9 && millis() < dl) {
        if (Serial.available()) { char d = Serial.read(); if (d >= '0' && d <= '9') g[k++] = d; }
      }
      if (k >= 6) {
        int hh = (g[0]-'0')*10 + (g[1]-'0');
        int mm = (g[2]-'0')*10 + (g[3]-'0');
        int ss = (g[4]-'0')*10 + (g[5]-'0');
        clockSetSec = (uint32_t)hh*3600 + mm*60 + ss;
        clockSetMs = millis();
        clockSet = true;
        if (k >= 9) { clockWeekday = (g[6]-'0') % 7; clockDate = (g[7]-'0')*10 + (g[8]-'0'); }
        Serial.print("CLOCK SET "); Serial.print(hh); Serial.print(":"); Serial.print(mm); Serial.print(":"); Serial.println(ss);
      }
    }
  }

  // 공격 시연 타이밍
  if (replayArmed && millis() >= replayAtMs) {   // 녹음해 둔 옛 방송을 지금 재생
    replayArmed = false;
    attackActive = true;
    attackUntilMs = millis() + 2000;
    advertiseBytes(replaySnap, SEC_FRAME);
    Serial.println("[공격] 녹음한 방송 재생 중 2초 → 상대 보드: 재생 거부");
  }
  if (attackActive && millis() >= attackUntilMs) {   // 공격 끝 → 정상 방송 복귀
    attackActive = false;
    updateAdvertising();
  }
  // 평소 주기적으로 카운터를 올려 둔다(재생 공격이 '옛 방송'으로 걸리도록, 키스트림도 매번 새것)
  static unsigned long lastBumpMs = 0;
  if (!attackActive && millis() - lastBumpMs >= 1500) {
    lastBumpMs = millis();
    updateAdvertising();
  }

  if (phonePending) {                    // 폰에서 온 메시지 → 분대에 방송(동료 단말에 벨)
    phonePending = false;
    outLen = phoneLen > MSG_MAX_UNITS ? MSG_MAX_UNITS : phoneLen;
    for (int i = 0; i < outLen; i++) outUnits[i] = phoneUnits[i];
    outChunks = (outLen + MSG_UNITS_PER_CHUNK - 1) / MSG_UNITS_PER_CHUNK;
    if (outChunks < 1) outChunks = 1;
    outCtr = (outCtr + 1) & 0x3F;
    outId = (uint8_t)(0x80 | (phoneTarget == 1 ? 0x40 : 0) | outCtr);   // 중앙행이면 동료는 무시, 기지국이 중앙에 올림
    outSlot = 0; outSlotMs = 0;
    outActive = true;
    outUntilMs = millis() + 20000;          // 20초 동안 반복 방송
    lastSentEvent = EVENT_NONE;
    Serial.print(phoneTarget == 1 ? "PHONE MSG -> CENTRAL id=" : "PHONE MSG -> SQUAD id="); Serial.print(outId);
    Serial.print(" units="); Serial.print(outLen); Serial.print(" target="); Serial.println(phoneTarget);
  }
  if (outActive) {                       // 조각들과 내 상태 프레임을 번갈아 100ms마다
    if (millis() > outUntilMs) {
      outActive = false;
      updateAdvertising();
    } else if (millis() - outSlotMs >= 100) {
      outSlotMs = millis();
      int k = outSlot % (outChunks + 1);
      outSlot++;
      if (k == outChunks) {                 // 마지막 칸: 평소 암호 상태 프레임(동료가 나를 계속 봄)
        outActive = false; updateAdvertising(); outActive = true;
      } else {
        uint8_t f[7 + MSG_UNITS_PER_CHUNK * 2];
        int start = k * MSG_UNITS_PER_CHUNK;
        int n = outLen - start; if (n > MSG_UNITS_PER_CHUNK) n = MSG_UNITS_PER_CHUNK; if (n < 0) n = 0;
        f[0] = COMPANY_ID & 0xFF; f[1] = COMPANY_ID >> 8; f[2] = 'S'; f[3] = 'M';
        f[4] = outId; f[5] = myId; f[6] = (uint8_t)((k << 4) | (outChunks & 0x0F));
        for (int i = 0; i < n; i++) { f[7 + i*2] = outUnits[start+i] & 0xFF; f[8 + i*2] = outUnits[start+i] >> 8; }
        advertiseBytes(f, 7 + n * 2, false);  // 공간 아끼려 flags 생략(최대 29바이트)
      }
    }
  }

  if (msgReadyPending) {
    uint16_t tmp[MSG_MAX_UNITS];
    noInterrupts();
    msgReadyPending = false;
    uint8_t id = rxMsgId;
    uint8_t from = rxFrom;
    int len = (rxChunkCount - 1) * MSG_UNITS_PER_CHUNK + rxLastChunkUnits;
    if (len > MSG_MAX_UNITS) len = MSG_MAX_UNITS;
    for (int i = 0; i < len; i++) tmp[i] = rxUnits[i];
    interrupts();
    uint16_t key = ((uint16_t)from << 8) | id;
    if (!seenKey(key)) {   // 새 메시지 → 수신함에 저장(화면은 안 덮음) + 폰에 전달
      seenKeys[seenHead] = key; seenHead = (seenHead + 1) % SEEN_N;
      lastCentralId = id;
      for (int i = 0; i < len; i++) cUnits[cHead][i] = tmp[i];
      cLen[cHead] = len;
      cId[cHead] = id;
      cFrom[cHead] = from;
      cTime[cHead] = totalSeconds() % 86400UL;
      notifyInboxToPhone(from, id, tmp, len);
      cHead = (cHead + 1) % CENTRAL_RING;
      if (cCount < CENTRAL_RING) cCount++;
      centralUnread = true;
      msgArriveMs = millis();
      Serial.print("MSG RX #");
      Serial.print(id);
      Serial.print(" from=");
      Serial.print(from, HEX);
      Serial.print(" units=");
      Serial.println(len);
    }
  }

  if (peerEventPending && peerEvent == EVENT_ACK) {
    peerEventPending = false;               // 동료의 메시지 수신 확인은 내 화면에 띄울 필요 없음
  }
  if (peerEventPending) {
    peerEventPending = false;
    if (isEnemy(peerEvent)) {
      enemyBearing = peerBearing % 360;
      enemyFrom = peerId;
      enemyFire = (peerEvent == EVENT_ENEMY_FIRE);
      enemyMs = millis();
    } else if (peerEvent == EVENT_CORRECTION) {
      enemyMs = 0;                 // 동료가 "잘못 봤음" → 적 표시 지움
      shownEvent = peerEvent;
      shownEventMs = millis();
    } else {
      shownEvent = peerEvent;      // OK / SOS 등
      shownEventMs = millis();
      if (peerEvent == EVENT_OK) peerUnread = true;   // 동료 상태 알림: 워치페이스에 '동료' 표시
    }
    notifyEventToPhone(peerId, peerEvent, enemyBearing);   // 폰(앱)에도 동료 이벤트 알림
    ledAnim = peerEvent;
    ledAnimStartMs = millis();
    Serial.print("RX ");
    Serial.print(eventName(peerEvent));
    if (isEnemy(peerEvent)) {
      Serial.print(" BRG=");
      Serial.print(enemyBearing);
    }
    Serial.println();
  }

  updateLeds(lost);

  static unsigned long lastBatteryMs = 0;
  if (millis() - lastBatteryMs >= BATTERY_READ_MS) {
    lastBatteryMs = millis();
    batteryVolts = readBatteryVolts();
    uint8_t pct = voltsToPercent(batteryVolts);
    if (abs((int)pct - (int)batteryPercent) >= 2) {
      batteryPercent = pct;
      updateAdvertising();
    }
  }

  static unsigned long lastDrawMs = 0;
  if (hasDisplay && millis() - lastDrawMs >= 100) {
    lastDrawMs = millis();
    drawScreen(lost);
  }

  // 1초마다 상태를 노트북으로 출력
  static unsigned long lastPrintMs = 0;
  if (millis() - lastPrintMs >= 1000) {
    lastPrintMs = millis();
    if (!i2cBusOk) {
      Serial.print("I2C=STUCK(SDA=");
      Serial.print(i2cSdaLevel);
      Serial.print(",SCL=");
      Serial.print(i2cSclLevel);
      Serial.print(") ");
    }
    Serial.print("IMU=");
    Serial.print(hasImu ? "ok" : "none");
    Serial.print(" OLED=");
    Serial.print(hasDisplay ? "ok" : "none");
    if (headingReady) {
      Serial.print(" HDG=");
      Serial.print(heading, 1);
    }
    if (hasImu) {
      Serial.print(biasLearned ? " BIAS=" : " BIAS?=");
      Serial.print(gyroBias[0], 2);
      Serial.print(",");
      Serial.print(gyroBias[1], 2);
      Serial.print(",");
      Serial.print(gyroBias[2], 2);
      Serial.print(" SD=");
      Serial.print(lastWinStd[0], 2);
      Serial.print(",");
      Serial.print(lastWinStd[1], 2);
      Serial.print(",");
      Serial.print(lastWinStd[2], 2);
      Serial.print(" dM=");
      Serial.print(lastWinMeanDiff[0], 2);
      Serial.print(",");
      Serial.print(lastWinMeanDiff[1], 2);
      Serial.print(",");
      Serial.print(lastWinMeanDiff[2], 2);
      Serial.print(" N=");
      Serial.print(lastWinCount);
    }
    Serial.print(" BAT=");
    Serial.print(batteryVolts, 2);
    Serial.print("V/");
    Serial.print(batteryPercent);
    Serial.print("% ");
    if (lost) Serial.println("LOST");
    else {
      Serial.print("LINK ");
      Serial.print((int)smoothedRssi);
      Serial.print(" DIST~");
      Serial.print(rssiToMeters(smoothedRssi), 1);
      Serial.println("m");
    }
  }

  delay(10);
}
