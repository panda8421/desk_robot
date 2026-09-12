/**
 * @file    conversation.c
 * @brief   对话状态机实现（纯事件驱动，esp_event handler 内做状态转移）
 *
 * 状态流（P1）：
 *   BOOT → NET_WAIT → (WiFi) → STANDBY → (KEY0) → LISTENING
 *        → (VAD断句) → RECOGNIZING → (ASR结果) → THINKING
 *        → (TTS就绪) → SPEAKING → (播放完成) → STANDBY
 * 连续模式（CONFIG_CHAT_CONTINUOUS，默认开）：
 *   SPEAKING → (播放完成) → LISTENING（免按键连续对话，静音超时回 STANDBY）
 */
#include "conversation.h"

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "app_events.h"
#include "app_types.h"
#include "svc_audio.h"
#include "svc_ai_chat.h"
#include "svc_status.h"

static const char *TAG = "conversation";

/* ---- 超时参数 ---- */
#define LISTEN_TIMEOUT_MS       15000   /* LISTENING 无语音超时 */
#define THINK_TIMEOUT_MS        30000   /* RECOGNIZING/THINKING 云端超时 */

/* ---- 私有状态 ---- */
typedef struct {
    volatile chat_state_t state;
    esp_timer_handle_t    timeout_timer;    /* LISTENING/THINKING 超时 */
    esp_timer_handle_t    rec_delay_timer;  /* 提示音播完后延迟开录音 */
} conversation_ctx_t;

static conversation_ctx_t s_ctx;

#define REC_DELAY_MS            350     /* TONE_WAKE 约 280ms，留余量 */

/* ============== 内部工具 ============== */

static void set_state(chat_state_t next)
{
    if (s_ctx.state == next) {
        return;
    }
    s_ctx.state = next;
    ESP_LOGI(TAG, "state -> %d", (int)next);

    /* 广播状态变化（行为层等第三方据此联动，如云台动画） */
    int32_t st = (int32_t)next;
    esp_event_post(CHAT_EVENT, CHAT_STATE_CHANGED, &st, sizeof(st), 0);

    /* 状态指示灯（覆盖 svc_status 心跳，直观反映对话状态） */
    switch (next) {
    case CHAT_STATE_STANDBY:
        svc_status_set_led_mode(SVC_LED_BLINK, 500);
        break;
    case CHAT_STATE_LISTENING:
    case CHAT_STATE_RECOGNIZING:
        svc_status_set_led_mode(SVC_LED_BLINK, 100);    /* 快闪 */
        break;
    case CHAT_STATE_THINKING:
    case CHAT_STATE_SPEAKING:
        svc_status_set_led_mode(SVC_LED_ON, 0);
        break;
    default:
        svc_status_set_led_mode(SVC_LED_BLINK, 1000);
        break;
    }
}

/* 通用兜底：清理并回到待机 */
static void abort_to_standby(bool play_err_tone)
{
    esp_timer_stop(s_ctx.timeout_timer);
    svc_audio_stop_play();
    if (play_err_tone) {
        svc_audio_play_tone(SVC_TONE_ERROR);
    }
    set_state(CHAT_STATE_STANDBY);
}

/* ---- 超时定时器回调（esp_timer 任务上下文） ---- */
static void rec_delay_cb(void *arg)
{
    (void)arg;
    svc_audio_start_recording();
}

static void timeout_cb(void *arg)
{
    (void)arg;
    switch (s_ctx.state) {
    case CHAT_STATE_LISTENING:
        ESP_LOGW(TAG, "listen timeout");
        /* 结束录音（触发 END 事件，空数据走 STANDBY 分支） */
        svc_audio_stop_recording();
        break;
    case CHAT_STATE_RECOGNIZING:
    case CHAT_STATE_THINKING:
        ESP_LOGW(TAG, "cloud timeout");
        abort_to_standby(true);
        break;
    default:
        break;
    }
}

