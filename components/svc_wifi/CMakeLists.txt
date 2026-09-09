idf_component_register(
    SRCS        "src/svc_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES    esp_wifi esp_event nvs_flash common
    PRIV_REQUIRES board console_cmd
)
