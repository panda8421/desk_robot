idf_component_register(
    SRCS        "src/svc_behavior.c"
    INCLUDE_DIRS "include"
    REQUIRES    common esp_event esp_timer
    PRIV_REQUIRES svc_pan_tilt console_cmd
)
