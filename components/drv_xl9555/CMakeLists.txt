idf_component_register(
    SRCS        "src/drv_xl9555.c"
    INCLUDE_DIRS "include"
    REQUIRES    driver
    PRIV_REQUIRES board common console_cmd
)
