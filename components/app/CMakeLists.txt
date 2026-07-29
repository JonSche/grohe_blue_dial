idf_component_register(
    SRCS "app.cpp" "dial_controller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board display ui encoder dial_state log freertos
)
