idf_component_register(
    SRCS "app.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board display ui encoder log freertos
)
