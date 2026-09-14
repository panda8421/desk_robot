/**
 * @file    svc_audio.c
 * @brief   音频前端服务实现（能量 VAD + 录放调度 + 提示音 + 离线伪唤醒）
 * @note    录音缓冲与提示音均在 PSRAM；VAD 为能量阈值法，参数可经 Kconfig/控制台调整。
 *          伪唤醒：空闲期 rec_task 持续读麦克风喂 esp-sr MultiNet，命中命令词
 *          （Kconfig 配置，默认"泡泡"）发布 AUDIO_WAKE_WORD_DETECTED；
 *          播放期间及结束后 600ms 内暂停判定，避免喇叭声误唤醒。
 */
#include "svc_audio.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_clk_tree.h"
#include "esp_console.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "drv_audio.h"
#include "drv_es8388.h"
#include "drv_xl9555.h"
#include "app_events.h"
#include "console_cmd.h"

#if CONFIG_SVC_WAKEWORD_ENABLE
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#endif

static const char *TAG = "svc_audio";

#if CONFIG_SVC_AUDIO_ENABLE

/* ============== 常量与私有配置 ============== */
#define REC_BUF_BYTES           (512 * 1024)    /* 录音缓冲 512KB PSRAM ≈ 16s */
#define FRAME_SAMPLES           320             /* VAD 处理帧：320 点 = 20ms */
#define PRE_TRIGGER_FRAMES      10              /* 起始语音前保留 10 帧 ≈ 200ms */
#define MIN_SPEECH_FRAMES       13              /* 有效语音最短 13 帧 ≈ 260ms */
#define MAX_SESSION_MS          15000           /* 单次会话最长时间 */

/*
 * 伪唤醒读取长度：必须与 drv_audio.c 的 I2S DMA 缓冲 dma_frame_num 默认值（480）
 * 一致，整缓冲消费。若读 320 留 160 残量，下一次 timeout=0 追加读会"部分拷贝 +
 * 超时"，已拷贝部分被整块丢弃——模型听到的语音每 30ms 缺 10ms，命令词永远
 * 匹配不上，宏观表现为 feed 恒 2/3 实时（deficit≈33%）且提频无效。
 */
#define WW_READ_FRAMES          480

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

    volatile bool     playing;              /* 播放进行中（唤醒侦测静默用） */
    volatile int64_t  wake_mute_until_us;   /* 播放结束后唤醒侦测静默截止时间 */

#if CONFIG_SVC_WAKEWORD_ENABLE
    srmodel_list_t   *srmodels;             /* esp-sr 模型列表（model 分区） */
    esp_mn_iface_t   *multinet;             /* MultiNet 接口 */
    model_iface_data_t *mn_data;            /* MultiNet 实例 */
    int16_t          *mn_chunk;             /* 喂模型的对齐缓冲 */
    int               mn_chunk_size;        /* 模型要求的 chunk 采样数 */
    int               mn_fill;              /* 已攒的采样数 */
    SemaphoreHandle_t ww_model_lock;        /* MultiNet 实例互斥（detect 非线程安全，
                                               离线自检与实时喂音分属不同任务） */
#endif
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

/* ============== 离线伪唤醒（MultiNet 命令词，CONFIG_SVC_WAKEWORD_ENABLE） ============== */
#if CONFIG_SVC_WAKEWORD_ENABLE

static uint64_t frame_energy(const int16_t *data, size_t samples);  /* 定义见 VAD 段 */

/* 未启用的 bool 选项不会写入 sdkconfig.h，补默认值 0（custom 模式） */
#ifndef CONFIG_SVC_WAKEWORD_STOCK_CMDS
#define CONFIG_SVC_WAKEWORD_STOCK_CMDS 0
#endif

/**
 * @brief  命令表模式（A/B 诊断）：Kconfig 为默认值，NVS 中的 "ww_stock" 可覆盖。
 *         stock = 模型出厂命令表（跳过运行时 FST 重建），custom = 自定义命令词。
 */
