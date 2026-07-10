#include "display_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

#define I2C_PORT I2C_NUM_0
#define I2C_FREQ_HZ 400000
#define I2C_TIMEOUT_MS 100
#define TITLE_DISPLAY_US (3000000LL)
#define STARTUP_SCREEN_US (3000000LL)
#define BOOT_LOG_MAX_LINES 8

static const char *TAG = "display_manager";
static uint8_t s_page = 0;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;
static u8g2_t s_u8g2;
static bool s_u8g2_ready = false;
static int64_t s_startup_until_us = 0;
static int64_t s_title_until_us = 0;
static int64_t s_reset_notification_until_us = 0;
static uint16_t s_line_scroll_px[4] = {0};
static int64_t s_line_scroll_last_us[4] = {0};
static char s_boot_log[BOOT_LOG_MAX_LINES][32] = {0};
static int s_boot_log_count = 0;
static int64_t s_nvs_save_until_us = 0;
static uint32_t s_nvs_countdown_ms = 0;
static bool s_nvs_saving = false;

static void draw_utf8_scrolling(uint8_t line_idx, int y, const char *text, int64_t now_us)
{
    int text_w;
    int gap = 16;
    int period;
    int64_t elapsed_us;
    int x;

    if (!text || line_idx >= 4) {
        return;
    }

    text_w = u8g2_GetUTF8Width(&s_u8g2, text);
    if (text_w <= OLED_WIDTH) {
        s_line_scroll_px[line_idx] = 0;
        s_line_scroll_last_us[line_idx] = now_us;
        u8g2_DrawUTF8(&s_u8g2, 0, y, text);
        return;
    }

    period = text_w + gap;
    if (s_line_scroll_last_us[line_idx] == 0) {
        s_line_scroll_last_us[line_idx] = now_us;
    }
    elapsed_us = now_us - s_line_scroll_last_us[line_idx];
    if (elapsed_us >= 120000) {
        uint16_t step = (uint16_t)(elapsed_us / 120000);
        s_line_scroll_px[line_idx] = (uint16_t)((s_line_scroll_px[line_idx] + step) % period);
        s_line_scroll_last_us[line_idx] += ((int64_t)step * 120000);
    }

    x = -(int)s_line_scroll_px[line_idx];
    u8g2_DrawUTF8(&s_u8g2, x, y, text);
    u8g2_DrawUTF8(&s_u8g2, x + period, y, text);
}

static void draw_wifi_icon(int x, int y, bool connected, int rssi)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    if (!connected) {
        u8g2_DrawLine(&s_u8g2, x, y, x + 12, y + 12);
        u8g2_DrawLine(&s_u8g2, x + 12, y, x, y + 12);
        return;
    }
    
    int bars = 0;
    if (rssi >= -50) bars = 4;
    else if (rssi >= -60) bars = 3;
    else if (rssi >= -70) bars = 2;
    else if (rssi >= -80) bars = 1;
    
    for (int i = 0; i < 4; i++) {
        int bar_x = x + i * 3;
        int bar_h = (i + 1) * 3;
        int bar_y = y + 12 - bar_h;
        
        if (i < bars) {
            u8g2_DrawBox(&s_u8g2, bar_x, bar_y, 2, bar_h);
        } else {
            u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, 2, bar_h);
        }
    }
}

