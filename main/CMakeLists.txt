set(babelbus_srcs
    "capture.c"
    "capture_hw.c"
    "capture_mjpeg.c"
    "http_server.c"
    "ethernet.c"
    "jpeg_frame.c"
    "i2s_audio.c"
    "wifi_net.c"
    "main.c"
    "tc358743.c"
)

idf_component_register(
    SRCS ${babelbus_srcs}
    INCLUDE_DIRS "."
    EMBED_FILES
        "index.html"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_cam
        esp_driver_isp
        esp_driver_i2s
        esp_http_server
        esp_eth
        esp_netif
        mdns
        esp_event
        nvs_flash
        esp_hw_support
        esp_psram
        esp_driver_jpeg
        esp_mm
        esp_timer
)