static bool ww_stock_mode_get(void)
{
    bool stock = CONFIG_SVC_WAKEWORD_STOCK_CMDS;
    nvs_handle_t h;
    if (nvs_open("svc_audio", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "ww_stock", &v) == ESP_OK) {
            stock = (v != 0);
        }
        nvs_close(h);
    }
    return stock;
}

/**
 * @brief  保存命令表模式到 NVS，重启后生效（模型 FST 只能在初始化时重建）
 */
static void ww_stock_mode_set(bool stock)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("svc_audio", NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_u8(h, "ww_stock", stock ? 1 : 0));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

/**
 * @brief  初始化伪唤醒：从 model 分区加载 mn6_cn，注册命令词
 * @note   只加载模型，不吃音频；喂帧在 rec_task 空闲期进行
 */
static esp_err_t wakeword_init(void)
{
    s_ctx.srmodels = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_ctx.srmodels != NULL, ESP_FAIL, TAG,
                        "model partition init failed");
    char *mn_name = esp_srmodel_filter(s_ctx.srmodels, ESP_MN_PREFIX, ESP_MN_CHINESE);
    ESP_RETURN_ON_FALSE(mn_name != NULL, ESP_FAIL, TAG,
                        "no chinese multinet model in partition");
    s_ctx.multinet = esp_mn_handle_from_name(mn_name);
    ESP_RETURN_ON_FALSE(s_ctx.multinet != NULL, ESP_FAIL, TAG,
                        "multinet handle failed");
    /* 30s 无命中由模型内部自动复位状态 */
    s_ctx.mn_data = s_ctx.multinet->create(mn_name, 30000);
    ESP_RETURN_ON_FALSE(s_ctx.mn_data != NULL, ESP_FAIL, TAG, "multinet create failed");
    /* 调试：无命中时让模型打印内部 CTC 贪心解码文本，直接观察"模型听见了什么" */
    if (s_ctx.multinet->open_log != NULL) {
        s_ctx.multinet->open_log(s_ctx.mn_data);
    }

    bool stock = ww_stock_mode_get();
    if (stock) {
        /* A/B 实验组：跳过运行时 FST 重建，保留模型出厂 312 条命令词，
         * 用于区分"重建后的 FST 坏了"还是"模型/喂音链路本身有问题" */
        ESP_LOGW(TAG, "ww mode=STOCK: model built-in commands "
                      "(try saying 'da kai kong tiao')");
    } else {
        /* 注册命令词：ID1 正式唤醒词；ID2 为加长的自然变体（比两音节更易命中） */
        ESP_RETURN_ON_ERROR(esp_mn_commands_alloc(s_ctx.multinet, s_ctx.mn_data),
                            TAG, "mn commands alloc failed");
        ESP_RETURN_ON_ERROR(esp_mn_commands_add(1, CONFIG_SVC_WAKEWORD_PHRASE),
                            TAG, "mn commands add failed");
        ESP_RETURN_ON_ERROR(esp_mn_commands_add(2, "ni hao pao pao"),
                            TAG, "mn commands add2 failed");
        esp_mn_error_t *err = esp_mn_commands_update();
        ESP_RETURN_ON_FALSE(err == NULL, ESP_FAIL, TAG, "mn phrase parse failed");
    }
    /* 阈值必须在 update 之后设置：set_speech_commands 可能重置内部阈值 */
    s_ctx.multinet->set_det_threshold(s_ctx.mn_data,
                                      CONFIG_SVC_WAKEWORD_THRESHOLD / 100.0f);

    s_ctx.mn_chunk_size = s_ctx.multinet->get_samp_chunksize(s_ctx.mn_data);
    /* 尾部预留一次最大读取量，保证整缓冲追加不越界 */
    s_ctx.mn_chunk = malloc((s_ctx.mn_chunk_size + WW_READ_FRAMES) * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(s_ctx.mn_chunk != NULL, ESP_ERR_NO_MEM, TAG,
                        "mn chunk alloc failed");
    s_ctx.mn_fill = 0;
    s_ctx.ww_model_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ctx.ww_model_lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "ww model lock alloc failed");

    if (!stock) {
        esp_mn_commands_print();        /* 已注册命令表 */
    }
    esp_mn_active_commands_print();     /* 模型内部 fst 生效命令表 */
    ESP_LOGI(TAG, "wake word: mode=%s model=%s phrase=%s rate=%d chunk=%d thr=%.2f",
             stock ? "stock" : "custom", mn_name, CONFIG_SVC_WAKEWORD_PHRASE,
             s_ctx.multinet->get_samp_rate(s_ctx.mn_data),
             s_ctx.mn_chunk_size, CONFIG_SVC_WAKEWORD_THRESHOLD / 100.0f);
    return ESP_OK;
}

