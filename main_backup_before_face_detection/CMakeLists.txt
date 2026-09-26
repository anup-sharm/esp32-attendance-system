idf_component_register(
    SRCS
        "main.c"
        "web/web_server.c"
        "wifi/wifi_manager.c"
        "camera/camera.c"
        "display/tft.c"

    INCLUDE_DIRS
        "."
        "web"
        "wifi"
        "camera"
        "display"

    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp32-camera
        esp_driver_gpio
        esp_driver_spi
)