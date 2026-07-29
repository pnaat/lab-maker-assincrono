idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif mqtt driver nvs_flash
    EMBED_TXTFILES "mqtt_eclipseprojects_io.pem"
)