static void wakeword_deinit(void)
{
    if (s_ctx.mn_data != NULL) {
        s_ctx.multinet->destroy(s_ctx.mn_data);
        s_ctx.mn_data = NULL;
    }
    esp_mn_commands_free();
    if (s_ctx.srmodels != NULL) {
        esp_srmodel_deinit(s_ctx.srmodels);
        s_ctx.srmodels = NULL;
    }
    free(s_ctx.mn_chunk);
    s_ctx.mn_chunk = NULL;
}

/**
 * @brief  命中详情：取模型识别结果（命令 ID/概率/文本），定位阈值与匹配质量
 */
static void ww_log_hit(bool offline)
{
    esp_mn_results_t *res = s_ctx.multinet->get_results(s_ctx.mn_data);
    if (res != NULL && res->num > 0) {
        ESP_LOGW(TAG, "%s wake word hit: id=%d prob=%.2f (%s)",
                 offline ? "ww offline selftest:" : "ww:",
                 res->command_id[0], res->prob[0], res->string);
    } else {
        ESP_LOGW(TAG, "%s wake word hit (no result detail)",
                 offline ? "ww offline selftest:" : "ww:");
    }
}

/**
 * @brief  空闲期喂一段音频给 MultiNet，攒满 chunk 判定，命中发唤醒事件
 * @note   播放期间/静默期内只攒不判，保持内部状态连续
 */
