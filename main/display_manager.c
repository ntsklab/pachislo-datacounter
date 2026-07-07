#include "display_manager.h"

#include <stdio.h>
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
    
    // Decompress slump points
    int32_t running_diff = 0;
    
    // Pass 1: find min/max
    int32_t min_diff = 0, max_diff = 0;
    for (uint32_t i = 0; i < stats->slump_graph_count; i++) {
        running_diff += stats->slump_graph[i].delta;
        if (running_diff < min_diff) min_diff = running_diff;
        if (running_diff > max_diff) max_diff = running_diff;
    }
    
    int32_t range = max_diff - min_diff;
    if (range == 0) range = 1;
    
    // 縦軸のラベル表示エリア（左側20px）
    int label_width = 20;
    int graph_x = x + label_width;
    int graph_w = w - label_width;
    
    // 縦軸のラベル表示
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    char buf[10];
    
    // 最大値（上）
    snprintf(buf, sizeof(buf), "%ld", (long)max_diff);
    u8g2_DrawStr(&s_u8g2, x, y + 8, buf);
    
    // 最小値（下）
    snprintf(buf, sizeof(buf), "%ld", (long)min_diff);
    u8g2_DrawStr(&s_u8g2, x, y + h - 2, buf);
    
    // 0ラインの表示（範囲内にある場合）
    if (min_diff < 0 && max_diff > 0) {
        int zero_y = y + h - 1 - (int)((0 - min_diff) * (h - 2) / range);
        u8g2_DrawHLine(&s_u8g2, graph_x, zero_y, graph_w);
    }
    
    // グラフエリアの枠
    u8g2_DrawFrame(&s_u8g2, graph_x, y, graph_w, h);
    
    // ボーナス区間の反転表示
    running_diff = 0;
    for (uint32_t i = 0; i < stats->slump_graph_count; i++) {
        if (stats->slump_graph[i].flags & SLUMP_FLAG_BONUS) {
            int px = graph_x + 1 + (int)(i * (graph_w - 2) / (stats->slump_graph_count - 1));
            u8g2_DrawVLine(&s_u8g2, px, y + 1, h - 2);
        }
        running_diff += stats->slump_graph[i].delta;
    }
    
    // Draw graph lines
    uint32_t start_idx = 0;
    if (stats->slump_graph_count > (uint32_t)graph_w) {
        start_idx = stats->slump_graph_count - graph_w;
    }
    
    uint32_t point_count = stats->slump_graph_count - start_idx;
    
    running_diff = 0;
    int prev_x = 0, prev_y = 0;
    for (uint32_t i = 0; i < point_count; i++) {
        uint32_t idx = start_idx + i;
        
        // Accumulate delta up to this index
        if (i == 0) {
            running_diff = 0;
            for (uint32_t j = 0; j <= idx; j++) {
                running_diff += stats->slump_graph[j].delta;
            }
        } else {
            running_diff += stats->slump_graph[idx].delta;
        }
        
        int px = graph_x + 1 + (int)(i * (graph_w - 2) / (point_count - 1));
        int py = y + h - 1 - (int)((running_diff - min_diff) * (h - 2) / range);
        
        if (i > 0) {
            u8g2_DrawLine(&s_u8g2, prev_x, prev_y, px, py);
        }
        prev_x = px;
        prev_y = py;
    }
}

static void draw_bonus_history(const counter_stats_t *stats, int x, int y, int w, int h)
{
    if (stats->bonus_history_count == 0) {
        u8g2_SetFont(&s_u8g2, u8g2_font_unifont_t_japanese1);
        u8g2_DrawUTF8(&s_u8g2, x + w/2 - 30, y + h/2 + 5, "データなし");
        return;
    }
    
    // Find max interval for scaling (cap at 1000)
    uint32_t max_interval = 1000;
    for (uint32_t i = 0; i < stats->bonus_history_count; i++) {
        if (stats->bonus_history[i].interval_games > max_interval) {
            max_interval = stats->bonus_history[i].interval_games;
        }
    }
    if (max_interval == 0) max_interval = 1;
    
    // 下部のラベルエリア（16px）
    int graph_h = h - 16;
    
    // Draw border
    u8g2_DrawFrame(&s_u8g2, x, y, w, graph_h);
    
    // Draw bars
    uint32_t bar_width = (w - 2) / stats->bonus_history_count;
    if (bar_width < 2) bar_width = 2;
    if (bar_width > 8) bar_width = 8;
    
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    
    for (uint32_t i = 0; i < stats->bonus_history_count; i++) {
        uint32_t interval = stats->bonus_history[i].interval_games;
        if (interval > 1000) interval = 1000;
        
        int bar_height = (int)((uint64_t)interval * (graph_h - 4) / max_interval);
        if (bar_height < 1) bar_height = 1;
        
        int bar_x = x + 1 + (int)(i * (w - 2) / stats->bonus_history_count);
        int bar_y = y + graph_h - 2 - bar_height;
        
        // Draw bar with pattern based on bonus type
        if (stats->bonus_history[i].bonus_type == 1) {
            // BONUS1 - filled bar
            u8g2_DrawBox(&s_u8g2, bar_x, bar_y, bar_width - 1, bar_height);
        } else {
            // BONUS2 - outlined bar
            u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_width - 1, bar_height);
        }
        
        // ボーナス種別を棒の上にマーク表示
        char type_mark = (stats->bonus_history[i].bonus_type == 1) ? '1' : '2';
        char mark_str[2] = {type_mark, '\0'};
        u8g2_DrawStr(&s_u8g2, bar_x, bar_y - 2, mark_str);
        
        // ゲーム数を棒の下に表示（回転して表示）
        char num_buf[12];
        snprintf(num_buf, sizeof(num_buf), "%lu", (unsigned long)stats->bonus_history[i].interval_games);
        // 短い数字だけ表示
        if (strlen(num_buf) <= 4) {
            u8g2_DrawStr(&s_u8g2, bar_x, y + graph_h + 10, num_buf);
        } else {
            // 長い場合は省略
            snprintf(num_buf, sizeof(num_buf), "%lu", (unsigned long)(stats->bonus_history[i].interval_games / 100));
            u8g2_DrawStr(&s_u8g2, bar_x, y + graph_h + 10, num_buf);
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
        draw_bonus_history(stats, 0, 16, OLED_WIDTH, OLED_HEIGHT - 16);
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
