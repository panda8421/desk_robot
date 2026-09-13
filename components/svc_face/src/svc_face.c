/**
 * @file    svc_face.c
 * @brief   表情服务实现：大眼萌（纯眼睛）+ 眨眼 + 扫视 + 情绪系统
 *
 * 无嘴设计：简单情绪靠眼型/点缀/节奏参数绘制（^ 弯月=开心、
 * 楔形+斜眉=生气、圆睁+眼神光=惊讶、眯眼+腮红=害羞、半闭慢节奏=困）；
 * 伤心这类复杂情绪用整帧手绘位图（垂目重眼皮大眼 + 泪珠 + 瘪嘴），
 * 位图由脚本生成后冻结进源码；说话时眼睛做果冻弹跳作为"正在说"反馈。
 *
 * 事件映射（与 svc_behavior 平级监听，conversation 零侵入）：
 *   CHAT_STATE_CHANGED → 状态默认表情（待机/倾听只有两颗大眼、说话果冻
 *                        弹跳、思考眯眼、BOOT 闭眼睡觉）
 *   CHAT_EMOTION       → 显式情绪（LLM 标记 [happy] 等/控制台 face 命令），
 *                        覆盖状态默认，EMOTION_HOLD_MS 后自动衰减回落
 *
 * 实现：独立 FreeRTOS 任务 50ms 周期渲染；事件回调只更新目标参数，
 * 渲染任务对参数做指数平滑（呆萌的"肉感"来源）。参数无变化时跳过
 * 刷屏（画面稳定、省总线），另每 2s 强制刷新一次自愈，控制台
 * disp test/clear 等外部显存改动最迟一个自愈周期内被覆盖。
 */
#include "svc_face.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_events.h"
#include "app_types.h"
#include "console_cmd.h"
#include "drv_display.h"

static const char *TAG = "svc_face";

#if CONFIG_SVC_FACE_ENABLE

#define FACE_TICK_MS        50      /* 渲染周期 */
#define REFRESH_PERIOD_MS   2000    /* 无变化时的强制自愈刷新间隔 */
#define EMOTION_HOLD_MS     7000    /* 显式情绪存活期，到期衰减回状态默认 */
#define BLINK_CLOSE_TICKS   3       /* 眨眼闭合保持 tick 数（150ms） */
#define EYE_H_MIN           4       /* 眨眼时眼睛压扁高度 */
#define JELLY_AMP           3       /* 说话果冻弹跳幅度（px） */
#define ARC_DEPTH           7       /* 弯月眼拱高（px） */
#define WEDGE_DROP          8       /* 楔形眼内端下沉量（px） */
#define WIDE_DIA            30      /* 惊讶圆睁眼直径（px） */
#define WIDE_SHIFT          2       /* 惊讶时两眼外移量（px） */

/* ---- 大眼萌几何（128x64） ---- */
#define EYE_W               20      /* 眼睛宽度 */
#define EYE_H_MAX           26      /* 睁眼最大高度 */
#define EYE_CY              26      /* 眼睛中心 y */
#define EYE_L_CX            38      /* 左眼中心 x（偏移 0 时） */
#define EYE_R_CX            90      /* 右眼中心 x */
#define EYE_DX_RANGE        9       /* 左右看最大水平偏移 */
#define BLUSH_Y             44      /* 腮红斜线最低点 y */

/* ---- 眼型 ---- */
typedef enum {
    EYE_SHAPE_RECT = 0,     /* 圆角大眼（常态） */
    EYE_SHAPE_ARC,          /* 弯月 ^（开心） */
    EYE_SHAPE_WEDGE,        /* 楔形（生气，内端下切） */
    EYE_SHAPE_BITMAP,       /* 整帧位图（委屈，手绘帧） */
    EYE_SHAPE_WIDE,         /* 圆睁 + 眼神光（惊讶） */
} eye_shape_t;

/* ---- 附加效果位 ---- */
#define EFFECT_NONE         0x00
#define EFFECT_BLUSH        0x01    /* 腮红斜线（害羞） */

/* ---- 情绪预设：显式情绪整体覆盖状态默认，存活期到后回落 ----
 * blink_pct：眨眼频率百分比（200=翻倍频闪的兴奋碎眨，40=懒洋洋）
 * smooth_div：平滑除数（越大过渡越慢越慵懒） */
