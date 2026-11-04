/*
 * The functions test/emul/Arduino.h declares, on Zephyr. Same header as the host unit tests, so the
 * upstream code compiles the same way, but time is real and nothing records pin writes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "Arduino.h"

extern "C" {
uint64_t port_uptime_ms(void);
uint32_t port_micros(void);
void port_delay_ms(uint32_t ms);
void port_delay_us(uint32_t us);
}

void delay(unsigned long ms) {
  port_delay_ms(ms);
}

void delayMicroseconds(unsigned long us) {
  port_delay_us(us);
}

uint32_t millis() {
  return static_cast<uint32_t>(port_uptime_ms());
}

unsigned long micros() {
  return port_micros();
}

uint64_t millis64(void) {
  return port_uptime_ms();
}

uint64_t get_timestamp(unsigned long ms) {
  return ms;
}

// No contactor or LED outputs on the S32K yet: every pin is GPIO_NUM_NC (hw_miv_s32k.h)
void pinMode(uint8_t pin, uint8_t mode) {}
void digitalWrite(uint8_t pin, uint8_t val) {}
int digitalRead(uint8_t pin) {
  return 0;
}

int max(int a, int b) {
  return (a > b) ? a : b;
}

bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, int8_t channel) {
  return true;
}
bool ledcWrite(uint8_t pin, uint32_t duty) {
  return true;
}
bool ledcWriteTone(uint8_t pin, uint32_t freq) {
  return true;
}

ESPClass ESP;
