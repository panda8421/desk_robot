/**
 * @file    svc_behavior.c
 * @brief   行为服务实现：事件 → 云台拟人动画（关键帧编排 + 抢占）
 *
 * 事件映射（灵动风格，帧间隔短于插值行程 → 动作连贯圆滑）：
 *   CHAT_STATE_CHANGED LISTENING → GESTURE_LISTEN（侧头倾听）
 *   CHAT_STATE_CHANGED SPEAKING  → GESTURE_NOD（边说边点头）
 *   CHAT_STATE_CHANGED STANDBY   → GESTURE_HOME（回正）
 *   GESTURE_EVENT（LLM 标记/控制台）→ 对应动画
 *   PAN_TILT_MANUAL_CMD（控制台 pt 命令）→ 抢占停止动画
 *
 * 实现：每个动画是静态关键帧序列 {pan, tilt, hold_ms}，用 esp_timer
 * 一次性定时器链式调度，逐帧下发 svc_pan_tilt_set_target（其内部
 * FreeRTOS 任务做 100°/s 平滑插值）。新动画直接覆盖当前调度。
 */
#include "svc_behavior.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "app_events.h"
#include "app_types.h"
#include "svc_pan_tilt.h"
#include "console_cmd.h"

static const char *TAG = "svc_behavior";

#if CONFIG_SVC_BEHAVIOR_ENABLE

#define TICK_MS     100     /* 动画调度最小间隔（ms） */

/* 中位角（与 svc_pan_tilt 一致：min/max 中点，编译期常量） */
#define HOME_PAN    ((CONFIG_SVC_PAN_TILT_PAN_MIN + CONFIG_SVC_PAN_TILT_PAN_MAX) / 2)
#define HOME_TILT   ((CONFIG_SVC_PAN_TILT_TILT_MIN + CONFIG_SVC_PAN_TILT_TILT_MAX) / 2)

/* ---- 关键帧 ---- */
typedef struct {
    int16_t     pan, tilt;      /* 本帧目标角（度） */
    uint32_t    hold_ms;        /* 与下一帧的间隔；末帧忽略 */
} gesture_step_t;

typedef struct {
    const gesture_step_t *steps;
    size_t               len;
} gesture_prog_t;

/* 倾听位：pan 偏转 + tilt 微低（+/- 方向若与整机装配相反，对调符号即可） */
static const gesture_step_t PROG_LISTEN[] = {
    { HOME_PAN + 28, HOME_TILT - 14, 500 },
};
/* 点头：tilt 快速两下（110ms/帧 < 行程时间 → 圆滑换向） */
static const gesture_step_t PROG_NOD[] = {
    { HOME_PAN, HOME_TILT + 13, 110 },
    { HOME_PAN, HOME_TILT - 9,  110 },
    { HOME_PAN, HOME_TILT,      100 },
};
/* 摇头：pan 左右摆 + 收尾 */
static const gesture_step_t PROG_SHAKE[] = {
    { HOME_PAN + 22, HOME_TILT, 120 },
    { HOME_PAN - 22, HOME_TILT, 120 },
    { HOME_PAN + 12, HOME_TILT, 110 },
    { HOME_PAN,      HOME_TILT, 100 },
};
/* 歪头：tilt 保持片刻后回正 */
static const gesture_step_t PROG_TILT[] = {
    { HOME_PAN, HOME_TILT + 15, 450 },
    { HOME_PAN, HOME_TILT,      120 },
};
/* 低头委屈：缓慢低头保持后回正 */
static const gesture_step_t PROG_SAD[] = {
    { HOME_PAN, HOME_TILT + 10, 420 },
    { HOME_PAN, HOME_TILT,      150 },
};
/* 仰头惊讶：快速后仰一下回正 */
static const gesture_step_t PROG_SURPRISED[] = {
    { HOME_PAN, HOME_TILT - 10, 160 },
    { HOME_PAN, HOME_TILT,      120 },
};
/* 打瞌睡：头一点点往下栽 */
static const gesture_step_t PROG_SLEEPY[] = {
    { HOME_PAN, HOME_TILT + 6,  450 },
    { HOME_PAN, HOME_TILT + 12, 500 },
    { HOME_PAN, HOME_TILT,      150 },
};
/* 回正 */
static const gesture_step_t PROG_HOME[] = {
    { HOME_PAN, HOME_TILT, 0 },
};

