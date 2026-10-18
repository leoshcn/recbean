// main/rb_text.c —— UTF-8 清洗/截断与时长格式化。
#include "rb_text.h"

#include <stdio.h>
#include <string.h>

int rb_utf8_char_len(const uint8_t *s, size_t remain)
{
    if (!s || remain == 0) return 0;
    uint8_t c = s[0];
    if (c < 0x80) return 1;
    int need;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        need = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 4;
        cp = c & 0x07;
    } else {
        return 0;   // 续字节 0x80..0xBF 或 0xF8 以上都不能作为首字节
    }
    if ((size_t)need > remain) return 0;
    for (int i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    // 过长编码（例如用 2 字节表示 ASCII）是常见的注入手段，一律拒绝。
    static const uint32_t MIN_CP[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < MIN_CP[need]) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0x10FFFF) return 0;
    return need;
}

// 扫描 src 的下一个“输出单元”：合法字符原样，非法字节 → '?'，控制字符 → ' '。
// 返回消耗的源字节数（≥1），*out/*out_len 为要输出的字节。
static size_t next_unit(const uint8_t *src, size_t remain, const uint8_t **out, size_t *out_len)
{
    static const uint8_t QUESTION = '?';
    static const uint8_t SPACE = ' ';
    int n = rb_utf8_char_len(src, remain);
    if (n <= 0) {
        *out = &QUESTION;
        *out_len = 1;
        return 1;
    }
    if (n == 1 && (src[0] < 0x20 || src[0] == 0x7F)) {
        *out = &SPACE;
        *out_len = 1;
        return 1;
    }
    *out = src;
    *out_len = (size_t)n;
    return (size_t)n;
}

size_t rb_utf8_copy_head(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    if (!dst || dst_cap == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    const uint8_t *p = (const uint8_t *)src;
    size_t used = 0;
    size_t i = 0;
    while (i < src_len && p[i] != 0) {
        const uint8_t *unit;
        size_t unit_len;
        size_t consumed = next_unit(p + i, src_len - i, &unit, &unit_len);
        if (used + unit_len > dst_cap - 1) break;
        memcpy(dst + used, unit, unit_len);
        used += unit_len;
        i += consumed;
    }
    dst[used] = '\0';
    return used;
}

size_t rb_utf8_copy_tail(char *dst, size_t dst_cap, const char *src, size_t src_len)
{
    if (!dst || dst_cap == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    const uint8_t *p = (const uint8_t *)src;
    // 先算清洗后的总长度，再跳过开头多余的单元。两遍扫描避免额外缓冲。
    size_t end = 0;
    while (end < src_len && p[end] != 0) end++;
    size_t total = 0;
    for (size_t i = 0; i < end;) {
        const uint8_t *unit;
        size_t unit_len;
        i += next_unit(p + i, end - i, &unit, &unit_len);
        total += unit_len;
    }
    size_t cap = dst_cap - 1;
    size_t skip_out = total > cap ? total - cap : 0;   // 需要丢弃的输出字节数（至少）
    size_t used = 0;
    size_t dropped = 0;
    for (size_t i = 0; i < end;) {
        const uint8_t *unit;
        size_t unit_len;
        i += next_unit(p + i, end - i, &unit, &unit_len);
        if (dropped < skip_out) {
            dropped += unit_len;     // 按整字符丢弃，可能比 skip_out 多丢几个字节
            continue;
        }
        memcpy(dst + used, unit, unit_len);
        used += unit_len;
    }
    dst[used] = '\0';
    return used;
}

int rb_utf8_encode(uint32_t cp, char *out)
{
    if (!out) return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

void rb_format_duration(char *out, size_t cap, uint64_t ms)
{
    if (!out || cap == 0) return;
    uint64_t total_s = ms / 1000u;
    unsigned h = (unsigned)(total_s / 3600u);
    unsigned m = (unsigned)((total_s / 60u) % 60u);
    unsigned s = (unsigned)(total_s % 60u);
    if (h > 0) snprintf(out, cap, "%u:%02u:%02u", h, m, s);
    else snprintf(out, cap, "%02u:%02u", m, s);
}
