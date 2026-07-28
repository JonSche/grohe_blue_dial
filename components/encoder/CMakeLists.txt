idf_component_register(
    SRCS "rotary_encoder.cpp" "button.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_gpio log
)