static void draw_slump_graph(const counter_stats_t *stats, int x, int y, int w, int h)
{
    if (stats->slump_graph_count < 2) {
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, x + w/2 - 30, y + h/2 + 5, "データなし");
        return;
    }
    
    int32_t running_diff = 0;
    
    // Pass 1: find min/max over all data
    int32_t min_diff = 0, max_diff = 0;
    for (uint32_t i = 0; i < stats->slump_graph_count; i++) {
        running_diff += stats->slump_graph[i].delta;
        if (running_diff < min_diff) min_diff = running_diff;
        if (running_diff > max_diff) max_diff = running_diff;
    }
    
    int32_t range = max_diff - min_diff;
    if (range == 0) range = 1;
    
    int label_width = 20;
    int graph_x = x + label_width;
    int graph_w = w - label_width;
    
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    char buf[10];
    
    snprintf(buf, sizeof(buf), "%ld", (long)max_diff);
    u8g2_DrawStr(&s_u8g2, x, y + 8, buf);
    
    snprintf(buf, sizeof(buf), "%ld", (long)min_diff);
    u8g2_DrawStr(&s_u8g2, x, y + h - 2, buf);
    
    if (min_diff < 0 && max_diff > 0) {
        int zero_y = y + h - 1 - (int)((0 - min_diff) * (h - 2) / range);
        u8g2_DrawHLine(&s_u8g2, graph_x, zero_y, graph_w);
    }
    
    u8g2_DrawFrame(&s_u8g2, graph_x, y, graph_w, h);
    
    // Determine visible window
    uint32_t start_idx = 0;
    if (stats->slump_graph_count > SLUMP_GRAPH_MAX_POINTS) {
        start_idx = stats->slump_graph_count - SLUMP_GRAPH_MAX_POINTS;
    }
    uint32_t point_count = stats->slump_graph_count - start_idx;
    
    // Helper: compute (x,y) position for relative index i
    #define GRAPH_PX(i) (graph_x + 1 + (int)((uint32_t)(i) * ((uint32_t)(graph_w) - 2) / (point_count - 1)))
    #define GRAPH_PY(diff) (y + h - 1 - (int)(((diff) - min_diff) * ((uint32_t)(h) - 2) / range))
    
    // Pass A: draw bonus white background rectangles
    running_diff = 0;
    uint32_t block_start = UINT32_MAX;
    int block_x0 = 0;
    for (uint32_t i = 0; i < point_count; i++) {
        uint32_t idx = start_idx + i;
        if (i == 0) {
            running_diff = 0;
            for (uint32_t j = 0; j <= idx; j++) {
                running_diff += stats->slump_graph[j].delta;
            }
        } else {
            running_diff += stats->slump_graph[idx].delta;
        }
        int px = GRAPH_PX(i);
        bool is_bonus = (stats->slump_graph[idx].flags & SLUMP_FLAG_BONUS) != 0;
        
        if (is_bonus && block_start == UINT32_MAX) {
            block_start = i;
            block_x0 = px;
        }
        if (!is_bonus && block_start != UINT32_MAX) {
            u8g2_DrawBox(&s_u8g2, block_x0, y + 1, px - block_x0, h - 2);
            block_start = UINT32_MAX;
        }
    }
    if (block_start != UINT32_MAX) {
        int final_x = GRAPH_PX(point_count - 1);
        u8g2_DrawBox(&s_u8g2, block_x0, y + 1, final_x - block_x0 + 1, h - 2);
    }
    
    // Pass B: draw all lines in white
    running_diff = 0;
    int prev_x = 0, prev_y = 0;
    u8g2_SetDrawColor(&s_u8g2, 1);
    for (uint32_t i = 0; i < point_count; i++) {
        uint32_t idx = start_idx + i;
        if (i == 0) {
            running_diff = 0;
            for (uint32_t j = 0; j <= idx; j++) {
                running_diff += stats->slump_graph[j].delta;
            }
        } else {
            running_diff += stats->slump_graph[idx].delta;
        }
        int px = GRAPH_PX(i);
        int py = GRAPH_PY(running_diff);
        if (i > 0) {
            u8g2_DrawLine(&s_u8g2, prev_x, prev_y, px, py);
        }
        prev_x = px;
        prev_y = py;
    }
    
    // Pass C: redraw bonus-only segments in black
    running_diff = 0;
    prev_x = 0; prev_y = 0;
    bool prev_bonus = false;
    u8g2_SetDrawColor(&s_u8g2, 0);
    for (uint32_t i = 0; i < point_count; i++) {
        uint32_t idx = start_idx + i;
        if (i == 0) {
            running_diff = 0;
            for (uint32_t j = 0; j <= idx; j++) {
                running_diff += stats->slump_graph[j].delta;
            }
        } else {
            running_diff += stats->slump_graph[idx].delta;
        }
        int px = GRAPH_PX(i);
        int py = GRAPH_PY(running_diff);
        bool cur_bonus = (stats->slump_graph[idx].flags & SLUMP_FLAG_BONUS) != 0;
        if (i > 0 && prev_bonus && cur_bonus) {
            u8g2_DrawLine(&s_u8g2, prev_x, prev_y, px, py);
        }
        prev_x = px;
        prev_y = py;
        prev_bonus = cur_bonus;
    }
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    #undef GRAPH_PX
    #undef GRAPH_PY
}

