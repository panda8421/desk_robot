#include "svc_pan_tilt.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include "drv_servo_pwm.h"

static const char *TAG = "svc_pan_tilt";

/* ---- 私有状态 ---- */
typedef struct {
    bool            initialized;
    bool            running;
    TaskHandle_t    task_handle;
    pan_tilt_angle_t current;
    pan_tilt_angle_t target;
    bool            moving;
} svc_pan_tilt_ctx_t;

static svc_pan_tilt_ctx_t s_ctx;

/* 单步插值：向 target 靠近 step 度 */
static int16_t step_toward(int16_t cur, int16_t tgt, int16_t step)
{
    if (cur < tgt) {
        int16_t n = cur + step;
        return (n > tgt) ? tgt : n;
    } else if (cur > tgt) {
        int16_t n = cur - step;
        return (n < tgt) ? tgt : n;
    }
    return cur;
}

static void svc_pan_tilt_task(void *arg)
{
    (void)arg;
    const int16_t step = CONFIG_SVC_PAN_TILT_STEP_DEG;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_SVC_PAN_TILT_TICK_MS);

    while (s_ctx.running) {
        if (s_ctx.moving) {
            int16_t new_pan = step_toward(s_ctx.current.pan, s_ctx.target.pan, step);
            int16_t new_tilt = step_toward(s_ctx.current.tilt, s_ctx.target.tilt, step);

            if (new_pan != s_ctx.current.pan) {
                drv_servo_pwm_set_angle(SERVO_CH_PAN, new_pan);
                s_ctx.current.pan = new_pan;
            }
            if (new_tilt != s_ctx.current.tilt) {
                drv_servo_pwm_set_angle(SERVO_CH_TILT, new_tilt);
                s_ctx.current.tilt = new_tilt;
            }

            /* 到达目标，发布事件 */
            if (s_ctx.current.pan == s_ctx.target.pan &&
                s_ctx.current.tilt == s_ctx.target.tilt) {
                s_ctx.moving = false;
                pan_tilt_angle_t evt = s_ctx.current;
                esp_event_post(PAN_TILT_EVENT, PAN_TILT_ANGLE_CHANGED,
                               &evt, sizeof(evt), 0);
            }
        }
        vTaskDelay(period);
    }
    vTaskDelete(NULL);
}

esp_err_t svc_pan_tilt_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 初始角度：中位 */
    s_ctx.current.pan  = (CONFIG_SVC_PAN_TILT_PAN_MIN + CONFIG_SVC_PAN_TILT_PAN_MAX) / 2;
    s_ctx.current.tilt = (CONFIG_SVC_PAN_TILT_TILT_MIN + CONFIG_SVC_PAN_TILT_TILT_MAX) / 2;
    s_ctx.target = s_ctx.current;
    s_ctx.moving = false;

    /* 初始化底层舵机驱动 */
    drv_servo_pwm_config_t servo_cfg = {
        .min_angle = CONFIG_SVC_PAN_TILT_PAN_MIN < CONFIG_SVC_PAN_TILT_TILT_MIN
                     ? CONFIG_SVC_PAN_TILT_PAN_MIN : CONFIG_SVC_PAN_TILT_TILT_MIN,
        .max_angle = CONFIG_SVC_PAN_TILT_PAN_MAX > CONFIG_SVC_PAN_TILT_TILT_MAX
                     ? CONFIG_SVC_PAN_TILT_PAN_MAX : CONFIG_SVC_PAN_TILT_TILT_MAX,
    };
    ESP_ERROR_CHECK(drv_servo_pwm_init(&servo_cfg));

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done, home pan=%d tilt=%d", s_ctx.current.pan, s_ctx.current.tilt);
    return ESP_OK;
}

esp_err_t svc_pan_tilt_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 启动底层舵机驱动 */
    ESP_ERROR_CHECK(drv_servo_pwm_start());
    s_ctx.running = true;
    BaseType_t ret = xTaskCreate(svc_pan_tilt_task, "svc_pan_tilt",
                                 CONFIG_SVC_PAN_TILT_TASK_STACK_SIZE, NULL,
                                 CONFIG_SVC_PAN_TILT_TASK_PRIORITY,
                                 &s_ctx.task_handle);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_pan_tilt_stop(void)
{
    s_ctx.running = false;
    /* task 自行退出 */
    return ESP_OK;
}

esp_err_t svc_pan_tilt_deinit(void)
{
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

void svc_pan_tilt_register_console_cmds(void)
{
    /* TODO: 注册 "pt set <pan> <tilt>" / "pt home" / "pt status" 命令 */
}

esp_err_t svc_pan_tilt_set_target(int16_t pan, int16_t tilt)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pan >= CONFIG_SVC_PAN_TILT_PAN_MIN &&
                        pan <= CONFIG_SVC_PAN_TILT_PAN_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "pan %d out of range", pan);
    ESP_RETURN_ON_FALSE(tilt >= CONFIG_SVC_PAN_TILT_TILT_MIN &&
                        tilt <= CONFIG_SVC_PAN_TILT_TILT_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "tilt %d out of range", tilt);

    s_ctx.target.pan = pan;
    s_ctx.target.tilt = tilt;
    s_ctx.moving = true;
    ESP_LOGD(TAG, "target pan=%d tilt=%d", pan, tilt);
    return ESP_OK;
}

esp_err_t svc_pan_tilt_get_current(pan_tilt_angle_t *angle)
{
    ESP_RETURN_ON_FALSE(angle != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *angle = s_ctx.current;
    return ESP_OK;
}