static void arm_timeout(uint64_t ms)
{
    esp_timer_stop(s_ctx.timeout_timer);
    esp_timer_start_once(s_ctx.timeout_timer, ms * 1000);
}

/* ============== 事件处理 ============== */

/* ---- KEY_EVENT：KEY0 开始对话 / KEY1 打断播放 / KEY2 清空上下文 ---- */
static void on_key(int32_t id, void *data)
{
    uint8_t key = *(uint8_t *)data;
    if (id != KEY_PRESSED) {
        return;
    }
    if (key == 0 && s_ctx.state == CHAT_STATE_STANDBY) {
        ESP_LOGI(TAG, "KEY0: start conversation");
        svc_audio_play_tone(SVC_TONE_WAKE);
        set_state(CHAT_STATE_LISTENING);
        arm_timeout(LISTEN_TIMEOUT_MS);
        /* 提示音与录音错开：避免咪头录到嘟嘟声污染 VAD/ASR */
        esp_timer_start_once(s_ctx.rec_delay_timer, REC_DELAY_MS * 1000);
    } else if (key == 1 && s_ctx.state == CHAT_STATE_SPEAKING) {
        ESP_LOGI(TAG, "KEY1: interrupt playback");
        svc_audio_stop_play();      /* DONE 事件负责回 STANDBY */
    } else if (key == 2) {
        svc_ai_chat_reset_context();
        svc_audio_play_tone(SVC_TONE_OK);
    }
}

/* ---- AUDIO_EVENT ---- */
static void on_audio(int32_t id, void *data)
{
    (void)data;
    switch (id) {
    case AUDIO_VAD_SPEECH_START:
        ESP_LOGI(TAG, "vad: speech start");
        break;

    case AUDIO_VAD_SPEECH_END: {
        if (s_ctx.state != CHAT_STATE_LISTENING) {
            break;      /* 非监听态的会话结束（超时兜底已处理），忽略 */
        }
        esp_timer_stop(s_ctx.timeout_timer);
        const int16_t *pcm = NULL;
        size_t samples = 0;
        svc_audio_get_record_data(&pcm, &samples);
        if (samples == 0) {
            ESP_LOGW(TAG, "no speech captured");
#if !CONFIG_CHAT_CONTINUOUS
            svc_audio_play_tone(SVC_TONE_ERROR);
#endif
            set_state(CHAT_STATE_STANDBY);
            break;
        }
        svc_ai_chat_recognize(pcm, samples);
        arm_timeout(THINK_TIMEOUT_MS);
        set_state(CHAT_STATE_RECOGNIZING);
        break;
    }

    case AUDIO_PLAYBACK_DONE:
        if (s_ctx.state == CHAT_STATE_SPEAKING) {
#if CONFIG_CHAT_CONTINUOUS
            /* 连续对话：播完自动重新监听（延迟开录避开喇叭尾音） */
            ESP_LOGI(TAG, "continuous: relisten");
            set_state(CHAT_STATE_LISTENING);
            arm_timeout(LISTEN_TIMEOUT_MS);
            esp_timer_start_once(s_ctx.rec_delay_timer, REC_DELAY_MS * 1000);
#else
            set_state(CHAT_STATE_STANDBY);
#endif
        }
        break;

    case AUDIO_ERROR:
        ESP_LOGE(TAG, "audio error");
        if (s_ctx.state == CHAT_STATE_LISTENING) {
            abort_to_standby(true);
        }
        break;

    default:
        break;
    }
}