typedef struct {
    eye_shape_t shape;
    uint8_t     eye_h;      /* 目标眼高（EYE_H_MIN~WIDE_DIA） */
    int8_t      eye_dx;     /* 视线水平偏移 */
    int8_t      brow_dir;   /* 斜眉：+1 内端向下(怒) / -1 内端向上(愁) / 0 无 */
    uint8_t     effect;     /* EFFECT_* 组合 */
    uint8_t     blink_pct;  /* 眨眼频率百分比 */
    uint8_t     smooth_div; /* 参数平滑除数 */
} face_preset_t;

static const face_preset_t EMO_PRESETS[EMO_MAX] = {
    /* shape             eye_h           dx             brow effect        blink div */
    [EMO_NEUTRAL]   = { EYE_SHAPE_RECT,  EYE_H_MAX,          0,  0, EFFECT_NONE,   100, 4 },
    [EMO_HAPPY]     = { EYE_SHAPE_ARC,   EYE_H_MAX,          0,  0, EFFECT_NONE,   200, 4 },
    [EMO_SAD]       = { EYE_SHAPE_BITMAP, 24, 0,  0, EFFECT_NONE,   100, 5 },
    [EMO_ANGRY]     = { EYE_SHAPE_WEDGE, EYE_H_MAX * 2 / 5,  0,  0, EFFECT_NONE,   100, 4 },
    [EMO_SURPRISED] = { EYE_SHAPE_WIDE,  WIDE_DIA,           0,  0, EFFECT_NONE,   100, 3 },
    [EMO_SHY]       = { EYE_SHAPE_RECT,  EYE_H_MAX / 2, EYE_DX_RANGE, 0, EFFECT_BLUSH, 100, 5 },
    [EMO_SLEEPY]    = { EYE_SHAPE_RECT,  6,                  0,  0, EFFECT_NONE,    40, 8 },
};

/* ---- 私有状态 ---- */
typedef struct {
    bool            running;

    /* 情绪目标（事件任务写，渲染任务读；均为标量，撕裂风险可忽略） */
    int             tgt_eye_h;      /* 眼睛目标高度 */
    int             tgt_eye_dx;     /* 眼睛目标水平偏移 */
    bool            speaking;       /* 说话中（驱动眼睛果冻弹跳） */
    eye_shape_t     shape;          /* 当前眼型（离散，不平滑） */
    int8_t          brow_dir;       /* 斜眉方向 */
    uint8_t         effect;         /* 附加效果位 */
    uint8_t         blink_pct;      /* 眨眼频率百分比 */
    uint8_t         smooth_div;     /* 参数平滑除数 */

    /* 渲染参数（渲染任务私有） */
    int             cur_eye_h;
    int             cur_eye_dx;

    /* 显式情绪（LLM 标记/控制台），存活期到回落状态默认 */
    emotion_id_t    emo;
    int             emo_hold;       /* 剩余存活 tick */

    /* 眨眼 */
    int             blink_countdown;    /* >0 闭合剩余 tick */
    int             next_blink_in;      /* 距下次眨眼的 tick 数 */

    /* 扫视 */
    int             next_glance_in;     /* 距下次扫视 tick 数 */

    /* 刷屏控制 */
    int             last_hash;          /* 上次已刷的帧指纹（无变化跳过） */
    int             refresh_countdown;  /* 距强制自愈刷新的 tick 数 */
} svc_face_ctx_t;

static svc_face_ctx_t s_ctx;

/* ============== 事件处理（默认事件循环上下文） ============== */

/* 状态默认底色：常规眼型、无点缀、标准节奏（显式情绪在其上覆盖） */
static void state_face_defaults(void)
{
    s_ctx.shape      = EYE_SHAPE_RECT;
    s_ctx.brow_dir   = 0;
    s_ctx.effect     = EFFECT_NONE;
    s_ctx.blink_pct  = 100;
    s_ctx.smooth_div = 4;
}

