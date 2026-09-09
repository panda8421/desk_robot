idf_component_register(
    SRCS        "main.c"
    INCLUDE_DIRS ""
    REQUIRES    board common svc_status svc_pan_tilt svc_wifi evt_log diag console_cmd
)
