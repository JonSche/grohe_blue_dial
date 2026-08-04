idf_component_register(
    SRCS "app.cpp" "dial_controller.cpp" "dispense_session.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board display ui encoder dial_state grohe_ble firmware_info log freertos esp_timer
)