static const gesture_prog_t PROGS[] = {
    [GESTURE_NOD]        = { PROG_NOD,   3 },
    [GESTURE_SHAKE]      = { PROG_SHAKE, 4 },
    [GESTURE_TILT_HEAD]  = { PROG_TILT,  2 },
    [GESTURE_LISTEN]     = { PROG_LISTEN, 1 },
    [GESTURE_HOME]       = { PROG_HOME,  1 },
    [GESTURE_SAD]        = { PROG_SAD,   2 },
    [GESTURE_SURPRISED]  = { PROG_SURPRISED, 2 },
    [GESTURE_SLEEPY]     = { PROG_SLEEPY, 3 },
};

/* ---- 私有状态 ---- */
typedef struct {
    bool               initialized;
    bool               running;
    esp_timer_handle_t chain_timer;
    const gesture_step_t *steps;    /* 当前动画剩余序列首指针 */
    size_t             remain;      /* 剩余帧数 */
} svc_behavior_ctx_t;

static svc_behavior_ctx_t s_ctx;

/* ============== 动画调度 ============== */

static void chain_cb(void *arg)
{
    (void)arg;
    if (s_ctx.remain == 0) {
        return;
    }
    const gesture_step_t *step = s_ctx.steps;
    svc_pan_tilt_set_target(step->pan, step->tilt);
    s_ctx.steps++;
    s_ctx.remain--;
    if (s_ctx.remain > 0 && step->hold_ms > 0) {
        esp_timer_start_once(s_ctx.chain_timer, (uint64_t)step->hold_ms * 1000);
    }
}

void svc_behavior_abort(void)
{
    if (!s_ctx.initialized) {
        return;
    }
    esp_timer_stop(s_ctx.chain_timer);
    s_ctx.steps = NULL;
    s_ctx.remain = 0;
}

