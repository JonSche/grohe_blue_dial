idf_component_register(
    SRCS "provisioning_server.cpp" "provisioning_secret.cpp" "api_secret.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json grohe_ble time_service dial_api mem_diag log
)
