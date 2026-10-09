/*
 * pet_skin_draw.c —— 雪人宠物「换肤 + 表情」绘制模块
 *
 * 移植自 watch 例程的 app_pet.c（雪人豆豆），去掉了 gui_app_fwk / pet_audio /
 * lv_ext 中文接口依赖，变成纯 LVGL 自包含模块，可直接加进 xiaozhi-sf32 工程。
 *
 * 绘制方式：纯 LVGL 对象（圆角矩形/圆）拼出"圆头+身体+眼+嘴+腮红+耳"，
 * 全部用 lv_obj_set_style_bg_color 上色，因此换肤 = 改一遍颜色，零图片资源。
 *
 * 中文：小智工程自带 DroidSansFallback 回退字体（已全局支持中文），
 *       直接用 lv_label_set_text 即可，无需 lv_ext_set_local_font。
 */

#include "pet_skin_draw.h"
#include <string.h>
#include <stdbool.h>

/* ---------------- 皮肤数据（把"外观自定义"抽象成可切换的数据） ---------------- */

typedef enum
{
    MOOD_NORMAL = 0,
    MOOD_HAPPY,
    MOOD_SLEEP,
    MOOD_MAX
} pet_mood_t;

typedef enum
{
    ACT_IDLE = 0,
    ACT_LISTENING,
    ACT_SPEAKING,
    ACT_THINKING,
    ACT_MAX
} pet_activity_t;

typedef struct
{
    const char *name;     /* 宠物名字（标题） */
    lv_color_t  bg;       /* 背景 */
    lv_color_t  pet;      /* 身体 / 头 */
    lv_color_t  eye;      /* 眼睛 */
    lv_color_t  mouth;    /* 嘴 */
    lv_color_t  blush;    /* 腮红 */
    bool        ears;     /* 是否有耳朵（造型差异的雏形） */
} pet_skin_t;

#define SKIN_COUNT 3

static pet_skin_t SKINS[SKIN_COUNT];

/* 运行时填充皮肤：lv_color_hex() 不是编译期常量，不能在 static 初始化器里直接
 * 用（arm-none-eabi-gcc 会报 initializer element is not constant），故改为运行时赋值。
 * 这与 watch 例程 app_pet.c 的改法一致。 */
static void pet_init_skins(void)
{
    SKINS[0] = (pet_skin_t){
        "豆豆",
        lv_color_hex(0x101418), lv_color_hex(0xF2F2F0), lv_color_hex(0x222222),
        lv_color_hex(0x9C6B6B), lv_color_hex(0xF5B8C4), false,
    };
    SKINS[1] = (pet_skin_t){
        "橘橘",
        lv_color_hex(0x1A1410), lv_color_hex(0xF6A04D), lv_color_hex(0x2E6B3A),
        lv_color_hex(0xB5502E), lv_color_hex(0xFFC2A0), true,
    };
    SKINS[2] = (pet_skin_t){
        "蓝蓝",
        lv_color_hex(0x0E1620), lv_color_hex(0x6FB7E8), lv_color_hex(0x10324A),
        lv_color_hex(0x355C7D), lv_color_hex(0xBFE3F5), false,
    };
}

/* ---------------- 运行时状态 ---------------- */

static int           g_skin = 0;
static pet_mood_t    g_mood = MOOD_NORMAL;
static pet_activity_t g_activity = ACT_IDLE;

static lv_obj_t *g_body    = NULL;
static lv_obj_t *g_head    = NULL;
static lv_obj_t *g_eye_l   = NULL;
static lv_obj_t *g_eye_r   = NULL;
static lv_obj_t *g_mouth   = NULL;
static lv_obj_t *g_blush_l = NULL;
static lv_obj_t *g_blush_r = NULL;
static lv_obj_t *g_ear_l   = NULL;
static lv_obj_t *g_ear_r   = NULL;
static lv_obj_t *g_title   = NULL;

static lv_timer_t *g_anim_timer = NULL;
static uint32_t    g_tick = 0;

/* ---------------- 内部工具 ---------------- */