esp_err_t svc_behavior_play(gesture_event_id_t g)
{
    if (!s_ctx.running || g < 0 || (size_t)g >= sizeof(PROGS) / sizeof(PROGS[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    const gesture_prog_t *prog = &PROGS[g];
    if (prog->len == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* 抢占：覆盖当前调度，立即执行第一帧 */
    esp_timer_stop(s_ctx.chain_timer);
    s_ctx.steps = prog->steps + 1;
    s_ctx.remain = prog->len - 1;
    svc_pan_tilt_set_target(prog->steps[0].pan, prog->steps[0].tilt);
    if (s_ctx.remain > 0 && prog->steps[0].hold_ms > 0) {
        esp_timer_start_once(s_ctx.chain_timer,
                             (uint64_t)prog->steps[0].hold_ms * 1000);
    }
    ESP_LOGD(TAG, "gesture %d", (int)g);
    return ESP_OK;
}

/* ============== 事件处理 ============== */

static void on_state_changed(int32_t state)
{
    switch ((chat_state_t)state) {
    case CHAT_STATE_LISTENING:
        svc_behavior_play(GESTURE_LISTEN);
        break;
    case CHAT_STATE_SPEAKING:
        svc_behavior_play(GESTURE_NOD);
        break;
    case CHAT_STATE_STANDBY:
        svc_behavior_play(GESTURE_HOME);
        break;
    default:
        /* RECOGNIZING/THINKING：保持当前姿态 */
        break;
    }
}

/* 情绪 → 云台动作联动（与 svc_face 平级消费 CHAT_EMOTION；NEUTRAL 无动作） */
static void on_emotion(int32_t emo)
{
    static const gesture_event_id_t map[EMO_MAX] = {
        [EMO_HAPPY]     = GESTURE_NOD,          /* 开心：点头 */
        [EMO_SAD]       = GESTURE_SAD,          /* 委屈：低头 */
        [EMO_ANGRY]     = GESTURE_SHAKE,        /* 生气：摇头 */
        [EMO_SURPRISED] = GESTURE_SURPRISED,    /* 惊讶：后仰 */
        [EMO_SHY]       = GESTURE_TILT_HEAD,    /* 害羞：歪头 */
        [EMO_SLEEPY]    = GESTURE_SLEEPY,       /* 困：打瞌睡 */
    };
    if (emo > (int32_t)EMO_NEUTRAL && emo < (int32_t)EMO_MAX) {
        svc_behavior_play(map[emo]);
    }
}

static void event_dispatch(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (!s_ctx.running) {
        return;
    }
    if (base == CHAT_EVENT && id == CHAT_STATE_CHANGED) {
        on_state_changed(*(int32_t *)data);
    } else if (base == CHAT_EVENT && id == CHAT_EMOTION) {
        on_emotion(*(int32_t *)data);
    } else if (base == GESTURE_EVENT) {
        svc_behavior_play((gesture_event_id_t)id);
    } else if (base == PAN_TILT_EVENT && id == PAN_TILT_MANUAL_CMD) {
        /* 用户手动接管，动画让位 */
        svc_behavior_abort();
    }
}

/* ============== 生命周期 ============== */

esp_err_t svc_behavior_init(void)
{
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = &chain_cb,
        .name = "behavior_chain",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ctx.chain_timer),
                        TAG, "create timer failed");
    s_ctx.initialized = true;
    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

esp_err_t svc_behavior_start(void)
{
    if (!s_ctx.initialized || s_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            CHAT_EVENT, CHAT_STATE_CHANGED, event_dispatch, NULL, NULL),
                        TAG, "sub chat state failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            CHAT_EVENT, CHAT_EMOTION, event_dispatch, NULL, NULL),
                        TAG, "sub emotion failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            GESTURE_EVENT, ESP_EVENT_ANY_ID, event_dispatch, NULL, NULL),
                        TAG, "sub gesture failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            PAN_TILT_EVENT, PAN_TILT_MANUAL_CMD, event_dispatch, NULL, NULL),
                        TAG, "sub manual cmd failed");
    s_ctx.running = true;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_behavior_stop(void)
{
    if (!s_ctx.running) {
        return ESP_ERR_INVALID_STATE;
    }
    svc_behavior_abort();
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t svc_behavior_deinit(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.running) {
        svc_behavior_stop();
    }
    esp_timer_delete(s_ctx.chain_timer);
    s_ctx.chain_timer = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}

/* ============== 控制台命令：gesture <动作名> ============== */
static int cmd_gesture(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: gesture <nod|shake|tilt|listen|home|sad|surprised|sleepy>\n");
        return 1;
    }
    gesture_event_id_t g;
    if (strcmp(argv[1], "nod") == 0) {
        g = GESTURE_NOD;
    } else if (strcmp(argv[1], "shake") == 0) {
        g = GESTURE_SHAKE;
    } else if (strcmp(argv[1], "tilt") == 0) {
        g = GESTURE_TILT_HEAD;
    } else if (strcmp(argv[1], "listen") == 0) {
        g = GESTURE_LISTEN;
    } else if (strcmp(argv[1], "home") == 0) {
        g = GESTURE_HOME;
    } else if (strcmp(argv[1], "sad") == 0) {
        g = GESTURE_SAD;
    } else if (strcmp(argv[1], "surprised") == 0) {
        g = GESTURE_SURPRISED;
    } else if (strcmp(argv[1], "sleepy") == 0) {
        g = GESTURE_SLEEPY;
    } else {
        printf("unknown gesture: %s\n", argv[1]);
        return 1;
    }
    esp_err_t err = svc_behavior_play(g);
    if (err != ESP_OK) {
        printf("play failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

void svc_behavior_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "gesture",
        .help = "Play a gesture: gesture <nod|shake|tilt|listen|home|sad|surprised|sleepy>",
        .func = cmd_gesture,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'gesture' cmd failed");
    }
}

#else /* CONFIG_SVC_BEHAVIOR_ENABLE */

esp_err_t svc_behavior_init(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_behavior_start(void)                   { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_behavior_stop(void)                    { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_behavior_deinit(void)                  { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_behavior_play(gesture_event_id_t g)    { (void)g; return ESP_ERR_NOT_SUPPORTED; }
void      svc_behavior_abort(void)                   {}
void      svc_behavior_register_console_cmds(void)   {}

#endif /* CONFIG_SVC_BEHAVIOR_ENABLE */
