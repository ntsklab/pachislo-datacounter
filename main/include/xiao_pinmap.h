#pragma once

#include "driver/gpio.h"

/*
 * XIAO pin-to-GPIO mapping for ESP32-C3 and ESP32-C6.
 *
 * Both boards share the same D0..D10 header labels but have different
 * underlying GPIO numbers.  Use XIAO_Dx in pin assignments so the same
 * app_config.h works on either target without modification.
 */

#if defined(CONFIG_IDF_TARGET_ESP32C3)
    /* XIAO ESP32-C3 */
    #define XIAO_D0  GPIO_NUM_2
    #define XIAO_D1  GPIO_NUM_3
    #define XIAO_D2  GPIO_NUM_4
    #define XIAO_D3  GPIO_NUM_5
    #define XIAO_D4  GPIO_NUM_6
    #define XIAO_D5  GPIO_NUM_7
    #define XIAO_D6  GPIO_NUM_21
    #define XIAO_D7  GPIO_NUM_20
    #define XIAO_D8  GPIO_NUM_8
    #define XIAO_D9  GPIO_NUM_9
    #define XIAO_D10 GPIO_NUM_10

#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    /* XIAO ESP32-C6 */
    #define XIAO_D0  GPIO_NUM_0
    #define XIAO_D1  GPIO_NUM_1
    #define XIAO_D2  GPIO_NUM_2
    #define XIAO_D3  GPIO_NUM_21
    #define XIAO_D4  GPIO_NUM_22
    #define XIAO_D5  GPIO_NUM_23
    #define XIAO_D6  GPIO_NUM_16
    #define XIAO_D7  GPIO_NUM_17
    #define XIAO_D8  GPIO_NUM_19
    #define XIAO_D9  GPIO_NUM_20
    #define XIAO_D10 GPIO_NUM_18

#else
    #error "Unsupported ESP32 target. Only ESP32-C3 and ESP32-C6 are supported."
#endif
