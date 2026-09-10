idf_component_register(
    SRCS        "src/svc_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES    driver
    PRIV_REQUIRES board common console_cmd drv_audio drv_es8388 drv_xl9555
)
