// tests/test_rb_transcript.c —— UTF-8 清洗/截断与转写显示缓冲主机测试。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rb_text.h"
#include "rb_transcript.h"

static rb_transcript_t s_ts;

static void test_utf8(void)
{
    char out[32];
    // "中文" = E4 B8 AD E6 96 87：容量 5（4 字节 + NUL）只能放下一个字。
    assert(rb_utf8_copy_head(out, 5, "中文", 6) == 3 && strcmp(out, "中") == 0);
    assert(rb_utf8_copy_head(out, 7, "中文", 6) == 6 && strcmp(out, "中文") == 0);
    assert(rb_utf8_copy_tail(out, 5, "中文", 6) == 3 && strcmp(out, "文") == 0);
    assert(rb_utf8_copy_tail(out, 6, "ab中文", 8) == 3 && strcmp(out, "文") == 0);
    assert(rb_utf8_copy_tail(out, 8, "ab中文", 8) == 7 && strcmp(out, "b中文") == 0);
    // 不完整的多字节序列、非法首字节、过长编码、代理区 → '?'。
    const char bad[] = {'a', (char)0xE4, (char)0xB8, 'b', (char)0xFF, (char)0xC0, (char)0x80,
                        (char)0xED, (char)0xA0, (char)0x80, 0};
    rb_utf8_copy_head(out, sizeof(out), bad, sizeof(bad) - 1);
    assert(strcmp(out, "a??b??????") == 0);   // 每个非法字节各替换为一个 ?
    // 控制字符 → 空格；NUL 结束输入。
    assert(rb_utf8_copy_head(out, sizeof(out), "a\nb\tc", 5) == 5 && strcmp(out, "a b c") == 0);
    assert(rb_utf8_copy_head(out, sizeof(out), "ab\0cd", 5) == 2);
    // 4 字节字符（emoji）保留。
    assert(rb_utf8_copy_head(out, sizeof(out), "\xF0\x9F\x98\x80", 4) == 4);
    assert(rb_utf8_char_len((const uint8_t *)"\xF4\x90\x80\x80", 4) == 0);   // > U+10FFFF
    assert(rb_utf8_copy_head(out, 0, "x", 1) == 0);
    assert(rb_utf8_copy_head(out, 1, "x", 1) == 0 && out[0] == '\0');

    char e[4];
    assert(rb_utf8_encode(0x4E2D, e) == 3 && memcmp(e, "中", 3) == 0);
    assert(rb_utf8_encode(0x1F600, e) == 4);
    assert(rb_utf8_encode(0xD800, e) == 0);

    rb_format_duration(out, sizeof(out), 0);
    assert(strcmp(out, "00:00") == 0);
    rb_format_duration(out, sizeof(out), 125999);
    assert(strcmp(out, "02:05") == 0);
    rb_format_duration(out, sizeof(out), 3723000);
    assert(strcmp(out, "1:02:03") == 0);
}

static void test_transcript(void)
{
    rb_transcript_reset(&s_ts);
    uint32_t v0 = s_ts.version;
    assert(rb_transcript_count(&s_ts) == 0);
    assert(rb_transcript_get(&s_ts, 0) == NULL);

    rb_transcript_set_partial(&s_ts, "你好", 6);
    assert(strcmp(s_ts.partial, "你好") == 0 && s_ts.version == v0 + 1);
    rb_transcript_set_partial(&s_ts, "你好", 6);         // 无变化不增加版本
    assert(s_ts.version == v0 + 1);

    assert(rb_transcript_add_final(&s_ts, 0, 1, 0, "你好。", strlen("你好。")));
    assert(s_ts.partial[0] == '\0');                    // 定稿清掉进行中句子
    assert(rb_transcript_add_final(&s_ts, 1, 2, 1500, "嗯", 3));
    assert(rb_transcript_count(&s_ts) == 2);
    // 同 idx：原地修订（说话人和文本都更新），不增加计数。
    assert(rb_transcript_add_final(&s_ts, 1, 1, 1500, "嗯嗯", 6));
    assert(rb_transcript_count(&s_ts) == 2 && s_ts.total_finals == 2);
    assert(rb_transcript_get(&s_ts, 1)->speaker == 1);
    assert(strcmp(rb_transcript_get(&s_ts, 1)->text, "嗯嗯") == 0);
    // 乱序（比最新旧且不在缓冲中）忽略。
    assert(!rb_transcript_add_final(&s_ts, -5, 1, 0, "x", 1));

    // 超长文本：按字符边界截断到 ≤ 256 字节。
    char longtext[1024];
    size_t n = 0;
    while (n + 3 < sizeof(longtext)) {
        memcpy(longtext + n, "长", 3);
        n += 3;
    }
    assert(rb_transcript_add_final(&s_ts, 2, 3, 0, longtext, n));
    const rb_sentence_t *s = rb_transcript_get(&s_ts, 2);
    assert(strlen(s->text) == 255 && strlen(s->text) <= RB_TS_TEXT_MAX);   // 85 个字
    rb_transcript_set_partial(&s_ts, longtext, n);
    assert(strlen(s_ts.partial) == 255);

    // 只保留最近 30 句：写入 40 句后最旧的是 idx 10+3=13。
    rb_transcript_reset(&s_ts);
    for (int i = 0; i < 40; i++) {
        char t[16];
        int len = snprintf(t, sizeof(t), "句%d", i);
        assert(rb_transcript_add_final(&s_ts, i, (i % 2) + 1, (uint32_t)i * 1000, t, (size_t)len));
    }
    assert(rb_transcript_count(&s_ts) == RB_TS_MAX_SENTENCES);
    assert(s_ts.total_finals == 40);
    assert(rb_transcript_get(&s_ts, 0)->idx == 10);
    assert(rb_transcript_get(&s_ts, RB_TS_MAX_SENTENCES - 1)->idx == 39);
    assert(strcmp(rb_transcript_get(&s_ts, 29)->text, "句39") == 0);
    // 已挤出的旧句不能再插入。
    assert(!rb_transcript_add_final(&s_ts, 5, 1, 0, "旧", 3));
    // 被保留的旧句仍可修订。
    assert(rb_transcript_add_final(&s_ts, 10, 2, 0, "改", 3));
    assert(strcmp(rb_transcript_get(&s_ts, 0)->text, "改") == 0);

    assert(rb_transcript_clamp_offset(&s_ts, 0) == 0);
    assert(rb_transcript_clamp_offset(&s_ts, 100) == RB_TS_MAX_SENTENCES - 1);
    rb_transcript_reset(&s_ts);
    assert(rb_transcript_clamp_offset(&s_ts, 3) == 0);
}

int main(void)
{
    test_utf8();
    test_transcript();
    return 0;
}
