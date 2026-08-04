idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota app_update esp_http_client mbedtls firmware_info log
)
