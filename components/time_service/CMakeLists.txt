idf_component_register(
    SRCS "sntp_time_provider.cpp" "wifi_connection.cpp" "wifi_credentials.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event lwip nvs_flash esp_timer log
)
