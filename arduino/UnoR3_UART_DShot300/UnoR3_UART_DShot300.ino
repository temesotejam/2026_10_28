/*
 * Arduino Uno R3 + single ESC: Pico UART bridge -> DShot300.
 * Wiring: Pico GP16 (PIO UART TX, 3.3V) -> Uno D0/RX.
 *          Uno D8 (PB0) -> ESC signal; common GND.
 *
 * Proto: A5 5A 01 seq CH3_L CH3_H FLAGS CRC8; 115200 8N1.
 * FLAGS: bit0 SBUS healthy, bit1 Pico safe to run.
 * Frame CRC8 poly0x07, init0. Missing/bad data => DShot 0.
 *
 * Pico FC reference: CH3 range 368..1680, 448 or below = STOP.
 * Bench testing only: REMOVE ALL PROPELLERS.
 * Important: Uno D0 is also the USB-to-serial RX input. Disconnect the
 * Pico TX wire during Uno uploads; do not open Serial Monitor while running.
 */
#include <Arduino.h>
#include <avr/io.h>
#if !defined(__AVR_ATmega328P__) || F_CPU != 16000000UL
#error Requires Arduino Uno R3 (ATmega328P 16MHz)
#endif
#define DELAY_CYCLES(n) __builtin_avr_delay_cycles(n)

static const uint32_t DSHOT_PERIOD_US = 2000UL; // 500Hz
static const uint32_t UART_TIMEOUT_US = 120000UL;
static const uint32_t BOOT_STOP_US = 3000000UL;
static const uint32_t LOW_REARM_US = 200000UL;
static const uint16_t CH3_STOP = 448;      // 368 + 80 as in original Pico FC
static const uint16_t CH3_MAX = 1680;
static const uint8_t LED_PIN = 7;

uint8_t packet[8] = {};
uint8_t index_rx = 0;
uint16_t ch3 = 0;
uint8_t status_bits = 0;
uint32_t lastRxUs = 0;
uint32_t bootUs = 0;
uint32_t lastDshotUs = 0;
uint32_t lowSinceUs = 0;
bool havePacket = false, isReady = false, lowSeen = false;

uint8_t crc8(const uint8_t *data, uint8_t length) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; ++j)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                         : (uint8_t)(crc << 1);
  }
  return crc;
}

void feedByte(uint8_t b) {
  if (index_rx == 0) {
    if (b == 0xA5) { packet[0] = b; index_rx = 1; }
    return;
  }
  if (index_rx == 1) {
    if (b == 0x5A) { packet[1] = b; index_rx = 2; }
    else index_rx = (b == 0xA5) ? 1 : 0;
    return;
  }
  packet[index_rx++] = b;
  if (index_rx < sizeof(packet)) return;
  index_rx = 0;
  if (packet[2] != 0x01 || crc8(packet, 7) != packet[7]) return;
  const uint16_t value = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8);
  if (value > 2047) return;
  ch3 = value;
  status_bits = packet[6];
  lastRxUs = micros();
  havePacket = true;
  if ((status_bits & 0x03) != 0x03) {
    isReady = false;
    lowSeen = false;
  }
}

uint16_t frameDshot(uint16_t command) {
  const uint16_t data = (uint16_t)(command << 1);
  const uint8_t crc = (data ^ (data >> 4) ^ (data >> 8)) & 0x0f;
  return (uint16_t)((data << 4) | crc);
}

// From the earlier working UnoR3_DShot300_StartHold_UpDown10s sketch.
// This bit-bang implementation writes ALL of PORTB: D9..D13 must remain free.
static inline void sendBit(bool one) {
  if (one) {
    PORTB = 0b00000001;
    DELAY_CYCLES(37);
    PORTB = 0b00000000;
    DELAY_CYCLES(7);
  } else {
    PORTB = 0b00000001;
    DELAY_CYCLES(16);
    PORTB = 0b00000000;
    DELAY_CYCLES(25);
  }
}
void sendFrame(uint16_t bits) {
  for (uint8_t i = 0; i < 16; ++i) {
    sendBit((bits & 0x8000u) != 0);
    bits <<= 1;
  }
}

void setup() {
  pinMode(8, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  PORTB = 0; // DShot idle LOW
  Serial.begin(115200); // ONLY RX from Pico, never Serial.print
  bootUs = micros();
  lastDshotUs = bootUs - DSHOT_PERIOD_US;
}

void loop() {
  while (Serial.available() > 0) feedByte((uint8_t)Serial.read());
  const uint32_t now = micros();
  const bool recent = havePacket && ((uint32_t)(now - lastRxUs) <= UART_TIMEOUT_US);
  const bool picoReady = (status_bits & 0x03) == 0x03;
  const bool linkOk = recent && picoReady;
  if (!linkOk || (uint32_t)(now - bootUs) < BOOT_STOP_US) {
    isReady = false;
    lowSeen = false;
  } else if (!isReady) {
    if (ch3 <= CH3_STOP) {
      if (!lowSeen) { lowSeen = true; lowSinceUs = now; }
      if ((uint32_t)(now - lowSinceUs) >= LOW_REARM_US) isReady = true;
    } else lowSeen = false;
  }

  uint16_t command = 0;
  if (linkOk && isReady && ch3 > CH3_STOP) {
    const uint16_t c = (ch3 >= CH3_MAX) ? CH3_MAX : ch3;
    command = (uint16_t)(48u + (uint32_t)(c - CH3_STOP) * 1999u /
                        (CH3_MAX - CH3_STOP));
  }
  // OFF = no validated UART; blink = awaiting low throttle; ON = armed.
  digitalWrite(LED_PIN, !recent ? LOW : (isReady ? HIGH : ((now / 250000UL) % 2 == 0)));
  const uint32_t txNow = micros();
  if ((uint32_t)(txNow - lastDshotUs) >= DSHOT_PERIOD_US) {
    lastDshotUs = txNow;
    noInterrupts();
    sendFrame(frameDshot(command));
    interrupts();
  }
}
