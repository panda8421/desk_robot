/**
 * @file    svc_audio.c
 * @brief   音频前端服务实现（P1：能量 VAD + 录放调度 + 提示音）
 * @note    录音缓冲与提示音均在 PSRAM；VAD 为能量阈值法，参数可经 Kconfig/控制台调整
 */
#include "svc_audio.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "drv_audio.h"
#include "drv_es8388.h"
#include "drv_xl9555.h"
#include "app_events.h"
#include "console_cmd.h"

static const char *TAG = "svc_audio";

#if CONFIG_SVC_AUDIO_ENABLE

/* ============== 常量与私有配置 ============== */
#define REC_BUF_BYTES           (512 * 1024)    /* 录音缓冲 512KB PSRAM ≈ 16s */
#define FRAME_SAMPLES           320             /* VAD 处理帧：320 点 = 20ms */
#define PRE_TRIGGER_FRAMES      10              /* 起始语音前保留 10 帧 ≈ 200ms */
#define MIN_SPEECH_FRAMES       13              /* 有效语音最短 13 帧 ≈ 260ms */
#define MAX_SESSION_MS          15000           /* 单次会话最长时间 */

/* ---- 私有状态 ---- */
typedef struct {
    bool             initialized;
    bool             running;
    bool             recording;             /* 录音会话进行中 */
    volatile bool    stop_req;              /* 手动停止录音请求 */
    volatile bool    play_abort;            /* 播放打断请求 */

    int16_t         *rec_buf;               /* 录音线性缓冲（PSRAM） */
    size_t           rec_total;             /* 会话累计采样点数 */
    size_t           rec_valid;             /* VAD 修剪后的有效采样点数 */
    bool             speech_detected;       /* 本次会话是否检测到语音 */
    uint32_t         vad_threshold;         /* VAD 能量门限（RMS 平方均值） */

    int16_t         *tone_buf[4];           /* 提示音缓冲（PSRAM） */
    size_t           tone_len[4];           /* 提示音采样点数 */

    SemaphoreHandle_t play_mutex;           /* 播放串行化 */
    TaskHandle_t      rec_task;
    QueueHandle_t     cmd_queue;            /* 录音任务命令队列 */
} svc_audio_ctx_t;

/* 录音任务命令 */
typedef enum {
    REC_CMD_START,          /* 开始一次录音会话 */
    REC_CMD_STOP,           /* 手动结束会话 */
} rec_cmd_t;

static svc_audio_ctx_t s_ctx;

/* ============== 事件发布 ============== */
static void post_audio_event(int32_t id, const void *arg, size_t arg_size)
{
    esp_event_post(AUDIO_EVENT, id, (void *)arg, arg_size, 0);
}

static void post_audio_err(int32_t code)
{
    post_audio_event(AUDIO_ERROR, &code, sizeof(code));
}

/* ============== 提示音生成（初始化时合成到 PSRAM） ============== */

/**
 * @brief  生成一段带 10ms 淡入淡出的正弦提示音
 */
static void tone_gen(int16_t *buf, size_t samples, float freq_hz, float amp)
{
    for (size_t i = 0; i < samples; i++) {
        /* 淡入淡出窗口（10ms = 160 点） */
        float edge = 1.0f;
        if (i < 160) {
            edge = (float)i / 160.0f;
        } else if (i > samples - 160) {
            edge = (float)(samples - i) / 160.0f;
        }
        float v = amp * edge * sinf(2.0f * (float)M_PI * freq_hz
                                     * (float)i / (float)DRV_AUDIO_SAMPLE_RATE);
        buf[i] = (int16_t)(v * 32767.0f);
    }
}

/* 拼 80ms 静音 */
static void tone_silence(int16_t *buf, size_t samples)
{
    memset(buf, 0, samples * sizeof(int16_t));
}

