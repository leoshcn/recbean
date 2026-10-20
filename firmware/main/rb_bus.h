// main/rb_bus.h —— 各任务向 app 任务投递事件的唯一入口（实现在 main.c）。
//
// app 任务独占状态机（rb_app_t）、转写缓冲（rb_transcript_t）和所有 LVGL 对象；
// 按键回调、网络任务、会话任务、websocket 事件任务只通过这里排队，不直接改 UI。
// 队列深度有限：投递函数最多等待 timeout_ms，满了就丢弃并返回 false
// （按键回调必须用 0 等待，绝不阻塞 esp_timer 任务）。
#pragma once

#include "rb_app.h"
#include "rb_transcript.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RB_POST_EVENT = 0,   // ev 字段有效：交给 rb_app_handle
    RB_POST_PARTIAL,     // text 为进行中句子
    RB_POST_FINAL,       // idx/speaker/start_ms/text 为定稿句
} rb_post_kind_t;

typedef struct {
    rb_post_kind_t kind;
    rb_event_t ev;
    int32_t idx;
    int32_t speaker;
    uint32_t start_ms;
    char text[RB_TS_TEXT_MAX + 1];
} rb_post_t;

// 投递一个状态机事件。可在任意任务调用（不能在 ISR 中调用）。
bool rb_bus_event(rb_ev_type_t type, int32_t a, int64_t b, int32_t c, uint32_t timeout_ms);

// 在中断中投递一个无参数事件（目前只有浅睡按键唤醒 RB_EV_WAKE_KEY）。队列满时丢弃并返回
// false。同一时刻只能有一个中断源调用（内部用静态暂存项，避免占用中断栈）。
bool rb_bus_event_from_isr(rb_ev_type_t type);

// 投递一条转写文本（websocket 事件任务调用）。text 已清洗、≤ 256 字节。
bool rb_bus_text(bool final, int32_t idx, int32_t speaker, uint32_t start_ms, const char *text);
