/*
 * SK6812 Sniffer Firmware — ESP32-H2
 * Reads SK6812/WS2812 data line via RMT (DMA mode),
 * decodes RGBW frames and streams them over USB CDC / UART.
 *
 * Protocol:
 *   - Each frame starts with 0xAA 0xBB
 *   - Followed by 2-byte LED count (little-endian)
 *   - Followed by NUM_LEDS * 4 bytes (R, G, B, W per LED)
 *   - Frame ends with 0xCC 0xDD
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_rx.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"

/* ── Configuration ─────────────────────────────────────── */
#define NUM_LEDS            60      // Change to match your strip
#define RMT_RX_GPIO         5       // Data line input (after level shifter)
#define RMT_CLK_RES_HZ      10000000 // 10 MHz → 100 ns resolution
#define UART_PORT           UART_NUM_0
#define UART_BAUD           921600

/* SK6812 timing thresholds (in RMT ticks @ 10 MHz = 100ns each) */
#define T0H_MIN   2    //  200 ns
#define T0H_MAX   4    //  400 ns
#define T1H_MIN   5    //  500 ns
#define T1H_MAX   8    //  800 ns
#define RESET_MIN 800  // 80 µs reset pulse

/* Frame framing bytes */
#define FRAME_SOF1  0xAA
#define FRAME_SOF2  0xBB
#define FRAME_EOF1  0xCC
#define FRAME_EOF2  0xDD

static const char *TAG = "sk6812_sniffer";

/* Buffer: 4 bits per symbol, SK6812 RGBW = 32 bits per LED */
#define BITS_PER_LED        32
#define SYMBOLS_PER_FRAME   (NUM_LEDS * BITS_PER_LED)
#define RMT_BUF_SYMBOLS     (SYMBOLS_PER_FRAME + 64) // extra headroom

static rmt_symbol_word_t rmt_raw_buf[RMT_BUF_SYMBOLS];
static uint8_t led_data[NUM_LEDS * 4]; // R, G, B, W

/* ── Decode one RMT symbol to a bit ────────────────────── */
static int decode_bit(const rmt_symbol_word_t *sym)
{
    uint16_t high = sym->duration0; // high pulse duration
    if (high >= T1H_MIN && high <= T1H_MAX) return 1;
    if (high >= T0H_MIN && high <= T0H_MAX) return 0;
    return -1; // invalid
}

/* ── Decode full RMT buffer into led_data[] ─────────────── */
static bool decode_frame(const rmt_rx_done_event_data_t *edata)
{
    size_t num_sym = edata->num_symbols;
    if (num_sym < SYMBOLS_PER_FRAME) {
        ESP_LOGW(TAG, "Short frame: %d symbols (expected %d)",
                 (int)num_sym, SYMBOLS_PER_FRAME);
        return false;
    }

    memset(led_data, 0, sizeof(led_data));

    for (int led = 0; led < NUM_LEDS; led++) {
        for (int bit = 0; bit < BITS_PER_LED; bit++) {
            int idx = led * BITS_PER_LED + bit;
            int b = decode_bit(&edata->received_symbols[idx]);
            if (b < 0) {
                ESP_LOGW(TAG, "Bad symbol at led=%d bit=%d", led, bit);
                return false;
            }
            /* SK6812 bit order: G7..G0 R7..R0 B7..B0 W7..W0 */
            int byte_pos;
            int bit_shift = 7 - (bit % 8);
            int group = bit / 8;
            /* Remap G→[1], R→[0], B→[2], W→[3] for RGBW storage */
            switch (group) {
                case 0: byte_pos = led * 4 + 1; break; // G
                case 1: byte_pos = led * 4 + 0; break; // R
                case 2: byte_pos = led * 4 + 2; break; // B
                case 3: byte_pos = led * 4 + 3; break; // W
                default: return false;
            }
            if (b) led_data[byte_pos] |= (1 << bit_shift);
        }
    }
    return true;
}

/* ── Send frame over UART ───────────────────────────────── */
static void send_frame_uart(void)
{
    uint8_t header[4] = {FRAME_SOF1, FRAME_SOF2,
                         (uint8_t)(NUM_LEDS & 0xFF),
                         (uint8_t)((NUM_LEDS >> 8) & 0xFF)};
    uint8_t footer[2] = {FRAME_EOF1, FRAME_EOF2};

    uart_write_bytes(UART_PORT, header, sizeof(header));
    uart_write_bytes(UART_PORT, led_data, sizeof(led_data));
    uart_write_bytes(UART_PORT, footer, sizeof(footer));
}

/* ── RMT receive-done callback (runs in ISR context) ────── */
static QueueHandle_t rmt_evt_queue;

static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t chan,
                                      const rmt_rx_done_event_data_t *edata,
                                      void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR(rmt_evt_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

/* ── Main task ──────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "SK6812 Sniffer starting — %d LEDs, GPIO %d",
             NUM_LEDS, RMT_RX_GPIO);

    /* UART init */
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0));

    /* RMT channel config */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num           = RMT_RX_GPIO,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = RMT_CLK_RES_HZ,
        .mem_block_symbols  = RMT_BUF_SYMBOLS,
        .flags.with_dma     = true,
        .flags.io_loop_back = false,
        .flags.invert_in    = false,
    };
    rmt_channel_handle_t rx_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx_chan));

    /* Callback & queue */
    rmt_evt_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rmt_rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL));

    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    /* Receive config */
    rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = 200,
        .signal_range_max_ns = 100000,
    };

    ESP_LOGI(TAG, "Listening on GPIO %d ...", RMT_RX_GPIO);

    rmt_rx_done_event_data_t evt;
    while (1) {
        ESP_ERROR_CHECK(rmt_receive(rx_chan, rmt_raw_buf,
                                    sizeof(rmt_raw_buf), &recv_cfg));
        if (xQueueReceive(rmt_evt_queue, &evt, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (decode_frame(&evt)) {
                send_frame_uart();
            }
        } else {
            ESP_LOGW(TAG, "Timeout waiting for frame");
        }
    }
}
