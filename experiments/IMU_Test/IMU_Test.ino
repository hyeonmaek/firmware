// NU-IMU (LSM6DS3TR-C) 읽기 테스트
// - 실제 IMU는 MPU6050이 아니라 NU-IMU-LSM6DS3TR (I2C 주소 0x6A 기본, SB1 브리지 시 0x6B)
// - 보드 충전 칩(BQ25186)도 0x6A라서 주소가 겹침
//   → IMU가 쓰는 레지스터(0x0F 이상)는 충전 칩 레지스터 범위(0x00~0x0C) 밖이므로 겹친 채로 읽어본다
// - WHO_AM_I(0x0F)가 0x6A면 IMU 응답 성공, 가속도 Z가 약 1g면 데이터도 정상

#include <Wire.h>

const uint8_t ADDRS[] = {0x6A, 0x6B};

const uint8_t REG_WHO_AM_I = 0x0F;   // LSM6DS3TR-C는 0x6A를 돌려줌
const uint8_t REG_CTRL1_XL = 0x10;   // 가속도 설정
const uint8_t REG_CTRL2_G  = 0x11;   // 자이로 설정
const uint8_t REG_OUTX_L_G = 0x22;   // 자이로 X,Y,Z → 가속도 X,Y,Z 순서로 12바이트

uint8_t imuAddr = 0;

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void findImu() {
  imuAddr = 0;
  for (uint8_t i = 0; i < sizeof(ADDRS); i++) {
    uint8_t who = 0;
    bool ok = readRegs(ADDRS[i], REG_WHO_AM_I, &who, 1);
    Serial.print("[WHO_AM_I] addr=0x");
    Serial.print(ADDRS[i], HEX);
    Serial.print(ok ? " value=0x" : " no response");
    if (ok) Serial.print(who, HEX);
    Serial.println(ok && who == 0x6A ? "  <- LSM6DS3TR-C 확인" : "");
    if (ok && who == 0x6A && imuAddr == 0) imuAddr = ADDRS[i];
  }
  if (imuAddr) {
    writeReg(imuAddr, REG_CTRL1_XL, 0x40);  // 104Hz, ±2g
    writeReg(imuAddr, REG_CTRL2_G, 0x40);   // 104Hz, ±250dps
  }
}

void setup() {
  Serial.begin(115200);
  Wire.setPins(26, 27);
  Wire.begin();
  Wire.setClock(100000);
}

void loop() {
  if (!imuAddr) {
    findImu();
    if (!imuAddr) { Serial.println("IMU 못 찾음, 2초 후 재시도"); delay(2000); return; }
  }

  uint8_t raw[12];
  if (!readRegs(imuAddr, REG_OUTX_L_G, raw, sizeof(raw))) {
    Serial.println("읽기 실패, 다시 찾기");
    imuAddr = 0;
    delay(500);
    return;
  }

  int16_t v[6];
  for (uint8_t i = 0; i < 6; i++) v[i] = (int16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));

  // ±2g: 0.061 mg/LSB, ±250dps: 8.75 mdps/LSB
  Serial.print("accel g  x=");
  Serial.print(v[3] * 0.000061f, 2);
  Serial.print(" y=");
  Serial.print(v[4] * 0.000061f, 2);
  Serial.print(" z=");
  Serial.print(v[5] * 0.000061f, 2);
  Serial.print("   gyro dps x=");
  Serial.print(v[0] * 0.00875f, 0);
  Serial.print(" y=");
  Serial.print(v[1] * 0.00875f, 0);
  Serial.print(" z=");
  Serial.println(v[2] * 0.00875f, 0);
  delay(300);
}
