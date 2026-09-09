#include "diag.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "diag";

typedef struct {
    bool initialized;
    bool running;
    esp_timer_handle_t timer;
} diag_ctx_t;

static diag_ctx_t s_ctx;

void diag_snapshot(void)
{
    uint32_t free = esp_get_free_heap_size();
    uint32_t min_free = esp_get_minimum_free_heap_size();
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "MEM free=%" PRIu32 " min_free=%" PRIu32 " largest=%u",
             free, min_free, (unsigned)info.largest_free_block);
    /* TODO: 遍历任务打印栈高水位 vTaskList / uxTaskGetStackHighWaterMark */
}

static void timer_cb(void *arg)
{
    (void)arg;
    diag_snapshot();
}

esp_err_t diag_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_timer_create_args_t args = {
        .callback = timer_cb,
        .name = "diag",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_ctx.timer));
    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t diag_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ctx.timer,
                  CONFIG_DIAG_INTERVAL_SEC * 1000000ULL));
    s_ctx.running = true;
    return ESP_OK;
}

esp_err_t diag_stop(void)
{
    if (s_ctx.running) {
        esp_timer_stop(s_ctx.timer);
        s_ctx.running = false;
    }
    return ESP_OK;
}

esp_err_t diag_deinit(void)
{
    if (s_ctx.timer) {
        esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
    }
    s_ctx.initialized = false;
    return ESP_OK;
}

void diag_register_console_cmds(void)
{
    /* TODO: 注册 "diag snapshot" 命令 */
}
