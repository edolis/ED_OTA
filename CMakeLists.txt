idf_component_register(
    SRCS "ED_OTA.cpp"
        "lz4.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        app_update
        mbedtls
        driver
        ED_SYS
        ED_MQTT
)