idf_component_register(
    SRCS "rotary_encoder.cpp" "button.cpp" "encoder_input.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_gpio esp_timer log
)
