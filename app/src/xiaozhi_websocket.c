/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <rtthread.h>
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "lwip/apps/websocket_client.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "xiaozhi_mqtt.h"
#include "xiaozhi_websocket.h"
#include "pet_skin_draw.h"
#include "bf0_hal.h"
#include "bts2_global.h"
#include "bts2_app_pan.h"
#include <cJSON.h>
#include "button.h"
#include "audio_server.h"
#include <webclient.h>
#include "bt_env.h"
#include "./iot/iot_c_api.h"
#include "./mcp/mcp_api.h"
#include "lv_timer.h"
#include "lv_display.h"
#include "lv_obj_pos.h"
#include "lv_tiny_ttf.h"
#include "lv_obj.h"
#include "lv_label.h"
#include "lib_et_asr.h"
#include "../weather/weather.h"
#ifdef BSP_USING_PM
    #include "gui_app_pm.h"
#endif // BSP_USING_PM
#include "xiaozhi_client_public.h"
#include "xiaozhi_ui.h"
#include "xiaozhi_audio.h"

#define MAX_WSOCK_HDR_LEN 4096
#define WEBSOC_RECONNECT 4
#define SPEAKING_THRESHOLD_MS (5 * 60 * 1000) // 小智讲话时间阈值 

extern BOOL g_pan_connected;
extern xz_audio_t *thiz;
extern rt_mailbox_t g_bt_app_mb;
extern lv_obj_t *main_container;
extern lv_obj_t *standby_screen;
extern uint8_t Initiate_disconnection_flag;
extern rt_mailbox_t g_ui_task_mb;
extern rt_tick_t last_listen_tick;
extern void pan_reconnect();


xiaozhi_ws_t g_xz_ws;
rt_mailbox_t g_button_event_mb;
enum DeviceState web_g_state;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(message) //6000地址
static char message[256];
L2_RET_BSS_SECT_END
#else
static char message[256] L2_RET_BSS_SECT(message);
#endif
static const char *mode_str[] = {"auto", "manual", "realtime"};
/* 上行音频帧缓冲：首字节 codec 头 + opus 数据（pet-ai 协议） */
static uint8_t g_up_buf[512];

// 倒计时动画
static lv_obj_t *countdown_screen = NULL;
static rt_thread_t countdown_thread = RT_NULL;
static bool  g_ota_verified = false;
bool shutdown_state = true;

typedef struct
{
    char code[7];
    bool is_activated;
    rt_sem_t sem;
} activation_context_t;

typedef struct
{
    char *url;
    char *token;
} websocket_context_t;

static activation_context_t g_activation_context;
static websocket_context_t g_websocket_context;
static rt_tick_t g_speaking_start_tick = 0;  // 讲话开始时间
static rt_tick_t g_total_speaking_time = 0;  // 累计讲话时间
static bool g_is_speaking = false;           // 是否正在讲话


void parse_helLo(const u8_t *data, u16_t len);

