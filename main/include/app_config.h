#pragma once

#include <stdint.h>

#include "xiao_pinmap.h"

/*
 * Wiring selection: 0 = standard (see below), 1 = alternate dev machine.
 */
#define USE_ALT_WIRING 0

/*
 * Pin assignment (XIAO Dx notation — works for both ESP32-C3 and ESP32-C6)
 * - All slot-machine signal inputs are OPEN-DRAIN assumption.
 * - Internal pull-up is always enabled in software.
 * - Set to -1 for unused signal pins.
 *
 * --- Standard wiring (USE_ALT_WIRING = 0) ---
 *   D0  = IN medal pulse     D5  = ERROR switch
 *   D1  = OUT medal pulse     D6  = PAGE button
 *   D2  = BONUS1 switch       D9  = OLED SDA
 *   D3  = BONUS2 switch       D10 = OLED SCL
 *   D4  = DOOR switch
 *
 * --- Alternate wiring (USE_ALT_WIRING = 1) ---
 *   D0  = DOOR                D5  = N/C
 *   D1  = MEDAL-IN            D6  = isERROR
 *   D2  = MEDAL-OUT           D7  = PAGE button
 *   D3  = isAT (BONUS2)       D8  = OLED SCL
 *   D4  = isBonus (BONUS1)    D9  = OLED SDA
 */
#if USE_ALT_WIRING == 0

#define PIN_SIG_IN_MEDAL     XIAO_D0
#define PIN_SIG_OUT_MEDAL    XIAO_D1
#define PIN_SIG_BONUS1       XIAO_D2
#define PIN_SIG_BONUS2       XIAO_D3
#define PIN_SIG_DOOR         XIAO_D4
#define PIN_SIG_ERROR        XIAO_D5

#define PIN_BTN_PAGE_SWITCH  XIAO_D6

#define PIN_I2C_SDA          XIAO_D9
#define PIN_I2C_SCL          XIAO_D10

#else
//開発機という名のえとたま用
#define PIN_SIG_IN_MEDAL     XIAO_D1
#define PIN_SIG_OUT_MEDAL    XIAO_D2
#define PIN_SIG_BONUS1       XIAO_D4
#define PIN_SIG_BONUS2       XIAO_D3
#define PIN_SIG_DOOR         XIAO_D0
#define PIN_SIG_ERROR        XIAO_D6

#define PIN_BTN_PAGE_SWITCH  XIAO_D7

#define PIN_I2C_SDA          XIAO_D9
#define PIN_I2C_SCL          XIAO_D10

#endif

/* OLED (SSD1306/SH1106-compatible I2C) */
#define OLED_I2C_ADDR        0x3C
#define OLED_WIDTH           128
#define OLED_HEIGHT          64
#define OLED_ROTATION_180    0

/* Debounce parameters [ms]（リレーチャタリング対策） */
#define DEBOUNCE_IN_MS       10
#define DEBOUNCE_OUT_MS      10
#define DEBOUNCE_BONUS1_MS   10
#define DEBOUNCE_BONUS2_MS   10
#define DEBOUNCE_DOOR_MS     10
#define DEBOUNCE_ERROR_MS    10
#define DEBOUNCE_BUTTON_MS   10

/* Pulse->counter mapping (machine profile default) */
#define IN_MEDAL_PER_PULSE   1
#define OUT_MEDAL_PER_PULSE  1
#define BONUS1_PER_PULSE     1
#define BONUS2_PER_PULSE     1

/*
 * Bonus signal labels shown in OLED/WebUI.
 * Examples:
 *   BONUS1_LABEL: "BONUS", "REG", "N/A"
 *   BONUS2_LABEL: "RB", "AT", "ART"
 */
#define BONUS1_LABEL         "BONUS"
#define BONUS2_LABEL         "AT"

/* Game count uses IN medal pulses only */
#define IN_MEDALS_PER_GAME             3

/*
 * Game counting strategy:
 * - Uses wait detection: if no medal input for GAME_WAIT_MS, count as 1 game
 * - Configure whether 1-medal/2-medal inputs count as games in normal/bonus modes
 * - GAME_WAIT_MS must be < 4100ms (slot machine wait time is 4.1s by regulation)
 */
#define GAME_WAIT_MS                   500
#define COUNT_1MEDAL_AS_GAME_NORMAL    0
#define COUNT_2MEDAL_AS_GAME_NORMAL    0
#define COUNT_1MEDAL_AS_GAME_BONUS     0
#define COUNT_2MEDAL_AS_GAME_BONUS     0

/* Web UI / Wi-Fi */
#include "wifi_secrets.h"
#define WOKWI_DISABLE_WIRELESS 0
#define WIFI_MAX_RETRY       10
#define WEB_SERVER_PORT      80

/* Page button long-press reset (ms) */
#define PAGE_BUTTON_LONG_PRESS_MS 3000

/* Display refresh */
#define OLED_REFRESH_MS      50

/* NVS save strategy */
#define NVS_SAVE_INTERVAL_MINUTES   30
#define NVS_SAVE_INTERVAL_GAMES     100
#define NVS_SAVE_DELAY_IDLE_MS      30000
#define NVS_SAVE_DELAY_URGENT_MS    10000

#define SLUMP_GRAPH_MAX_POINTS      40000
#define BONUS_HISTORY_MAX           32
