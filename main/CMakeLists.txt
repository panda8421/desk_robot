idf_component_register(
    SRCS        "main.c" "conversation.c"
    INCLUDE_DIRS ""
    REQUIRES    board common svc_status svc_pan_tilt svc_wifi svc_audio svc_ai_chat evt_log diag console_cmd
)