static void on_chat_state(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    chat_state_t st = (chat_state_t)(int32_t)(data ? *(int32_t *)data : 0);
    state_face_defaults();
    switch (st) {
    case CHAT_STATE_LISTENING:
        s_ctx.tgt_eye_h   = EYE_H_MAX;
        s_ctx.tgt_eye_dx  = EYE_DX_RANGE;   /* 配合云台向右转头倾听 */
        s_ctx.speaking    = false;
        break;
    case CHAT_STATE_RECOGNIZING:
    case CHAT_STATE_THINKING:
        /* 思考中：眯眼一点 */
        s_ctx.tgt_eye_h   = EYE_H_MAX * 6 / 10;
        s_ctx.speaking    = false;
        break;
    case CHAT_STATE_SPEAKING:
        s_ctx.tgt_eye_h   = EYE_H_MAX;
        s_ctx.tgt_eye_dx  = 0;
        s_ctx.speaking    = true;           /* 眼睛果冻弹跳 */
        break;
    case CHAT_STATE_STANDBY:
        s_ctx.tgt_eye_h   = EYE_H_MAX;
        s_ctx.tgt_eye_dx  = 0;
        s_ctx.speaking    = false;
        break;
    default:    /* BOOT / NET_WAIT / ERROR：闭眼睡觉 */
        s_ctx.tgt_eye_h   = EYE_H_MIN;
        s_ctx.tgt_eye_dx  = 0;
        s_ctx.speaking    = false;
        break;
    }
}

/* 显式情绪：登记情绪与存活期，渲染任务逐 tick 应用预设并平滑过渡 */
static void on_emotion(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    int32_t emo = data ? *(int32_t *)data : (int32_t)EMO_NEUTRAL;
    if (emo > (int32_t)EMO_NEUTRAL && emo < (int32_t)EMO_MAX) {
        s_ctx.emo = (emotion_id_t)emo;
        s_ctx.emo_hold = EMOTION_HOLD_MS / FACE_TICK_MS;
        ESP_LOGD(TAG, "emotion %ld", (long)emo);
    }
}

/* ============== 绘制 ============== */

/* 圆角实心块（角部逐层收进，近似圆角） */
static void draw_round_rect(int cx, int cy, int w, int h)
{
    int x = cx - w / 2, y = cy - h / 2;
    drv_display_fill_rect(x, y, w, h, true);
    for (int r = 0; r < 4 && r < h / 2 && r < w / 2; r++) {
        drv_display_fill_rect(x, y + r, r + 1, 1, false);               /* 左上 */
        drv_display_fill_rect(x + w - 1 - r, y + r, r + 1, 1, false);   /* 右上 */
        drv_display_fill_rect(x, y + h - 1 - r, r + 1, 1, false);       /* 左下 */
        drv_display_fill_rect(x + w - 1 - r, y + h - 1 - r, r + 1, 1, false); /* 右下 */
    }
}

/* 弯月眼 ^（开心笑眼）：上拱弧线，厚度 3px */
static void draw_arc_eye(int cx, int cy, int w)
{
    int half = w / 2;
    int hh = half * half;
    for (int dx = -half; dx <= half; dx++) {
        int lift = ARC_DEPTH * (hh - dx * dx) / hh;     /* 中央最高 */
        drv_display_fill_rect(cx + dx, cy - lift, 1, 3, true);
    }
}

/* 斜切眼：inner_low=true 内端下沉（怒）/ false 外端下沉（垂）；drop 为下沉量 */
static void draw_slant_eye(int cx, int cy, int w, int h, int inner_dir, int drop, bool inner_low)
{
    int half = w / 2;
    for (int dx = -half; dx <= half; dx++) {
        int t = (inner_dir > 0) ? (dx + half) : (half - dx);    /* 内端侧 0..w */
        int d = (inner_low ? t : w - t) * drop / w;
        drv_display_fill_rect(cx + dx, cy - h / 2 + d, 1, h, true);
    }
}

/* 圆睁眼（惊讶）：大圆 + 2x3 眼神光空洞 */
static void draw_wide_eye(int cx, int cy, int dia, int shift)
{
    draw_round_rect(cx + shift, cy, dia, dia);
    drv_display_fill_rect(cx + shift - 4, cy - 5, 3, 3, false);
}

/* 斜眉：dir=+1 内端向下（怒）/ -1 内端向上（愁），inner_dir 左眼 +1 右眼 -1 */
static void draw_brow(int cx, int cy, int dir, int inner_dir)
{
    for (int dx = -7; dx <= 7; dx++) {
        int y = cy + dir * inner_dir * dx / 2;
        drv_display_fill_rect(cx + dx, y, 1, 2, true);
    }
}

