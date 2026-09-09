idf_component_register(
    SRCS        "src/svc_pan_tilt.c"
    INCLUDE_DIRS "include"
    REQUIRES    common esp_event esp_timer
    PRIV_REQUIRES drv_servo_pwm console_cmd
)