static esp_err_t tones_init(void)
{
    /* TONE_WAKE: 880Hz 100ms + 静 60ms + 1320Hz 120ms ≈ 280ms */
    size_t n = (size_t)DRV_AUDIO_SAMPLE_RATE * 300 / 1000;
    s_ctx.tone_buf[SVC_TONE_WAKE] = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    s_ctx.tone_len[SVC_TONE_WAKE] = n;

    /* TONE_OK: 1200Hz 120ms；TONE_END: 900Hz 120ms；TONE_ERROR: 300Hz 350ms */
    size_t n_ok  = DRV_AUDIO_SAMPLE_RATE * 120 / 1000;
    size_t n_end = DRV_AUDIO_SAMPLE_RATE * 120 / 1000;
    size_t n_err = DRV_AUDIO_SAMPLE_RATE * 350 / 1000;
    s_ctx.tone_buf[SVC_TONE_OK]    = heap_caps_malloc(n_ok  * 2, MALLOC_CAP_SPIRAM);
    s_ctx.tone_buf[SVC_TONE_END]   = heap_caps_malloc(n_end * 2, MALLOC_CAP_SPIRAM);
    s_ctx.tone_buf[SVC_TONE_ERROR] = heap_caps_malloc(n_err * 2, MALLOC_CAP_SPIRAM);
    s_ctx.tone_len[SVC_TONE_OK]    = n_ok;
    s_ctx.tone_len[SVC_TONE_END]   = n_end;
    s_ctx.tone_len[SVC_TONE_ERROR] = n_err;

    for (int i = 0; i < 4; i++) {
        ESP_RETURN_ON_FALSE(s_ctx.tone_buf[i] != NULL, ESP_ERR_NO_MEM, TAG,
                            "tone buf alloc failed");
    }

    tone_gen(s_ctx.tone_buf[SVC_TONE_WAKE], DRV_AUDIO_SAMPLE_RATE * 100 / 1000, 880.0f, 0.6f);
    tone_silence(s_ctx.tone_buf[SVC_TONE_WAKE] + DRV_AUDIO_SAMPLE_RATE * 100 / 1000,
                 DRV_AUDIO_SAMPLE_RATE * 60 / 1000);
    tone_gen(s_ctx.tone_buf[SVC_TONE_WAKE] + DRV_AUDIO_SAMPLE_RATE * 160 / 1000,
             DRV_AUDIO_SAMPLE_RATE * 140 / 1000, 1320.0f, 0.6f);

    tone_gen(s_ctx.tone_buf[SVC_TONE_OK], n_ok, 1200.0f, 0.5f);
    tone_gen(s_ctx.tone_buf[SVC_TONE_END], n_end, 900.0f, 0.5f);
    tone_gen(s_ctx.tone_buf[SVC_TONE_ERROR], n_err, 300.0f, 0.6f);
    return ESP_OK;
}

/* ============== 播放 ============== */

/* 内部播放：写入 I2S直到完成/打断；调用方需持有 play_mutex */
static esp_err_t play_pcm_locked(const int16_t *pcm, size_t samples)
{
    s_ctx.play_abort = false;
    size_t chunk = FRAME_SAMPLES;               /* 20ms 一块，便于及时响应打断 */
    size_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < samples && !s_ctx.play_abort) {
        size_t n = samples - done;
        if (n > chunk) {
            n = chunk;
        }
        ret = drv_audio_write(pcm + done, n, 200);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "play write failed: %s", esp_err_to_name(ret));
            break;
        }
        done += n;
    }
    if (s_ctx.play_abort) {
        /* 打断：复位 I2S 清空 DMA 残留，避免拖尾音 */
        drv_audio_stop();
        drv_audio_start();
    }
    s_ctx.play_abort = false;
    return ret;
}

esp_err_t svc_audio_play_pcm(const int16_t *pcm, size_t samples)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pcm != NULL && samples > 0, ESP_ERR_INVALID_ARG, TAG, "null pcm");

    xSemaphoreTake(s_ctx.play_mutex, portMAX_DELAY);
    esp_err_t ret = play_pcm_locked(pcm, samples);
    xSemaphoreGive(s_ctx.play_mutex);

    post_audio_event(AUDIO_PLAYBACK_DONE, NULL, 0);
    return ret;
}

esp_err_t svc_audio_stop_play(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    s_ctx.play_abort = true;
    return ESP_OK;
}

esp_err_t svc_audio_play_tone(svc_tone_id_t id)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (id < SVC_TONE_WAKE || id > SVC_TONE_ERROR) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ctx.play_mutex, portMAX_DELAY);
    esp_err_t ret = play_pcm_locked(s_ctx.tone_buf[id], s_ctx.tone_len[id]);
    xSemaphoreGive(s_ctx.play_mutex);
    return ret;
}