/* 生成一个圆形/椭圆色块（装饰件，不可点击，避免挡住界面交互） */
static lv_obj_t *make_round_obj(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

/* 表情静态部分：根据 mood 设定眼/嘴/腮红的"基准"形态 */
static void pet_ui_update(void)
{
    if (MOOD_SLEEP == g_mood)
    {
        lv_obj_set_size(g_eye_l, 16, 4);     /* 睡觉：眼睛压平成横线 */
        lv_obj_set_size(g_eye_r, 16, 4);
        lv_obj_set_size(g_mouth, 10, 5);
        lv_obj_set_style_bg_opa(g_blush_l, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_blush_r, LV_OPA_40, LV_PART_MAIN);
    }
    else
    {
        lv_obj_set_size(g_eye_l, 14, 16);    /* 睁眼 */
        lv_obj_set_size(g_eye_r, 14, 16);
        lv_obj_set_style_bg_opa(g_blush_l, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_blush_r, LV_OPA_COVER, LV_PART_MAIN);

        if (MOOD_HAPPY == g_mood)
            lv_obj_set_size(g_mouth, 32, 18);   /* 开心：咧嘴大笑 */
        else
            lv_obj_set_size(g_mouth, 14, 8);    /* 正常：小嘴 */
    }

    if (g_title)
        lv_label_set_text(g_title, SKINS[g_skin].name);
}

/* 应用当前皮肤：把所有颜色/名字刷一遍（换肤核心） */
static void pet_apply_skin(void)
{
    const pet_skin_t *s = &SKINS[g_skin];

    lv_obj_set_style_bg_color(g_body, s->pet, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_head, s->pet, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_eye_l, s->eye, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_eye_r, s->eye, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_mouth, s->mouth, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_blush_l, s->blush, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_blush_r, s->blush, LV_PART_MAIN);

    if (s->ears)
    {
        lv_obj_clear_flag(g_ear_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_ear_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(g_ear_l, s->pet, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_ear_r, s->pet, LV_PART_MAIN);
    }
    else
    {
        lv_obj_add_flag(g_ear_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ear_r, LV_OBJ_FLAG_HIDDEN);
    }

    pet_ui_update();
}

/* 动画主循环：呼吸 / 眨眼 / 聆听·说话嘴动 */
static void pet_anim_timer_cb(lv_timer_t *t)
{
    (void)t;
    g_tick++;

    if (!g_body || !g_eye_l || !g_eye_r || !g_mouth)
        return;

    /* 呼吸：身体高度在 96~104 间缓慢起伏（整数三角波，约 3.2s 周期） */
    int ph = g_tick % 40;
    int tri = (ph < 20) ? (ph * 8 - 80) : (240 - ph * 8);
    int breath = 100 + tri / 20;
    lv_obj_set_size(g_body, 150, 100 * breath / 100);

    if (ACT_LISTENING == g_activity)
    {
        /* 聆听：睁眼 + 嘴微张 */
        lv_obj_set_size(g_eye_l, 14, 16);
        lv_obj_set_size(g_eye_r, 14, 16);
        int m = (g_tick % 8 < 4) ? 20 : 14;
        lv_obj_set_size(g_mouth, m, m);
        return;
    }

    if (ACT_SPEAKING == g_activity || ACT_THINKING == g_activity)
    {
        /* 说话/思考：嘴快速开合 */
        lv_obj_set_size(g_eye_l, 14, 16);
        lv_obj_set_size(g_eye_r, 14, 16);
        int m = (g_tick % 6 < 3) ? 24 : 12;
        lv_obj_set_size(g_mouth, m, m);
        return;
    }

    if (MOOD_SLEEP == g_mood)
        return;   /* 睡觉：眼已压平，不眨眼 */

    /* 待命：每约 2.4s 闭眼一次（约 240ms） */
    bool closed = ((g_tick % 30) < 3);
    int eh = closed ? 3 : 16;
    lv_obj_set_size(g_eye_l, 14, eh);
    lv_obj_set_size(g_eye_r, 14, eh);
}

/* ---------------- 对外接口 ---------------- */

void pet_skin_init(lv_obj_t *parent)
{
    pet_init_skins();   /* 运行时填充皮肤颜色（避免 static 初始化器非恒定报错） */

    if (!parent)
        return;

    /* 大宠物：雪人造型（身体 + 圆头，同色叠放） */
    g_body = make_round_obj(parent, 150, 100, lv_color_white());
    lv_obj_align(g_body, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_radius(g_body, 48, LV_PART_MAIN);

    g_head = make_round_obj(parent, 116, 112, lv_color_white());
    lv_obj_align(g_head, LV_ALIGN_CENTER, 0, -32);

    /* 耳朵（换肤时按皮肤显隐） */
    g_ear_l = make_round_obj(parent, 26, 32, lv_color_white());
    lv_obj_align_to(g_ear_l, g_head, LV_ALIGN_CENTER, -34, -78);
    g_ear_r = make_round_obj(parent, 26, 32, lv_color_white());
    lv_obj_align_to(g_ear_r, g_head, LV_ALIGN_CENTER, 34, -78);

    /* 眼睛（相对头部定位） */
    g_eye_l = make_round_obj(parent, 14, 16, lv_color_black());
    lv_obj_align_to(g_eye_l, g_head, LV_ALIGN_CENTER, -22, -8);
    g_eye_r = make_round_obj(parent, 14, 16, lv_color_black());
    lv_obj_align_to(g_eye_r, g_head, LV_ALIGN_CENTER, 22, -8);

    /* 腮红 */
    g_blush_l = make_round_obj(parent, 15, 10, lv_color_white());
    lv_obj_align_to(g_blush_l, g_head, LV_ALIGN_CENTER, -40, 12);
    g_blush_r = make_round_obj(parent, 15, 10, lv_color_white());
    lv_obj_align_to(g_blush_r, g_head, LV_ALIGN_CENTER, 40, 12);

    /* 嘴 */
    g_mouth = make_round_obj(parent, 14, 8, lv_color_black());
    lv_obj_align_to(g_mouth, g_head, LV_ALIGN_CENTER, 0, 24);

    /* 名字标题（小智自带中文回退字体，直接用默认字体即可） */
    g_title = lv_label_create(parent);
    lv_obj_align(g_title, LV_ALIGN_CENTER, 0, 130);
    lv_obj_set_style_text_color(g_title, lv_color_white(), LV_PART_MAIN);

    /* 用默认皮肤初始化所有颜色/名字 */
    pet_apply_skin();

    /* 启动表情动画定时器（80ms 周期） */
    if (g_anim_timer)
        lv_timer_del(g_anim_timer);
    g_anim_timer = lv_timer_create(pet_anim_timer_cb, 80, NULL);
}

void pet_skin_deinit(void)
{
    if (g_anim_timer)
    {
        lv_timer_del(g_anim_timer);
        g_anim_timer = NULL;
    }
    /* 对象随 parent 销毁自动释放，这里只清指针 + 复位状态 */
    g_body = g_head = g_eye_l = g_eye_r = g_mouth = NULL;
    g_blush_l = g_blush_r = g_ear_l = g_ear_r = g_title = NULL;
    g_tick = 0;
    g_mood = MOOD_NORMAL;
    g_activity = ACT_IDLE;
}

void pet_skin_set(int skin_id)
{
    if (skin_id < 0)
        g_skin = (g_skin + 1) % SKIN_COUNT;   /* -1 = 循环下一套 */
    else if (skin_id < SKIN_COUNT)
        g_skin = skin_id;

    pet_apply_skin();
}

void pet_skin_set_mood(const char *emotion)
{
    if (!emotion)
    {
        g_mood = MOOD_NORMAL;
    }
    else if (strstr(emotion, "happy") || strstr(emotion, "funny"))
    {
        g_mood = MOOD_HAPPY;
    }
    else if (strstr(emotion, "sleep"))
    {
        g_mood = MOOD_SLEEP;
    }
    else
    {
        g_mood = MOOD_NORMAL;   /* neutral / sad / angry / surprised 等 */
    }
    pet_ui_update();
}

void pet_skin_set_activity(const char *act)
{
    if (!act)
    {
        g_activity = ACT_IDLE;
    }
    else if (strstr(act, "listen"))
    {
        g_activity = ACT_LISTENING;
    }
    else if (strstr(act, "think"))
    {
        g_activity = ACT_THINKING;
    }
    else if (strstr(act, "speak"))
    {
        g_activity = ACT_SPEAKING;
    }
    else
    {
        g_activity = ACT_IDLE;
    }
}
