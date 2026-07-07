#include "input_monitor.h"

#include <stddef.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Relay contact input handler — periodic sampling + voting debounce.
 *
 * Samples all inputs every SAMPLE_INTERVAL_US and applies an integral
 * (voting) debounce filter.  A state change is confirmed only when the
 * raw level remains in the new state for N consecutive samples, where
 *
 *   N = ceil(debounce_ms / SAMPLE_INTERVAL_MS)
 *
 * This is the standard industrial approach for relay / dry-contact inputs.
 * Edge-triggered interrupts are NOT used — they are unreliable with long
 * bounce chains and electrical noise.
 */

#define SAMPLE_INTERVAL_US 5000
#define SAMPLE_INTERVAL_MS 5

/* ------------------------------------------------------------------ */

typedef struct {
    int pin;
    signal_type_t type;
    uint32_t debounce_ms;

    /* voting debounce state */
    uint16_t threshold;       /* consecutive samples needed to confirm */
    uint16_t count;           /* current consecutive samples matching target */
    bool     target;          /* level we are debouncing toward */
    bool     confirmed;       /* last confirmed (output) level */
} input_sampler_t;

static const char *TAG = "input_monitor";
static QueueHandle_t s_event_queue = NULL;
static esp_timer_handle_t s_timer = NULL;

static input_sampler_t s_inputs[] = {
    {.pin = PIN_SIG_IN_MEDAL,    .type = SIGNAL_IN,        .debounce_ms = DEBOUNCE_IN_MS},
    {.pin = PIN_SIG_OUT_MEDAL,   .type = SIGNAL_OUT,       .debounce_ms = DEBOUNCE_OUT_MS},
    {.pin = PIN_SIG_BONUS1,      .type = SIGNAL_BONUS1,    .debounce_ms = DEBOUNCE_BONUS1_MS},
    {.pin = PIN_SIG_BONUS2,      .type = SIGNAL_BONUS2,    .debounce_ms = DEBOUNCE_BONUS2_MS},
    {.pin = PIN_SIG_DOOR,        .type = SIGNAL_DOOR,      .debounce_ms = DEBOUNCE_DOOR_MS},
    {.pin = PIN_SIG_ERROR,       .type = SIGNAL_ERROR,     .debounce_ms = DEBOUNCE_ERROR_MS},
    {.pin = PIN_BTN_PAGE_SWITCH, .type = SIGNAL_PAGE_SWITCH, .debounce_ms = DEBOUNCE_BUTTON_MS},
};

#define INPUT_COUNT (sizeof(s_inputs) / sizeof(s_inputs[0]))

/* ------------------------------------------------------------------ */

static void sample_timer_cb(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < INPUT_COUNT; i++) {
        if (s_inputs[i].pin < 0) {
            continue;
        }

        bool raw = (gpio_get_level(s_inputs[i].pin) == 0);

        if (raw == s_inputs[i].target) {
            if (s_inputs[i].count < s_inputs[i].threshold) {
                s_inputs[i].count++;
            }
        } else {
            s_inputs[i].count = 0;
            s_inputs[i].target = raw;
        }

        if (s_inputs[i].count >= s_inputs[i].threshold
            && s_inputs[i].confirmed != s_inputs[i].target) {

            s_inputs[i].confirmed = s_inputs[i].target;

            signal_event_t evt = {
                .type   = s_inputs[i].type,
                .active = s_inputs[i].target,
            };
            xQueueSend(s_event_queue, &evt, 0);
        }
    }
}

/* ------------------------------------------------------------------ */

bool input_monitor_init(void)
{
    s_event_queue = xQueueCreate(64, sizeof(signal_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "event queue create failed");
        return false;
    }

    for (size_t i = 0; i < INPUT_COUNT; i++) {
        if (s_inputs[i].pin < 0) {
            continue;
        }

        /* Configure as input with pull-up, NO interrupts. */
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_inputs[i].pin,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        /* Read initial level. */
        bool level = (gpio_get_level(s_inputs[i].pin) == 0);
        s_inputs[i].target    = level;
        s_inputs[i].confirmed = level;
        s_inputs[i].count     = 0;
        s_inputs[i].threshold = (s_inputs[i].debounce_ms
                                 + SAMPLE_INTERVAL_MS - 1)
                                / SAMPLE_INTERVAL_MS;

        if (s_inputs[i].threshold < 1) {
            s_inputs[i].threshold = 1;
        }

        /* Queue init event so the consumer knows the starting state. */
        signal_event_t init_evt = {
            .type   = s_inputs[i].type,
            .active = level,
        };
        xQueueSend(s_event_queue, &init_evt, 0);
    }

    /* Start periodic sampling timer. */
    const esp_timer_create_args_t timer_args = {
        .callback = sample_timer_cb,
        .name     = "input_sampler",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, SAMPLE_INTERVAL_US));

    ESP_LOGI(TAG, "input monitor initialized (%u inputs, %d ms interval)",
             (unsigned)INPUT_COUNT, SAMPLE_INTERVAL_MS);
    return true;
}

QueueHandle_t input_monitor_get_queue(void)
{
    return s_event_queue;
}
