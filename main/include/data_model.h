#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

// Compressed slump point: 3 bytes. diff_medal delta (int16) + flags (is_bonus)
typedef struct {
    int16_t delta;
    uint8_t flags;
} slump_packed_t;

#define SLUMP_FLAG_BONUS 0x01

typedef struct {
    uint32_t games;
    int32_t diff_medal;
    bool is_bonus;
} slump_point_t;

// Uncompressed view for external consumers
typedef struct {
    uint16_t interval_games;  // max 65535 (理論上青天井でも十分)
    uint8_t bonus_type;
} bonus_history_entry_t;

typedef struct {
    // 累計カウンタ（uint32: 数百万枚・数十万ゲーム想定）
    uint32_t total_games;
    uint32_t total_games_including_bonus;
    uint32_t total_games_excluding_bonus;
    uint32_t in_medal;
    uint32_t out_medal;
    uint32_t bonus1_count;
    uint32_t bonus2_count;
    uint32_t bonus_total;
    int32_t diff_medal;
    
    // 状態フラグ
    bool bonus_active;
    bool bonus1_active;
    bool bonus2_active;
    bool door_open;
    bool error_active;
    
    // スランプグラフ（圧縮：delta int16 + flags, 3byte/point）
    slump_packed_t *slump_graph;
    uint16_t slump_graph_count;
    uint16_t slump_graph_capacity;
    
    // ボーナス履歴
    bonus_history_entry_t *bonus_history;
    uint16_t bonus_history_count;
} counter_stats_t;

typedef enum {
    COUNTER_EVENT_IN,
    COUNTER_EVENT_OUT,
    COUNTER_EVENT_BONUS1,
    COUNTER_EVENT_BONUS2
} counter_event_t;

void data_model_init(void);
void data_model_reset(void);
void data_model_on_event(counter_event_t event_type);
void data_model_check_wait_timeout(void);
void data_model_get_snapshot(counter_stats_t *out);
void data_model_set_bonus1_active(bool is_active);
void data_model_set_bonus2_active(bool is_active);
void data_model_set_door_open(bool is_open);
void data_model_set_error_active(bool is_active);

void data_model_set_stats(const counter_stats_t *stats);
bool data_model_is_dirty(void);
void data_model_clear_dirty(void);
uint32_t data_model_get_games_since_save(void);
void data_model_on_save(void);

// Read compressed slump graph: fills points[0..count-1], returns count
uint32_t data_model_read_slump(const counter_stats_t *stats, slump_point_t *points, uint32_t max_points);
