// main/rb_transcript.c —— 转写显示缓冲实现。规则见 rb_transcript.h。
#include "rb_transcript.h"

#include "rb_text.h"

#include <string.h>

void rb_transcript_reset(rb_transcript_t *ts)
{
    if (!ts) return;
    uint32_t version = ts->version;
    memset(ts, 0, sizeof(*ts));
    ts->version = version + 1;   // 版本号单调递增，保证 UI 一定会清屏
}

static rb_sentence_t *slot(rb_transcript_t *ts, uint32_t i)
{
    return &ts->items[(ts->first + i) % RB_TS_MAX_SENTENCES];
}

bool rb_transcript_add_final(rb_transcript_t *ts, int32_t idx, int32_t speaker,
                             uint32_t start_ms, const char *text, size_t text_len)
{
    if (!ts) return false;
    rb_sentence_t *target = NULL;
    for (uint32_t i = 0; i < ts->count; i++) {
        rb_sentence_t *s = slot(ts, i);
        if (s->idx == idx) {
            target = s;
            break;
        }
    }
    if (!target) {
        if (ts->count > 0 && idx <= slot(ts, ts->count - 1)->idx) return false;
        if (ts->count == RB_TS_MAX_SENTENCES) {
            ts->first = (ts->first + 1) % RB_TS_MAX_SENTENCES;   // 丢弃最旧句
            ts->count--;
        }
        target = slot(ts, ts->count);
        ts->count++;
        ts->total_finals++;
    }
    target->idx = idx;
    target->speaker = speaker;
    target->start_ms = start_ms;
    rb_utf8_copy_head(target->text, sizeof(target->text), text, text_len);
    ts->partial[0] = '\0';
    ts->version++;
    return true;
}

void rb_transcript_set_partial(rb_transcript_t *ts, const char *text, size_t text_len)
{
    if (!ts) return;
    char tmp[RB_TS_TEXT_MAX + 1];
    rb_utf8_copy_tail(tmp, sizeof(tmp), text, text ? text_len : 0);
    if (strcmp(tmp, ts->partial) == 0) return;   // 无变化不触发重绘
    memcpy(ts->partial, tmp, sizeof(ts->partial));
    ts->version++;
}

const rb_sentence_t *rb_transcript_get(const rb_transcript_t *ts, uint32_t i)
{
    if (!ts || i >= ts->count) return NULL;
    return &ts->items[(ts->first + i) % RB_TS_MAX_SENTENCES];
}

uint32_t rb_transcript_count(const rb_transcript_t *ts)
{
    return ts ? ts->count : 0;
}

uint32_t rb_transcript_clamp_offset(const rb_transcript_t *ts, uint32_t offset)
{
    uint32_t count = rb_transcript_count(ts);
    uint32_t max = count > 0 ? count - 1 : 0;
    return offset > max ? max : offset;
}
