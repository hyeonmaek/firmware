// I2C 스캐너: Qwiic(SDA=P0.26, SCL=P0.27)에 연결된 장치 주소를 찾아 시리얼로 출력
// - OLED(SSD1306)는 보통 0x3C 또는 0x3D, IMU(MPU6050)는 0x68 또는 0x69
// - 찾은 장치 수만큼 LED가 켜짐 (시리얼 없이도 확인 가능)

#include <Wire.h>

const uint8_t LED_PINS[] = {PIN_LED1, PIN_LED2, PIN_LED3, PIN_LED4};

void setup() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], !LED_STATE_ON);
  }
  Serial.begin(115200);
  Wire.begin();
}

void loop() {
  uint8_t found = 0;
  Serial.print("SCAN:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print(" 0x");
      if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      found++;
    }
  }
  if (found == 0) Serial.print(" none");
  Serial.println();

  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(LED_PINS[i], i < found ? LED_STATE_ON : !LED_STATE_ON);
  }
  delay(1000);
}
