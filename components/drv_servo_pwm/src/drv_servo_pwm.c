#include "drv_servo_pwm.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/mcpwm_prelude.h"
#include "board.h"

static const char *TAG = "drv_servo_pwm";

#if CONFIG_DRV_SERVO_PWM_ENABLE

/* MCPWM 时基：1MHz 分辨率（1 tick = 1us），周期由舵机 PWM 频率决定（50Hz -> 20000us） */
#define MCPWM_TIMEBASE_RES_HZ   1000000

/* 通道 -> 板级引脚映射（顺序必须与 servo_channel_t 一致） */
static const int s_servo_gpio[SERVO_CH_MAX] = {
    [SERVO_CH_PAN]  = BOARD_SERVO_PAN_GPIO,
    [SERVO_CH_TILT] = BOARD_SERVO_TILT_GPIO,
};

/* ---- 私有状态 ---- */
typedef struct {
    bool                    initialized;
    bool                    running;
    mcpwm_timer_handle_t    timer;
    mcpwm_oper_handle_t     oper[SERVO_CH_MAX];
    mcpwm_cmpr_handle_t     cmp[SERVO_CH_MAX];
    mcpwm_gen_handle_t      gen[SERVO_CH_MAX];
    int16_t                 current_angle[SERVO_CH_MAX];
    int16_t                 min_angle;
    int16_t                 max_angle;
} drv_servo_pwm_ctx_t;

static drv_servo_pwm_ctx_t s_ctx;

/* 角度 -> 脉宽(us) 线性映射 */
static uint32_t angle_to_pulse_us(int16_t angle, int16_t min_angle, int16_t max_angle)
{
    uint32_t min_pulse = CONFIG_DRV_SERVO_PWM_MIN_PULSE_US;
    uint32_t max_pulse = CONFIG_DRV_SERVO_PWM_MAX_PULSE_US;
    int32_t span_pulse = (int32_t)max_pulse - (int32_t)min_pulse;
    int32_t span_angle = (int32_t)max_angle - (int32_t)min_angle;

    int32_t pulse = (int32_t)min_pulse
                  + ((int32_t)(angle - min_angle) * span_pulse) / span_angle;

    /* 钳位，防止配置异常时输出越界脉宽 */
    if (pulse < (int32_t)min_pulse) pulse = min_pulse;
    if (pulse > (int32_t)max_pulse) pulse = max_pulse;
    return (uint32_t)pulse;
}

