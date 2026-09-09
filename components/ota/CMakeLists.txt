idf_component_register(
    SRCS "ota_server.cpp" "ota_secret.cpp" "ota_rollback.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server app_update firmware_info time_service mem_diag log freertos
)
