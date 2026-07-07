#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    SIGNAL_IN,
    SIGNAL_OUT,
    SIGNAL_BONUS1,
    SIGNAL_BONUS2,
    SIGNAL_DOOR,
    SIGNAL_ERROR,
    SIGNAL_PAGE_SWITCH,
} signal_type_t;

typedef struct {
    signal_type_t type;
    bool active;
} signal_event_t;

bool input_monitor_init(void);
QueueHandle_t input_monitor_get_queue(void);
