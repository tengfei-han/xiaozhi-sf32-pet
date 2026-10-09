/*
 * pet_skin_draw.h —— 雪人宠物「换肤 + 表情」绘制模块（自包含，可移植到 xiaozhi-sf32）
 *
 * 设计目标：把"外观自定义"从 watch 例程里抽出来，做成不依赖 gui_app_fwk /
 * pet_audio / lv_ext 中文接口的纯 LVGL 模块，方便塞进小智固件替换官方 UI。
 *
 * 用法（在 xiaozhi_ui.c 里）：
 *   1) 对话界面创建时：pet_skin_init(lv_scr_act());
 *   2) 收到服务器 emotion：pet_skin_set_mood("happy");
 *   3) 状态切换（聆听/说话）：pet_skin_set_activity("listening" / "speaking");
 *   4) 用户切换皮肤（按键/菜单）：pet_skin_set(-1);   // -1 = 循环下一套
 *   5) 界面销毁时：pet_skin_deinit();
 */

#ifndef PET_SKIN_DRAW_H
#define PET_SKIN_DRAW_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化雪人宠物到 parent（对话界面创建时调用一次） */
void pet_skin_init(lv_obj_t *parent);

/* 销毁（界面退出时调用，清理动画定时器） */
void pet_skin_deinit(void);

/*
 * 切换皮肤：
 *   skin_id = 0 豆豆(雪人, 默认) / 1 橘橘(橘猫·带耳) / 2 蓝蓝(蓝团)
 *   skin_id = -1 表示从当前皮肤循环切换到下一套（给用户"换肤"入口用）
 */
void pet_skin_set(int skin_id);

/*
 * 根据服务器下发的 emotion 字符串设置表情。
 * 小智服务端常见值：neutral / happy / funny / sad / angry / sleepy / sleep / surprised ...
 * 这里做模糊映射：happy/funny→开心，sleep/sleepy→睡觉，其余→正常。
 */
void pet_skin_set_mood(const char *emotion);

/*
 * 设置活动态（与表情叠加，控制嘴动/眼动）：
 *   "idle"      待命（眨眼+呼吸）
 *   "listening" 聆听中（睁眼+嘴微张）
 *   "speaking"  讲话中（嘴快速开合）
 *   "thinking"  思考中（同 speaking）
 */
void pet_skin_set_activity(const char *act);

#ifdef __cplusplus
}
#endif

#endif /* PET_SKIN_DRAW_H */
