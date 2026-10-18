// main/rb_fonts.c —— 字体初始化与字形覆盖自检。
#include "rb_fonts.h"

#include "esp_log.h"

static const char *TAG = "rb_fonts";

LV_FONT_DECLARE(recbean_font_16);

// 可写的描述符副本：只改 fallback，与应用同寿命（静态）。
static lv_font_t s_cn;
static bool s_ready;

void rb_fonts_init(void)
{
    if (s_ready) return;
    s_cn = recbean_font_16;
    s_cn.fallback = &lv_font_montserrat_14;   // 图标 fallback；Montserrat 自身无 fallback，不成环
    s_ready = true;
}

const lv_font_t *rb_font_cn(void)
{
    return s_ready ? &s_cn : &recbean_font_16;
}

static bool has_glyph(const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t g = {0};
    return lv_font_get_glyph_dsc(font, &g, cp, 0) && !g.is_placeholder;
}

bool rb_fonts_selfcheck(void)
{
    // 抽查：界面高频字、全角标点、GB2312 首末字、ASCII，以及一个 GB2312 之外的反例。
    // 完整的界面字符覆盖由主机测试 tests/test_recbean_font.py 逐字检查。
    static const uint32_t must[] = {
        0x5F55, 0x97F3, 0x8C46,   // 录 音 豆
        0x8BF4, 0x8BDD, 0x4EBA,   // 说 话 人
        0xFF0C, 0x3002, 0xFF1F,   // ， 。 ？
        0x554A, 0x9F44,           // 啊（GB2312 第一个汉字）齄（最后一个）
        0x0041, 0x0039,           // A 9
    };
    const uint32_t missing_expected = 0x9F98;   // 龘：不在 GB2312，应当缺字
    const lv_font_t *f = rb_font_cn();
    bool ok = true;
    for (size_t i = 0; i < sizeof(must) / sizeof(must[0]); i++) {
        if (!has_glyph(f, must[i])) {
            ESP_LOGE(TAG, "缺字 U+%04lX", (unsigned long)must[i]);
            ok = false;
        }
    }
    if (!has_glyph(f, 0xF00C)) {
        ESP_LOGE(TAG, "图标 fallback 缺失 (LV_SYMBOL_OK)");
        ok = false;
    }
    if (has_glyph(f, missing_expected)) {
        ESP_LOGW(TAG, "反例 U+%04lX 意外存在：覆盖检查可能失效", (unsigned long)missing_expected);
    }
    ESP_LOGI(TAG, "字形自检 %s（行高 %d px）", ok ? "通过" : "失败", (int)f->line_height);
    return ok;
}
