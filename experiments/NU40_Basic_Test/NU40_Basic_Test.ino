#include <Arduino.h>

const uint8_t LED_PINS[] = {
  PIN_LED1,
  PIN_LED2,
  PIN_LED3,
  PIN_LED4
};

const size_t LED_COUNT =
    sizeof(LED_PINS) / sizeof(LED_PINS[0]);

const unsigned long STEP_DELAY_MS = 500;

void setup() {
  for (size_t i = 0; i < LED_COUNT; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], !LED_STATE_ON);
  }
}

void loop() {
  for (size_t i = 0; i < LED_COUNT; i++) {
    digitalWrite(LED_PINS[i], LED_STATE_ON);
    delay(STEP_DELAY_MS);
    digitalWrite(LED_PINS[i], !LED_STATE_ON);
  }
}
