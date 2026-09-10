/**
 * @file    drv_audio.c
 * @brief   I2S 全双工音频数据通路驱动实现
 * @note    ESP-IDF v5.x 新 I2S 标准模式 API；本板音频规格 16kHz/16bit/mono
 */
#include "drv_audio.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_console.h"
#include "driver/i2s_std.h"
#include "board.h"
#include "console_cmd.h"

static const char *TAG = "drv_audio";

#if CONFIG_DRV_AUDIO_ENABLE

/* ---- 私有状态 ---- */
typedef struct {
    bool             initialized;
    bool             running;
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    float            input_gain;         /* 录音软件数字增益（饱和处理） */
} drv_audio_ctx_t;

static drv_audio_ctx_t s_ctx;

/* ============== 生命周期 ============== */

esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t desc_num  = cfg ? cfg->dma_desc_num  : 3;
    uint8_t frame_num = cfg ? cfg->dma_frame_num : 240;   /* 240 帧 @16k ≈ 15ms */

    /* 标准模式下单 FIFO 通道即可全双工（tx/rx 共用一个 chanel 组） */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = desc_num,
        .dma_frame_num = frame_num,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_ctx.tx_handle, &s_ctx.rx_handle),
                        TAG, "new i2s channel failed");

    /* 标准 I2S（Philips）：16bit / mono，MCLK 对 ES8388 输出 256*fs */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DRV_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_SCK_GPIO,
            .ws   = BOARD_I2S_LRCK_GPIO,
            .dout = BOARD_I2S_SDOUT_GPIO,       /* 播放 */
            .din  = BOARD_I2S_SDIN_GPIO,        /* 录音 */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* ES8388 需 256*fs 的 MCLK */
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    /* 播放：mono 数据复制到左右双声道（喇叭/耳机双声道出声） */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_ctx.tx_handle, &std_cfg),
                        TAG, "init tx std mode failed");
    /* 录音：只取左声道（ES8388 ADC 左声道 = LIN1 板载咪头） */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_ctx.rx_handle, &std_cfg),
                        TAG, "init rx std mode failed");

    s_ctx.input_gain = 1.0f;
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (%dHz/%dbit/%dch)",
             DRV_AUDIO_SAMPLE_RATE, DRV_AUDIO_BIT_WIDTH, DRV_AUDIO_CHANNELS);
    return ESP_OK;
}

esp_err_t drv_audio_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_ctx.tx_handle), TAG, "enable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_ctx.rx_handle), TAG, "enable rx failed");
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t drv_audio_stop(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        i2s_channel_disable(s_ctx.tx_handle);
        i2s_channel_disable(s_ctx.rx_handle);
        s_ctx.running = false;
    }
    return ESP_OK;
}

esp_err_t drv_audio_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    drv_audio_stop();
    i2s_del_channel(s_ctx.tx_handle);
    i2s_del_channel(s_ctx.rx_handle);
    s_ctx.tx_handle = NULL;
    s_ctx.rx_handle = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 业务 API ============== */

void drv_audio_set_input_gain(float gain)
{
    if (gain < 0.01f) {
        gain = 0.01f;
    }
    s_ctx.input_gain = gain;
}

esp_err_t drv_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "null buf");

    size_t bytes_read = 0;
    size_t want = samples * sizeof(int16_t);
    esp_err_t ret = i2s_channel_read(s_ctx.rx_handle, buf, want, &bytes_read,
                                     (TickType_t)timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "read failed");
    if (bytes_read < want) {
        /* 超时/未读满：不足部分不保证有效，返回超时语义 */
        return ESP_ERR_TIMEOUT;
    }

    /* 软件数字增益（饱和处理） */
    if (s_ctx.input_gain != 1.0f) {
        for (size_t i = 0; i < samples; i++) {
            int32_t v = (int32_t)(buf[i] * s_ctx.input_gain);
            buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    return ESP_OK;
}

esp_err_t drv_audio_write(const int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "null buf");

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_ctx.tx_handle, buf, samples * sizeof(int16_t),
                                      &bytes_written, (TickType_t)timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "write failed");
    if (bytes_written < samples * sizeof(int16_t)) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ============== 控制台命令：audio rec？调试用最小命令 ---- */
static int cmd_audio_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("audio %s (rate=%d, gain=%.2f)\n",
           s_ctx.running ? "running" : "stopped",
           DRV_AUDIO_SAMPLE_RATE, s_ctx.input_gain);
    return 0;
}

void drv_audio_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audioidsp",
        .help = "I2S driver status (debug)",
        .func = cmd_audio_status,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register cmd failed");
    }
}

#else /* CONFIG_DRV_AUDIO_ENABLE */

/* 驱动被裁剪时提供空实现，保证链接通过 */
esp_err_t drv_audio_init(const drv_audio_cfg_t *cfg)
{
    (void)cfg; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_audio_start(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_audio_stop(void)                      { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t drv_audio_deinit(void)                    { return ESP_ERR_NOT_SUPPORTED; }
void      drv_audio_register_console_cmds(void)     {}
esp_err_t drv_audio_read(int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    (void)buf; (void)samples; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t drv_audio_write(const int16_t *buf, size_t samples, uint32_t timeout_ms)
{
    (void)buf; (void)samples; (void)timeout_ms; return ESP_ERR_NOT_SUPPORTED;
}
void drv_audio_set_input_gain(float gain)   { (void)gain; }

#endif /* CONFIG_DRV_AUDIO_ENABLE */