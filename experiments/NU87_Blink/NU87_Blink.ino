// NU-87 TinyDK 동작 확인: RGB LED 빨강 → 초록 → 파랑 (누코드 시작 가이드 예제)
void setup() {
  Serial.begin(115200);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
}

void blink(int pin, const char* name) {
  digitalWrite(pin, HIGH);
  Serial.println(name);
  delay(1000);
  digitalWrite(pin, LOW);
  delay(300);
}

void loop() {
  blink(LED_R, "RED");
  blink(LED_G, "GREEN");
  blink(LED_B, "BLUE");
}
