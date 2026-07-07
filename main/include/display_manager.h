#pragma once

#include <stdbool.h>

#include "data_model.h"

bool display_manager_init(void);
bool display_manager_is_interactive(void);
void display_manager_next_page(void);
void display_manager_render(const counter_stats_t *stats, const char *ip_addr, bool wifi_connected, int wifi_rssi);
void display_manager_notify_reset(void);
void display_manager_boot_log(const char *msg);
void display_manager_boot_done(void);
void display_manager_nvs_save_countdown(uint32_t remaining_ms);
void display_manager_nvs_saving(void);
void display_manager_nvs_save_done(void);
