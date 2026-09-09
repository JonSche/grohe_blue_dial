idf_component_register(
    SRCS "dial_settings.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dial_state nvs_flash log
)
