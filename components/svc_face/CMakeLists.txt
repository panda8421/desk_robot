idf_component_register(
    SRCS        "src/svc_face.c"
    INCLUDE_DIRS "include"
    REQUIRES    common esp_event esp_timer
    PRIV_REQUIRES drv_display freertos console_cmd
)
