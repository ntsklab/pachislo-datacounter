#include <stdbool.h>
#include <stdlib.h>

#include "app_config.h"
#include "data_model.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "input_monitor.h"
#include "nvs_storage.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "app_main";

static void poll_level_signals(void)
{
    data_model_set_door_open(gpio_get_level(PIN_SIG_DOOR) == 0);
    data_model_set_error_active(gpio_get_level(PIN_SIG_ERROR) == 0);
}

static void signal_consumer_task(void *arg)
{
    (void)arg;
    QueueHandle_t queue = input_monitor_get_queue();
    signal_event_t event;
    bool in_initialized = false;
    bool out_initialized = false;
    bool bonus1_state_initialized = false;
    bool bonus2_state_initialized = false;
    bool bonus1_last_active = false;
    bool bonus2_last_active = false;
    bool button_pressed = false;
    bool button_reset_done = false;
    TickType_t button_press_tick = 0;
    TickType_t last_signal_tick = xTaskGetTickCount();

    while (true) {
        if (xQueueReceive(queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            last_signal_tick = xTaskGetTickCount();
            switch (event.type) {
            case SIGNAL_IN:
                if (!in_initialized) {
                    in_initialized = true;
                    break;
                }
                if (event.active) {
                    data_model_on_event(COUNTER_EVENT_IN);
                }
                break;
            case SIGNAL_OUT:
                if (!out_initialized) {
                    out_initialized = true;
                    break;
                }
                if (event.active) {
                    data_model_on_event(COUNTER_EVENT_OUT);
                }
                break;
            case SIGNAL_BONUS1:
                if (!bonus1_state_initialized) {
                    bonus1_last_active = event.active;
                    bonus1_state_initialized = true;
                    data_model_set_bonus1_active(event.active);
                    break;
                }
                if (event.active != bonus1_last_active) {
                    bool rising = (!bonus1_last_active && event.active);
                    bonus1_last_active = event.active;
                    data_model_set_bonus1_active(event.active);
                    if (rising) {
                        data_model_on_event(COUNTER_EVENT_BONUS1);
                    }
                }
                break;
            case SIGNAL_BONUS2:
                if (!bonus2_state_initialized) {
                    bonus2_last_active = event.active;
                    bonus2_state_initialized = true;
                    data_model_set_bonus2_active(event.active);
                    break;
                }
                if (event.active != bonus2_last_active) {
                    bool rising = (!bonus2_last_active && event.active);
                    bonus2_last_active = event.active;
                    data_model_set_bonus2_active(event.active);
                    if (rising) {
                        data_model_on_event(COUNTER_EVENT_BONUS2);
                    }
                }
                break;
            case SIGNAL_DOOR:
                data_model_set_door_open(event.active);
                break;
            case SIGNAL_ERROR:
                data_model_set_error_active(event.active);
                break;
            case SIGNAL_PAGE_SWITCH:
                if (event.active) {
                    if (!button_pressed) {
                        button_pressed = true;
                        button_reset_done = false;
                        button_press_tick = xTaskGetTickCount();
                    }
                } else {
                    if (button_pressed && !button_reset_done && display_manager_is_interactive()) {
                        display_manager_next_page();
                    }
                    button_pressed = false;
                }
                break;
            default:
                break;
            }
        }

        if (button_pressed && !button_reset_done) {
            TickType_t duration = xTaskGetTickCount() - button_press_tick;
            if (duration >= pdMS_TO_TICKS(PAGE_BUTTON_LONG_PRESS_MS)) {
                data_model_init();
                nvs_storage_clear();
                button_reset_done = true;
                ESP_LOGI(TAG, "factory reset (long press)");
            }
        }

        poll_level_signals();
        data_model_check_wait_timeout();

        TickType_t now = xTaskGetTickCount();
        TickType_t idle_ms = (now - last_signal_tick) * portTICK_PERIOD_MS;

        if (data_model_is_dirty()) {
            bool urgent = (data_model_get_games_since_save() >= NVS_SAVE_INTERVAL_GAMES);
            uint32_t delay_ms = urgent ? NVS_SAVE_DELAY_URGENT_MS : NVS_SAVE_DELAY_IDLE_MS;

            if ((uint32_t)idle_ms >= delay_ms) {
                display_manager_nvs_saving();
                counter_stats_t *stats = malloc(sizeof(counter_stats_t));
                if (stats) {
                    data_model_get_snapshot(stats);
                    if (nvs_storage_save(stats)) {
                        data_model_on_save();
                        ESP_LOGI(TAG, "NVS saved (idle=%lums)", (unsigned long)idle_ms);
                    }
                    free(stats);
                }
                display_manager_nvs_save_done();
            } else {
                display_manager_nvs_save_countdown(delay_ms - (uint32_t)idle_ms);
            }
        }
    }
}

static void display_task(void *arg)
{
    (void)arg;
    counter_stats_t *stats = malloc(sizeof(counter_stats_t));
    if (!stats) {
        ESP_LOGE(TAG, "display_task: malloc failed");
        vTaskDelete(NULL);
        return;
    }
    char ip[32] = {0};

    while (true) {
        data_model_get_snapshot(stats);
        wifi_manager_get_ip_str(ip, sizeof(ip));
        display_manager_render(stats, ip, wifi_manager_is_connected(), wifi_manager_get_rssi());
        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_MS));
    }
}

