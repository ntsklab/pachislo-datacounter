idf_component_register(
    SRCS
        "app_main.c"
        "data_model.c"
        "input_monitor.c"
        "display_manager.c"
        "nvs_storage.c"
        "web_server.c"
        "wifi_manager.c"
    INCLUDE_DIRS
        "."
        "include"
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        freertos
        nvs_flash
        u8g2
)

# esp_libc adds its own -specs=...picolibc.specs but does not remove
# the toolchain-default -specs=picolibc.specs (short form).  The resulting
# duplicate causes "attempt to rename spec 'link' to already defined spec".
if(CONFIG_LIBC_PICOLIBC)
    idf_toolchain_remove_flags(COMPILE_OPTIONS "-specs=picolibc\.specs")
endif()
