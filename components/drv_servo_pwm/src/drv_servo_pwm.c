#include "drv_servo_pwm.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/mcpwm_prelude.h"
#include "board.h"

static const char *TAG = "drv_servo_pwm";

/* ---- 私有状态 ---- */
typedef struct {
    bool                initialized;
    bool                running;
    mcpwm_timer_handle_t   timer;
    mcpwm_oper_handle_t    oper[SERVO_CH_MAX];
    mcpwm_cmpr_handle_t    cmp[SERVO_CH_MAX];
    mcpwm_gen_handle_t     gen[SERVO_CH_MAX];
    int16_t             current_angle[SERVO_CH_MAX];
    int16_t             min_angle;
    int16_t             max_angle;
} drv_servo_pwm_ctx_t;

static drv_servo_pwm_ctx_t s_ctx;

/* 角度 -> 脉宽(us) 换算 */
static uint32_t angle_to_pulse_us(int16_t angle, int16_t min_angle, int16_t max_angle)
{
    /* TODO: 线性映射 angle[min~max] -> pulse[CONFIG_DRV_SERVO_PWM_MIN_PULSE_US ~ MAX] */
    (void)angle;
    (void)min_angle;
    (void)max_angle;
    return CONFIG_DRV_SERVO_PWM_MIN_PULSE_US;
}

esp_err_t drv_servo_pwm_init(const drv_servo_pwm_config_t *config)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.min_angle = (config && config->min_angle != 0) ? config->min_angle : 0;
    s_ctx.max_angle = (config && config->max_angle != 0) ? config->max_angle : 180;

    /* TODO: 创建 MCPWM timer（50Hz）、operator、comparator、generator
     *       绑定 BOARD_SERVO_PAN_GPIO / BOARD_SERVO_TILT_GPIO
     *       参考：mcpwm_new_timer / mcpwm_new_operator / mcpwm_new_comparator / mcpwm_new_generator
     */

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (min=%d max=%d)", s_ctx.min_angle, s_ctx.max_angle);
    return ESP_OK;
}

esp_err_t drv_servo_pwm_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }

    /* TODO: mcpwm_timer_enable(timer); mcpwm_timer_start(timer, MCPWM_TIMER_START_NO_STOP); */
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_servo_pwm_stop(void)
{
    /* TODO: mcpwm_timer_stop / disable */
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t drv_servo_pwm_deinit(void)
{
    /* TODO: mcpwm_del_generator / del_comparator / del_operator / del_timer */
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

void drv_servo_pwm_register_console_cmds(void)
{
    /* TODO: 注册 "servo set <ch> <angle>" 等命令 */
}

esp_err_t drv_servo_pwm_set_angle(servo_channel_t channel, int16_t angle)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(channel < SERVO_CH_MAX, ESP_ERR_INVALID_ARG, TAG, "bad channel");
    ESP_RETURN_ON_FALSE(angle >= s_ctx.min_angle && angle <= s_ctx.max_angle,
                        ESP_ERR_INVALID_ARG, TAG, "angle %d out of range", angle);

    uint32_t pulse_us = angle_to_pulse_us(angle, s_ctx.min_angle, s_ctx.max_angle);
    /* TODO: mcpwm_comparator_set_compare_value(cmp[channel], ticks_from_us(pulse_us)) */
    (void)pulse_us;

    s_ctx.current_angle[channel] = angle;
    ESP_LOGD(TAG, "ch=%d angle=%d pulse=%" PRIu32 "us", channel, angle, pulse_us);
    return ESP_OK;
}

esp_err_t drv_servo_pwm_get_angle(servo_channel_t channel, int16_t *angle)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(channel < SERVO_CH_MAX, ESP_ERR_INVALID_ARG, TAG, "bad channel");
    ESP_RETURN_ON_FALSE(angle != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *angle = s_ctx.current_angle[channel];
    return ESP_OK;
}