void app_main(void)
{
    // 初期化中のWDTタイムアウトを30秒に延長
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = (1 << 0),
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);

    // OLED初期化を最初に行う
    if (!display_manager_init()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    
    display_manager_boot_log("NVS初期化中...");
    if (!nvs_storage_init()) {
        ESP_LOGE(TAG, "NVS init failed");
        display_manager_boot_log("NVS初期化失敗");
        return;
    }
    display_manager_boot_log("NVS OK");
    vTaskDelay(pdMS_TO_TICKS(10));

    data_model_init();

    display_manager_boot_log("データ復元中...");
    counter_stats_t *loaded_stats = calloc(1, sizeof(counter_stats_t));
    if (loaded_stats && nvs_storage_load(loaded_stats)) {
        data_model_set_stats(loaded_stats);
        ESP_LOGI(TAG, "Restored from NVS");
        display_manager_boot_log("データ復元完了");
    } else {
        display_manager_boot_log("新規データ");
    }
    if (loaded_stats) {
        nvs_storage_load_free(loaded_stats);
        free(loaded_stats);
    }
    vTaskDelay(pdMS_TO_TICKS(10));

#if WOKWI_DISABLE_WIRELESS
    ESP_LOGI(TAG, "wireless disabled for Wokwi");
    display_manager_boot_log("WiFi: Wokwi無効");
#else
    display_manager_boot_log("WiFi接続中...");
    if (wifi_manager_init_sta()) {
        display_manager_boot_log("WiFi接続完了");
    } else {
        display_manager_boot_log("WiFi接続失敗");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    display_manager_boot_log("入力監視開始...");
    if (!input_monitor_init()) {
        ESP_LOGE(TAG, "input monitor init failed");
        display_manager_boot_log("入力監視失敗");
        return;
    }
    display_manager_boot_log("入力監視OK");
    vTaskDelay(pdMS_TO_TICKS(10));

#if !WOKWI_DISABLE_WIRELESS
    display_manager_boot_log("Webサーバー開始...");
    if (!web_server_start()) {
        ESP_LOGW(TAG, "web server start failed");
        display_manager_boot_log("Webサーバー失敗");
    } else {
        display_manager_boot_log("WebサーバーOK");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    xTaskCreate(signal_consumer_task, "signal_consumer", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "pachislo data counter started");
    display_manager_boot_log("起動完了");

    // ブートログ表示時間を確保
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ブートログをクリアして通常表示に移行
    display_manager_boot_done();

    // 通常表示タスクを起動
    xTaskCreate(display_task, "display_task", 4096, NULL, 5, NULL);

    // 通常運用時のWDTタイムアウトに戻す
    wdt_config.timeout_ms = 5000;
    esp_task_wdt_reconfigure(&wdt_config);
}
