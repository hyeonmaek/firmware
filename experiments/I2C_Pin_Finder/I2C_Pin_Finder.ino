// I2C 핀 찾기: Qwiic 단자가 어느 핀 쌍에 연결돼 있는지 후보 핀 조합을 전부 스캔
// - 공식 핀맵에서 I2C로 표시된 핀들(P0.04, P0.20, P0.21, P0.26, P0.27, P1.07, P1.13, P1.14)의 모든 쌍을 시도
// - 장치가 응답한 쌍만 시리얼로 출력 (보드 충전 칩 0x6A가 P0.26/P0.27에서 보이는 것은 정상)

#include <Wire.h>

// Arduino 핀 번호: P0.x = x, P1.x = 32 + x
const uint8_t CANDIDATES[] = {4, 20, 21, 26, 27, 39, 45, 46};
const uint8_t COUNT = sizeof(CANDIDATES);

void scanPair(uint8_t sda, uint8_t scl) {
  Wire.end();
  Wire.setPins(sda, scl);
  Wire.begin();

  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!any) {
        Serial.print("SDA=");
        Serial.print(sda);
        Serial.print(" SCL=");
        Serial.print(scl);
        Serial.print(" :");
        any = true;
      }
      Serial.print(" 0x");
      Serial.print(addr, HEX);
    }
  }
  if (any) Serial.println();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED1, OUTPUT);
}

void loop() {
  Serial.println("--- round ---");
  for (uint8_t i = 0; i < COUNT; i++) {
    for (uint8_t j = 0; j < COUNT; j++) {
      if (i == j) continue;
      digitalWrite(PIN_LED1, LED_STATE_ON);  // 스캔 중 표시
      scanPair(CANDIDATES[i], CANDIDATES[j]);
      digitalWrite(PIN_LED1, !LED_STATE_ON);
    }
  }
  delay(2000);
}
