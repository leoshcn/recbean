// main/rb_text.h —— UTF-8 安全的文本工具（纯 C，可主机测试）。
//
// 设备端显示的转写文本来自网络，必须保证：
//   1. 截断位置落在完整字符边界（不切断多字节 UTF-8）；
//   2. 非法字节、控制字符不进入 LVGL 标签（替换为 '?' 或空格）；
//   3. 结果总是以 NUL 结尾，长度 ≤ 指定字节数。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 返回 s[0..remain) 开头一个合法 UTF-8 字符的字节数（1..4）；非法或不完整返回 0。
// 拒绝过长编码、代理区码点（U+D800..DFFF）和 > U+10FFFF 的码点。
int rb_utf8_char_len(const uint8_t *s, size_t remain);

// 把 src 清洗后复制到 dst（容量 dst_cap，含 NUL）：
//   - 合法字符原样保留；
//   - 非法字节替换为 '?'；
//   - 换行/制表等控制字符替换为空格；
//   - 只保留开头不超过 dst_cap-1 字节的完整字符。
// 返回写入的字节数（不含 NUL）。dst_cap 为 0 时什么都不做返回 0。
size_t rb_utf8_copy_head(char *dst, size_t dst_cap, const char *src, size_t src_len);

// 与 rb_utf8_copy_head 相同的清洗规则，但保留【末尾】不超过 dst_cap-1 字节的完整字符。
// 用于进行中（partial）的长句：屏幕上最新说出的字更有价值。
size_t rb_utf8_copy_tail(char *dst, size_t dst_cap, const char *src, size_t src_len);

// 把码点 cp 编码为 UTF-8 写入 out（至少 4 字节），返回字节数；非法码点返回 0。
int rb_utf8_encode(uint32_t cp, char *out);

// 把毫秒格式化为 "MM:SS"（< 1 小时）或 "H:MM:SS"。out 至少 12 字节。
void rb_format_duration(char *out, size_t cap, uint64_t ms);
