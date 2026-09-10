idf_component_register(
    SRCS        "src/svc_ai_chat.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_http_client mbedtls json nvs_flash
    PRIV_REQUIRES board common console_cmd drv_audio
)
