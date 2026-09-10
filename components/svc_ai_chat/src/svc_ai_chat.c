/**
 * @file    svc_ai_chat.c
 * @brief   云端对话服务实现（P1：HTTP 串行流水线 ASR→LLM→TTS）
 * @note    协议端点以阿里 DashScope 官方文档为准（OpenAI 兼容模式）；
 *          ASR 识别音频经 base64 内嵌 JSON；TTS 返回 WAV，内部统一转 16k/mono/16bit
 */
#include "svc_ai_chat.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "app_events.h"
#include "app_types.h"
#include "console_cmd.h"

static const char *TAG = "svc_ai_chat";

#if CONFIG_SVC_AI_CHAT_ENABLE

/* ============== 常量 ============== */
#define NVS_NAMESPACE           "ai_chat"
#define NVS_KEY_API_KEY         "api_key"
#define HTTP_TIMEOUT_MS         20000
#define JSON_RESP_MAX           (64 * 1024)     /* LLM/ASR JSON 响应上限 */
#define TTS_BUF_MAX             (1024 * 1024)   /* TTS WAV 缓冲上限（PSRAM） */
#define HISTORY_ROUNDS          4               /* 保留最近 4 轮上下文 */
#define HISTORY_SLOTS           (HISTORY_ROUNDS * 2 + 1)

/* ---- 私有状态 ---- */
typedef struct {
    bool            initialized;
    bool            running;
    char            api_key[128];
    QueueHandle_t   req_queue;
    TaskHandle_t    worker;

    /* 多轮上下文：环形历史 + 覆写 */
    struct {
        char role[10];
        char content[CHAT_TEXT_MAX_LEN];
    } history[HISTORY_SLOTS];
    int  hist_cnt;

    /* TTS 结果 */
    int16_t        *tts_buf;        /* PSRAM，统一 16k/mono/16bit */
    size_t          tts_samples;
} svc_ai_chat_ctx_t;

/* 工作请求 */
typedef enum {
    REQ_ASR,
    REQ_ASK,
} req_type_t;

typedef struct {
    req_type_t       type;
    const int16_t   *pcm;       /* REQ_ASR：录音数据 */
    size_t           samples;
    char             text[CHAT_TEXT_MAX_LEN];   /* REQ_ASK：用户文本 */
} chat_req_t;

static svc_ai_chat_ctx_t s_ctx;

/* TTS 前向声明（在 do_llm_and_tts 中调用） */
static esp_err_t do_tts(const char *text);

/* ============== 事件发布 ============== */
static void post_chat_text(int32_t id, const char *text)
{
    chat_text_t payload = {0};
    strncpy(payload.text, text ? text : "", sizeof(payload.text) - 1);
    esp_event_post(CHAT_EVENT, id, &payload, sizeof(payload), 0);
}

static void post_chat_error(int32_t code, char stage)
{
    chat_err_info_t payload = { .code = code, .stage = stage };
    ESP_LOGE(TAG, "chat error: stage=%c code=%s(%ld)", stage,
             esp_err_to_name((esp_err_t)code), (long)code);
    esp_event_post(CHAT_EVENT, CHAT_ERROR, &payload, sizeof(payload), 0);
}

/* ============== HTTP 通用请求（TLS 证书走 esp_crt_bundle） ============== */

/**
 * @brief  POST JSON 并接收响应
 * @param  url       完整 URL
 * @param  body      请求体（JSON）
 * @param  resp_buf  输出：调用方提供缓冲（NULL 则只收头部）
 * @param  resp_max  响应缓冲容量
 * @param  resp_len  输出：实际响应长度
 * @return ESP_OK（HTTP 2xx）/ ESP_FAIL（HTTP 非 2xx，状态码记日志）/ 错误码
 */
static esp_err_t http_post(const char *url, const char *body,
                           uint8_t *resp_buf, size_t resp_max, size_t *resp_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 内置主流 CA 根证书 */
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "http init failed");

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_ctx.api_key);
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", auth));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", "application/json"));

    esp_err_t ret = ESP_FAIL;
    int status = -1;
    *resp_len = 0;

    if (esp_http_client_open(client, (int)strlen(body)) != ESP_OK) {
        ESP_LOGE(TAG, "http open failed");
        goto out;
    }
    if (esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        ESP_LOGE(TAG, "http write failed");
        goto out;
    }
    status = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    if (resp_buf != NULL) {
        size_t got = 0;
        while (got < resp_max - 1) {
            int n = esp_http_client_read(client, (char *)resp_buf + got,
                                         (int)(resp_max - 1 - got));
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        resp_buf[got] = '\0';
        *resp_len = got;
    }

    if (status >= 200 && status < 300) {
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "http status %d", status);
        if (resp_buf != NULL && resp_len != NULL && *resp_len > 0) {
            ESP_LOGE(TAG, "resp: %.*s", (int)(*resp_len > 512 ? 512 : *resp_len),
                     (const char *)resp_buf);
        }
        ret = ESP_FAIL;
    }
