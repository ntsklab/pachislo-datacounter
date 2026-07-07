#include "data_model.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static counter_stats_t s_stats;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_bonus1_active;
static bool s_bonus2_active;

static TickType_t s_last_in_medal_tick;
static uint32_t s_game_start_in_medal;
static bool s_has_pending_medal;
static uint32_t s_normal_games;
static uint32_t s_total_games;
static uint32_t s_games_since_last_bonus;
static uint8_t s_last_bonus_type;
static int32_t s_last_recorded_diff;  // 最後に記録した diff_medal
static bool s_dirty;
static uint32_t s_games_since_save;

static bool bonus_active_locked(void)
{
    return s_bonus1_active || s_bonus2_active;
}

static void recalc_diff_locked(void)
{
    s_stats.diff_medal = (int32_t)s_stats.out_medal - (int32_t)s_stats.in_medal;
    s_stats.bonus_active = bonus_active_locked();
    s_stats.bonus1_active = s_bonus1_active;
    s_stats.bonus2_active = s_bonus2_active;
    s_stats.total_games = s_normal_games;
    s_stats.total_games_including_bonus = s_total_games;
    s_stats.total_games_excluding_bonus = s_normal_games;
    s_dirty = true;
}

static void record_slump_point_locked(void)
{
    if (s_total_games == 0) return;
    if (s_stats.slump_graph_count >= s_stats.slump_graph_capacity) return;
    
    int32_t delta = s_stats.diff_medal - s_last_recorded_diff;
    if (delta > 32767) delta = 32767;
    if (delta < -32768) delta = -32768;
    
    s_stats.slump_graph[s_stats.slump_graph_count].delta = (int16_t)delta;
    s_stats.slump_graph[s_stats.slump_graph_count].flags = bonus_active_locked() ? SLUMP_FLAG_BONUS : 0;
    s_stats.slump_graph_count++;
    
    s_last_recorded_diff = s_stats.diff_medal;
}

static void count_game_locked(void)
{
    if (!s_has_pending_medal) {
        return;
    }

    uint32_t medals_this_game = s_stats.in_medal - s_game_start_in_medal;
    bool is_bonus = bonus_active_locked();
    
    uint32_t three_medal_games = medals_this_game / 3;
    uint32_t remainder = medals_this_game % 3;
    
    bool count_remainder = false;
    if (remainder == 1) {
        count_remainder = is_bonus ? COUNT_1MEDAL_AS_GAME_BONUS : COUNT_1MEDAL_AS_GAME_NORMAL;
    } else if (remainder == 2) {
        count_remainder = is_bonus ? COUNT_2MEDAL_AS_GAME_BONUS : COUNT_2MEDAL_AS_GAME_NORMAL;
    }
    
    uint32_t games_to_add = three_medal_games + (count_remainder ? 1 : 0);
    
    if (games_to_add > 0) {
        s_total_games += games_to_add;
        if (!is_bonus) {
            s_normal_games += games_to_add;
            s_games_since_last_bonus += games_to_add;
        }
        s_games_since_save += games_to_add;
        
        // ゲーム数加算後にスランプグラフを記録
        record_slump_point_locked();
    }
    
    s_has_pending_medal = false;
    s_game_start_in_medal = s_stats.in_medal;
}

void data_model_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    
    if (s_stats.slump_graph) {
        free(s_stats.slump_graph);
    }
    if (s_stats.bonus_history) {
        free(s_stats.bonus_history);
    }
    
    s_stats = (counter_stats_t){0};
    s_stats.slump_graph = calloc(SLUMP_GRAPH_MAX_POINTS, sizeof(slump_packed_t));
    s_stats.slump_graph_capacity = SLUMP_GRAPH_MAX_POINTS;
    s_stats.bonus_history = calloc(BONUS_HISTORY_MAX, sizeof(bonus_history_entry_t));
    
    s_bonus1_active = false;
    s_bonus2_active = false;
    s_last_in_medal_tick = 0;
    s_game_start_in_medal = 0;
    s_has_pending_medal = false;
    s_normal_games = 0;
    s_total_games = 0;
    s_games_since_last_bonus = 0;
    s_last_bonus_type = 0;
    s_last_recorded_diff = 0;
    s_dirty = false;
    s_games_since_save = 0;
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_reset(void)
{
    data_model_init();
}

