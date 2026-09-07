idf_component_register(
    SRCS "main.cc" "radio_catalog.cc" "radio_display_settings.cc"
         "radio_easter_egg.cc"
         "radio_player.cc" "radio_sleep_timer.cc"
         "radio_ui.cc"
         "radio_font.c" "radio_font_title.c" "fap_screenshot.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp
        nvs_flash
        esp_http_client
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_lcd
        esp_partition
        esp_rom
        spi_flash
        mbedtls
        json
        esp_audio_codec
        esp_driver_usb_serial_jtag
        esp-wifi-connect
)
