/*
 * The functions test/emul/Arduino.h declares, on Zephyr. Same header as the host unit tests, so the
 * upstream code compiles the same way, but time and pins are real.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "Arduino.h"

extern "C" {
uint64_t port_uptime_ms(void);
uint32_t port_micros(void);
void port_delay_ms(uint32_t ms);
void port_delay_us(uint32_t us);
#include "be_gpio.h"
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

// The pin numbers come from hw_miv_s32k.h. GPIO_NUM_NC arrives here as 255 and is ignored.
void pinMode(uint8_t pin, uint8_t mode) {
  be_gpio_configure(pin, mode == OUTPUT);
}
void digitalWrite(uint8_t pin, uint8_t val) {
  be_gpio_write(pin, val != 0);
}
int digitalRead(uint8_t pin) {
  return be_gpio_read(pin);
}

int max(int a, int b) {
  return (a > b) ? a : b;
}

// No PWM: the economized hold of the contactors is refused with its setting (be_link_cmd.c)
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
