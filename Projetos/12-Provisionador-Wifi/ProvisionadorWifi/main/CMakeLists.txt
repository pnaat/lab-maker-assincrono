idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "root.html"
                       REQUIRES nvs_flash esp_wifi esp_http_server mdns driver dns_server)