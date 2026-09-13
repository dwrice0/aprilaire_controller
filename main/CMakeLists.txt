idf_component_register(SRCS "main.c" "provisioning.c"
                    REQUIRES app_update esp_driver_uart esp_driver_gpio nvs_flash esp_wifi mqtt esp_http_server esp_https_ota
                    INCLUDE_DIRS ".")
