#include "drv_led.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "board.h"

static const char *TAG = "drv_led";

#if CONFIG_DRV_LED_ENABLE

/* ---- 私有状态 ---- */
typedef struct {
    bool initialized;
    bool running;
    bool on;                 /* 逻辑状态：true=亮（已对极性做归一化） */
} drv_led_ctx_t;

static drv_led_ctx_t s_ctx;

/* 逻辑亮灭 -> 实际 GPIO 电平（active-low 时亮=低电平） */
static inline uint32_t level_for(bool on)
{
#if CONFIG_DRV_LED_ACTIVE_LOW
    return on ? 0u : 1u;
#else
    return on ? 1u : 0u;
#endif
}

esp_err_t drv_led_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LED_STATUS_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");

    /* 配置后立即置灭，避免上电瞬间出现杂散点亮 */
    s_ctx.on = false;
    gpio_set_level(BOARD_LED_STATUS_GPIO, level_for(false));

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (gpio=%d, active-%s)",
             BOARD_LED_STATUS_GPIO,
             CONFIG_DRV_LED_ACTIVE_LOW ? "low" : "high");
    return ESP_OK;
}

esp_err_t drv_led_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.running = true;
    return ESP_OK;
}

esp_err_t drv_led_stop(void)
{
    if (s_ctx.initialized) {
        drv_led_set(false);
    }
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t drv_led_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    drv_led_set(false);
    gpio_reset_pin(BOARD_LED_STATUS_GPIO);
    s_ctx.initialized = false;
    s_ctx.running = false;
    return ESP_OK;
}

void drv_led_register_console_cmds(void)
{
    /* 业务命令（led on/off/blink）在 svc_status 中注册，驱动层不提供 */
}

esp_err_t drv_led_set(bool on)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    gpio_set_level(BOARD_LED_STATUS_GPIO, level_for(on));
    s_ctx.on = on;
    return ESP_OK;
}

esp_err_t drv_led_get(bool *on)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(on != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *on = s_ctx.on;
    return ESP_OK;
}

esp_err_t drv_led_toggle(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return drv_led_set(!s_ctx.on);
}

#else /* CONFIG_DRV_LED_ENABLE */

/* 驱动被裁剪时提供空实现，保证链接通过 */
esp_err_t drv_led_init(void)             { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_led_start(void)            { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_led_stop(void)             { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_led_deinit(void)           { return ESP_ERR_NOT_SUPPORTED; }
void      drv_led_register_console_cmds(void) {}
esp_err_t drv_led_set(bool on)           { (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_led_get(bool *on)          { (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_led_toggle(void)           { return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_DRV_LED_ENABLE */