static void wakeword_feed(const int16_t *frame, size_t samples)
{
    memcpy(s_ctx.mn_chunk + s_ctx.mn_fill, frame, samples * sizeof(int16_t));
    s_ctx.mn_fill += samples;
    if (s_ctx.mn_fill < s_ctx.mn_chunk_size) {
        return;
    }
    if (s_ctx.playing || esp_timer_get_time() < s_ctx.wake_mute_until_us) {
        goto consume;                           /* 喇叭声不喂模型 */
    }
    /* MultiNet 实例非线程安全：离线自检（事件任务/rec 任务）可能正独占模型 */
    xSemaphoreTake(s_ctx.ww_model_lock, portMAX_DELAY);
    int64_t t0 = esp_timer_get_time();
    esp_mn_state_t st = s_ctx.multinet->detect(s_ctx.mn_data, s_ctx.mn_chunk);
    int64_t dt_us = esp_timer_get_time() - t0;
    /* 诊断：每 5s 打印喂入速率与判定计数。deficit = 1 - 喂入速率/16000，
     * 持续 >0 说明读循环跟不上实时、I2S 环形缓冲被覆盖丢音频；
     * dt 为单次 detect 耗时（avg/max），判断算力是否满足实时（须 <32ms） */
    static uint32_t ww_detects = 0;
    static uint32_t ww_dt_cnt = 0;
    static uint64_t ww_dt_sum_us = 0;
    static uint64_t ww_dt_max_us = 0;
    static uint64_t ww_peak_e = 0;
    static uint64_t ww_fed_samples = 0;
    static int64_t ww_last_log_us = 0;
    ww_detects++;
    ww_dt_cnt++;
    ww_dt_sum_us += (uint64_t)dt_us;
    if ((uint64_t)dt_us > ww_dt_max_us) {
        ww_dt_max_us = (uint64_t)dt_us;
    }
    ww_fed_samples += s_ctx.mn_chunk_size;  /* 每次 detect 实际消耗 chunk_size 样本 */
    {
        uint64_t e = frame_energy(s_ctx.mn_chunk, s_ctx.mn_fill);
        if (e > ww_peak_e) {
            ww_peak_e = e;
        }
    }
    if (esp_timer_get_time() - ww_last_log_us > 5000000) {
        int64_t now = esp_timer_get_time();
        if (ww_last_log_us == 0) {
            ww_last_log_us = now;
        } else {
            float sec = (now - ww_last_log_us) / 1000000.0f;
            float rate = ww_fed_samples / sec;
            float deficit = (1.0f - rate / DRV_AUDIO_SAMPLE_RATE) * 100.0f;
            esp_mn_results_t *r = s_ctx.multinet->get_results(s_ctx.mn_data);
            uint32_t clk_hz = 0;
            esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                         ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                         &clk_hz);
            ESP_LOGI(TAG, "ww debug: detects=%u st=%d playing=%d peak_e=%lu "
                          "feed=%.0fHz deficit=%.1f%% cand=%d "
                          "dt=%.1f/%.1fms clk=%ldMHz",
                     (unsigned)ww_detects, (int)st, (int)s_ctx.playing,
                     (unsigned long)ww_peak_e, rate, deficit,
                     r != NULL ? (int)r->num : -1,
                     ww_dt_cnt > 0 ?
                         (float)ww_dt_sum_us / ww_dt_cnt / 1000.0f : 0.0f,
                     ww_dt_max_us / 1000.0f,
                     (long)(clk_hz / 1000000));
            if (r != NULL && r->num > 0) {
                ESP_LOGI(TAG, "ww debug: top cand id=%d prob=%.2f str=%.60s",
                         r->command_id[0], r->prob[0], r->string);
            }
            ww_last_log_us = now;
            ww_fed_samples = 0;
            ww_dt_cnt = 0;
            ww_dt_sum_us = 0;
            ww_dt_max_us = 0;
        }
        ww_peak_e = 0;
    }
    if (st == ESP_MN_STATE_DETECTED) {
        s_ctx.multinet->clean(s_ctx.mn_data);
        ww_log_hit(false);
        post_audio_event(AUDIO_WAKE_WORD_DETECTED, NULL, 0);
    } else if (st == ESP_MN_STATE_TIMEOUT) {
        s_ctx.multinet->clean(s_ctx.mn_data);
    }
    xSemaphoreGive(s_ctx.ww_model_lock);
consume:
    memmove(s_ctx.mn_chunk, s_ctx.mn_chunk + s_ctx.mn_chunk_size,
            (s_ctx.mn_fill - s_ctx.mn_chunk_size) * sizeof(int16_t));
    s_ctx.mn_fill -= s_ctx.mn_chunk_size;
}

/**
 * @brief  离线自检：把一段 PCM 完整回喂 MultiNet（无实时压力、无丢帧），
 *         用于区分"喂链路问题"还是"模型/命令词问题"
 */