static void draw_bonus_history(const counter_stats_t *stats, int x, int y, int w, int h, bool show_labels)
{
    if (stats->current_interval == 0 && stats->bonus_history_count == 0) {
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, x + w/2 - 30, y + h/2 + 5, "データなし");
        return;
    }
    
    int total_bars = stats->bonus_history_count + 1;  // +1 for current (0)
    
    uint32_t max_interval = 1000;
    if (stats->current_interval > max_interval) max_interval = stats->current_interval;
    for (uint32_t i = 0; i < stats->bonus_history_count; i++) {
        if (stats->bonus_history[i].interval_games > max_interval) {
            max_interval = stats->bonus_history[i].interval_games;
        }
    }
    if (max_interval == 0) max_interval = 1;
    
    int graph_h = h - 16;
    
    u8g2_DrawFrame(&s_u8g2, x, y, w, graph_h);
    
    uint32_t bar_width = (w - 2) / total_bars;
    if (bar_width < 2) bar_width = 2;
    if (bar_width > 8) bar_width = 8;
    
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    
    const char *label1 = BONUS1_LABEL;
    const char *label2 = BONUS2_LABEL;
    
    // Blink state for "0" bar during bonus (500ms)
    bool blink_on = false;
    if (stats->bonus_active) {
        blink_on = ((esp_timer_get_time() / 500000LL) & 1) != 0;
    }
    
    for (int bar_idx = 0; bar_idx < total_bars; bar_idx++) {
        uint32_t interval;
        uint8_t bonus_type;
        
        if (bar_idx == 0) {
            interval = stats->current_interval;
            bonus_type = 0;
        } else {
            uint32_t hist_i = stats->bonus_history_count - bar_idx;
            interval = stats->bonus_history[hist_i].interval_games;
            bonus_type = stats->bonus_history[hist_i].bonus_type;
        }
        
        uint32_t capped = (interval > 1000) ? 1000 : interval;
        int bar_height = (int)((uint64_t)capped * (graph_h - 4) / max_interval);
        if (bar_height < 1) bar_height = 1;
        
        int bar_x = x + 1 + (int)(bar_idx * (w - 2) / total_bars);
        int bar_y = y + graph_h - 2 - bar_height;
        
        // 0 bar blink: skip drawing during "on" phase
        bool skip = (bar_idx == 0 && blink_on);
        
        int stagger = (bar_idx % 2) * 6;
        
        // N-back number
        char nbuf[12];
        snprintf(nbuf, sizeof(nbuf), "%u", (unsigned int)bar_idx);
        if (!skip && show_labels) {
            u8g2_DrawStr(&s_u8g2, bar_x, y - 4, nbuf);
        }
        
        // Game count / interval above bar
        uint32_t display_val = (bar_idx == 0) ? stats->current_interval : 
            stats->bonus_history[stats->bonus_history_count - bar_idx].interval_games;
        char num_buf[12];
        if (display_val >= 1000) {
            snprintf(num_buf, sizeof(num_buf), "%lu", (unsigned long)(display_val / 100));
        } else {
            snprintf(num_buf, sizeof(num_buf), "%lu", (unsigned long)display_val);
        }
        if (!skip && show_labels) {
            u8g2_DrawStr(&s_u8g2, bar_x, bar_y - 2 - stagger, num_buf);
        }
        
        // Bar
        if (!skip) {
            if (bar_idx == 0 || bonus_type == 1) {
                u8g2_DrawBox(&s_u8g2, bar_x, bar_y, bar_width - 1, bar_height);
            } else {
                u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_width - 1, bar_height);
            }
        }
        
        // Bonus type label below graph
        if (bar_idx > 0 && !skip) {
            char label_buf[4];
            if (bonus_type == 1) {
                label_buf[0] = label1[0];
                label_buf[1] = label1[1] ? label1[1] : ' ';
                label_buf[2] = '\0';
            } else {
                label_buf[0] = label2[0];
                label_buf[1] = label2[1] ? label2[1] : ' ';
                label_buf[2] = '\0';
            }
            u8g2_DrawStr(&s_u8g2, bar_x, y + graph_h + 10 + stagger, label_buf);
        }
    }
}