out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

/* ============== WAV 工具 ============== */

/* 在 PCM 前构建 44 字节标准 WAV 头（16k/mono/16bit） */
static void wav_build_header(uint8_t *hdr, uint32_t samples)
{
    uint32_t data_bytes = samples * 2;
    uint32_t chunk = 36 + data_bytes;
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16, rate = DRV_AUDIO_SAMPLE_RATE, byte_rate = rate * 2;
    uint16_t fmt_tag = 1, ch = 1, block = 2, bits = 16;
    memcpy(hdr + 16, &fmt_len, 4);
    memcpy(hdr + 20, &fmt_tag, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);
}

/**
 * @brief  解析 WAV 并转换为 16k/mono/16bit（线性插值重采样，取左声道）
 * @note   支持任意采样率、16bit、单/双声道 PCM
 */
static esp_err_t wav_to_16k_mono(const uint8_t *data, size_t len,
                                 int16_t *out, size_t out_cap, size_t *out_samples)
{
    if (len < 44 || memcmp(data, "RIFF", 4) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 定位 fmt 与 data 块 */
    size_t pos = 12;
    uint16_t channels = 1, bits = 16;
    uint32_t rate = 16000;
    const uint8_t *pcm = NULL;
    uint32_t pcm_bytes = 0;
    while (pos + 8 <= len) {
        uint32_t ck_size;
        memcpy(&ck_size, data + pos + 4, 4);
        if (memcmp(data + pos, "fmt ", 4) == 0 && ck_size >= 16) {
            memcpy(&channels, data + pos + 10, 2);
            memcpy(&rate, data + pos + 12, 4);
            memcpy(&bits, data + pos + 22, 2);
        } else if (memcmp(data + pos, "data", 4) == 0) {
            pcm = data + pos + 8;
            pcm_bytes = ck_size < (uint32_t)(len - pos - 8) ? ck_size : (uint32_t)(len - pos - 8);
            break;
        }
        pos += 8 + ck_size + (ck_size & 1);     /* 奇数块对齐 */
    }
    if (pcm == NULL || bits != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t in_frames = pcm_bytes / (2 * channels);      /* 每帧含所有声道 */
    if (in_frames == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 重采样：输出点数 = 输入帧数 * 16000 / rate */
    size_t out_n = (size_t)((uint64_t)in_frames * DRV_AUDIO_SAMPLE_RATE / rate);
    if (out_n > out_cap) {
        out_n = out_cap;
    }
    const int16_t *src = (const int16_t *)pcm;
    for (size_t i = 0; i < out_n; i++) {
        /* 输出点 i 对应输入浮点位置 */
        float fpos = (float)i * rate / DRV_AUDIO_SAMPLE_RATE;
        size_t idx = (size_t)fpos;
        float frac = fpos - (float)idx;
        int32_t a = src[idx * channels];            /* 左声道 */
        int32_t b = (idx + 1 < in_frames) ? src[(idx + 1) * channels] : a;
        out[i] = (int16_t)(a + (int32_t)((b - a) * frac));
    }
    *out_samples = out_n;
    return ESP_OK;
}

/* ============== ASR（语音识别） ============== */

static esp_err_t do_asr(const int16_t *pcm, size_t samples)
{
    if (samples == 0) {
        post_chat_error(ESP_ERR_INVALID_ARG, 'a');
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. PCM → WAV */
    size_t wav_len = samples * 2 + 44;
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(wav != NULL, ESP_ERR_NO_MEM, TAG, "wav buf failed");
    wav_build_header(wav, (uint32_t)samples);
    memcpy(wav + 44, pcm, samples * 2);

    /* 2. WAV → base64 */
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, wav, wav_len);     /* 先取长度 */
    char *json = heap_caps_malloc(b64_len + 1024, MALLOC_CAP_SPIRAM);
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (json == NULL) {
        goto cleanup_wav;
    }
    size_t b64_written = 0;
    ret = mbedtls_base64_encode((unsigned char *)json, b64_len + 1024, &b64_written,
                                wav, wav_len);
    if (ret != 0) {
        ret = ESP_FAIL;
        goto cleanup_json;
    }

    /* 3. 组装请求 JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_SVC_AI_CHAT_ASR_MODEL);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(msg, "content");
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "type", "input_audio");
    cJSON *ia = cJSON_CreateObject();
    cJSON_AddStringToObject(ia, "data", json);
    cJSON_AddStringToObject(ia, "format", "wav");
    cJSON_AddItemToObject(audio, "input_audio", ia);
    cJSON_AddItemToArray(content, audio);
    cJSON_AddItemToArray(msgs, msg);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup_json;
    }

    /* 4. 发送 */
    ESP_LOGI(TAG, "asr: %.1fs audio, %u bytes body",
             (float)samples / DRV_AUDIO_SAMPLE_RATE, (unsigned)strlen(body));
    uint8_t *resp = heap_caps_malloc(JSON_RESP_MAX, MALLOC_CAP_SPIRAM);
    size_t resp_len = 0;
    if (resp == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup_body;
    }
    ret = http_post(CONFIG_SVC_AI_CHAT_API_BASE "/chat/completions", body,
                    resp, JSON_RESP_MAX, &resp_len);
    if (ret != ESP_OK) {
        goto cleanup_resp;
    }

    /* 5. 解析响应 */
    cJSON *jresp = cJSON_Parse((const char *)resp);
    if (jresp == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup_resp;
    }
    cJSON *text_item = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(jresp, "choices"), 0), "message");
    const char *text = text_item ? cJSON_GetStringValue(
        cJSON_GetObjectItem(text_item, "content")) : NULL;
    if (text == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(jresp);
        goto cleanup_resp;
    }
    ESP_LOGI(TAG, "asr result: %s", text);
    post_chat_text(CHAT_ASR_RESULT, text);
    ret = ESP_OK;
    cJSON_Delete(jresp);

cleanup_resp:
    free(resp);
cleanup_body:
    cJSON_free(body);
cleanup_json:
    free(json);
cleanup_wav:
    free(wav);
    if (ret != ESP_OK) {
        post_chat_error(ret, 'a');
    }
    return ret;
}

/* ============== LLM 对话 ============== */

static esp_err_t push_history(const char *role, const char *content)
{
    if (s_ctx.hist_cnt >= HISTORY_SLOTS) {
        /* 满了：丢弃最老的一轮（前两条） */
        memmove(&s_ctx.history[0], &s_ctx.history[2],
                sizeof(s_ctx.history[0]) * (HISTORY_SLOTS - 2));
        s_ctx.hist_cnt = HISTORY_SLOTS - 2;
    }
    strncpy(s_ctx.history[s_ctx.hist_cnt].role, role,
            sizeof(s_ctx.history[0].role) - 1);
    strncpy(s_ctx.history[s_ctx.hist_cnt].content, content,
            sizeof(s_ctx.history[0].content) - 1);
    s_ctx.hist_cnt++;
    return ESP_OK;
}

static esp_err_t do_llm_and_tts(const char *user_text)
{
    /* 记录用户消息 */
    push_history("user", user_text);

    /* 组装 messages（system + 历史） */
    cJSON *msgs = cJSON_CreateArray();
    if (strlen(CONFIG_SVC_AI_CHAT_SYSTEM_PROMPT) > 0) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", CONFIG_SVC_AI_CHAT_SYSTEM_PROMPT);
        cJSON_AddItemToArray(msgs, sys);
    }
    for (int i = 0; i < s_ctx.hist_cnt; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", s_ctx.history[i].role);
        cJSON_AddStringToObject(m, "content", s_ctx.history[i].content);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_SVC_AI_CHAT_LLM_MODEL);
    cJSON_AddItemToObject(root, "messages", msgs);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "llm json failed");

    esp_err_t ret;
    uint8_t *resp = heap_caps_malloc(JSON_RESP_MAX, MALLOC_CAP_SPIRAM);
    size_t resp_len = 0;
    if (resp == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup_body;
    }
    ret = http_post(CONFIG_SVC_AI_CHAT_API_BASE "/chat/completions", body,
                    resp, JSON_RESP_MAX, &resp_len);
    if (ret != ESP_OK) {
        goto cleanup_resp;
    }

    cJSON *jresp = cJSON_Parse((const char *)resp);
    if (jresp == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup_resp;
    }
    cJSON *choice0 = cJSON_GetObjectItem(cJSON_GetObjectItem(jresp, "choices"), 0);
    const char *reply = cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetObjectItem(choice0, "message"), "content"));
    if (reply == NULL || strlen(reply) == 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
        cJSON_Delete(jresp);
        goto cleanup_resp;
    }
    ESP_LOGI(TAG, "llm reply: %s", reply);
    post_chat_text(CHAT_LLM_REPLY, reply);

    /* 记录助手回复（截断到缓冲长度） */
    push_history("assistant", reply);

    /* TTS */
    ret = do_tts(reply);
    cJSON_Delete(jresp);
    if (ret == ESP_OK) {
        esp_event_post(CHAT_EVENT, CHAT_TTS_READY, NULL, 0, 0);
    }

