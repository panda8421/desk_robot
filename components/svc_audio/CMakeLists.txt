idf_component_register(
    SRCS        "src/svc_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES    driver esp_hw_support
    PRIV_REQUIRES board common console_cmd drv_audio drv_es8388 drv_xl9555 nvs_flash
)

if(CONFIG_SVC_WAKEWORD_ENABLE)
    list(APPEND PRIV_REQUIRES espressif__esp-sr)
endif()