static void ww_selftest_pcm(const int16_t *pcm, size_t samples)
{
    if (s_ctx.mn_data == NULL || samples == 0) {
        return;
    }
    /* 独占模型：实时喂音（audio_rec 任务）此刻可能正在 detect，须等它让出 */
    if (xSemaphoreTake(s_ctx.ww_model_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "ww selftest: model busy, skipped");
        return;
    }
    s_ctx.multinet->clean(s_ctx.mn_data);
    /* 诊断：确认喂入的确实是语音（RMS² 正常语音 1e5~1e7，静音 <1000） */
    {
        int16_t peak = 0;
        for (size_t i = 0; i < samples; i++) {
            int16_t v = pcm[i] >= 0 ? pcm[i] : (int16_t)-pcm[i];
            if (v > peak) {
                peak = v;
            }
        }
        ESP_LOGI(TAG, "ww selftest input: samples=%u rms2=%lu peak=%d",
                 (unsigned)samples, (unsigned long)frame_energy(pcm, samples),
                 peak);
    }
    const int16_t *p = pcm;
    size_t left = samples;
    esp_mn_state_t st = ESP_MN_STATE_DETECTING;
    /* 每次 detect 固定消耗 chunk_size 样本；先 detect 后推进，
     * 循环条件保证读取不越过缓冲末尾（尾端不足一个 chunk 的部分丢弃） */
    /* 检测有数百毫秒延迟：语音结束后补喂 2s 静音，给模型留出出结果的时间窗 */
    static int16_t *silence = NULL;
    if (silence == NULL) {
        silence = calloc((size_t)s_ctx.mn_chunk_size, sizeof(int16_t));
    }
    size_t silence_left = (silence != NULL) ?
                          2 * DRV_AUDIO_SAMPLE_RATE / (size_t)s_ctx.mn_chunk_size : 0;
    int yield_cnt = 0;
    while (st == ESP_MN_STATE_DETECTING &&
           (left >= (size_t)s_ctx.mn_chunk_size || silence_left > 0)) {
        if (left >= (size_t)s_ctx.mn_chunk_size) {
            st = s_ctx.multinet->detect(s_ctx.mn_data, (int16_t *)p);
            p += s_ctx.mn_chunk_size;
            left -= s_ctx.mn_chunk_size;
        } else {
            silence_left--;
            st = s_ctx.multinet->detect(s_ctx.mn_data, silence);
        }
        if (++yield_cnt >= 8) {     /* 周期让步，避免长音频独占 CPU 饿死 IDLE 触发 task_wdt */
            yield_cnt = 0;
            taskYIELD();
        }
    }
    if (st == ESP_MN_STATE_DETECTED) {
        ww_log_hit(true);
    } else {
        ESP_LOGI(TAG, "ww offline selftest: no hit (st=%d, %.1fs audio)",
                 (int)st, (float)samples / DRV_AUDIO_SAMPLE_RATE);
        esp_mn_results_t *r = s_ctx.multinet->get_results(s_ctx.mn_data);
        if (r != NULL && r->num > 0) {
            ESP_LOGI(TAG, "ww offline selftest: top cand id=%d prob=%.2f "
                          "str=%.80s",
                     r->command_id[0], r->prob[0], r->string);
        }
    }
    s_ctx.multinet->clean(s_ctx.mn_data);
    xSemaphoreGive(s_ctx.ww_model_lock);
}

#endif /* CONFIG_SVC_WAKEWORD_ENABLE */

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
    s_ctx.playing = true;
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
    s_ctx.playing = false;
    /* 播放结束后静默 600ms 再恢复唤醒判定，避开喇叭拖尾 */
    s_ctx.wake_mute_until_us = esp_timer_get_time() + 600000;
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
#if CONFIG_SVC_WAKEWORD_ENABLE
    /* 离线自检：把本次录音完整回喂 MultiNet（无实时压力、无丢帧），
     * 喊"泡泡 xxx"后按 KEY0 触发一次对话即可验证 */
    ww_selftest_pcm(s_ctx.rec_buf, s_ctx.rec_valid);
#endif
    post_audio_event(AUDIO_VAD_SPEECH_END, NULL, 0);
}