cleanup_resp:
    free(resp);
cleanup_body:
    cJSON_free(body);
    if (ret != ESP_OK) {
        post_chat_error(ret, ret == ESP_ERR_INVALID_RESPONSE ? 'l' : 'l');
    }
    return ret;
}

/* ============== TTS（语音合成，qwen-tts，返回 WAV） ============== */

static esp_err_t do_tts(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_SVC_AI_CHAT_TTS_MODEL);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", CONFIG_SVC_AI_CHAT_TTS_VOICE);
    cJSON_AddStringToObject(root, "response_format", "wav");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "tts json failed");

    esp_err_t ret;
    uint8_t *wav_buf = heap_caps_malloc(TTS_BUF_MAX, MALLOC_CAP_SPIRAM);
    size_t wav_len = 0;
    if (wav_buf == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup_body;
    }
    ret = http_post(CONFIG_SVC_AI_CHAT_API_BASE "/audio/speech", body,
                    wav_buf, TTS_BUF_MAX, &wav_len);
    if (ret != ESP_OK) {
        goto cleanup_wav;
    }
    if (wav_len < 44 || memcmp(wav_buf, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "tts response is not wav");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup_wav;
    }

    /* WAV → 16k/mono/16bit */
    ret = wav_to_16k_mono(wav_buf, wav_len, s_ctx.tts_buf, TTS_BUF_MAX / 2,
                          &s_ctx.tts_samples);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "tts: %.1fs audio ready",
                 (float)s_ctx.tts_samples / DRV_AUDIO_SAMPLE_RATE);
    }

