idf_component_register(SRCS "main.c" "provisioning.c"
                    REQUIRES app_update esp_driver_uart esp_driver_gpio nvs_flash esp_wifi mqtt esp_http_server esp_https_ota
                    INCLUDE_DIRS ".")

                    # Base version for release/dev tracking
set(APP_BASE_VERSION "v0.0.2")

# Compute Git metadata at build time
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-list --count HEAD
    OUTPUT_VARIABLE GIT_COMMIT_COUNT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_SHORT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_COMMIT_COUNT)
    set(GIT_COMMIT_COUNT "0")
endif()

if(NOT GIT_SHORT_SHA)
    set(GIT_SHORT_SHA "unknown")
endif()

set(APP_VERSION "aprilaire-${APP_BASE_VERSION}-${GIT_COMMIT_COUNT}-g${GIT_SHORT_SHA}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_VERSION=\"${APP_VERSION}\")