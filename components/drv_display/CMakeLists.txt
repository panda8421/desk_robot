if(CONFIG_DRV_DISPLAY_ENABLE)
    idf_component_register(
        SRCS        "src/drv_display.c"
        INCLUDE_DIRS "include"
        REQUIRES    driver
        PRIV_REQUIRES board
    )
else()
    idf_component_register()
endif()
