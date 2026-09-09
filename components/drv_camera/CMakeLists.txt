if(CONFIG_DRV_CAMERA_ENABLE)
    idf_component_register(
        SRCS        "src/drv_camera.c"
        INCLUDE_DIRS "include"
        REQUIRES    esp_cam
        PRIV_REQUIRES board
    )
else()
    idf_component_register()
endif()