/* 腮红：两眼下外侧各三道同向 "/" 斜线（害羞） */
static void draw_blush(void)
{
    for (int i = 0; i < 3; i++) {
        int x = EYE_L_CX - 14 + i * 3;                  /* 左眼下外 */
        for (int k = 0; k <= 3; k++) {
            drv_display_fill_rect(x + k, BLUSH_Y - k, 1, 2, true);
        }
        x = EYE_R_CX + 8 + i * 3;                       /* 右眼下外 */
        for (int k = 0; k <= 3; k++) {
            drv_display_fill_rect(x + k, BLUSH_Y - k, 1, 2, true);
        }
    }
}

/* ---- 委屈脸整帧位图（128x64，行优先 MSB 在前）：垂目重眼皮大眼 +
 * 泪珠 + 瘪嘴，脚本绘制后冻结；改动请重新生成位图 ---- */
static const uint8_t SAD_FRAME[64 * 16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=00 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=01 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=02 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=03 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=04 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=05 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=06 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=07 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=08 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=09 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=10 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=11 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=12 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=13 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=14 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=15 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=16 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x07, 0x80, 0x00, 0x00, 0x00, 0x00,   /* y=17 */
    0x00, 0x00, 0x00, 0x00, 0x07, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xF0, 0x00, 0x00, 0x00, 0x00,   /* y=18 */
    0x00, 0x00, 0x00, 0x00, 0x1F, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x3C, 0xFC, 0x00, 0x00, 0x00, 0x00,   /* y=19 */
    0x00, 0x00, 0x00, 0x00, 0x7F, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x3C, 0xFF, 0x00, 0x00, 0x00, 0x00,   /* y=20 */
    0x00, 0x00, 0x00, 0x01, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xC0, 0x00, 0x00, 0x00,   /* y=21 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xF0, 0x00, 0x00, 0x00,   /* y=22 */
    0x00, 0x00, 0x00, 0x3F, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFE, 0x00, 0x00, 0x00,   /* y=23 */
    0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xEF, 0x00, 0x00, 0x00,   /* y=24 */
    0x00, 0x00, 0x00, 0x47, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE1, 0x00, 0x00, 0x00,   /* y=25 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=26 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=27 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=28 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=29 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=30 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=31 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=32 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=33 */
    0x00, 0x00, 0x00, 0x07, 0xFF, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xE0, 0x00, 0x00, 0x00,   /* y=34 */
    0x00, 0x00, 0x00, 0x03, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xC0, 0x00, 0x00, 0x00,   /* y=35 */
    0x00, 0x00, 0x00, 0x03, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xC0, 0x00, 0x00, 0x00,   /* y=36 */
    0x00, 0x00, 0x00, 0x03, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xC0, 0x00, 0x00, 0x00,   /* y=37 */
    0x00, 0x00, 0x00, 0x01, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0x80, 0x00, 0x00, 0x00,   /* y=38 */
    0x00, 0x00, 0x00, 0x00, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x03, 0xFC, 0x00, 0x00, 0x00, 0x00,   /* y=39 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=40 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=41 */
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x38, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=42 */
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x3E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=43 */
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=44 */
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=45 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=46 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=47 */
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x30, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=48 */
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=49 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=50 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=51 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=52 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=53 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=54 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=55 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=56 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=57 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=58 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=59 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=60 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=61 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=62 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* y=63 */
};

/* 位图帧上屏：逐位写入显存（仅置 1 的像素，约 2k 次显存写，20Hz 无压力） */
static void draw_sad_frame(void)
{
    for (int y = 0; y < 64; y++) {
        for (int i = 0; i < 16; i++) {
            uint8_t b = SAD_FRAME[y * 16 + i];
            if (!b) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (b & (0x80u >> bit)) {
                    drv_display_set_px(i * 8 + bit, y, true);
                }
            }
        }
    }
}

/* ============== 渲染任务 ============== */

static int ease(int cur, int tgt, int div)
{
    return cur + (tgt - cur) / div;
}

