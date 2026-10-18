// main/rb_audio.h —— 录音采集任务：ES8311 16 kHz/16 bit/单声道 → IMA-ADPCM → 断网缓存环形缓冲。
//
// 线程模型：
//   - rb_audio 任务（优先级 6）在录音期间循环 bsp_audio_read() 200 ms PCM，编码成 1608 B 帧，
//     压入静态环形缓冲（rb_ring，约 80 KB），然后给会话任务发通知位 RB_AUDIO_NOTIFY_BIT。
//   - 会话任务通过下面的 peek/mark/rewind 接口取帧发送。
//   - 环形缓冲由一把互斥锁保护；锁内只做 memcpy（≤ 1.6 KB），不做 I/O。
// 录音期间不播放（design.md §7）。录音结束后 codec 进入低功耗（bsp_audio_sleep）。
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

// 会话任务的通知位：有新帧进入环形缓冲。
#define RB_AUDIO_NOTIFY_BIT (1u << 0)

// 创建采集任务。consumer 为接收 RB_AUDIO_NOTIFY_BIT 的任务（会话任务）。
// 必须在 bsp_audio_init() 之后调用一次。
esp_err_t rb_audio_init(TaskHandle_t consumer);

// 清空缓冲并开始采集。阻塞等待 codec 打开结果（≤ 1 s）；失败返回错误且不采集。
// 只能由会话任务调用。
esp_err_t rb_audio_start(void);

// 停止采集并等待采集循环退出（最多约 300 ms，即最后一次 200 ms 读取）。幂等。
// 停止后缓冲内容保留，会话任务仍可把剩余帧发完。
void rb_audio_stop(void);

// 下面的接口都持锁访问环形缓冲，语义见 rb_ring.h。
bool rb_audio_peek(uint8_t *out_frame, uint32_t *seq);
void rb_audio_mark_sent(uint32_t seq);
uint32_t rb_audio_rewind(uint32_t from_seq);
uint32_t rb_audio_tail(void);
uint32_t rb_audio_backlog(void);
bool rb_audio_overflowed(void);