/* ---- CHAT_EVENT ---- */
static void on_chat(int32_t id, void *data)
{
    switch (id) {
    case CHAT_ASR_RESULT: {
        if (s_ctx.state != CHAT_STATE_RECOGNIZING) {
            break;
        }
        chat_text_t *text = (chat_text_t *)data;
        if (strlen(text->text) == 0) {
            ESP_LOGW(TAG, "asr empty result");
            svc_audio_play_tone(SVC_TONE_ERROR);
            set_state(CHAT_STATE_STANDBY);
            break;
        }
        svc_ai_chat_ask(text->text);
        arm_timeout(THINK_TIMEOUT_MS);
        set_state(CHAT_STATE_THINKING);
        break;
    }

    case CHAT_LLM_REPLY: {
        chat_text_t *text = (chat_text_t *)data;
        ESP_LOGI(TAG, "reply: %s", text->text);
        break;
    }

    case CHAT_TTS_READY: {
        /* THINKING：语音流程；STANDBY：控制台 chat ask 直调（绕过状态机） */
        if (s_ctx.state != CHAT_STATE_THINKING &&
            s_ctx.state != CHAT_STATE_STANDBY) {
            break;
        }
        const int16_t *pcm = NULL;
        size_t samples = 0;
        svc_ai_chat_get_tts_data(&pcm, &samples);
        if (samples == 0) {
            abort_to_standby(true);
            break;
        }
        esp_timer_stop(s_ctx.timeout_timer);
        svc_audio_play_pcm(pcm, samples);   /* 完成后发 AUDIO_PLAYBACK_DONE */
        set_state(CHAT_STATE_SPEAKING);
        break;
    }

    case CHAT_ERROR:
        abort_to_standby(true);
        break;

    default:
        break;
    }
}

/* ---- WiFi 事件 ---- */
static void on_wifi(int32_t id, void *data)
{
    (void)data;
    if (id == WIFI_GOT_IP) {
        ESP_LOGI(TAG, "network ready");
        if (s_ctx.state == CHAT_STATE_NET_WAIT) {
            svc_audio_play_tone(SVC_TONE_OK);
            set_state(CHAT_STATE_STANDBY);
        }
    } else if (id == WIFI_DISCONNECTED) {
        ESP_LOGW(TAG, "network lost");
        if (s_ctx.state != CHAT_STATE_NET_WAIT && s_ctx.state != CHAT_STATE_BOOT) {
            svc_audio_stop_recording();
            svc_audio_stop_play();
            set_state(CHAT_STATE_NET_WAIT);
        }
    }
}

/* ---- 统一事件分发器 ---- */
static void event_dispatch(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == KEY_EVENT) {
        on_key(id, data);
    } else if (base == AUDIO_EVENT) {
        on_audio(id, data);
    } else if (base == CHAT_EVENT) {
        on_chat(id, data);
    } else if (base == APP_WIFI_EVENT) {
        on_wifi(id, data);
    }
}

/* ============== 公开 API ============== */

void conversation_init(void)
{
    /* 超时定时器 */
    const esp_timer_create_args_t timer_args = {
        .callback = &timeout_cb,
        .name = "chat_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.timeout_timer));

    /* 提示音延迟开录音定时器 */
    const esp_timer_create_args_t rec_timer_args = {
        .callback = &rec_delay_cb,
        .name = "rec_delay",
    };
    ESP_ERROR_CHECK(esp_timer_create(&rec_timer_args, &s_ctx.rec_delay_timer));

    /* 订阅四类事件（默认事件循环） */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(KEY_EVENT, ESP_EVENT_ANY_ID,
                                                        event_dispatch, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(AUDIO_EVENT, ESP_EVENT_ANY_ID,
                                                        event_dispatch, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(CHAT_EVENT, ESP_EVENT_ANY_ID,
                                                        event_dispatch, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(APP_WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        event_dispatch, NULL, NULL));

    s_ctx.state = CHAT_STATE_NET_WAIT;
    ESP_LOGI(TAG, "conversation state machine ready");
}

void conversation_reset(void)
{
    svc_ai_chat_reset_context();
    if (s_ctx.state == CHAT_STATE_NET_WAIT) {
        return;     /* 网络未就绪，保持等待 */
    }
    abort_to_standby(false);
}

chat_state_t conversation_get_state(void)
{
    return s_ctx.state;
}