void data_model_on_event(counter_event_t event_type)
{
    taskENTER_CRITICAL(&s_lock);
    switch (event_type) {
    case COUNTER_EVENT_IN:
        s_stats.in_medal += IN_MEDAL_PER_PULSE;
        s_last_in_medal_tick = xTaskGetTickCount();
        s_has_pending_medal = true;
        break;
    case COUNTER_EVENT_OUT:
        s_stats.out_medal += OUT_MEDAL_PER_PULSE;
        break;
    case COUNTER_EVENT_BONUS1:
        s_stats.bonus1_count += BONUS1_PER_PULSE;
        break;
    case COUNTER_EVENT_BONUS2:
        s_stats.bonus2_count += BONUS2_PER_PULSE;
        break;
    default:
        break;
    }
    s_stats.bonus_total = s_stats.bonus1_count + s_stats.bonus2_count;
    recalc_diff_locked();
    
    // IN/OUTメダル時にスランプグラフを記録
    if (event_type == COUNTER_EVENT_IN || event_type == COUNTER_EVENT_OUT) {
        record_slump_point_locked();
    }
    
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_check_wait_timeout(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_has_pending_medal && s_last_in_medal_tick > 0) {
        TickType_t elapsed = xTaskGetTickCount() - s_last_in_medal_tick;
        if (elapsed >= pdMS_TO_TICKS(GAME_WAIT_MS)) {
            count_game_locked();
            recalc_diff_locked();
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_set_bonus1_active(bool is_active)
{
    taskENTER_CRITICAL(&s_lock);
    bool was_bonus_active = bonus_active_locked();
    s_bonus1_active = is_active;
    if (was_bonus_active && !bonus_active_locked()) {
        // Bonus ended - record history
        if (s_stats.bonus_history_count < BONUS_HISTORY_MAX) {
            s_stats.bonus_history[s_stats.bonus_history_count].interval_games = s_games_since_last_bonus;
            s_stats.bonus_history[s_stats.bonus_history_count].bonus_type = s_last_bonus_type;
            s_stats.bonus_history_count++;
        } else {
            // Shift history left
            for (uint32_t i = 0; i < BONUS_HISTORY_MAX - 1; i++) {
                s_stats.bonus_history[i] = s_stats.bonus_history[i + 1];
            }
            s_stats.bonus_history[BONUS_HISTORY_MAX - 1].interval_games = s_games_since_last_bonus;
            s_stats.bonus_history[BONUS_HISTORY_MAX - 1].bonus_type = s_last_bonus_type;
        }
        s_normal_games = 0;
        s_has_pending_medal = false;
        s_game_start_in_medal = s_stats.in_medal;
        s_games_since_last_bonus = 0;
        
        // ボーナス終了時にスランプグラフを記録
        record_slump_point_locked();
    }
    if (is_active) {
        s_last_bonus_type = 1;
        // ボーナス開始時にスランプグラフを記録
        record_slump_point_locked();
    }
    recalc_diff_locked();
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_set_bonus2_active(bool is_active)
{
    taskENTER_CRITICAL(&s_lock);
    bool was_bonus_active = bonus_active_locked();
    s_bonus2_active = is_active;
    if (was_bonus_active && !bonus_active_locked()) {
        // Bonus ended - record history
        if (s_stats.bonus_history_count < BONUS_HISTORY_MAX) {
            s_stats.bonus_history[s_stats.bonus_history_count].interval_games = s_games_since_last_bonus;
            s_stats.bonus_history[s_stats.bonus_history_count].bonus_type = s_last_bonus_type;
            s_stats.bonus_history_count++;
        } else {
            // Shift history left
            for (uint32_t i = 0; i < BONUS_HISTORY_MAX - 1; i++) {
                s_stats.bonus_history[i] = s_stats.bonus_history[i + 1];
            }
            s_stats.bonus_history[BONUS_HISTORY_MAX - 1].interval_games = s_games_since_last_bonus;
            s_stats.bonus_history[BONUS_HISTORY_MAX - 1].bonus_type = s_last_bonus_type;
        }
        s_normal_games = 0;
        s_has_pending_medal = false;
        s_game_start_in_medal = s_stats.in_medal;
        s_games_since_last_bonus = 0;
        
        // ボーナス終了時にスランプグラフを記録
        record_slump_point_locked();
    }
    if (is_active) {
        s_last_bonus_type = 2;
        // ボーナス開始時にスランプグラフを記録
        record_slump_point_locked();
    }
    recalc_diff_locked();
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_get_snapshot(counter_stats_t *out)
{
    if (!out) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_set_door_open(bool is_open)
{
    taskENTER_CRITICAL(&s_lock);
    s_stats.door_open = is_open;
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_set_error_active(bool is_active)
{
    taskENTER_CRITICAL(&s_lock);
    s_stats.error_active = is_active;
    taskEXIT_CRITICAL(&s_lock);
}

void data_model_set_stats(const counter_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL(&s_lock);
    
    // Copy scalar fields
    s_stats.total_games = stats->total_games;
    s_stats.total_games_including_bonus = stats->total_games_including_bonus;
    s_stats.total_games_excluding_bonus = stats->total_games_excluding_bonus;
    s_stats.in_medal = stats->in_medal;
    s_stats.out_medal = stats->out_medal;
    s_stats.bonus1_count = stats->bonus1_count;
    s_stats.bonus2_count = stats->bonus2_count;
    s_stats.bonus_total = stats->bonus_total;
    s_stats.diff_medal = stats->diff_medal;
    s_stats.bonus_active = stats->bonus_active;
    s_stats.bonus1_active = stats->bonus1_active;
    s_stats.bonus2_active = stats->bonus2_active;
    s_stats.door_open = stats->door_open;
    s_stats.error_active = stats->error_active;
    
    // Copy slump graph data
    if (s_stats.slump_graph && stats->slump_graph) {
        uint32_t count = stats->slump_graph_count;
        if (count > s_stats.slump_graph_capacity) {
            count = s_stats.slump_graph_capacity;
        }
        memcpy(s_stats.slump_graph, stats->slump_graph, count * sizeof(slump_packed_t));
        s_stats.slump_graph_count = count;
    }
    
    // Copy bonus history data
    if (s_stats.bonus_history && stats->bonus_history) {
        uint32_t count = stats->bonus_history_count;
        if (count > BONUS_HISTORY_MAX) {
            count = BONUS_HISTORY_MAX;
        }
        memcpy(s_stats.bonus_history, stats->bonus_history, count * sizeof(bonus_history_entry_t));
        s_stats.bonus_history_count = count;
    }
    
    s_dirty = false;
    s_games_since_save = 0;
    taskEXIT_CRITICAL(&s_lock);
}

bool data_model_is_dirty(void)
{
    bool dirty;
    taskENTER_CRITICAL(&s_lock);
    dirty = s_dirty;
    taskEXIT_CRITICAL(&s_lock);
    return dirty;
}

void data_model_clear_dirty(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_dirty = false;
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t data_model_get_games_since_save(void)
{
    uint32_t games;
    taskENTER_CRITICAL(&s_lock);
    games = s_games_since_save;
    taskEXIT_CRITICAL(&s_lock);
    return games;
}

void data_model_on_save(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_dirty = false;
    s_games_since_save = 0;
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t data_model_read_slump(const counter_stats_t *stats, slump_point_t *points, uint32_t max_points)
{
    if (!stats || !points || max_points == 0) return 0;
    
    uint32_t count = stats->slump_graph_count;
    if (count > max_points) count = max_points;
    
    int32_t running_diff = 0;
    for (uint32_t i = 0; i < count; i++) {
        running_diff += stats->slump_graph[i].delta;
        points[i].games = i + 1;
        points[i].diff_medal = running_diff;
        points[i].is_bonus = (stats->slump_graph[i].flags & SLUMP_FLAG_BONUS) != 0;
    }
    
    return count;
}
