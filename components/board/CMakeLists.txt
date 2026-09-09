idf_component_register(
    SRCS        "src/board.c"
    INCLUDE_DIRS "include"
    REQUIRES    driver esp_hw_support esp_event
    PRIV_REQUIRES nvs_flash
)
