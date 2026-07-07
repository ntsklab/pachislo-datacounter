#include "nvs_storage.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "data_model.h"

static const char *TAG = "nvs_storage";
static const char *NVS_NAMESPACE = "pachislo";

static const char *KEY_IN_MEDAL = "in_medal";
static const char *KEY_OUT_MEDAL = "out_medal";
static const char *KEY_BONUS1_COUNT = "bonus1_count";
static const char *KEY_BONUS2_COUNT = "bonus2_count";
static const char *KEY_NORMAL_GAMES = "normal_games";
static const char *KEY_TOTAL_GAMES = "total_games";
static const char *KEY_SLUMP_GRAPH = "slump_graph";
static const char *KEY_BONUS_HISTORY = "bonus_hst";

bool nvs_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition invalid, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool nvs_storage_load(counter_stats_t *out)
{
    if (!out) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved data found");
        return false;
    }

    uint32_t val;
    bool loaded = false;

    if (nvs_get_u32(handle, KEY_IN_MEDAL, &val) == ESP_OK) {
        out->in_medal = val;
        loaded = true;
    }
    if (nvs_get_u32(handle, KEY_OUT_MEDAL, &val) == ESP_OK) {
        out->out_medal = val;
        loaded = true;
    }
    if (nvs_get_u32(handle, KEY_BONUS1_COUNT, &val) == ESP_OK) {
        out->bonus1_count = val;
        loaded = true;
    }
    if (nvs_get_u32(handle, KEY_BONUS2_COUNT, &val) == ESP_OK) {
        out->bonus2_count = val;
        loaded = true;
    }
    if (nvs_get_u32(handle, KEY_NORMAL_GAMES, &val) == ESP_OK) {
        out->total_games = val;
        loaded = true;
    }
    if (nvs_get_u32(handle, KEY_TOTAL_GAMES, &val) == ESP_OK) {
        out->total_games_including_bonus = val;
        loaded = true;
    }
    
    // Load slump graph
    out->slump_graph = calloc(SLUMP_GRAPH_MAX_POINTS, sizeof(slump_packed_t));
    if (out->slump_graph) {
        size_t graph_size = SLUMP_GRAPH_MAX_POINTS * sizeof(slump_packed_t);
        size_t actual_size = graph_size;
        if (nvs_get_blob(handle, KEY_SLUMP_GRAPH, out->slump_graph, &actual_size) == ESP_OK) {
            out->slump_graph_count = actual_size / sizeof(slump_packed_t);
            loaded = true;
        }
    }
    
    // Load bonus history
    out->bonus_history = calloc(BONUS_HISTORY_MAX, sizeof(bonus_history_entry_t));
    if (out->bonus_history) {
        size_t history_size = BONUS_HISTORY_MAX * sizeof(bonus_history_entry_t);
        size_t history_actual_size = history_size;
        if (nvs_get_blob(handle, KEY_BONUS_HISTORY, out->bonus_history, &history_actual_size) == ESP_OK) {
            out->bonus_history_count = history_actual_size / sizeof(bonus_history_entry_t);
            loaded = true;
        }
    }

    out->diff_medal = (int32_t)out->out_medal - (int32_t)out->in_medal;
    out->bonus_total = out->bonus1_count + out->bonus2_count;
    out->total_games_excluding_bonus = out->total_games;

    nvs_close(handle);

    if (loaded) {
        ESP_LOGI(TAG, "Loaded: in=%lu out=%lu b1=%lu b2=%lu games=%lu",
                 (unsigned long)out->in_medal,
                 (unsigned long)out->out_medal,
                 (unsigned long)out->bonus1_count,
                 (unsigned long)out->bonus2_count,
                 (unsigned long)out->total_games);
    }
    return loaded;
}

bool nvs_storage_save(const counter_stats_t *stats)
{
    if (!stats) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    bool changed = false;
    uint32_t old_val;

    if (nvs_get_u32(handle, KEY_IN_MEDAL, &old_val) != ESP_OK || old_val != stats->in_medal) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_IN_MEDAL, stats->in_medal));
        changed = true;
    }
    if (nvs_get_u32(handle, KEY_OUT_MEDAL, &old_val) != ESP_OK || old_val != stats->out_medal) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_OUT_MEDAL, stats->out_medal));
        changed = true;
    }
    if (nvs_get_u32(handle, KEY_BONUS1_COUNT, &old_val) != ESP_OK || old_val != stats->bonus1_count) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_BONUS1_COUNT, stats->bonus1_count));
        changed = true;
    }
    if (nvs_get_u32(handle, KEY_BONUS2_COUNT, &old_val) != ESP_OK || old_val != stats->bonus2_count) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_BONUS2_COUNT, stats->bonus2_count));
        changed = true;
    }
    if (nvs_get_u32(handle, KEY_NORMAL_GAMES, &old_val) != ESP_OK || old_val != stats->total_games) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_NORMAL_GAMES, stats->total_games));
        changed = true;
    }
    if (nvs_get_u32(handle, KEY_TOTAL_GAMES, &old_val) != ESP_OK || old_val != stats->total_games_including_bonus) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, KEY_TOTAL_GAMES, stats->total_games_including_bonus));
        changed = true;
    }
    
    // Save slump graph
    if (stats->slump_graph_count > 0) {
        size_t graph_size = stats->slump_graph_count * sizeof(slump_packed_t);
        size_t old_size = 0;
        esp_err_t err = nvs_get_blob(handle, KEY_SLUMP_GRAPH, NULL, &old_size);
        if (err != ESP_OK || old_size != graph_size || changed) {
            ESP_ERROR_CHECK(nvs_set_blob(handle, KEY_SLUMP_GRAPH, stats->slump_graph, graph_size));
            changed = true;
        }
    }
    
    // Save bonus history
    if (stats->bonus_history_count > 0) {
        size_t history_size = stats->bonus_history_count * sizeof(bonus_history_entry_t);
        size_t old_history_size = 0;
        esp_err_t err = nvs_get_blob(handle, KEY_BONUS_HISTORY, NULL, &old_history_size);
        if (err != ESP_OK || old_history_size != history_size || changed) {
            ESP_ERROR_CHECK(nvs_set_blob(handle, KEY_BONUS_HISTORY, stats->bonus_history, history_size));
            changed = true;
        }
    }

    if (changed) {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
            nvs_close(handle);
            return false;
        }
        ESP_LOGI(TAG, "Saved: in=%lu out=%lu b1=%lu b2=%lu games=%lu",
                 (unsigned long)stats->in_medal,
                 (unsigned long)stats->out_medal,
                 (unsigned long)stats->bonus1_count,
                 (unsigned long)stats->bonus2_count,
                 (unsigned long)stats->total_games);
    }

    nvs_close(handle);
    return true;
}

void nvs_storage_load_free(counter_stats_t *stats)
{
    if (!stats) return;
    if (stats->slump_graph) {
        free(stats->slump_graph);
        stats->slump_graph = NULL;
    }
    if (stats->bonus_history) {
        free(stats->bonus_history);
        stats->bonus_history = NULL;
    }
}

void nvs_storage_clear(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "NVS cleared");
    }
}
