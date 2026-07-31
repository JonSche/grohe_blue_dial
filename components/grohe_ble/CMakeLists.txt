idf_component_register(
    SRCS "ble_manager.cpp" "grohe_client.cpp" "grohe_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash freertos log
)