void send_iot_descriptors(void)
{
    const char *desc = iot_get_descriptors_json();
    if (desc == NULL)
    {
        rt_kprintf("Failed to get IoT descriptors\n");
        return;
    }

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"descriptors\":%s}",
             g_xz_ws.session_id, desc);

    rt_kprintf("Sending IoT descriptors:\n");
    rt_kprintf(msg);
    rt_kprintf("\n");
    if (g_xz_ws.is_connected == 1)
    {
        wsock_write(&g_xz_ws.clnt, msg, strlen(msg), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void send_iot_states(void)
{
    const char *state = iot_get_states_json();
    if (state == NULL)
    {
        rt_kprintf("Failed to get IoT states\n");
        return;
    }

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"states\":%s}",
             g_xz_ws.session_id, state);

    rt_kprintf("Sending IoT states:\n");
    rt_kprintf(msg);
    rt_kprintf("\n");
    if (g_xz_ws.is_connected == 1)
    {
        wsock_write(&g_xz_ws.clnt, msg, strlen(msg), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_speak_abort(void *ws, char *session_id, int reason)
{
    rt_kprintf("speak abort\n");
    rt_snprintf(message, 256, "{\"session_id\":\"%s\",\"type\":\"abort\"",
                session_id);
    if (reason)
        strcat(message, ",\"reason\":\"wake_word_detected\"}");
    else
        strcat(message, "}");

    if (g_xz_ws.is_connected == 1)
    {
        wsock_write((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_listen_start(void *ws, char *session_id, enum ListeningMode mode)
{
    rt_kprintf("listen start,mode=%d\n",mode);
    rt_snprintf(message, 256,
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":"
                "\"start\",\"mode\":\"%s\"}",
                session_id, mode_str[mode]);
    // rt_kputs("\r\n");
    // rt_kputs(message);
    // rt_kputs("\r\n");
    if (g_xz_ws.is_connected == 1)
    {
        wsock_write((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_listen_stop(void *ws, char *session_id)
{
    rt_kprintf("listen stop\n");
    rt_snprintf(
        message, 256,
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
        session_id);
    if (g_xz_ws.is_connected == 1)
    {
        wsock_write((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}
/* pet-ai 自有协议：hello 必须带 device_id（服务端 pydantic 必填），version 用字符串。
 * 动态构造（device_id 来自 chip_id），替换原静态 hello_message 的发送路径。 */
extern char *get_client_id(void);
static const char *hello_message_fmt =
    "{"
    "\"type\":\"hello\","
    "\"device_id\":\"%s\","
    "\"version\":\"3\","
    "\"transport\":\"websocket\","
    "\"audio_params\":{"
    "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, "
    "\"frame_duration\":60"
    "}}";

static void build_hello(char *out, int sz)
{
    char *cid = get_client_id();
    snprintf(out, sz, hello_message_fmt, cid ? cid : "unknown");
}

void ws_send_hello(void *ws)
{
    if (g_xz_ws.is_connected == 1)
    {
        build_hello(message, sizeof(message));
        wsock_write((wsock_state_t *)ws, message, strlen(message),
                    OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}
void xz_audio_send_using_websocket(uint8_t *data, int len)
{
    if (g_xz_ws.is_connected == 1)
    {
        /* pet-ai 协议：二进制帧首字节 = codec 头（1=opus），其余为音频数据 */
        if (len + 1 <= (int)sizeof(g_up_buf))
        {
            g_up_buf[0] = 1; /* PET_CODEC_OPUS */
            memcpy(&g_up_buf[1], data, len);
            err_t err = wsock_write(&g_xz_ws.clnt, g_up_buf, len + 1, OPCODE_BINARY);
            // rt_kprintf("send audio = %d len=%d\n", err, len);
        }
    }
    // else
    //     rt_kprintf("Websocket disconnected\n");
}

err_t my_wsapp_fn(int code, char *buf, size_t len)
{
    if (code == WS_CONNECT)
    {
        rt_kprintf("websocket connected\n");
        int status = (uint16_t)(uint32_t)buf;
        if (status == 101) // wss setup success
        {
            rt_sem_release(g_xz_ws.sem);
            g_xz_ws.is_connected = 1;
        }
    }
    else if (code == WS_DISCONNECT)
    {
        if (!g_xz_ws.is_connected)
        {
            rt_sem_release(g_xz_ws.sem);
        }
        else
        {
            //  #ifdef BSP_USING_PM
            //             // 关闭 VAD
            //             if(thiz->vad_enabled)
            //             {
            //                 thiz->vad_enabled = false;
            //                 rt_kprintf("web_cloae,so vad_close\n");
            //             }
            //  #endif
            MCP_RGBLED_CLOSE();

            xiaozhi_ui_chat_status("休眠中...");
            xiaozhi_ui_chat_output("请按键或语音唤醒");
            xiaozhi_ui_standby_chat_output("小智已断开请按键唤醒");//待机界面
            xiaozhi_ui_update_emoji("sleepy");
            xiaozhi_ui_update_standby_emoji("sleepy");
            if(!g_pan_connected)
            {
                ui_swith_to_standby_screen();
            }
        }
        rt_kprintf("WebSocket closed\n");
        g_xz_ws.is_connected = 0;
    }
    else if (code == WS_TEXT)
    {
        // 打印原始数据
        rt_kprintf("web send to me:\n");
        rt_kprintf("%.*s\n", (int)len, buf); // 打印接收到的文本数据
        parse_helLo(buf, len);
    }
    else
    {
        // Receive Audio Data
        xz_audio_downlink(buf, len, NULL, 0);
    }
    return 0;
}
void xiaozhi2(int argc, char **argv);

static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    rt_kprintf("in ws button handle\n");
    lv_display_trigger_activity(NULL);
    gui_pm_fsm(GUI_PM_ACTION_WAKEUP); // 唤醒设备
     rt_kprintf("in ws button handle2\n");
    // 如果当前处于KWS模式，则退出KWS模式
        if (g_kws_running) 
        {  
            rt_kprintf("KWS exit\n");
            g_kws_force_exit = 1;
        }
    static button_action_t last_action = BUTTON_RELEASED;
    if (last_action == action)
        return;
    last_action = action;

    if (action == BUTTON_PRESSED)
    {
        lv_obj_t *now_screen = lv_screen_active();
        rt_kprintf("pressed\r\n");
        rt_kprintf("按键->对话");

        // 检查是否弹窗显示中
        xiaozhi_ui_update_confirm_button_event(1); // 模拟点击更新按钮

        if (now_screen == standby_screen)
        {
            ui_switch_to_xiaozhi_screen();
        }
        // ⚠️ 按下时**不开麦**。KEY1 长按会走板级电源路径(表现为重启)，
        //    所以交互改成"单击切换 开始/结束"，逻辑见 BUTTON_CLICKED 分支。
    }
    else if (action == BUTTON_CLICKED)
    {
        // 单击 = 开始/结束切换（由 UI 线程里的 g_ptt_listening 状态机决定）
        rt_kprintf("clicked -> toggle listen\r\n");
        lv_obj_t *now_screen = lv_screen_active();
        if (now_screen == standby_screen)
        {
            ui_switch_to_xiaozhi_screen();
        }

        // 1. 检查是否处于睡眠状态（WebSocket未连接）
        if (!g_xz_ws.is_connected)
        {
            rt_mb_send(g_bt_app_mb, WEBSOC_RECONNECT); // 发送重连消息
            xiaozhi_ui_chat_status("连接小智...");
        }
        else
        {
            rt_mb_send(g_button_event_mb, BUTTON_EVENT_CLICKED);
        }
    }
    else if (action == BUTTON_RELEASED)
    {
#ifdef BSP_USING_PM
        gui_pm_fsm(GUI_PM_ACTION_WAKEUP);
#endif
        rt_kprintf("released\r\n");
        // 单击切换模式下，松开不做任何事（停止由下一次单击触发）
    }
    else if (action == BUTTON_LONG_PRESSED)
    {
        // 长按忽略：KEY1 长按在部分板型上会触发 PMU 电源路径，这里不做任何处理
        rt_kprintf("long pressed ignored\r\n");
    }
}
#ifndef XIAOZHI_USING_MQTT
void simulate_button_pressed()
{
    rt_kprintf("ws simulate_button_pressed pressed\r\n");
    if(Initiate_disconnection_flag)//蓝牙主动断开不允许mic触发
    {
        rt_kprintf("Initiate_disconnection_flag\r\n");
        return;
    }
    // VAD（xiaozhi_audio.c 自动语音检测）/ KWS 唤醒词入口。
    // ⚠️ 直接发 mailbox，不再绕 xz_button_event_handler：
    //    那个 handler 里有 `if (last_action == action) return;` 去重，
    //    连续两次 PRESSED 会被吞掉，导致 VAD 触发的第二句话失效。
    rt_mb_send(g_button_event_mb, BUTTON_EVENT_PRESSED);
}
void simulate_button_released()
{
    rt_kprintf("ws simulate_button_released released\r\n");
    if(Initiate_disconnection_flag)
    {
        return;
    }
    // VAD 检测到静音 → 结束聆听并通知服务端（见 ui.c 的 ptt_stop）
    rt_mb_send(g_button_event_mb, BUTTON_EVENT_RELEASED);
}
#endif

static void xz_button2_event_handler(int32_t pin, button_action_t action)
{
    if (action == BUTTON_PRESSED)
    {

        rt_kprintf("xz_button2_event_handler - pressed\n");

        xiaozhi_ui_update_confirm_button_event(0); // 模拟点击取消按钮

    }
    else if (action == BUTTON_LONG_PRESSED)
    {
        // 按下超过3秒，触发关机
        rt_kprintf("xz_button2_event_handler - long pressed\n");
        //检查设备是否已绑定设备码
        // if (g_activation_context.is_activated)
        // {
        //     rt_sem_release(g_activation_context.sem);
        // }

            // 长按3秒，直接发送关机消息到ui_task
        lv_obj_t *now_screen = lv_screen_active();
        if (now_screen != standby_screen && g_activation_context.sem != RT_NULL)
        {
            rt_sem_release(g_activation_context.sem);
        }
        shutdown_state = false;
        gui_pm_fsm(GUI_PM_ACTION_WAKEUP); // 唤醒设备
        rt_thread_mdelay(100);
        rt_mb_send(g_ui_task_mb, UI_EVENT_SHUTDOWN);
    }

    else if (action == BUTTON_RELEASED)
    {
        rt_kprintf("xz_button2_event_handler - released\n");
    }
}

void xz_ws_button_init(void) // Session key
{
    static int initialized = 0;
    rt_kprintf("xz_ws_button_init\n");
    if (initialized == 0)
    {
        // 按键1（对话+唤醒）
        button_cfg_t cfg1;
        cfg1.pin = BSP_KEY1_PIN;
        cfg1.active_state = KEY1_ACTIVE_LEVEL;
        cfg1.mode = PIN_MODE_INPUT;
        cfg1.button_handler = xz_button_event_handler; // Session key
        int32_t id1 = button_init(&cfg1);
        RT_ASSERT(id1 >= 0);
        RT_ASSERT(SF_EOK == button_enable(id1));
        initialized = 1;
    }
}
void xz_ws_button_init2(void)
{
    static int initialized = 0;
    rt_kprintf("xz_ws_button2_init\n");
    if (initialized == 0)
    {
        // 按键2（关机）
        button_cfg_t cfg2;
        cfg2.pin = BSP_KEY2_PIN;
        cfg2.active_state = KEY2_ACTIVE_LEVEL;
        cfg2.mode = PIN_MODE_INPUT;
        cfg2.button_handler = xz_button2_event_handler;
        int32_t id2 = button_init(&cfg2);
        RT_ASSERT(SF_EOK == button_enable(id2));
        RT_ASSERT(id2 >= 0);
        initialized = 1;
    }
}
void xz_ws_audio_init()
{
    rt_kprintf("xz_audio_init\n");
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 8); // 设置音量
    xz_audio_decoder_encoder_open(1); // 打开音频解码器和编码器

}
void parse_helLo(const u8_t *data, u16_t len)
{
    cJSON *item = NULL;
    cJSON *root = NULL;
    rt_kprintf(data);
    rt_kprintf("--\r\n");
    root = cJSON_Parse(data); /*json_data 为MQTT的原始数据*/
    if (!root)
    {
        rt_kprintf("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !type_item->valuestring)
    {
        rt_kprintf("no type field\r\n");
        cJSON_Delete(root);
        return;
    }
    char *type = type_item->valuestring;
    rt_kprintf("type = %s\n", type);
    /* pet-ai 协议：服务端回 hello_ack（无 audio_params），兼容原 hello */
    if (strcmp(type, "hello_ack") == 0 || strcmp(type, "hello") == 0)
    {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (sid && sid->valuestring)
        {
            rt_kprintf("session_id = %s\n", sid->valuestring);
            strncpy(g_xz_ws.session_id, sid->valuestring,
                    sizeof(g_xz_ws.session_id) - 1);
        }
        cJSON *audio_param = cJSON_GetObjectItem(root, "audio_params");
        if (audio_param) /* 小智原生 hello 才有；pet-ai hello_ack 没有 */
        {
            cJSON *sr = cJSON_GetObjectItem(audio_param, "sample_rate");
            cJSON *du = cJSON_GetObjectItem(audio_param, "duration");
            if (sr && sr->valuestring) g_xz_ws.sample_rate = atoi(sr->valuestring);
            if (du && du->valuestring) g_xz_ws.frame_duration = atoi(du->valuestring);
        }
        web_g_state = kDeviceStateIdle;

        rt_kprintf("exit sniff mode\n");
        bt_interface_exit_sniff_mode(
            (unsigned char *)&g_bt_app_env.bd_addr); // exit sniff mode
        bt_interface_wr_link_policy_setting(
            (unsigned char *)&g_bt_app_env.bd_addr,
            BT_NOTIFY_LINK_POLICY_ROLE_SWITCH); // close role switch
#ifndef CONFIG_IOT_PROTOCOL_MCP
        send_iot_descriptors(); // 发送iot描述
        send_iot_states();      // 发送iot状态
#endif// CONFIG_IOT_PROTOCOL_MCP
        xiaozhi_ui_chat_status("待命中...");
        xiaozhi_ui_chat_output("灵儿已连接!");
        xiaozhi_ui_update_emoji("neutral");
        xiaozhi_ui_update_standby_emoji("funny");
        rt_kprintf("hello->对话\n");
        ui_switch_to_xiaozhi_screen();//切换到小智对话界面
    }
    else if (strcmp(type, "emote") == 0)
    {
        /* pet-ai：宠物表情/动作 → 雪人 UI */
        cJSON *m = cJSON_GetObjectItem(root, "mood");
        cJSON *a = cJSON_GetObjectItem(root, "action");
        const char *mood = (m && m->valuestring) ? m->valuestring : "idle";
        const char *act = (a && a->valuestring) ? a->valuestring : NULL;
        pet_skin_set_mood(mood);
        if (act)
            pet_skin_set_activity(act);
        if (strcmp(mood, "speaking") == 0)
        {
            web_g_state = kDeviceStateSpeaking;
            xz_speaker(1);
        }
        else if (strcmp(mood, "listening") == 0)
        {
            web_g_state = kDeviceStateListening;
        }
        else if (strcmp(mood, "idle") == 0)
        {
            web_g_state = kDeviceStateIdle;
            xz_speaker(0);
        }
        lv_display_trigger_activity(NULL);
    }
    else if (strcmp(type, "llm_text") == 0)
    {
        /* pet-ai：LLM 回复文本 → 对话框显示 */
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        if (tx && tx->valuestring)
            xiaozhi_ui_tts_output(tx->valuestring);
        last_listen_tick = rt_tick_get();
        lv_display_trigger_activity(NULL);
    }
    else if (strcmp(type, "session_end") == 0 || strcmp(type, "goodbye") == 0)
    {
        web_g_state = kDeviceStateUnknown;
        rt_kprintf("session ended\n");
        pet_skin_set_activity("idle");
        xiaozhi_ui_chat_status("睡眠中...");
        xiaozhi_ui_chat_output("等待唤醒...");
        xiaozhi_ui_update_emoji("sleep");
    }
    else if (strcmp(type, "stt") == 0)
    {
        char *txt = cJSON_GetObjectItem(root, "text")->valuestring;
        xiaozhi_ui_chat_output(txt);
        last_listen_tick = rt_tick_get();
        web_g_state = kDeviceStateSpeaking;
        xz_speaker(1);
    }
    else if (strcmp(type, "tts") == 0)
    {
        char *txt = cJSON_GetObjectItem(root, "text")->valuestring;
        rt_kprintf(txt);
        rt_kprintf("--\r\n");

        char *state = cJSON_GetObjectItem(root, "state")->valuestring;

        if (strcmp(state, "start") == 0)
        {
            if (web_g_state == kDeviceStateIdle ||
                web_g_state == kDeviceStateListening)
            {
                web_g_state = kDeviceStateSpeaking;
                xz_speaker(1); // 打开扬声器
                xiaozhi_ui_chat_status("讲话中...");

                // 开始累计讲话时间
                g_is_speaking = true;
                g_speaking_start_tick = rt_tick_get();
            }
        }
        else if (strcmp(state, "stop") == 0)
        {           
            // 计算本次讲话时间并累加
            if (g_is_speaking) 
            {
                rt_tick_t current_tick = rt_tick_get();
                rt_tick_t speaking_duration = current_tick - g_speaking_start_tick;
                g_total_speaking_time += speaking_duration;
                g_is_speaking = false;

                rt_kprintf("xiaozhi total_speaking_time: %d ticks\n", g_total_speaking_time);
                // 检查是否达到5分钟阈值
                if (g_total_speaking_time >= rt_tick_from_millisecond(SPEAKING_THRESHOLD_MS)) 
                {
                    rt_kprintf("Speaking time reached 5 minutes, reinitializing audio\n");
                    g_total_speaking_time = 0; // 重置累计时间
                    xiaozhi_ui_reinit_audio();
                }
            }
            
            web_g_state = kDeviceStateIdle;
            xz_speaker(0); // 关闭扬声器
            xiaozhi_ui_chat_status("待命中...");
        }
        else if (strcmp(state, "sentence_start") == 0)
        {
            char *txt = cJSON_GetObjectItem(root, "text")->valuestring;
            // rt_kputs(txt);
            xiaozhi_ui_tts_output(txt); // 使用专用函数处理 tts 输出
        }
        else
        {
            rt_kprintf("Unkown test: %s\n", state);
        }
    }
    else if (strcmp(type, "llm") == 0)
    {
        rt_kprintf(cJSON_GetObjectItem(root, "emotion")->valuestring);
        xiaozhi_ui_update_emoji(
            cJSON_GetObjectItem(root, "emotion")->valuestring);
    }
    else if (strcmp(type, "iot") == 0)
    {
#ifndef CONFIG_IOT_PROTOCOL_MCP
        rt_kprintf("iot command\n");
        cJSON *commands = cJSON_GetObjectItem(root, "commands");
        // rt_kprintf("commands: %s\n", cJSON_Print(commands));
        for (int i = 0; i < cJSON_GetArraySize(commands); i++)
        {
            // rt_kprintf("command %d: %s\n", i,
            // cJSON_Print(cJSON_GetArrayItem(commands, i)));
            cJSON *cmd = cJSON_GetArrayItem(commands, i);
            // rt_kprintf("cmd: %s\n", cJSON_Print(cmd));
            char *cmd_str = cJSON_PrintUnformatted(cmd);
            // rt_kprintf("cmd_str: %s\n", cmd_str);
            if (cmd_str)
            {
                iot_invoke((uint8_t *)cmd_str, strlen(cmd_str));
                send_iot_states(); // 发送 IoT 状态
                rt_free(cmd_str);
            }
        }
#endif // 定义了MCP就不走IOT
    }
    else if (strcmp(type, "mcp") == 0)
    {
        rt_kprintf("mcp command\n");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload && cJSON_IsObject(payload))
        {
            McpServer_ParseMessage(cJSON_PrintUnformatted(payload));
        }
    }
    else
    {
        rt_kprintf("Unkown type: %s\n", type);
    }

    cJSON_Delete(root); /*每次调用cJSON_Parse函数后，都要释放内存*/
}

static void svr_found_callback(const char *name, const ip_addr_t *ipaddr,
                               void *callback_arg)
{
    if (ipaddr != NULL)
    {
        rt_kprintf("DNS lookup succeeded, IP: %s\n", ipaddr_ntoa(ipaddr));
    }
}

void xiaozhi_ws_connect(void)
{
    if (!g_pan_connected)
    {
        xiaozhi_ui_chat_status("请开启网络共享");
        xiaozhi_ui_chat_output("请在手机上开启网络共享后重新发起连接");
        xiaozhi_ui_update_emoji("embarrassed");
        return;
    }

    if (g_activation_context.is_activated)
    {
        she_bei_ma = 0;
        char str_temp[256];
        snprintf(str_temp, sizeof(str_temp),
                "设备未添加，请前往 xiaozhi.me "
                "输入绑定码: \n %s \n ",
                g_activation_context.code);
        xiaozhi_ui_chat_output(str_temp);
        xiaozhi_ui_standby_chat_output(str_temp);//待机界面也显示一份
        rt_sem_take(g_activation_context.sem, RT_WAITING_FOREVER);
        g_activation_context.is_activated = false;
        she_bei_ma = 1;
        lv_display_trigger_activity(NULL);
        
    }
    // 检查 WebSocket 的 TCP 控制块状态是否为 CLOSED
    if (g_xz_ws.clnt.pcb != NULL && g_xz_ws.clnt.pcb->state != CLOSED)
    {
        rt_kprintf("WebSocket is not in CLOSED state, cannot reconnect\n");
        return;
    }
    err_t err;
    uint32_t retry = 10;
    while (retry-- > 0)
    {

        if (g_xz_ws.sem == NULL)
            g_xz_ws.sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);

        wsock_init(&g_xz_ws.clnt, 0, 1,
                   my_wsapp_fn); // 初始化websocket,注册回调函数（ssl_enabled=0：明文 ws）
        char *Client_Id = get_client_id();
        err = wsock_connect(
            &g_xz_ws.clnt, MAX_WSOCK_HDR_LEN, XIAOZHI_HOST, XIAOZHI_WSPATH,
            XIAOZHI_WSPORT, XIAOZHI_TOKEN, NULL,
            "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
            get_mac_address(), Client_Id);
        rt_kprintf("Web socket connection %d\r\n", err);
        if (err == 0)
        {
            rt_kprintf("err = 0\n");
            if (RT_EOK == rt_sem_take(g_xz_ws.sem, 50000))
            {
                rt_kprintf("g_xz_ws.is_connected = %d\n", g_xz_ws.is_connected);
                if (g_xz_ws.is_connected)
                {
                    build_hello(message, sizeof(message));
                    err = wsock_write(&g_xz_ws.clnt, message,
                                      strlen(message), OPCODE_TEXT);

                    rt_kprintf("Web socket write %d\r\n", err);
                    break;
                }
                else
                {
                    rt_kprintf("err = wsock_write_Web socket disconnected\r\n");
                }
            }
            else
            {
                rt_kprintf("Web socket connected timeout\r\n");
            }
        }
        else
        {
            rt_kprintf("Waiting ws_connect ready%d... \r\n", retry);
            xiaozhi_ui_chat_output("小智连接失败!");
            rt_thread_mdelay(1000);
            ui_swith_to_standby_screen();
        }
    }
}

static void parse_ota_response(const char *response,
                               activation_context_t *active,
                               websocket_context_t *websocket)
{
    if (!response || !active || !websocket)
    {
        rt_kprintf("parse_ota_response: Invalid parameters\n");
        return;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root)
    {
        rt_kprintf("parse_ota_response: Failed to parse JSON, error: [%s]\n",
                   cJSON_GetErrorPtr());
        return;
    }

    // 初始化结构体
    active->code[0] = '\0';
    active->is_activated = false;
    if (websocket->url)
    {
        rt_free(websocket->url);
        websocket->url = NULL;
    }
    if (websocket->token)
    {
        rt_free(websocket->token);
        websocket->token = NULL;
    }

    // 解析 websocket 部分
    cJSON *websocket_obj = cJSON_GetObjectItem(root, "websocket");
    if (websocket_obj && cJSON_IsObject(websocket_obj))
    {
        cJSON *url_item = cJSON_GetObjectItem(websocket_obj, "url");
        if (url_item && cJSON_IsString(url_item))
        {
            size_t url_len = strlen(url_item->valuestring) + 1;
            websocket->url = (char *)rt_malloc(url_len);
            if (websocket->url)
            {
                strncpy(websocket->url, url_item->valuestring, url_len);
                rt_kprintf("Websocket URL: %s\n", websocket->url);
            }
        }

        cJSON *token_item = cJSON_GetObjectItem(websocket_obj, "token");
        if (token_item && cJSON_IsString(token_item))
        {
            size_t token_len = strlen(token_item->valuestring) + 1;
            websocket->token = (char *)rt_malloc(token_len);
            if (websocket->token)
            {
                strncpy(websocket->token, token_item->valuestring, token_len);
                rt_kprintf("Websocket Token: %s\n", websocket->token);
            }
        }
    }

    // 解析 activation 部分（可能不存在）
    cJSON *activation_obj = cJSON_GetObjectItem(root, "activation");
    if (activation_obj && cJSON_IsObject(activation_obj))
    {
        cJSON *code_item = cJSON_GetObjectItem(activation_obj, "code");
        if (code_item && cJSON_IsString(code_item))
        {
            strncpy(active->code, code_item->valuestring,
                    sizeof(active->code) - 1);
            active->is_activated = true;
            rt_kprintf("Activation code: %s\n", active->code);
        }
    }
    else
    {
        rt_kprintf("No activation section found, device is activated\n");
        active->is_activated = false;
    }

    cJSON_Delete(root);
}

void xiaozhi2(int argc, char **argv)
{
    g_activation_context.sem =
        rt_sem_create("activation_sem", 0, RT_IPC_FLAG_FIFO);
    char *my_ota_version;
    uint32_t retry = 10;

        // 检查并重连蓝牙和PAN连接
    if (!g_bt_app_env.bt_connected)   //未连接蓝牙               
    {
        xiaozhi_ui_chat_status("蓝牙连接中...");
        xiaozhi_ui_chat_output("正在重连蓝牙...");
        rt_kprintf("Bluetooth not connected, attempting to reconnect Bluetooth\n");

        bt_interface_conn_ext((char *)&g_bt_app_env.bd_addr, BT_PROFILE_HID);
        // 等待蓝牙连接
        uint32_t bt_retry = 50; // 最多等待5秒 (50 * 100ms)
        while (bt_retry-- > 0 && !g_bt_app_env.bt_connected) {
            rt_thread_mdelay(100);
        }
    }

    if (g_bt_app_env.bt_connected && !g_pan_connected) {  // 蓝牙已连接但PAN未连接
        xiaozhi_ui_chat_status("网络连接中...");
        xiaozhi_ui_chat_output("正在重连网络...");
        rt_kprintf("Bluetooth connected but PAN not connected, attempting to reconnect PAN\n");
        pan_reconnect();
        // 等待PAN连接
        uint32_t pan_retry = 50; // 最多等待5秒 (50 * 100ms)
        while (pan_retry-- > 0 && !g_pan_connected) {
            rt_thread_mdelay(100);
        }
    }

    rt_thread_mdelay(2000);
    if (!g_pan_connected)
    {
        xiaozhi_ui_chat_status("请开启网络共享");
        xiaozhi_ui_chat_output("请在手机上开启网络共享后重新发起连接");
        xiaozhi_ui_update_emoji("embarrassed");
        return;
    }
    rt_kprintf("ota_ver:%d\n", g_ota_verified);
    if (!g_ota_verified) {
        while (retry-- > 0)
        {
            xiaozhi_ui_chat_output("正在网络准备...");
            my_ota_version = get_xiaozhi();
            if (my_ota_version)
            {
                rt_kprintf("my_ota_version = %s\n", my_ota_version);
                parse_ota_response(my_ota_version, &g_activation_context,
                                &g_websocket_context);
                if (g_activation_context.is_activated)
                {
                    she_bei_ma = 0;
                    char str_temp[256];
                    snprintf(str_temp, sizeof(str_temp),
                            "设备未添加，请前往 xiaozhi.me "
                            "输入绑定码: \n %s \n ",
                            g_activation_context.code);
                    xiaozhi_ui_chat_output(str_temp);
                    xiaozhi_ui_standby_chat_output(str_temp);//待机界面也显示一份
                    rt_sem_take(g_activation_context.sem, RT_WAITING_FOREVER);
                    g_activation_context.is_activated = false;
                    she_bei_ma = 1;
                    lv_display_trigger_activity(NULL);
                    
                }

                // OTA验证成功，设置标志
                g_ota_verified = true;
                rt_free(my_ota_version);
                break;
            }
            else
            {
                rt_kprintf("Waiting internet ready(%d)... \r\n", retry);
                xiaozhi_ui_chat_status("等待网络...");
                xiaozhi_ui_chat_output("等待网络重新准备...");
                xiaozhi_ui_standby_chat_output("等待网络重新准备...");
                rt_thread_mdelay(1000);
            }
        }
        if (!my_ota_version)
        {
            xiaozhi_ui_chat_output("请检查网络连接后重试");
            xiaozhi_ui_standby_chat_output("OTA获取失败,请检查网络连接后重试");
            return;
        }
    }
    else
    {
        rt_kprintf("OTA verification skipped, already verified\n");

    }
                xiaozhi_ws_connect();
}
MSH_CMD_EXPORT(xiaozhi2, Get Xiaozhi)

/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
