/*
 * Pico/Pico 2 SBUS (UART0 GP1 100000 8E2) -> PIO UART TX GP16 (115200 8N1).
 *
 * The SBUS decoder and safety behavior follow temesotejam/pico-copter-fc:
 * 25-byte frames, CH3 index 2, 368..1680, loss/failsafe flags, 100 ms timeout,
 * throttle-low recovery latch. No ESC is driven from the Pico.
 *
 * UART bridge packet every 20 ms (8 bytes):
 * A5 5A 01 SEQ CH3_LO CH3_HI FLAGS CRC8
 * FLAGS bit0 = healthy SBUS; bit1 = armed (low throttle seen 200ms).
 * CRC-8 polynomial 0x07, init 0, computed over first 7 bytes.
 */
#include <cstdint>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "uart_tx.pio.h"

namespace {
constexpr uint SBUS_RX_GPIO = 1;          // Same as the existing FC hardware
constexpr uint BRIDGE_TX_GPIO = 16;       // PIO emulates UART, avoids UART0 conflict
constexpr uint32_t SBUS_BAUD = 100000;
constexpr uint32_t BRIDGE_BAUD = 115200;
constexpr uint32_t SBUS_TIMEOUT_US = 100000;
constexpr uint32_t FRAME_GAP_US = 5000;
constexpr uint32_t REPORT_US = 20000;
constexpr uint32_t REARM_LOW_US = 200000;
constexpr uint16_t CH3_LOW_LIMIT = 448;  // 368 + 80, existing FC threshold
constexpr uint8_t SBUS_LOSS_MASK = 0x0C;

volatile uint16_t g_ch3 = 0;
volatile uint8_t g_flags = 0x0C;
volatile uint32_t g_last_good_us = 0;
volatile bool g_received_good = false;
volatile bool g_bad_frame_seen = true;
volatile uint32_t g_good_count = 0;
volatile uint32_t g_bad_event_count = 0;

class SbusParser {
public:
    // Parse complete 25-byte SBUS frames, same packing / end bytes as
    // pico-copter-fc/firmware/pico_copter/sbus_parser.hpp.
    bool push(uint8_t b, uint32_t now, uint16_t &ch3, uint8_t &flags) {
        if (count_ && (uint32_t)(now - last_byte_us_) > FRAME_GAP_US) count_ = 0;
        last_byte_us_ = now;
        if (count_ == 0 && b != 0x0f) return false;
        data_[count_++] = b;
        if (count_ < sizeof(data_)) return false;
        count_ = 0;
        const uint8_t end = data_[24];
        if (!(end == 0x00 || end == 0x04 || end == 0x14 ||
              end == 0x24 || end == 0x34)) return false;
        // CH3 = third 11-bit channel (bit offset 22)
        const uint32_t packed =
            (uint32_t)data_[3] | ((uint32_t)data_[4] << 8) |
            ((uint32_t)data_[5] << 16);
        ch3 = (uint16_t)((packed >> 6) & 0x07ff);
        flags = data_[23];
        return true;
    }
private:
    uint8_t data_[25]{};
    uint8_t count_ = 0;
    uint32_t last_byte_us_ = 0;
};

SbusParser g_parser;
void on_uart0_rx() {
    while (uart_is_readable(uart0)) {
        const uint8_t b = uart_getc(uart0);
        const uint32_t now = time_us_32();
        uint16_t value = 0;
        uint8_t flags = 0;
        if (!g_parser.push(b, now, value, flags)) continue;
        g_flags = flags;
        if (flags & SBUS_LOSS_MASK) {
            // A bad frame must invalidate the link immediately.
            g_bad_frame_seen = true;
            g_received_good = false;
            ++g_bad_event_count;
        } else {
            g_ch3 = value;
            g_last_good_us = now;
            g_received_good = true;
            g_bad_frame_seen = false;
            ++g_good_count;
        }
    }
}

uint8_t crc8(const uint8_t *data, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
    }
    return crc;
}
}

int main() {
    stdio_init_all();
    uart_init(uart0, SBUS_BAUD);
    gpio_set_function(SBUS_RX_GPIO, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8, 2, UART_PARITY_EVEN);
    uart_set_fifo_enabled(uart0, true);
    irq_set_exclusive_handler(UART0_IRQ, on_uart0_rx);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(uart0, true, false);

    PIO pio = pio0;
    const uint sm = 0;
    const uint offset = pio_add_program(pio, &uart_tx_program);
    uart_tx_program_init(pio, sm, offset, BRIDGE_TX_GPIO, BRIDGE_BAUD);

    bool armed = false;
    bool low_seen = false;
    uint32_t low_since = 0;
    uint8_t seq = 0;
    uint32_t observed_bad_events = 0;
    uint32_t next_report = time_us_32();

    for (;;) {
        const uint32_t now = time_us_32();
        if ((int32_t)(now - next_report) < 0) {
            tight_loop_contents();
            continue;
        }
        next_report += REPORT_US;
        // Never burst old frames after a scheduling delay.
        if ((int32_t)(now - next_report) >= 0) next_report = now + REPORT_US;

        uint16_t ch3;
        uint32_t last_good, bad_events;
        bool have_good, bad_seen;
        uint32_t irq = save_and_disable_interrupts();
        ch3 = g_ch3;
        last_good = g_last_good_us;
        have_good = g_received_good;
        bad_seen = g_bad_frame_seen;
        bad_events = g_bad_event_count;
        restore_interrupts(irq);

        // Do not miss a brief loss/failsafe event that is followed by a good
        // SBUS frame before the next 20 ms UART reporting tick.
        const bool unsafe_event = bad_events != observed_bad_events;
        observed_bad_events = bad_events;
        const bool healthy = !unsafe_event && have_good && !bad_seen &&
                             (uint32_t)(now - last_good) <= SBUS_TIMEOUT_US;
        if (!healthy) {
            armed = false;
            low_seen = false;
        } else if (!armed) {
            if (ch3 <= CH3_LOW_LIMIT) {
                if (!low_seen) {
                    low_seen = true;
                    low_since = now;
                }
                if ((uint32_t)(now - low_since) >= REARM_LOW_US) armed = true;
            } else {
                low_seen = false;
            }
        }

        const uint8_t status = (healthy ? 0x01 : 0) | (armed ? 0x02 : 0);
        const uint8_t pkt[8] = {
            0xA5, 0x5A, 0x01, seq++,
            (uint8_t)(ch3 & 0xff), (uint8_t)(ch3 >> 8), status, 0
        };
        uint8_t out[8];
        memcpy(out, pkt, sizeof(out));
        out[7] = crc8(out, 7);
        for (uint8_t b : out) uart_tx_program_putc(pio, sm, b);
    }
}