/* ============== 录音会话（audio_rec 任务） ============== */

/**
 * @brief  计算一帧 RMS 的平方均值（避免开方，直接与门限平方比较）
 */
static uint64_t frame_energy(const int16_t *buf, size_t n)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = buf[i];
        sum += (uint64_t)(v * v);
    }
    return sum / n;
}

/**
 * @brief  结束当前会话：修剪无效头部并发布 VAD 结束事件
 */
static void session_end(void)
{
    s_ctx.recording = false;
    /* 修剪：保留语音起点前 200ms；未检出语音则丢弃（samples=0） */
    if (s_ctx.speech_detected) {
        s_ctx.rec_valid = s_ctx.rec_total;
    } else {
        s_ctx.rec_valid = 0;
    }
    ESP_LOGI(TAG, "session end: total=%.1fs valid=%.1fs",
             (float)s_ctx.rec_total / DRV_AUDIO_SAMPLE_RATE,
             (float)s_ctx.rec_valid / DRV_AUDIO_SAMPLE_RATE);
    post_audio_event(AUDIO_VAD_SPEECH_END, NULL, 0);
}

static void rec_task(void *arg)
{
    (void)arg;
    int16_t frame[FRAME_SAMPLES];
    rec_cmd_t cmd;

    while (s_ctx.running) {
        /* 空闲时等命令 */
        if (!s_ctx.recording) {
            if (xQueueReceive(s_ctx.cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (cmd == REC_CMD_START) {
                s_ctx.recording = true;
                s_ctx.rec_total = 0;
                s_ctx.rec_valid = 0;
                s_ctx.speech_detected = false;
                s_ctx.stop_req = false;
                ESP_LOGI(TAG, "recording start");
            } else {
                continue;   /* 未在录音时收到 STOP，忽略 */
            }
        }

        uint32_t session_ms = 0;
        uint32_t silence_ms = 0;            /* 连续静音时长 */
        uint32_t speech_ms = 0;             /* 已检出语音时长 */
        bool in_speech = false;
        size_t speech_start_frame = 0;
        size_t frame_idx = 0;

        while (s_ctx.running && s_ctx.recording) {
            /* 优先处理手动停止命令 */
            if (xQueueReceive(s_ctx.cmd_queue, &cmd, 0) == pdTRUE) {
                if (cmd == REC_CMD_STOP) {
                    break;
                }
            }

            esp_err_t ret = drv_audio_read(frame, FRAME_SAMPLES, 1000);
            if (ret == ESP_ERR_TIMEOUT) {
                continue;                   /* I2S 停了或暂时无数据 */
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(ret));
                post_audio_err(ret);
                break;
            }

            /* 存入会话缓冲（防溢出，超出即强制结束） */
            size_t cap = REC_BUF_BYTES / sizeof(int16_t);
            if (s_ctx.rec_total + FRAME_SAMPLES > cap) {
                ESP_LOGW(TAG, "record buffer full, force end");
                break;
            }
            memcpy(s_ctx.rec_buf + s_ctx.rec_total, frame,
                   FRAME_SAMPLES * sizeof(int16_t));
            s_ctx.rec_total += FRAME_SAMPLES;
            frame_idx++;

            /* 能量 VAD 状态机 */
            uint64_t e = frame_energy(frame, FRAME_SAMPLES);
            uint64_t thr2 = (uint64_t)s_ctx.vad_threshold * s_ctx.vad_threshold;
            bool loud = e > thr2;
            session_ms += 20;

            if (!in_speech) {
                if (loud) {
                    silence_ms = 0;
                    speech_ms += 20;
                    if (speech_ms >= 60) {  /* 连续 60ms 超门限，判定开始说话 */
                        in_speech = true;
                        s_ctx.speech_detected = true;
                        speech_start_frame = frame_idx > 3 ? frame_idx - 3 : 0;
                        ESP_LOGI(TAG, "speech start (energy=%lu)",
                                 (unsigned long)e);
                        post_audio_event(AUDIO_VAD_SPEECH_START, NULL, 0);
                    }
                } else {
                    speech_ms = 0;
                    /* 未说话超时由状态机处理（LISTENING 15s 超时） */
                }
            } else {
                if (loud) {
                    silence_ms = 0;
                    speech_ms += 20;
                } else {
                    silence_ms += 20;
                    speech_ms += 20;
                    if (silence_ms >= CONFIG_SVC_AUDIO_SILENCE_MS) {
                        /* 说话后静音达到断句时长 */
                        if (speech_ms >= MIN_SPEECH_FRAMES * 20) {
                            ESP_LOGI(TAG, "speech end by silence");
                            break;      /* 正常断句 */
                        }
                    }
                }
            }

            /* 单会话最长保护 */
            if (session_ms >= MAX_SESSION_MS) {
                ESP_LOGW(TAG, "session max duration reached");
                break;
            }
        }

        if (s_ctx.recording) {
            /* 计算 VAD 修剪起点（回退 200ms 预触发） */
            if (s_ctx.speech_detected) {
                size_t trim_frames = speech_start_frame > PRE_TRIGGER_FRAMES
                                     ? speech_start_frame - PRE_TRIGGER_FRAMES : 0;
                size_t trim_samples = trim_frames * FRAME_SAMPLES;
                if (trim_samples < s_ctx.rec_total) {
                    memmove(s_ctx.rec_buf, s_ctx.rec_buf + trim_samples,
                            (s_ctx.rec_total - trim_samples) * sizeof(int16_t));
                    s_ctx.rec_total -= trim_samples;
                } else {
                    s_ctx.rec_total = 0;
                }
            }
            session_end();
        }
    }
    s_ctx.rec_task = NULL;
    vTaskDelete(NULL);
}

/* ============== 业务 API ============== */

esp_err_t svc_audio_start_recording(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    ESP_RETURN_ON_FALSE(!s_ctx.recording, ESP_ERR_INVALID_STATE, TAG, "already recording");
    rec_cmd_t cmd = REC_CMD_START;
    xQueueSend(s_ctx.cmd_queue, &cmd, 0);
    return ESP_OK;
}

esp_err_t svc_audio_stop_recording(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    if (s_ctx.recording) {
        rec_cmd_t cmd = REC_CMD_STOP;
        xQueueSend(s_ctx.cmd_queue, &cmd, 0);
    }
    return ESP_OK;
}

esp_err_t svc_audio_get_record_data(const int16_t **pcm, size_t *samples)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pcm != NULL && samples != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "null ptr");
    *pcm = s_ctx.rec_buf;
    *samples = s_ctx.rec_valid;
    return ESP_OK;
}

