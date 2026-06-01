idf_component_register(SRCS "main.c" "provisioning.c"
                    REQUIRES esp_driver_uart esp_driver_gpio nvs_flash esp_wifi mqtt esp_http_server
                    INCLUDE_DIRS ".")
