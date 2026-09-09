idf_component_register(
    SRCS        "src/evt_log.c"
    INCLUDE_DIRS "include"
    REQUIRES    nvs_flash esp_event common
)
