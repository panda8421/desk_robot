#include "svc_pan_tilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include "drv_servo_pwm.h"
#include "console_cmd.h"

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

    /* 显式把归位角度下发到底层（与驱动默认中点一致，确保服务状态为唯一事实来源） */
    ESP_ERROR_CHECK(drv_servo_pwm_set_angle(SERVO_CH_PAN, s_ctx.current.pan));
    ESP_ERROR_CHECK(drv_servo_pwm_set_angle(SERVO_CH_TILT, s_ctx.current.tilt));

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

/* ---- 控制台命令：pt set <pan> <tilt> | pt home | pt status ---- */
static void print_usage(void)
{
    printf("usage:\n"
           "  pt set <pan> <tilt>   set target angles (deg)\n"
           "  pt home               move to center position\n"
           "  pt status             show current/target angles\n");
}

static int cmd_pt(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        int16_t pan  = (int16_t)strtol(argv[2], NULL, 10);
        int16_t tilt = (int16_t)strtol(argv[3], NULL, 10);
        esp_err_t err = svc_pan_tilt_set_target(pan, tilt);
        if (err != ESP_OK) {
            printf("set failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ok, moving to pan=%d tilt=%d\n", pan, tilt);
        return 0;
    }

    if (strcmp(argv[1], "home") == 0) {
        int16_t pan  = (CONFIG_SVC_PAN_TILT_PAN_MIN + CONFIG_SVC_PAN_TILT_PAN_MAX) / 2;
        int16_t tilt = (CONFIG_SVC_PAN_TILT_TILT_MIN + CONFIG_SVC_PAN_TILT_TILT_MAX) / 2;
        esp_err_t err = svc_pan_tilt_set_target(pan, tilt);
        if (err != ESP_OK) {
            printf("home failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ok, going home pan=%d tilt=%d\n", pan, tilt);
        return 0;
    }

    if (strcmp(argv[1], "status") == 0) {
        pan_tilt_angle_t cur, tgt;
        if (svc_pan_tilt_get_current(&cur) != ESP_OK) {
            printf("service not ready\n");
            return 1;
        }
        tgt = s_ctx.target;
        printf("pan:  current=%d target=%d%s\n",
               cur.pan, tgt.pan, s_ctx.moving ? " (moving)" : "");
        printf("tilt: current=%d target=%d\n", cur.tilt, tgt.tilt);
        return 0;
    }

    print_usage();
    return 1;
}

void svc_pan_tilt_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "pt",
        .help = "Pan-Tilt control: pt set <pan> <tilt> | pt home | pt status",
        .func = cmd_pt,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'pt' cmd failed");
    }
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
