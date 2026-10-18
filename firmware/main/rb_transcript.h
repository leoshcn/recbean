// main/rb_transcript.h —— 录音页的转写显示缓冲（纯 C，可主机测试）。
//
// 只保留最近 RB_TS_MAX_SENTENCES 句定稿句（旧句被覆盖释放）和一条进行中句子，
// 控制设备内存（design.md §3/§7）。每句文本 ≤ RB_TS_TEXT_MAX 字节且 UTF-8 完整。
// 整个结构静态分配（约 8 KB），由 app 任务独占读写，UI 渲染也在 app 任务中进行，
// 因此无需加锁。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_TS_MAX_SENTENCES 30
#define RB_TS_TEXT_MAX      256      // 单句文本最大字节数（不含 NUL），与协议上限一致

typedef struct {
    int32_t idx;                      // 服务端句序号（final.idx），本次录音内递增
    int32_t speaker;                  // 说话人编号（final.speaker），<= 0 表示未知
    uint32_t start_ms;                // 句子起始时间（相对录音开始）
    char text[RB_TS_TEXT_MAX + 1];    // UTF-8 文本，已清洗
} rb_sentence_t;

typedef struct {
    rb_sentence_t items[RB_TS_MAX_SENTENCES]; // 环形存储
    uint32_t first;                   // 最旧句所在下标
    uint32_t count;                   // 当前保留的句数，0..RB_TS_MAX_SENTENCES
    uint32_t total_finals;            // 本次录音收到的不同定稿句数（含已被挤掉的）
    char partial[RB_TS_TEXT_MAX + 1]; // 进行中句子，空串表示无
    uint32_t version;                 // 任何可见变化都 +1，UI 据此判断是否需要重绘
} rb_transcript_t;

void rb_transcript_reset(rb_transcript_t *ts);

// 加入/更新一条定稿句。规则：
//   - 已保留的同 idx 句：原地更新文本与说话人（服务端修订结果）；
//   - idx 大于最新句：追加，满时丢弃最旧句；
//   - 其他（比保留的最旧句还旧、或乱序插入）：忽略，返回 false。
// 定稿后清空 partial（进行中句子已被这条定稿取代）。text 不需要以 NUL 结尾，
// 长度由 text_len 给出；超长按字符边界截断到 RB_TS_TEXT_MAX。
bool rb_transcript_add_final(rb_transcript_t *ts, int32_t idx, int32_t speaker,
                             uint32_t start_ms, const char *text, size_t text_len);

// 设置进行中句子（保留末尾 RB_TS_TEXT_MAX 字节以内的完整字符）。空文本清除。
void rb_transcript_set_partial(rb_transcript_t *ts, const char *text, size_t text_len);

// 取第 i 句（0 = 最旧的保留句，count-1 = 最新），越界返回 NULL。
const rb_sentence_t *rb_transcript_get(const rb_transcript_t *ts, uint32_t i);

uint32_t rb_transcript_count(const rb_transcript_t *ts);

// 计算浏览视图：offset 表示“从最新句往回翻了几句”（0 = 跟随最新）。
// 返回被夹到 [0, max(count-1, 0)] 的 offset。
uint32_t rb_transcript_clamp_offset(const rb_transcript_t *ts, uint32_t offset);
