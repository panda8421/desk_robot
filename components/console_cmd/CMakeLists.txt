idf_component_register(
    SRCS        "src/console_cmd.c"
    INCLUDE_DIRS "include"
    REQUIRES    console driver vfs esp_timer
)