cleanup_wav:
    free(wav_buf);
cleanup_body:
    cJSON_free(body);
    if (ret != ESP_OK) {
        post_chat_error(ret, 't');
    }
    return ret;
}

/* ============== 工作任务 ============== */

static void worker_task(void *arg)
{
    (void)arg;
    chat_req_t req;
    while (s_ctx.running) {
        if (xQueueReceive(s_ctx.req_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (req.type) {
        case REQ_ASR:
            do_asr(req.pcm, req.samples);
            break;
        case REQ_ASK:
            do_llm_and_tts(req.text);
            break;
        }
    }
    s_ctx.worker = NULL;
    vTaskDelete(NULL);
}

/* ============== 业务 API ============== */

esp_err_t svc_ai_chat_recognize(const int16_t *pcm, size_t samples)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    chat_req_t req = {
        .type = REQ_ASR,
        .pcm = pcm,
        .samples = samples,
    };
    if (xQueueSend(s_ctx.req_queue, &req, 0) != pdTRUE) {
        post_chat_error(ESP_ERR_INVALID_STATE, 'a');
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t svc_ai_chat_ask(const char *user_text)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized && s_ctx.running, ESP_ERR_INVALID_STATE,
                        TAG, "not running");
    ESP_RETURN_ON_FALSE(user_text != NULL, ESP_ERR_INVALID_ARG, TAG, "null text");
    chat_req_t req = { .type = REQ_ASK };
    strncpy(req.text, user_text, sizeof(req.text) - 1);
    if (xQueueSend(s_ctx.req_queue, &req, 0) != pdTRUE) {
        post_chat_error(ESP_ERR_INVALID_STATE, 'l');
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t svc_ai_chat_get_tts_data(const int16_t **pcm, size_t *samples)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pcm != NULL && samples != NULL, ESP_ERR_INVALID_ARG, TAG, "null ptr");
    *pcm = s_ctx.tts_buf;
    *samples = s_ctx.tts_samples;
    return ESP_OK;
}

esp_err_t svc_ai_chat_set_api_key(const char *key)
{
    ESP_RETURN_ON_FALSE(key != NULL && strlen(key) > 0, ESP_ERR_INVALID_ARG, TAG,
                        "empty key");
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    esp_err_t ret = nvs_set_str(h, NVS_KEY_API_KEY, key);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs write failed");
    strncpy(s_ctx.api_key, key, sizeof(s_ctx.api_key) - 1);
    s_ctx.api_key[sizeof(s_ctx.api_key) - 1] = '\0';
    ESP_LOGI(TAG, "api key saved");
    return ESP_OK;
}

esp_err_t svc_ai_chat_reset_context(void)
{
    s_ctx.hist_cnt = 0;
    ESP_LOGI(TAG, "context reset");
    return ESP_OK;
}

/* ============== 生命周期 ============== */

esp_err_t svc_ai_chat_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* TTS 输出缓冲（PSRAM 1MB） */
    s_ctx.tts_buf = heap_caps_malloc(TTS_BUF_MAX, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ctx.tts_buf != NULL, ESP_ERR_NO_MEM, TAG, "tts buf failed");
    s_ctx.tts_samples = 0;
    s_ctx.hist_cnt = 0;

    /* NVS 读取 API Key */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_ctx.api_key);
        nvs_get_str(h, NVS_KEY_API_KEY, s_ctx.api_key, &len);
        nvs_close(h);
    }
    if (strlen(s_ctx.api_key) == 0) {
        ESP_LOGW(TAG, "DashScope API key not set! use: chat setkey <key>");
    }

    s_ctx.req_queue = xQueueCreate(2, sizeof(chat_req_t));
    ESP_RETURN_ON_FALSE(s_ctx.req_queue != NULL, ESP_FAIL, TAG, "req queue failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done (llm=%s, asr=%s, tts=%s)",
             CONFIG_SVC_AI_CHAT_LLM_MODEL, CONFIG_SVC_AI_CHAT_ASR_MODEL,
             CONFIG_SVC_AI_CHAT_TTS_MODEL);
    return ESP_OK;
}

