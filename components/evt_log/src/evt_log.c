#include "evt_log.h"

#include "esp_log.h"

static const char *TAG = "evt_log";

typedef struct {
    bool initialized;
} evt_log_ctx_t;

static evt_log_ctx_t s_ctx;

esp_err_t evt_log_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO: 打开 NVS namespace "evt_log"，加载环形缓冲区索引 */
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (ring=%d)", CONFIG_EVT_LOG_RING_SIZE);
    return ESP_OK;
}

esp_err_t evt_log_start(void)
{
    /* TODO: 订阅白名单事件，自动 record */
    return ESP_OK;
}

esp_err_t evt_log_stop(void)
{
    return ESP_OK;
}

esp_err_t evt_log_deinit(void)
{
    s_ctx.initialized = false;
    return ESP_OK;
}

void evt_log_register_console_cmds(void)
{
    /* TODO: 注册 "evt_log list" / "evt_log clear" 命令 */
}

esp_err_t evt_log_record(esp_event_base_t base, int32_t id,
                         const void *data, size_t len)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO: 写入环形缓冲区，必要时持久化到 NVS */
    (void)base; (void)id; (void)data; (void)len;
    ESP_LOGD(TAG, "record base=%s id=%ld len=%u", base, (long)id, (unsigned)len);
    return ESP_OK;
}