static void face_task(void *arg)
{
    int phase = 0;
    while (s_ctx.running) {
        /* ---- 更新眨眼/扫视计时 ---- */
        if (s_ctx.blink_countdown > 0) {
            s_ctx.blink_countdown--;
        } else if (--s_ctx.next_blink_in <= 0) {
            s_ctx.blink_countdown  = BLINK_CLOSE_TICKS;
            s_ctx.next_blink_in    = ((int)(esp_random() % 4000u) / FACE_TICK_MS
                                      + 2500 / FACE_TICK_MS) * s_ctx.blink_pct / 100;
        }

        /* ---- 显式情绪覆盖：存活期内改写目标，到期自然回落状态默认 ---- */
        if (s_ctx.emo_hold > 0) {
            s_ctx.emo_hold--;
            const face_preset_t *p = &EMO_PRESETS[s_ctx.emo];
            s_ctx.shape      = p->shape;
            s_ctx.tgt_eye_h  = p->eye_h;
            s_ctx.tgt_eye_dx = p->eye_dx;
            s_ctx.brow_dir   = p->brow_dir;
            s_ctx.effect     = p->effect;
            s_ctx.blink_pct  = p->blink_pct;
            s_ctx.smooth_div = p->smooth_div;
        }

        /* 扫视：空闲且无显式情绪时偶尔换个方向看（随机 -1/0/+1） */
        if (--s_ctx.next_glance_in <= 0) {
            if (s_ctx.emo_hold <= 0 &&
                s_ctx.tgt_eye_h == EYE_H_MAX && s_ctx.tgt_eye_dx == 0) {
                int dir = (int)(esp_random() % 3u) - 1;
                s_ctx.tgt_eye_dx = dir * EYE_DX_RANGE;
            }
            s_ctx.next_glance_in = (int)(esp_random() % 5000u) / FACE_TICK_MS
                                   + 3000 / FACE_TICK_MS;       /* 3~8s */
        }

        /* ---- 参数平滑（除数随情绪变化：困时慵懒、惊讶时迅捷） ---- */
        int eye_h = ease(s_ctx.cur_eye_h, s_ctx.tgt_eye_h, s_ctx.smooth_div);
        int eye_dx = ease(s_ctx.cur_eye_dx, s_ctx.tgt_eye_dx, s_ctx.smooth_div);
        s_ctx.cur_eye_h = eye_h;
        s_ctx.cur_eye_dx = eye_dx;

        /* 眨眼覆盖：闭眼期间统一退化为闭眼线 */
        int draw_h = (s_ctx.blink_countdown > 0) ? EYE_H_MIN : eye_h;

        /* 说话反馈：眼睛果冻弹跳（竖向弹、横向反向压缩），约 2.6Hz */
        int bounce = 0;
        if (s_ctx.speaking) {
            phase = (phase + 1) % 24;
            bounce = (int)(JELLY_AMP * sinf(phase * 0.26f));
            draw_h += bounce;
        }
        if (draw_h < EYE_H_MIN) {
            draw_h = EYE_H_MIN;
        }
        if (draw_h > WIDE_DIA + JELLY_AMP) {
            draw_h = WIDE_DIA + JELLY_AMP;
        }
        int draw_w = EYE_W - bounce / 2;

        /* ---- 帧指纹去重：无变化跳过刷屏；周期自愈覆盖外部显存改动 ---- */
        int hash = draw_h * 1000000 + draw_w * 20000 + eye_dx * 500
                   + s_ctx.shape * 6 + (s_ctx.brow_dir + 1) * 2
                   + s_ctx.effect + (s_ctx.speaking ? 10000000 : 0);
        if (hash != s_ctx.last_hash || --s_ctx.refresh_countdown <= 0) {
            s_ctx.last_hash = hash;
            s_ctx.refresh_countdown = REFRESH_PERIOD_MS / FACE_TICK_MS;
            drv_display_clear();
            /* 两只眼睛按当前眼型绘制 */
            switch (s_ctx.shape) {
            case EYE_SHAPE_ARC:
                draw_arc_eye(EYE_L_CX + eye_dx, EYE_CY + 2, draw_w);
                draw_arc_eye(EYE_R_CX + eye_dx, EYE_CY + 2, draw_w);
                break;
            case EYE_SHAPE_WEDGE:
                draw_slant_eye(EYE_L_CX + eye_dx, EYE_CY, draw_w, draw_h, +1, WEDGE_DROP, true);
                draw_slant_eye(EYE_R_CX + eye_dx, EYE_CY, draw_w, draw_h, -1, WEDGE_DROP, true);
                break;
            case EYE_SHAPE_BITMAP:
                draw_sad_frame();
                break;
            case EYE_SHAPE_WIDE:
                draw_wide_eye(EYE_L_CX + eye_dx, EYE_CY, draw_h, -WIDE_SHIFT);
                draw_wide_eye(EYE_R_CX + eye_dx, EYE_CY, draw_h, +WIDE_SHIFT);
                break;
            default:    /* EYE_SHAPE_RECT */
                draw_round_rect(EYE_L_CX + eye_dx, EYE_CY, draw_w, draw_h);
                draw_round_rect(EYE_R_CX + eye_dx, EYE_CY, draw_w, draw_h);
                break;
            }
            /* 点缀层 */
            if (s_ctx.brow_dir != 0) {
                int brow_y = EYE_CY - draw_h / 2 - 7;
                draw_brow(EYE_L_CX + eye_dx, brow_y, s_ctx.brow_dir, +1);
                draw_brow(EYE_R_CX + eye_dx, brow_y, s_ctx.brow_dir, -1);
            }
            if (s_ctx.effect & EFFECT_BLUSH) {
                draw_blush();
            }
            drv_display_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(FACE_TICK_MS));
    }
    vTaskDelete(NULL);
}

