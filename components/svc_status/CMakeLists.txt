idf_component_register(
    SRCS        "src/svc_status.c"
    INCLUDE_DIRS "include"
    REQUIRES    common esp_timer
    PRIV_REQUIRES drv_led console_cmd
)