static uint8_t u8x8_byte_i2c_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static uint8_t buffer[132];
    static size_t buf_idx;
    (void)u8x8;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT: {
        if (s_oled_dev) {
            return 1;
        }
        i2c_device_config_t dev_conf = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_I2C_ADDR,
            .scl_speed_hz = I2C_FREQ_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_conf, &s_oled_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
            return 0;
        }
        return 1;
    }
    case U8X8_MSG_BYTE_START_TRANSFER:
        buf_idx = 0;
        return 1;
    case U8X8_MSG_BYTE_SEND: {
        uint8_t *src = (uint8_t *)arg_ptr;
        if (buf_idx + arg_int > sizeof(buffer)) {
            return 0;
        }
        for (size_t i = 0; i < arg_int; i++) {
            buffer[buf_idx++] = src[i];
        }
        return 1;
    }
    case U8X8_MSG_BYTE_END_TRANSFER:
        if (buf_idx > 0 && s_oled_dev) {
            esp_err_t err = i2c_master_transmit(s_oled_dev, buffer, buf_idx, I2C_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2c_master_transmit failed: %s", esp_err_to_name(err));
                return 0;
            }
        }
        return 1;
    case U8X8_MSG_BYTE_SET_DC:
        return 1;
    default:
        return 0;
    }
}

static uint8_t u8x8_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        return 1;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(arg_int * 10);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
        __asm__ __volatile__("nop");
        return 1;
    case U8X8_MSG_DELAY_I2C:
        if (arg_int == 0) {
            return 1;
        }
        esp_rom_delay_us(5 / arg_int);
        return 1;
    case U8X8_MSG_GPIO_RESET:
        return 1;
    default:
        return 0;
    }
}

bool display_manager_init(void)
{
    i2c_master_bus_config_t bus_conf = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_conf, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

#if OLED_ROTATION_180
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R2, u8x8_byte_i2c_cb, u8x8_gpio_and_delay_cb);
#else
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0, u8x8_byte_i2c_cb, u8x8_gpio_and_delay_cb);
#endif
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    s_u8g2_ready = true;
    
    // 起動ロゴ表示
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
    u8g2_DrawUTF8(&s_u8g2, 0, 20, "PACHISLO COUNTER");
    u8g2_DrawUTF8(&s_u8g2, 0, 40, "起動中...");
    u8g2_SendBuffer(&s_u8g2);
    
    ESP_LOGI(TAG, "OLED initialized with U8G2");
    return true;
}