/* ============== 生命周期 ============== */

esp_err_t svc_face_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ctx.running, ESP_ERR_INVALID_STATE, TAG, "already running");

    if (!drv_display_ready()) {
        ESP_LOGW(TAG, "display not ready, face disabled");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.tgt_eye_h = EYE_H_MIN;
    s_ctx.tgt_eye_dx = 0;
    s_ctx.speaking = false;
    s_ctx.cur_eye_h = EYE_H_MIN;
    s_ctx.cur_eye_dx = 0;
    s_ctx.next_blink_in = 2000 / FACE_TICK_MS;
    s_ctx.next_glance_in = 4000 / FACE_TICK_MS;
    s_ctx.emo = EMO_NEUTRAL;
    s_ctx.emo_hold = 0;
    state_face_defaults();
    s_ctx.last_hash = -1;                                   /* 强制首帧上屏 */
    s_ctx.refresh_countdown = REFRESH_PERIOD_MS / FACE_TICK_MS;
    return ESP_OK;
}

esp_err_t svc_face_start(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.running == false, ESP_ERR_INVALID_STATE, TAG, "already started");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(CHAT_EVENT, CHAT_STATE_CHANGED, on_chat_state, NULL),
        TAG, "register chat state handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(CHAT_EVENT, CHAT_EMOTION, on_emotion, NULL),
        TAG, "register emotion handler failed");

    s_ctx.running = true;
    if (xTaskCreate(face_task, "face", 3072, NULL, 4, NULL) != pdPASS) {
        s_ctx.running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t svc_face_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.running, ESP_ERR_INVALID_STATE, TAG, "not running");
    s_ctx.running = false;      /* 任务自行退出 */
    esp_event_handler_unregister(CHAT_EVENT, CHAT_STATE_CHANGED, on_chat_state);
    esp_event_handler_unregister(CHAT_EVENT, CHAT_EMOTION, on_emotion);
    drv_display_clear();
    drv_display_flush();
    return ESP_OK;
}

/* ============== 控制台命令：face <neutral|happy|sad|angry|surprised|shy|sleepy> ============== */
static int cmd_face(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: face <neutral|happy|sad|angry|surprised|shy|sleepy>\n");
        return 1;
    }
    static const struct {
        const char   *name;
        emotion_id_t  emo;
    } table[] = {
        { "neutral",   EMO_NEUTRAL },   { "happy", EMO_HAPPY },
        { "sad",       EMO_SAD },       { "angry", EMO_ANGRY },
        { "surprised", EMO_SURPRISED }, { "shy",   EMO_SHY },
        { "sleepy",    EMO_SLEEPY },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(argv[1], table[i].name) == 0) {
            /* 走事件通道：表情与云台动作联动一致 */
            int32_t emo = (int32_t)table[i].emo;
            esp_event_post(CHAT_EVENT, CHAT_EMOTION, &emo, sizeof(emo), 0);
            return 0;
        }
    }
    printf("unknown emotion: %s\n", argv[1]);
    return 1;
}

void svc_face_register_console_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "face",
        .help = "Set face emotion: face <neutral|happy|sad|angry|surprised|shy|sleepy>",
        .func = cmd_face,
    };
    if (console_cmd_add(&cmd) != ESP_OK) {
        ESP_LOGW(TAG, "register 'face' cmd failed");
    }
}

#else /* CONFIG_SVC_FACE_ENABLE */

esp_err_t svc_face_init(void)   { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_face_start(void)  { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t svc_face_stop(void)   { return ESP_ERR_NOT_SUPPORTED; }
void      svc_face_register_console_cmds(void) {}

#endif /* CONFIG_SVC_FACE_ENABLE */