esp_err_t drv_servo_pwm_init(const drv_servo_pwm_config_t *config)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.min_angle = (config && config->min_angle != 0) ? config->min_angle : 0;
    s_ctx.max_angle = (config && config->max_angle != 0) ? config->max_angle : 180;

    /* 1. 创建 MCPWM 定时器：两路舵机共用一个定时器（同频同相） */
    const uint32_t period_ticks = MCPWM_TIMEBASE_RES_HZ / CONFIG_DRV_SERVO_PWM_PWM_FREQ_HZ;
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = MCPWM_TIMEBASE_RES_HZ,
        .period_ticks = period_ticks,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_config, &s_ctx.timer),
                        TAG, "new timer failed");

    /* 2. 每路舵机：operator -> comparator -> generator */
    for (int ch = 0; ch < SERVO_CH_MAX; ch++) {
        mcpwm_operator_config_t oper_config = {
            .group_id = 0,   /* 必须与 timer 同组 */
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_operator(&oper_config, &s_ctx.oper[ch]),
                            TAG, "ch%d new operator failed", ch);
        ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(s_ctx.oper[ch], s_ctx.timer),
                            TAG, "ch%d connect timer failed", ch);

        mcpwm_comparator_config_t cmp_config = {
            .flags.update_cmp_on_tez = true,   /* 每个周期末（归零）时更新脉宽，无毛刺 */
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_comparator(s_ctx.oper[ch], &cmp_config, &s_ctx.cmp[ch]),
                            TAG, "ch%d new comparator failed", ch);

        mcpwm_generator_config_t gen_config = {
            .gen_gpio_num = s_servo_gpio[ch],
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_generator(s_ctx.oper[ch], &gen_config, &s_ctx.gen[ch]),
                            TAG, "ch%d new generator (gpio%d) failed", ch, s_servo_gpio[ch]);

        /* 计数归零 -> 输出高；计数到达比较值 -> 输出低（由此形成 0.5~2.5ms 正脉冲） */
        ESP_RETURN_ON_ERROR(
            mcpwm_generator_set_action_on_timer_event(
                s_ctx.gen[ch],
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH)),
            TAG, "ch%d set timer action failed", ch);
        ESP_RETURN_ON_ERROR(
            mcpwm_generator_set_action_on_compare_event(
                s_ctx.gen[ch],
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                               s_ctx.cmp[ch],
                                               MCPWM_GEN_ACTION_LOW)),
            TAG, "ch%d set compare action failed", ch);

        /* 初始脉宽 = 角度范围中点，避免上电瞬间舵机猛跳 */
        s_ctx.current_angle[ch] = (int16_t)(((int32_t)s_ctx.min_angle + s_ctx.max_angle) / 2);
        uint32_t home_pulse = angle_to_pulse_us(s_ctx.current_angle[ch],
                                                s_ctx.min_angle, s_ctx.max_angle);
        ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(s_ctx.cmp[ch], home_pulse),
                            TAG, "ch%d set home pulse failed", ch);
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (min=%d max=%d, %dHz, pan=gpio%d tilt=gpio%d)",
             s_ctx.min_angle, s_ctx.max_angle, CONFIG_DRV_SERVO_PWM_PWM_FREQ_HZ,
             s_servo_gpio[SERVO_CH_PAN], s_servo_gpio[SERVO_CH_TILT]);
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

    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(s_ctx.timer), TAG, "timer enable failed");
    ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(s_ctx.timer, MCPWM_TIMER_START_NO_STOP),
                        TAG, "timer start failed");
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_servo_pwm_stop(void)
{
    if (s_ctx.initialized && s_ctx.running) {
        mcpwm_timer_start_stop(s_ctx.timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(s_ctx.timer);
    }
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t drv_servo_pwm_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        drv_servo_pwm_stop();
    }
    for (int ch = 0; ch < SERVO_CH_MAX; ch++) {
        if (s_ctx.gen[ch])  { mcpwm_del_generator(s_ctx.gen[ch]);  s_ctx.gen[ch] = NULL; }
        if (s_ctx.cmp[ch])  { mcpwm_del_comparator(s_ctx.cmp[ch]); s_ctx.cmp[ch] = NULL; }
        if (s_ctx.oper[ch]) { mcpwm_del_operator(s_ctx.oper[ch]); s_ctx.oper[ch] = NULL; }
    }
    if (s_ctx.timer) {
        mcpwm_del_timer(s_ctx.timer);
        s_ctx.timer = NULL;
    }
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

void drv_servo_pwm_register_console_cmds(void)
{
    /* 业务命令（pt set/home/status）统一在 svc_pan_tilt 中注册 */
}

esp_err_t drv_servo_pwm_set_angle(servo_channel_t channel, int16_t angle)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(channel < SERVO_CH_MAX, ESP_ERR_INVALID_ARG, TAG, "bad channel");
    ESP_RETURN_ON_FALSE(angle >= s_ctx.min_angle && angle <= s_ctx.max_angle,
                        ESP_ERR_INVALID_ARG, TAG, "angle %d out of range", angle);

    uint32_t pulse_us = angle_to_pulse_us(angle, s_ctx.min_angle, s_ctx.max_angle);
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(s_ctx.cmp[channel], pulse_us),
                        TAG, "ch%d set compare failed", channel);

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

#else /* CONFIG_DRV_SERVO_PWM_ENABLE */

esp_err_t drv_servo_pwm_init(const drv_servo_pwm_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_servo_pwm_start(void)             { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_servo_pwm_stop(void)              { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_servo_pwm_deinit(void)            { return ESP_ERR_NOT_SUPPORTED; }
void drv_servo_pwm_register_console_cmds(void)  {}
esp_err_t drv_servo_pwm_set_angle(servo_channel_t channel, int16_t angle)
{
    (void)channel; (void)angle;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_servo_pwm_get_angle(servo_channel_t channel, int16_t *angle)
{
    (void)channel; (void)angle;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_DRV_SERVO_PWM_ENABLE */
