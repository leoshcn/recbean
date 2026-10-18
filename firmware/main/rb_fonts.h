// main/rb_fonts.h —— RecBean 界面字体。
//
// 中文：assets/fonts/recbean_font_16.c（Noto Sans SC Regular 16 px，4 bpp，
//       ASCII + GB2312 全部可显示字符（6763 汉字 + 符号区）+ 界面用到的其他字符，见 assets/README.md）。
//       运行时复制一份描述符并把 fallback 指向 Montserrat 14，以便同一标签里混排
//       LV_SYMBOL_* 图标；源字体描述符在 Flash 中保持只读。
// 数字/图标：Montserrat 14/20/28/40（LVGL 内置，仅含 ASCII 与 LV_SYMBOL_*）。
// 支持范围：转写等动态文本只保证 GB2312 + ASCII 范围；超出范围的字显示为 LVGL
// 缺字占位框（CONFIG_LV_USE_FONT_PLACEHOLDER=y，故意不隐藏）。
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

// 在 LVGL 初始化之后、创建任何标签之前调用一次（需持有 bsp_lvgl_lock）。
void rb_fonts_init(void);

// 16 px 中文字体（含 ASCII，fallback 到 Montserrat 14 图标）。
const lv_font_t *rb_font_cn(void);

// 字形覆盖自检：检查若干界面关键字符与一个已知缺字的反例，结果打印到日志。
// 返回关键字符是否全部覆盖。需持有 bsp_lvgl_lock。
bool rb_fonts_selfcheck(void);

#define RB_FONT_ICON  (&lv_font_montserrat_14)
#define RB_FONT_NUM20 (&lv_font_montserrat_20)
#define RB_FONT_NUM28 (&lv_font_montserrat_28)
#define RB_FONT_NUM40 (&lv_font_montserrat_40)
