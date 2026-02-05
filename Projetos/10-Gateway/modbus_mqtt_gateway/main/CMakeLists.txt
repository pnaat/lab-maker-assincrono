idf_component_register(
    SRCS "main.c" "modbus_rtu.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif mqtt driver nvs_flash
)