void display_manager_boot_log(const char *msg)
{
    if (!s_u8g2_ready || !msg) return;
    
    // ログをシフト
    if (s_boot_log_count >= BOOT_LOG_MAX_LINES) {
        for (int i = 0; i < BOOT_LOG_MAX_LINES - 1; i++) {
            strncpy(s_boot_log[i], s_boot_log[i + 1], sizeof(s_boot_log[i]) - 1);
            s_boot_log[i][sizeof(s_boot_log[i]) - 1] = '\0';
        }
        s_boot_log_count = BOOT_LOG_MAX_LINES - 1;
    }
    
    // 新しいログを追加
    strncpy(s_boot_log[s_boot_log_count], msg, sizeof(s_boot_log[s_boot_log_count]) - 1);
    s_boot_log[s_boot_log_count][sizeof(s_boot_log[s_boot_log_count]) - 1] = '\0';
    s_boot_log_count++;
    
    // 表示更新
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    
    for (int i = 0; i < s_boot_log_count; i++) {
        u8g2_DrawStr(&s_u8g2, 0, 8 + i * 8, s_boot_log[i]);
    }
    
    u8g2_SendBuffer(&s_u8g2);
    
    // CPUを解放してWDTをリセット
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool display_manager_is_interactive(void)
{
    if (!s_u8g2_ready) {
        return false;
    }
    // ブートログ表示中は非インタラクティブ
    if (s_boot_log_count > 0) {
        return false;
    }
    return esp_timer_get_time() >= s_startup_until_us;
}

void display_manager_next_page(void)
{
    s_page = (s_page + 1) % 5;
    s_title_until_us = esp_timer_get_time() + TITLE_DISPLAY_US;
    memset(s_line_scroll_px, 0, sizeof(s_line_scroll_px));
    memset(s_line_scroll_last_us, 0, sizeof(s_line_scroll_last_us));
}

void display_manager_notify_reset(void)
{
    s_reset_notification_until_us = esp_timer_get_time() + 2000000;
}

void display_manager_render(const counter_stats_t *stats, const char *ip_addr, bool wifi_connected, int wifi_rssi)
{
    char line0[128] = {0};
    char line1[128] = {0};
    char line2[128] = {0};
    char line3[128] = {0};
    const char *title = "";
    bool alarm_active;
    bool invert;
    bool show_title;
    int64_t now_us;

    if (!stats || !s_u8g2_ready) {
        return;
    }

    if (!display_manager_is_interactive()) {
        u8g2_ClearBuffer(&s_u8g2);
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, 0, 20, "PACHISLO COUNTER");
        u8g2_DrawUTF8(&s_u8g2, 0, 40, "起動中...");
        u8g2_DrawUTF8(&s_u8g2, 0, 60, "v2026.07.06");
        u8g2_SendBuffer(&s_u8g2);
        return;
    }

    alarm_active = (stats->door_open || stats->error_active);
    invert = alarm_active && (((esp_timer_get_time() / 1000000LL) & 0x1LL) != 0);
    show_title = (esp_timer_get_time() < s_title_until_us);
    now_us = esp_timer_get_time();

    switch (s_page) {
    case 0: {
        char bonus1_prob[16] = {0};
        char bonus2_prob[16] = {0};
        char total_prob[16] = {0};
        uint32_t prob_games = stats->total_games_excluding_bonus;
        title = "ゲーム情報";

        snprintf(line0, sizeof(line0), "回転:%lu", (unsigned long)stats->total_games);

        if (stats->bonus_total > 0 && prob_games > 0) {
            uint64_t scaled = ((uint64_t)prob_games * 10ULL) / (uint64_t)stats->bonus_total;
            unsigned long whole = (unsigned long)(scaled / 10ULL);
            unsigned long frac = (unsigned long)(scaled % 10ULL);
            snprintf(total_prob, sizeof(total_prob), "1/%lu.%lu", whole, frac);
        } else {
            snprintf(total_prob, sizeof(total_prob), "--");
        }

        if (stats->bonus1_count > 0 && prob_games > 0) {
            snprintf(bonus1_prob, sizeof(bonus1_prob), "1/%lu", (unsigned long)(prob_games / stats->bonus1_count));
        } else {
            snprintf(bonus1_prob, sizeof(bonus1_prob), "--");
        }

        if (stats->bonus2_count > 0 && prob_games > 0) {
            snprintf(bonus2_prob, sizeof(bonus2_prob), "1/%lu", (unsigned long)(prob_games / stats->bonus2_count));
        } else {
            snprintf(bonus2_prob, sizeof(bonus2_prob), "--");
        }

        snprintf(line1, sizeof(line1), "%.8s:%lu %.8s:%lu", BONUS1_LABEL, (unsigned long)stats->bonus1_count, BONUS2_LABEL, (unsigned long)stats->bonus2_count);
        snprintf(line2, sizeof(line2), "合算:%s %.8s:%s %.8s:%s", total_prob, BONUS1_LABEL, bonus1_prob, BONUS2_LABEL, bonus2_prob);
        snprintf(line3, sizeof(line3), "差枚:%ld", (long)stats->diff_medal);
        break;
    }
    case 1:
        title = "入出力状態";
        snprintf(line0, sizeof(line0), "投入:%lu 払出:%lu", (unsigned long)stats->in_medal, (unsigned long)stats->out_medal);
        snprintf(line1, sizeof(line1), "ボーナス:%s", stats->bonus_active ? "ON" : "OFF");
        snprintf(line2, sizeof(line2), "ドア:%s エラー:%s", stats->door_open ? "ON" : "OFF", stats->error_active ? "ON" : "OFF");
        snprintf(line3, sizeof(line3), "警報:%s", (stats->door_open || stats->error_active) ? "ON" : "OFF");
        break;
    default:
        title = "警報と通信";
        snprintf(line0, sizeof(line0), "警報:%s ドア:%s", (stats->door_open || stats->error_active) ? "ON" : "OFF", stats->door_open ? "ON" : "OFF");
        snprintf(line1, sizeof(line1), "エラー:%s", stats->error_active ? "ON" : "OFF");
        snprintf(line2, sizeof(line2), "IP:%s", ip_addr ? ip_addr : "0.0.0.0");
        snprintf(line3, sizeof(line3), "差枚:%ld", (long)stats->diff_medal);
        break;
    case 3:
        title = "スランプグラフ";
        break;
    case 4:
        title = "ボーナス履歴";
        break;
    }

    u8g2_ClearBuffer(&s_u8g2);

    if (invert) {
        u8g2_DrawBox(&s_u8g2, 0, 0, OLED_WIDTH, OLED_HEIGHT);
        u8g2_SetDrawColor(&s_u8g2, 0);
    } else {
        u8g2_SetDrawColor(&s_u8g2, 1);
    }

    if (show_title) {
        u8g2_SetDrawColor(&s_u8g2, invert ? 0 : 1);
        u8g2_DrawBox(&s_u8g2, 0, 0, OLED_WIDTH, 16);
        u8g2_SetDrawColor(&s_u8g2, invert ? 1 : 0);
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, 0, 14, title);
        u8g2_SetDrawColor(&s_u8g2, invert ? 0 : 1);
    }

    u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
    if (s_page == 3) {
        // Slump graph page
        draw_slump_graph(stats, 0, 16, OLED_WIDTH, OLED_HEIGHT - 16);
    } else if (s_page == 4) {
        // Bonus history page
        draw_bonus_history(stats, 0, 16, OLED_WIDTH, OLED_HEIGHT - 16, !show_title);
    } else if (show_title) {
        draw_utf8_scrolling(0, 30, line0, now_us);
        draw_utf8_scrolling(1, 46, line1, now_us);
        draw_utf8_scrolling(2, 62, line2, now_us);
    } else {
        draw_utf8_scrolling(0, 14, line0, now_us);
        draw_utf8_scrolling(1, 30, line1, now_us);
        draw_utf8_scrolling(2, 46, line2, now_us);
        draw_utf8_scrolling(3, 62, line3, now_us);
    }

    if (now_us < s_reset_notification_until_us) {
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawBox(&s_u8g2, 0, 48, OLED_WIDTH, 16);
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, 8, 63, "リセット実行");
        u8g2_SetDrawColor(&s_u8g2, 1);
    }

    if (s_page == 0) {
        draw_wifi_icon(OLED_WIDTH - 16, 0, wifi_connected, wifi_rssi);
        
        // Bonus label blink (500ms invert per active label)
        if (stats->bonus_active && s_line_scroll_px[1] == 0) {
            bool bonus_blink = ((esp_timer_get_time() / 500000LL) & 1) != 0;
            if (bonus_blink) {
                char c1[16];
                snprintf(c1, sizeof(c1), "%lu", (unsigned long)stats->bonus1_count);
                u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
                int l1w = u8g2_GetUTF8Width(&s_u8g2, BONUS1_LABEL);
                int l2w = u8g2_GetUTF8Width(&s_u8g2, BONUS2_LABEL);
                int colon_w = u8g2_GetUTF8Width(&s_u8g2, ":");
                int c1w = u8g2_GetUTF8Width(&s_u8g2, c1);
                int sp_w = u8g2_GetUTF8Width(&s_u8g2, " ");
                int line1_y = show_title ? 46 : 30;
                int y_top = line1_y - 14;

                if (stats->bonus1_active) {
                    u8g2_DrawBox(&s_u8g2, 0, y_top, l1w, 16);
                    u8g2_SetDrawColor(&s_u8g2, 0);
                    u8g2_DrawUTF8(&s_u8g2, 0, line1_y, BONUS1_LABEL);
                    u8g2_SetDrawColor(&s_u8g2, 1);
                }
                if (stats->bonus2_active) {
                    int x2 = l1w + colon_w + c1w + sp_w;
                    u8g2_DrawBox(&s_u8g2, x2, y_top, l2w, 16);
                    u8g2_SetDrawColor(&s_u8g2, 0);
                    u8g2_DrawUTF8(&s_u8g2, x2, line1_y, BONUS2_LABEL);
                    u8g2_SetDrawColor(&s_u8g2, 1);
                }
            }
        }
        
        // NVS save indicator (bottom-right corner)
        if (s_nvs_saving) {
            u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
            u8g2_DrawStr(&s_u8g2, OLED_WIDTH - 40, OLED_HEIGHT - 2, "SAVE");
        } else if (s_nvs_countdown_ms > 0) {
            char buf[10];
            snprintf(buf, sizeof(buf), "%lus", (unsigned long)(s_nvs_countdown_ms / 1000));
            u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
            u8g2_DrawStr(&s_u8g2, OLED_WIDTH - 24, OLED_HEIGHT - 2, buf);
        }
    }

    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SendBuffer(&s_u8g2);
}

void display_manager_boot_done(void)
{
    s_boot_log_count = 0;
    s_startup_until_us = esp_timer_get_time() + 1000000;
    s_title_until_us = esp_timer_get_time() + TITLE_DISPLAY_US;
}

void display_manager_nvs_save_countdown(uint32_t remaining_ms)
{
    s_nvs_countdown_ms = remaining_ms;
    s_nvs_saving = false;
}

void display_manager_nvs_saving(void)
{
    s_nvs_saving = true;
    s_nvs_countdown_ms = 0;
}

void display_manager_nvs_save_done(void)
{
    s_nvs_saving = false;
    s_nvs_countdown_ms = 0;
}
