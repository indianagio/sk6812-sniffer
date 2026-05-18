/*
 * SK6812 Sniffer Firmware — ESP32-H2-DevKitM-1
 * PlatformIO + ESP-IDF framework (pioarduino)
 *
 * Wiring:
 *   Controller GND  → ESP32-H2 GND
 *   Controller DATA → 10kΩ → GPIO5 (RMT_RX)
 *                           ↓
 *                          20kΩ
 *                           ↓
 *                          GND
 *   (voltage divider 5V → 3.3V)
 *
 * Connect PC to the UART USB-C port (CP2102N), NOT the USB port.
 *
 * Frame format sent over UART:
 *   0xAA 0xBB        — SOF
 *   uint16_le        — num_leds
 *   [R G B W] * N   — LED data
 *   0xCC 0xDD        — EOF
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "driver/uart.h"
#include "esp_log.h"

/* ── User config ──────────────────────────────────────── */
#define NUM_LEDS         60      // ← Change to your strip LED count
#define RMT_RX_GPIO      5       // GPIO connected to DATA line
#define UART_PORT_NUM    UART_NUM_0
#define UART_BAUD        921600

/* ── SK6812 timing @ 10MHz RMT clock (1 tick = 100ns) ─── */
#define RMT_CLK_HZ       10000000
#define T0H_MIN_TICKS    2    // 200ns
#define T0H_MAX_TICKS    4    // 400ns
#define T1H_MIN_TICKS    5    // 500ns
#define T1H_MAX_TICKS    8    // 800ns

/* ── Frame markers ─────────────────────────────────────── */
#define SOF1  0xAA
#define SOF2  0xBB
#define EOF1  0xCC
#define EOF2  0xDD

/* ── Internal constants ─────────────────────────────────  */
#define BITS_PER_LED       32   // SK6812 RGBW
#define SYMBOLS_PER_FRAME  (NUM_LEDS * BITS_PER_LED)
#define RMT_BUF_SIZE       (SYMBOLS_PER_FRAME + 64)

static const char *TAG = "sk6812";

static rmt_symbol_word_t s_rmt_buf[RMT_BUF_SIZE];
static uint8_t           s_led_data[NUM_LEDS * 4];  // RGBW
static QueueHandle_t     s_evt_queue;

/* ── Decode one RMT symbol → bit value (-1 = error) ─────  */
static int symbol_to_bit(const rmt_symbol_word_t *s)
{
    uint16_t h = s->duration0;
    if (h >= T1H_MIN_TICKS && h <= T1H_MAX_TICKS) return 1;
    if (h >= T0H_MIN_TICKS && h <= T0H_MAX_TICKS) return 0;
    return -1;
}

/* ── Decode full RMT buffer → s_led_data[] ──────────────  */
static bool decode_frame(const rmt_rx_done_event_data_t *ev)
{
    if (ev->num_symbols < (size_t)SYMBOLS_PER_FRAME) {
        ESP_LOGW(TAG, "Short frame: %u/%d symbols",
                 (unsigned)ev->num_symbols, SYMBOLS_PER_FRAME);
        return false;
    }
    memset(s_led_data, 0, sizeof(s_led_data));
    for (int led = 0; led < NUM_LEDS; led++) {
        for (int bit = 0; bit < BITS_PER_LED; bit++) {
            int b = symbol_to_bit(&ev->received_symbols[led * BITS_PER_LED + bit]);
            if (b < 0) {
                ESP_LOGW(TAG, "Bad symbol led=%d bit=%d", led, bit);
                return false;
            }
            /*
             * SK6812 wire order: G[7:0] R[7:0] B[7:0] W[7:0]
             * Store as RGBW: byte[0]=R [1]=G [2]=B [3]=W
             */
            int shift = 7 - (bit % 8);
            int group = bit / 8;   // 0=G 1=R 2=B 3=W
            int ch;                // channel index in RGBW output
            switch (group) {
                case 0: ch = 1; break; // G
                case 1: ch = 0; break; // R
                case 2: ch = 2; break; // B
                default: ch = 3; break; // W
            }
            if (b) s_led_data[led * 4 + ch] |= (1 << shift);
        }
    }
    return true;
}

/* ── Send frame over UART ───────────────────────────────  */
static void uart_send_frame(void)
{
    const uint8_t hdr[4] = {
        SOF1, SOF2,
        (uint8_t)(NUM_LEDS & 0xFF),
        (uint8_t)(NUM_LEDS >> 8)
    };
    const uint8_t ftr[2] = {EOF1, EOF2};
    uart_write_bytes(UART_PORT_NUM, hdr, sizeof(hdr));
    uart_write_bytes(UART_PORT_NUM, s_led_data, sizeof(s_led_data));
    uart_write_bytes(UART_PORT_NUM, ftr, sizeof(ftr));
}

/* ── RMT callback (ISR context) ──────────────────────────  */
static bool IRAM_ATTR on_rmt_done(rmt_channel_handle_t ch,
                                   const rmt_rx_done_event_data_t *ev,
                                   void *ctx)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt_queue, ev, &woken);
    return woken == pdTRUE;
}

/* ── app_main ────────────────────────────────────────────  */
void app_main(void)
{
    ESP_LOGI(TAG, "SK6812 sniffer — %d LEDs on GPIO%d", NUM_LEDS, RMT_RX_GPIO);
    ESP_LOGI(TAG, "Connect PC to UART USB-C port @ %d baud", UART_BAUD);

    /* UART */
    const uart_config_t ucfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &ucfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 512, 0, 0, NULL, 0));

    /* RMT RX channel — no DMA (ESP32-H2 RMT RX does not support DMA) */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num          = RMT_RX_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_CLK_HZ,
        .mem_block_symbols = RMT_BUF_SIZE,
        .flags.with_dma    = false,   // H2 RMT RX: DMA not supported
    };
    rmt_channel_handle_t rx_ch = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx_ch));

    s_evt_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
    const rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_rmt_done };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_ch, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_ch));

    const rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = 200,     // ignore glitches < 200ns
        .signal_range_max_ns = 100000,  // max 100µs (covers reset pulse)
    };

    ESP_LOGI(TAG, "Listening...");
    rmt_rx_done_event_data_t evt;
    while (1) {
        ESP_ERROR_CHECK(rmt_receive(rx_ch, s_rmt_buf, sizeof(s_rmt_buf), &recv_cfg));
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(5000))) {
            if (decode_frame(&evt))
                uart_send_frame();
        } else {
            ESP_LOGW(TAG, "No signal for 5s — check wiring");
        }
    }
}
