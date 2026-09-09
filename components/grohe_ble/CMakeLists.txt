idf_component_register(
    SRCS "ble_manager.cpp" "grohe_client.cpp" "grohe_protocol.cpp"
         "grohe_auth.cpp" "grohe_credentials.cpp" "nvs_credentials_provider.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash freertos log mbedtls time_service mem_diag
)
