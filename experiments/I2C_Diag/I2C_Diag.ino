// I2C 종합 점검: Qwiic(SDA=P0.26, SCL=P0.27)에 연결된 장치를 세 가지 방식으로 스캔
// 1) 하드웨어 I2C 100kHz (공식 예제와 동일)
// 2) 하드웨어 I2C 400kHz
// 3) 소프트웨어 I2C 약 5kHz + 칩 내부 풀업 (선이 길거나 신호가 약할 때도 잡히는지 확인)
// - 2초마다 반복하므로 케이블을 바꿔 꽂으면서 결과를 실시간으로 볼 수 있음
// - IMU(MPU6050)는 0x68 또는 0x69, OLED는 0x3C 또는 0x3D, 보드 충전 칩은 0x6A

#include <Wire.h>

const uint8_t SDA_PIN = 26;
const uint8_t SCL_PIN = 27;
const unsigned int HALF_PERIOD_US = 100;  // 소프트웨어 I2C 반주기 (약 5kHz)

// ---------- 소프트웨어 I2C (오픈드레인 흉내: LOW는 출력, HIGH는 입력+풀업) ----------
void lineLow(uint8_t pin)     { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
void lineRelease(uint8_t pin) { pinMode(pin, INPUT_PULLUP); }

void bbStart() {
  lineRelease(SDA_PIN); lineRelease(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
  lineLow(SDA_PIN); delayMicroseconds(HALF_PERIOD_US);
  lineLow(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
}

void bbStop() {
  lineLow(SDA_PIN); delayMicroseconds(HALF_PERIOD_US);
  lineRelease(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
  lineRelease(SDA_PIN); delayMicroseconds(HALF_PERIOD_US);
}

// 1바이트 보내고 ACK(장치가 SDA를 LOW로 당김) 여부 반환
bool bbWriteByte(uint8_t value) {
  for (int8_t bit = 7; bit >= 0; bit--) {
    if (value & (1 << bit)) lineRelease(SDA_PIN); else lineLow(SDA_PIN);
    delayMicroseconds(HALF_PERIOD_US);
    lineRelease(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
    lineLow(SCL_PIN);
  }
  lineRelease(SDA_PIN); delayMicroseconds(HALF_PERIOD_US);
  lineRelease(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
  bool ack = digitalRead(SDA_PIN) == LOW;
  lineLow(SCL_PIN); delayMicroseconds(HALF_PERIOD_US);
  return ack;
}

void scanSoftware() {
  Serial.print("[SW  5kHz ]");
  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bbStart();
    bool ack = bbWriteByte(addr << 1);
    bbStop();
    if (ack) { Serial.print(" 0x"); Serial.print(addr, HEX); any = true; }
  }
  if (!any) Serial.print(" none");
  Serial.println();
}

void scanHardware(uint32_t clock, const char* label) {
  Wire.setPins(SDA_PIN, SCL_PIN);
  Wire.begin();
  Wire.setClock(clock);
  Serial.print(label);
  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) { Serial.print(" 0x"); Serial.print(addr, HEX); any = true; }
  }
  if (!any) Serial.print(" none");
  Serial.println();
  Wire.end();
}

void setup() {
  Serial.begin(115200);
}

void loop() {
  // 선 상태: 아무것도 안 할 때 두 선은 풀업 때문에 HIGH(1)여야 정상. 0이면 누군가 선을 붙잡고 있음
  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);
  delay(5);
  Serial.print("[LINE    ] SDA=");
  Serial.print(digitalRead(SDA_PIN));
  Serial.print(" SCL=");
  Serial.println(digitalRead(SCL_PIN));

  scanHardware(100000, "[HW 100kHz]");
  scanHardware(400000, "[HW 400kHz]");
  scanSoftware();
  Serial.println("----");
  delay(2000);
}
