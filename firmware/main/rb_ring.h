// main/rb_ring.h —— ADPCM 音频帧环形缓冲（纯 C，可主机测试）。
//
// 用途：录音期间 audio 任务把每 200 ms 一帧的 ADPCM 帧压入本缓冲，session 任务按
// 序号顺序取出发送。断网时帧继续累积；重连后从仍保留的最旧帧开始重发
// （服务端按 seq 去重），从而在缓存时长内做到录音连续无缺失。
//
// 容量：RB_RING_FRAMES × 1608 B，默认 50 帧 ≈ 80.4 KB ≈ 10 s（design.md §7, D6）。
// 存储由调用方静态分配（rb_ring_t 整体放 .bss），本模块不做任何堆分配。
//
// 序号语义（全部为 uint32，按无符号差值比较，可容忍回绕）：
//   tail_seq ≤ send_seq ≤ head_seq
//   [tail_seq, head_seq)  当前保留在缓冲中的帧
//   [send_seq, head_seq)  尚未发送过的帧（backlog）
// 缓冲满时继续写入会丢弃最旧帧；若被丢弃的帧还没发送过（send_seq == tail_seq），
// 则置 overflow 标志：说明断网时间超过缓存时长，本次录音必须结束（PRD R6）。
//
// 线程安全：本模块本身【不加锁】。设备端由 rb_audio/rb_session 用同一把互斥锁
// 包住所有调用（见 rb_capture.c）。
#pragma once

#include "rb_adpcm.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef RB_RING_FRAMES
// 断网缓存帧数。改小可节省静态 RAM（每帧 1608 B），但缩短可容忍的断网时长。
#define RB_RING_FRAMES 50
#endif

typedef struct {
    uint8_t frames[RB_RING_FRAMES][RB_ADPCM_FRAME_BYTES]; // 帧存储，下标 = seq % RB_RING_FRAMES
    uint32_t head_seq;   // 下一个要写入的序号（= 已写入帧总数）
    uint32_t tail_seq;   // 仍保留的最旧帧序号
    uint32_t send_seq;   // 下一个要发送的序号
    bool overflow;       // 曾有未发送的帧被覆盖；一旦置位直到 rb_ring_reset 才清除
} rb_ring_t;

// 清空缓冲，序号从 0 重新开始，并清除 overflow。每次开始新录音调用。
void rb_ring_reset(rb_ring_t *ring);

// 下一帧将获得的序号（编码前用来写帧头）。
uint32_t rb_ring_next_seq(const rb_ring_t *ring);

// 压入一帧（frame 为 RB_ADPCM_FRAME_BYTES 字节）。帧头 seq 会被改写为分配的序号，
// 保证序号严格连续。满时丢弃最旧帧（必要时置 overflow）。返回分配的序号。
uint32_t rb_ring_push(rb_ring_t *ring, const uint8_t *frame);

// 若有未发送帧，复制 send_seq 对应的帧到 out 并返回 true（不推进 send_seq）。
// 复制而不是返回指针，是为了让调用方在锁外慢慢发送，不怕被 audio 任务覆盖。
bool rb_ring_peek_unsent(const rb_ring_t *ring, uint8_t *out, uint32_t *seq);

// 确认 seq 已发送：仅当 seq == send_seq 时推进；返回是否推进。
bool rb_ring_mark_sent(rb_ring_t *ring, uint32_t seq);

// 断线重连时调用：把 send_seq 回退到 from_seq（被夹在 [tail_seq, head_seq] 内），
// 返回实际的起始序号（即 resume 消息中的 next_seq）。
uint32_t rb_ring_rewind(rb_ring_t *ring, uint32_t from_seq);

// 未发送的帧数（head - send）。
uint32_t rb_ring_backlog(const rb_ring_t *ring);

// 当前保留的帧数（head - tail）。
uint32_t rb_ring_retained(const rb_ring_t *ring);

// 最旧的保留帧序号。
uint32_t rb_ring_tail(const rb_ring_t *ring);

bool rb_ring_overflowed(const rb_ring_t *ring);
