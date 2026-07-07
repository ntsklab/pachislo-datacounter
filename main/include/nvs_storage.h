#pragma once

#include <stdbool.h>
#include "data_model.h"

bool nvs_storage_init(void);
bool nvs_storage_load(counter_stats_t *out);
void nvs_storage_load_free(counter_stats_t *stats);
bool nvs_storage_save(const counter_stats_t *stats);
void nvs_storage_clear(void);
