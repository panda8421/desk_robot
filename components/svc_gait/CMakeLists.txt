idf_component_register(
    SRCS        "src/svc_gait.c"
    INCLUDE_DIRS "include"
    REQUIRES    common esp_event
    PRIV_REQUIRES driver console_cmd
)
