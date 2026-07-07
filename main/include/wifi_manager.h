#pragma once

#include <stdbool.h>
#include <stddef.h>

bool wifi_manager_init_sta(void);
void wifi_manager_get_ip_str(char *buf, size_t len);
bool wifi_manager_is_connected(void);
int wifi_manager_get_rssi(void);