void svc_audio_set_vad_threshold(uint32_t thr)
{
    s_ctx.vad_threshold = thr;
    ESP_LOGI(TAG, "vad threshold = %lu", (unsigned long)thr);
}

/* ============== 生命周期 ============== */

esp_err_t svc_audio_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 底层驱动：IO 扩展（功放/按键）+ Codec 配置 + I2S 通路
     * （App 层不直接依赖 Driver，xl9555 生命周期由本服务托管） */
    ESP_ERROR_CHECK(drv_xl9555_init());
    ESP_ERROR_CHECK(drv_es8388_init());
    ESP_ERROR_CHECK(drv_audio_init(NULL));

    /* PSRAM 大缓冲 */
    s_ctx.rec_buf = heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ctx.rec_buf != NULL, ESP_ERR_NO_MEM, TAG, "rec buf alloc failed");

    ESP_RETURN_ON_ERROR(tones_init(), TAG, "tones init failed");

    s_ctx.vad_threshold = CONFIG_SVC_AUDIO_VAD_THRESHOLD;
    s_ctx.play_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.play_mutex != NULL, ESP_FAIL, TAG, "play mutex failed");
    s_ctx.cmd_queue = xQueueCreate(4, sizeof(rec_cmd_t));
    ESP_RETURN_ON_FALSE(s_ctx.cmd_queue != NULL, ESP_FAIL, TAG, "cmd queue failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

esp_err_t svc_audio_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(drv_xl9555_speaker_enable(true));   /* 打开功放 */
    ESP_ERROR_CHECK(drv_es8388_start());
    ESP_ERROR_CHECK(drv_audio_start());

    s_ctx.running = true;
    BaseType_t ok = xTaskCreate(rec_task, "audio_rec", 4096, NULL,
                                tskIDLE_PRIORITY + 3, &s_ctx.rec_task);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_FAIL, TAG, "create rec task failed");

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_audio_stop(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        s_ctx.running = false;
        vTaskDelay(pdMS_TO_TICKS(50));      /* 等待任务退出 */
        drv_audio_stop();
        drv_es8388_stop();
        drv_xl9555_speaker_enable(false);
    }
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t svc_audio_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    svc_audio_stop();
    vSemaphoreDelete(s_ctx.play_mutex);
    vQueueDelete(s_ctx.cmd_queue);
    for (int i = 0; i < 4; i++) {
        free(s_ctx.tone_buf[i]);
        s_ctx.tone_buf[i] = NULL;
    }
    free(s_ctx.rec_buf);
    s_ctx.rec_buf = NULL;
    drv_audio_deinit();
    drv_es8388_deinit();
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 控制台命令：audio ---- */
static int cmd_audio(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: audio <vol <0-100>|thr <val>|rec <sec>|tone <wake|ok|end|err>|stop>\n");
        return 1;
    }
    if (strcmp(argv[1], "vol") == 0 && argc >= 3) {
        uint8_t vol = (uint8_t)atoi(argv[2]);
        ESP_ERROR_CHECK(drv_es8388_set_volume(vol));
        printf("volume = %u\n", vol);
    } else if (strcmp(argv[1], "thr") == 0 && argc >= 3) {
        svc_audio_set_vad_threshold((uint32_t)strtoul(argv[2], NULL, 10));
    } else if (strcmp(argv[1], "rec") == 0 && argc >= 3) {
        /* 自测：录音 sec 秒后回放 */
        int sec = atoi(argv[2]);
        if (sec < 1 || sec > 15) {
            printf("sec range 1~15\n");
            return 1;
        }
        ESP_ERROR_CHECK(svc_audio_start_recording());
        vTaskDelay(pdMS_TO_TICKS(sec * 1000));
        ESP_ERROR_CHECK(svc_audio_stop_recording());
        vTaskDelay(pdMS_TO_TICKS(100));     /* 等会话结束事件 */
        const int16_t *pcm = NULL;
        size_t samples = 0;
        svc_audio_get_record_data(&pcm, &samples);
        printf("captured %.2fs, playing back...\n",
               (float)samples / DRV_AUDIO_SAMPLE_RATE);
        if (samples > 0) {
            svc_audio_play_pcm(pcm, samples);
        }
    } else if (strcmp(argv[1], "tone") == 0 && argc >= 3) {
        svc_tone_id_t id = SVC_TONE_WAKE;
        if (strcmp(argv[2], "ok") == 0) {
            id = SVC_TONE_OK;
        } else if (strcmp(argv[2], "end") == 0) {
            id = SVC_TONE_END;
        } else if (strcmp(argv[2], "err") == 0) {
            id = SVC_TONE_ERROR;
        }
        ESP_ERROR_CHECK(svc_audio_play_tone(id));
    } else if (strcmp(argv[1], "stop") == 0) {
        ESP_ERROR_CHECK(svc_audio_stop_play());
    } else {
        printf("unknown sub-command\n");
        return 1;
    }
    return 0;
}

void svc_audio_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audio",
        .help = "Audio self-test: audio <vol|thr|rec|tone|stop>",
        .func = cmd_audio,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'audio' cmd failed");
    }
}

#else /* CONFIG_SVC_AUDIO_ENABLE */

/* 服务被裁剪时提供空实现，保证链接通过 */
esp_err_t svc_audio_init(void)                      { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_start(void)                     { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_stop(void)                      { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_deinit(void)                    { return ESP_ERR_NOT_SUPPORTED; }
void      svc_audio_register_console_cmds(void)     {}
esp_err_t svc_audio_start_recording(void)           { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_stop_recording(void)            { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_get_record_data(const int16_t **pcm, size_t *samples)
{
    (void)pcm; (void)samples; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t svc_audio_play_pcm(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t svc_audio_stop_play(void)                 { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_audio_play_tone(svc_tone_id_t id)     { (void)id; return ESP_ERR_NOT_SUPPORTED; }
void svc_audio_set_vad_threshold(uint32_t thr)      { (void)thr; }

#endif /* CONFIG_SVC_AUDIO_ENABLE */