static void rec_task(void *arg)
{
    (void)arg;
    int16_t frame[FRAME_SAMPLES];
#if CONFIG_SVC_WAKEWORD_ENABLE
    int16_t ww_frame[WW_READ_FRAMES];   /* 与 I2S DMA 缓冲等长，整缓冲读取 */
#endif
    rec_cmd_t cmd;

    while (s_ctx.running) {
        /* 空闲时等命令；伪唤醒开启时轮询（不阻塞），空闲期喂模型 */
        if (!s_ctx.recording) {
#if CONFIG_SVC_WAKEWORD_ENABLE
            BaseType_t have = xQueueReceive(s_ctx.cmd_queue, &cmd, 0);
#else
            BaseType_t have = xQueueReceive(s_ctx.cmd_queue, &cmd, portMAX_DELAY);
#endif
            if (have == pdTRUE) {
                if (cmd == REC_CMD_START) {
                    s_ctx.recording = true;
                    s_ctx.rec_total = 0;
                    s_ctx.rec_valid = 0;
                    s_ctx.speech_detected = false;
                    s_ctx.stop_req = false;
                    ESP_LOGI(TAG, "recording start");
                }
                continue;   /* 未在录音时收到 STOP，忽略 */
            }
#if CONFIG_SVC_WAKEWORD_ENABLE
            /* 伪唤醒：空闲期读麦克风喂 MultiNet。读取与 I2S DMA 缓冲（480 帧）
             * 整块对齐——若读 320 留残量，timeout=0 的追加读跨缓冲边界时会
             * "部分拷贝+超时"而整块丢弃已拷贝数据，导致模型每 30ms 缺 10ms、
             * deficit 恒 33%。首帧阻塞等数据，后续把积压一次读完（最多 8 块
             * ≈ 240ms）追赶实时 */
            for (int i = 0; i < 8; i++) {
                esp_err_t wret = drv_audio_read(ww_frame, WW_READ_FRAMES,
                                                i == 0 ? 100 : 0);
                if (wret != ESP_OK) {
                    /* 首帧阻塞读失败才算异常；追加读 timeout=0 在积压追平后
                     * 立即返回属正常流控，不计数不打日志 */
                    if (i == 0) {
                        static uint32_t ww_read_errs = 0;
                        if (++ww_read_errs % 50 == 0) {
                            ESP_LOGW(TAG, "ww debug: first read err x%u (%s)",
                                     (unsigned)ww_read_errs,
                                     esp_err_to_name(wret));
                        }
                    }
                    break;
                }
                wakeword_feed(ww_frame, WW_READ_FRAMES);
            }
#endif
            continue;
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

void svc_audio_ww_test_pcm(const int16_t *pcm, size_t samples)
{
#if CONFIG_SVC_WAKEWORD_ENABLE
    ww_selftest_pcm(pcm, samples);
#else
    (void)pcm;
    (void)samples;
#endif
}

/* ============== 生命周期 ============== */

esp_err_t svc_audio_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 底层驱动：Codec 配置 + I2S 通路
     * （App 层不直接依赖 Driver，xl9555 生命周期由 main.c 应用层托管，
     *   本服务只借用 drv_xl9555_speaker_enable() 控制功放） */
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

#if CONFIG_SVC_WAKEWORD_ENABLE
    ESP_RETURN_ON_ERROR(wakeword_init(), TAG, "wake word init failed");
#endif

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

    ESP_ERROR_CHECK(drv_xl9555_speaker_enable(true));   /* 打开功放（XL9555 已由应用层启动） */
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
#if CONFIG_SVC_WAKEWORD_ENABLE
    wakeword_deinit();
#endif
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
        printf("usage: audio <vol <0-100>|thr <val>|rec <sec>|tone <wake|ok|end|err>|stop"
#if CONFIG_SVC_WAKEWORD_ENABLE
               "|ww [stock|custom]"
#endif
               ">\n");
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
#if CONFIG_SVC_WAKEWORD_ENABLE
    } else if (strcmp(argv[1], "ww") == 0) {
        /* A/B 诊断：stock=模型出厂命令表（喊"打开空调"测试），custom=自定义命令词。
         * FST 只能在初始化时重建，切换后需重启（此处自动重启） */
        if (argc >= 3 &&
            (strcmp(argv[2], "stock") == 0 || strcmp(argv[2], "custom") == 0)) {
            bool stock = (strcmp(argv[2], "stock") == 0);
            ww_stock_mode_set(stock);
            printf("wakeword mode = %s, rebooting...\n", argv[2]);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        } else {
            printf("wakeword mode = %s (usage: audio ww <stock|custom>)\n"
                   "  stock : model built-in commands, test with 'da kai kong tiao'\n"
                   "  custom: '%s' + 3 test commands\n",
                   ww_stock_mode_get() ? "stock" : "custom",
                   CONFIG_SVC_WAKEWORD_PHRASE);
        }
#endif
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
        .help = "Audio self-test: audio <vol|thr|rec|tone|stop|ww>",
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