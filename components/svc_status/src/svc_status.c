#include "svc_status.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "drv_led.h"
#include "console_cmd.h"

static const char *TAG = "svc_status";

#if CONFIG_SVC_STATUS_ENABLE

/* ---- 私有状态 ---- */
typedef struct {
    bool                    initialized;
    bool                    running;
    svc_status_led_mode_t   mode;
    uint32_t                period_ms;
    esp_timer_handle_t      blink_timer;
} svc_status_ctx_t;

static svc_status_ctx_t s_ctx;

/* 闪烁定时器回调：每次触发翻转一次 LED（运行在 esp_timer 任务，回调需简短） */
static void blink_timer_cb(void *arg)
{
    (void)arg;
    drv_led_toggle();
}

/* 按当前 mode/period 重新应用 LED 与定时器 */
static esp_err_t apply_mode(void)
{
    esp_timer_stop(s_ctx.blink_timer);

    switch (s_ctx.mode) {
    case SVC_LED_ON:
        drv_led_set(true);
        break;
    case SVC_LED_OFF:
        drv_led_set(false);
        break;
    case SVC_LED_BLINK:
        drv_led_set(false);
        ESP_RETURN_ON_ERROR(
            esp_timer_start_periodic(s_ctx.blink_timer,
                                     (uint64_t)s_ctx.period_ms * 1000u),
            TAG, "timer start failed");
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t svc_status_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 初始化底层 LED 驱动 */
    ESP_ERROR_CHECK(drv_led_init());

    s_ctx.mode = SVC_LED_BLINK;
    s_ctx.period_ms = CONFIG_SVC_STATUS_HEARTBEAT_MS;

    const esp_timer_create_args_t timer_args = {
        .callback = blink_timer_cb,
        .name = "led_blink",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.blink_timer));

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (heartbeat=%lums)",
             (unsigned long)CONFIG_SVC_STATUS_HEARTBEAT_MS);
    return ESP_OK;
}

esp_err_t svc_status_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_ERROR_CHECK(drv_led_start());
    s_ctx.running = true;
    ESP_ERROR_CHECK(apply_mode());
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_status_stop(void)
{
    if (s_ctx.initialized) {
        esp_timer_stop(s_ctx.blink_timer);
        drv_led_stop();
    }
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t svc_status_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_timer_stop(s_ctx.blink_timer);
    esp_timer_delete(s_ctx.blink_timer);
    drv_led_deinit();
    s_ctx.blink_timer = NULL;
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t svc_status_set_led_mode(svc_status_led_mode_t mode, uint32_t period_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (mode == SVC_LED_BLINK && period_ms > 0) {
        s_ctx.period_ms = period_ms;
    }
    s_ctx.mode = mode;
    return apply_mode();
}

/* ---- 控制台命令：led on | off | blink [半周期ms] ---- */
static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: led <on|off|blink [half_period_ms]>\n");
        return 1;
    }

    if (strcmp(argv[1], "on") == 0) {
        ESP_ERROR_CHECK(svc_status_set_led_mode(SVC_LED_ON, 0));
    } else if (strcmp(argv[1], "off") == 0) {
        ESP_ERROR_CHECK(svc_status_set_led_mode(SVC_LED_OFF, 0));
    } else if (strcmp(argv[1], "blink") == 0) {
        uint32_t period = 0;
        if (argc >= 3) {
            period = (uint32_t)strtoul(argv[2], NULL, 10);
        }
        ESP_ERROR_CHECK(svc_status_set_led_mode(SVC_LED_BLINK, period));
    } else {
        printf("unknown mode '%s', use: on | off | blink\n", argv[1]);
        return 1;
    }
    return 0;
}

void svc_status_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "led",
        .help = "Control status LED: led <on|off|blink [half_period_ms]>",
        .func = cmd_led,
    };
    /* 注册失败不阻断启动，仅告警 */
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'led' cmd failed");
    }
}

#else /* CONFIG_SVC_STATUS_ENABLE */

esp_err_t svc_status_init(void)                 { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_status_start(void)                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_status_stop(void)                 { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_status_deinit(void)               { return ESP_ERR_NOT_SUPPORTED; }
void      svc_status_register_console_cmds(void) {}
esp_err_t svc_status_set_led_mode(svc_status_led_mode_t mode, uint32_t period_ms)
{
    (void)mode; (void)period_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_SVC_STATUS_ENABLE */