esp_err_t svc_ai_chat_start(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        return ESP_OK;
    }
    s_ctx.running = true;
    BaseType_t ok = xTaskCreate(worker_task, "chat_worker", 12288, NULL,
                                tskIDLE_PRIORITY + 3, &s_ctx.worker);
    ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_FAIL, TAG, "create worker failed");
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_ai_chat_stop(void)
{
    if (s_ctx.running) {
        s_ctx.running = false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t svc_ai_chat_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    svc_ai_chat_stop();
    vQueueDelete(s_ctx.req_queue);
    free(s_ctx.tts_buf);
    s_ctx.tts_buf = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 控制台命令：chat ---- */
static int cmd_chat(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: chat <setkey <key>|ask <text>|status|reset>\n");
        return 1;
    }
    if (strcmp(argv[1], "setkey") == 0 && argc >= 3) {
        if (svc_ai_chat_set_api_key(argv[2]) != ESP_OK) {
            printf("set key failed\n");
            return 1;
        }
        printf("api key saved to nvs\n");
    } else if (strcmp(argv[1], "ask") == 0 && argc >= 3) {
        /* 调试：跳过语音，直接文本对话（结果看日志与事件） */
        if (svc_ai_chat_ask(argv[2]) != ESP_OK) {
            printf("enqueue failed\n");
            return 1;
        }
        printf("request queued, see logs\n");
    } else if (strcmp(argv[1], "reset") == 0) {
        svc_ai_chat_reset_context();
        printf("context reset\n");
    } else if (strcmp(argv[1], "status") == 0) {
        printf("api_key: %s\n", strlen(s_ctx.api_key) > 0 ? "set" : "NOT SET");
        printf("history rounds: %d\n", s_ctx.hist_cnt / 2);
        printf("tts ready: %.1fs\n", (float)s_ctx.tts_samples / DRV_AUDIO_SAMPLE_RATE);
    } else {
        printf("unknown sub-command\n");
        return 1;
    }
    return 0;
}

void svc_ai_chat_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "chat",
        .help = "AI chat: chat <setkey|ask|status|reset>",
        .func = cmd_chat,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'chat' cmd failed");
    }
}

#else /* CONFIG_SVC_AI_CHAT_ENABLE */

/* 服务被裁剪时提供空实现，保证链接通过 */
esp_err_t svc_ai_chat_init(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_ai_chat_start(void)                   { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_ai_chat_stop(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_ai_chat_deinit(void)                  { return ESP_ERR_NOT_SUPPORTED; }
void      svc_ai_chat_register_console_cmds(void)   {}
esp_err_t svc_ai_chat_recognize(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t svc_ai_chat_ask(const char *user_text)
{
    (void)user_text; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t svc_ai_chat_get_tts_data(const int16_t **pcm, size_t *samples)
{
    (void)pcm; (void)samples; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t svc_ai_chat_set_api_key(const char *key)  { (void)key; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_ai_chat_reset_context(void)           { return ESP_OK; }

#endif /* CONFIG_SVC_AI_CHAT_ENABLE */