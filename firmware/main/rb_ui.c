// main/rb_ui.c —— 录音豆界面实现。布局与页面说明见 rb_ui.h。
//
// 屏幕四角被 BSP 以半径 30 px 遮罩为黑色，因此顶栏左右各内缩 ~20 px，
// 内容区左右留 14 px、底部留 ≥ 14 px。
// 转写区做了“虚拟化”：固定 RB_UI_SLOTS 个句子槽位，按浏览位置把转写缓冲中对应的
// 句子以 lv_label_set_text_static() 挂上去（文本存放在静态的 rb_transcript_t 中，
// 不复制进 LVGL 堆），从而 LVGL 内存不随句数增长。
#include "rb_ui.h"

#include "rb_fonts.h"
#include "rb_setup.h"
#include "rb_text.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

// ---- 配色（RGB888）----
#define C_BG       0x0E1116   // 背景
#define C_PANEL    0x1A1F27   // 卡片/列表行
#define C_PANEL_HI 0x2A3340   // 选中行
#define C_TEXT     0xE8EDF3   // 正文
#define C_MUTED    0x8A94A0   // 次要文字
#define C_PARTIAL  0x7C8591   // 进行中句子（灰）
#define C_RED      0xE53950   // 录音红
#define C_GREEN    0x4CC38A
#define C_AMBER    0xF5A623

// 说话人配色：1 蓝、2 橙、3 绿、4 粉、5 紫、6 黄，之后循环。
static const uint32_t SPEAKER_COLORS[] = {0x4FC3F7, 0xFFB74D, 0x81C784, 0xF06292, 0xBA68C8, 0xFFF176};

#define RB_UI_SLOTS 8             // 同时挂载的句子槽位数（屏幕约能显示 13 行）
#define CONTENT_Y   40            // 内容区顶端（顶栏之下）
#define CONTENT_H   (320 - CONTENT_Y)

// ---- 顶栏 ----
static lv_obj_t *s_bar_wifi;      // Wi-Fi 图标
static lv_obj_t *s_bar_srv;       // 服务器状态圆点
static lv_obj_t *s_bar_title;     // 页面名
static lv_obj_t *s_bar_rec_dot;   // 录音红点（闪烁）
static lv_obj_t *s_bar_rec_time;  // 录音计时
static lv_obj_t *s_bar_cache;     // “断网缓存 Ns”
static lv_obj_t *s_bar_batt;      // 右上角电量

// ---- 页面容器 ----
static lv_obj_t *s_pg_idle, *s_pg_connect, *s_pg_rec, *s_pg_done, *s_pg_settings, *s_pg_prov, *s_pg_pair;

// 待机
static lv_obj_t *s_idle_status, *s_idle_addr;
// 连接中
static lv_obj_t *s_connect_label;
// 录音
static lv_obj_t *s_rec_list;
static lv_obj_t *s_slot_row[RB_UI_SLOTS];
static lv_obj_t *s_slot_tag[RB_UI_SLOTS];
static lv_obj_t *s_slot_text[RB_UI_SLOTS];
static char s_slot_tag_buf[RB_UI_SLOTS][24];   // 槽位说话人标签文本（static 文本必须常驻）
static lv_obj_t *s_rec_partial;
static lv_obj_t *s_rec_empty;
static lv_obj_t *s_rec_browse;
static lv_obj_t *s_rec_confirm, *s_rec_confirm_sub;
static lv_obj_t *s_rec_saving, *s_rec_saving_label;
// 结束
static lv_obj_t *s_done_icon_bg, *s_done_icon, *s_done_title, *s_done_note, *s_done_stats, *s_done_url;
// 设置
static lv_obj_t *s_set_rows[RB_SET_COUNT];
static lv_obj_t *s_set_labels[RB_SET_COUNT];
static lv_obj_t *s_set_list, *s_set_detail, *s_set_detail_title, *s_set_detail_body;
// 配网 / 配对
static lv_obj_t *s_prov_status;
static lv_obj_t *s_pair_code, *s_pair_url, *s_pair_status;
// 提示条
static lv_obj_t *s_toast, *s_toast_label;

// ---- 渲染缓存：只在变化时更新，减少重排与刷屏 ----
static int s_last_view = -1;
static uint32_t s_last_ts_version = UINT32_MAX;
static uint32_t s_last_offset = UINT32_MAX;

static const char *SETTINGS_TEXT[RB_SET_COUNT] = {"重新配网", "服务器地址", "设备信息", "返回"};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// 文本相同则不动（lv_label_set_text 每次都会重新布局并刷新）。
static void set_text(lv_obj_t *label, const char *text)
{
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

static void set_color(lv_obj_t *label, uint32_t rgb)
{
    lv_obj_set_style_text_color(label, lv_color_hex(rgb), 0);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

// 宽度固定、自动换行、居中的中文标签。
static lv_obj_t *wrap_label(lv_obj_t *parent, uint32_t color, const char *text, int width)
{
    lv_obj_t *l = label(parent, rb_font_cn(), color, text);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// 透明、不可滚动的容器。
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *panel(lv_obj_t *parent, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *o = panel(parent, size, size, color, LV_RADIUS_CIRCLE);
    return o;
}

static lv_obj_t *page(lv_obj_t *scr)
{
    lv_obj_t *p = box(scr, 0, CONTENT_Y, 240, CONTENT_H);
    show(p, false);
    return p;
}

static uint32_t speaker_color(int32_t speaker)
{
    if (speaker <= 0) return C_MUTED;
    return SPEAKER_COLORS[(speaker - 1) % (int32_t)(sizeof(SPEAKER_COLORS) / sizeof(SPEAKER_COLORS[0]))];
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------

static void build_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = box(scr, 22, 8, 196, 26);
    s_bar_wifi = label(bar, RB_FONT_ICON, C_MUTED, LV_SYMBOL_WIFI);
    lv_obj_align(s_bar_wifi, LV_ALIGN_LEFT_MID, 0, 0);
    s_bar_srv = dot(bar, 8, C_MUTED);
    lv_obj_align(s_bar_srv, LV_ALIGN_LEFT_MID, 22, 0);
    s_bar_title = label(bar, rb_font_cn(), C_MUTED, "");
    lv_obj_align(s_bar_title, LV_ALIGN_LEFT_MID, 36, 0);

    s_bar_rec_dot = dot(bar, 10, C_RED);
    lv_obj_align(s_bar_rec_dot, LV_ALIGN_LEFT_MID, 0, 0);
    s_bar_rec_time = label(bar, RB_FONT_NUM20, C_TEXT, "00:00");
    lv_obj_align(s_bar_rec_time, LV_ALIGN_LEFT_MID, 16, 0);
    s_bar_cache = label(bar, rb_font_cn(), C_AMBER, "");
    lv_obj_align(s_bar_cache, LV_ALIGN_LEFT_MID, 84, 0);

    s_bar_batt = label(bar, RB_FONT_ICON, C_TEXT, "");
    lv_obj_align(s_bar_batt, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void build_idle(lv_obj_t *scr)
{
    s_pg_idle = page(scr);
    lv_obj_t *title = label(s_pg_idle, rb_font_cn(), C_TEXT, "录音豆");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    // 中央红色“录音键”：提示单击确定开始录音。
    lv_obj_t *ring = dot(s_pg_idle, 112, 0x3A1A20);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *btn = dot(ring, 88, C_RED);
    lv_obj_center(btn);
    lv_obj_t *mic = label(btn, RB_FONT_NUM40, 0xFFFFFF, LV_SYMBOL_AUDIO);
    lv_obj_center(mic);

    lv_obj_t *hint = label(s_pg_idle, rb_font_cn(), C_TEXT, "按 确定 开始录音");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 166);
    lv_obj_t *sub = label(s_pg_idle, rb_font_cn(), C_MUTED, "长按 确定 进入设置");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 190);

    s_idle_status = wrap_label(s_pg_idle, C_MUTED, "", 210);
    lv_obj_align(s_idle_status, LV_ALIGN_TOP_MID, 0, 222);
    s_idle_addr = label(s_pg_idle, RB_FONT_ICON, C_MUTED, "");
    lv_obj_align(s_idle_addr, LV_ALIGN_TOP_MID, 0, 244);
}

static void build_connect(lv_obj_t *scr)
{
    s_pg_connect = page(scr);
    lv_obj_t *ring = dot(s_pg_connect, 72, 0x3A1A20);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t *icon = label(ring, RB_FONT_NUM28, C_RED, LV_SYMBOL_UPLOAD);
    lv_obj_center(icon);
    s_connect_label = label(s_pg_connect, rb_font_cn(), C_TEXT, "正在连接服务器");
    lv_obj_align(s_connect_label, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_t *sub = label(s_pg_connect, rb_font_cn(), C_MUTED, "长按 确定 取消");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 188);
}

static void build_recording(lv_obj_t *scr)
{
    s_pg_rec = page(scr);
    // 句子列表：纵向 flex，底部对齐——最新句贴底，较旧的句子被向上挤出可视区。
    s_rec_list = box(s_pg_rec, 14, 0, 212, CONTENT_H - 16);
    lv_obj_set_flex_flow(s_rec_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rec_list, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_rec_list, 6, 0);
    for (int i = 0; i < RB_UI_SLOTS; i++) {
        lv_obj_t *row = lv_obj_create(s_rec_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_slot_row[i] = row;
        s_slot_tag[i] = label(row, rb_font_cn(), C_MUTED, "");
        s_slot_text[i] = label(row, rb_font_cn(), C_TEXT, "");
        lv_obj_set_width(s_slot_text[i], lv_pct(100));
        lv_label_set_long_mode(s_slot_text[i], LV_LABEL_LONG_MODE_WRAP);
        show(row, false);
    }
    s_rec_partial = label(s_rec_list, rb_font_cn(), C_PARTIAL, "");
    lv_obj_set_width(s_rec_partial, lv_pct(100));
    lv_label_set_long_mode(s_rec_partial, LV_LABEL_LONG_MODE_WRAP);
    show(s_rec_partial, false);

    s_rec_empty = wrap_label(s_pg_rec, C_MUTED, "正在聆听…\n说话后文字会显示在这里", 200);
    lv_obj_align(s_rec_empty, LV_ALIGN_TOP_MID, 0, 100);

    // 翻看指示：右下角小胶囊。
    s_rec_browse = panel(s_pg_rec, LV_SIZE_CONTENT, LV_SIZE_CONTENT, C_PANEL_HI, 10);
    lv_obj_set_style_pad_hor(s_rec_browse, 8, 0);
    lv_obj_set_style_pad_ver(s_rec_browse, 2, 0);
    lv_obj_t *bl = label(s_rec_browse, rb_font_cn(), C_TEXT, "");
    lv_obj_center(bl);
    lv_obj_align(s_rec_browse, LV_ALIGN_TOP_MID, 0, 0);
    show(s_rec_browse, false);

    // 二次确认层。
    s_rec_confirm = panel(s_pg_rec, 204, 104, C_PANEL, 16);
    lv_obj_set_style_border_color(s_rec_confirm, lv_color_hex(C_RED), 0);
    lv_obj_set_style_border_width(s_rec_confirm, 2, 0);
    lv_obj_center(s_rec_confirm);
    lv_obj_t *ct = label(s_rec_confirm, rb_font_cn(), C_TEXT, "再按 确定 结束录音");
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 26);
    s_rec_confirm_sub = label(s_rec_confirm, rb_font_cn(), C_MUTED, "");
    lv_obj_align(s_rec_confirm_sub, LV_ALIGN_TOP_MID, 0, 56);
    show(s_rec_confirm, false);

    // 保存中层。
    s_rec_saving = panel(s_pg_rec, 204, 84, C_PANEL, 16);
    lv_obj_center(s_rec_saving);
    s_rec_saving_label = label(s_rec_saving, rb_font_cn(), C_TEXT, "正在保存");
    lv_obj_center(s_rec_saving_label);
    show(s_rec_saving, false);
}

static void build_done(lv_obj_t *scr)
{
    s_pg_done = page(scr);
    s_done_icon_bg = dot(s_pg_done, 64, C_GREEN);
    lv_obj_align(s_done_icon_bg, LV_ALIGN_TOP_MID, 0, 8);
    s_done_icon = label(s_done_icon_bg, RB_FONT_NUM28, 0xFFFFFF, LV_SYMBOL_OK);
    lv_obj_center(s_done_icon);
    s_done_title = label(s_pg_done, rb_font_cn(), C_TEXT, "");
    lv_obj_align(s_done_title, LV_ALIGN_TOP_MID, 0, 84);
    s_done_note = wrap_label(s_pg_done, C_MUTED, "", 208);
    lv_obj_align(s_done_note, LV_ALIGN_TOP_MID, 0, 108);
    s_done_stats = wrap_label(s_pg_done, C_TEXT, "", 208);
    lv_obj_align(s_done_stats, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *web = label(s_pg_done, rb_font_cn(), C_MUTED, "在网页查看、回放和修改");
    lv_obj_align(web, LV_ALIGN_TOP_MID, 0, 196);
    s_done_url = label(s_pg_done, RB_FONT_ICON, C_TEXT, "");
    lv_obj_align(s_done_url, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_t *hint = label(s_pg_done, rb_font_cn(), C_MUTED, "按任意键返回");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 244);
}

static void build_settings(lv_obj_t *scr)
{
    s_pg_settings = page(scr);
    s_set_list = box(s_pg_settings, 0, 0, 240, CONTENT_H);
    lv_obj_t *title = label(s_set_list, rb_font_cn(), C_TEXT, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    for (int i = 0; i < RB_SET_COUNT; i++) {
        s_set_rows[i] = panel(s_set_list, 200, 40, C_PANEL, 10);
        lv_obj_align(s_set_rows[i], LV_ALIGN_TOP_MID, 0, 34 + i * 48);
        s_set_labels[i] = label(s_set_rows[i], rb_font_cn(), C_TEXT, SETTINGS_TEXT[i]);
        lv_obj_align(s_set_labels[i], LV_ALIGN_LEFT_MID, 14, 0);
    }
    lv_obj_t *hint = label(s_set_list, rb_font_cn(), C_MUTED, "上/下 选择 · 长按 确定 返回");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 232);

    s_set_detail = box(s_pg_settings, 0, 0, 240, CONTENT_H);
    s_set_detail_title = label(s_set_detail, rb_font_cn(), C_TEXT, "");
    lv_obj_align(s_set_detail_title, LV_ALIGN_TOP_MID, 0, 4);
    s_set_detail_body = label(s_set_detail, rb_font_cn(), C_TEXT, "");
    lv_obj_set_width(s_set_detail_body, 204);
    lv_label_set_long_mode(s_set_detail_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(s_set_detail_body, 4, 0);
    lv_obj_align(s_set_detail_body, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *back = label(s_set_detail, rb_font_cn(), C_MUTED, "按任意键返回");
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 244);
    show(s_set_detail, false);
}

static void build_prov(lv_obj_t *scr)
{
    s_pg_prov = page(scr);
    lv_obj_t *title = label(s_pg_prov, rb_font_cn(), C_TEXT, "配网");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *s1 = label(s_pg_prov, rb_font_cn(), C_MUTED, "1. 手机连接 Wi-Fi 热点");
    lv_obj_align(s1, LV_ALIGN_TOP_LEFT, 20, 34);
    lv_obj_t *ap = label(s_pg_prov, RB_FONT_NUM20, C_TEXT, "RecBean-Setup");
    lv_obj_align(ap, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *s2 = label(s_pg_prov, rb_font_cn(), C_MUTED, "2. 浏览器打开（通常自动弹出）");
    lv_obj_align(s2, LV_ALIGN_TOP_LEFT, 20, 90);
    lv_obj_t *ip = label(s_pg_prov, RB_FONT_NUM20, C_TEXT, "192.168.4.1");
    lv_obj_align(ip, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *s3 = wrap_label(s_pg_prov, C_MUTED, "3. 选择 Wi-Fi，填写密码和服务器地址", 200);
    lv_obj_set_style_text_align(s3, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s3, LV_ALIGN_TOP_LEFT, 20, 146);
    s_prov_status = wrap_label(s_pg_prov, C_AMBER, "", 204);
    lv_obj_align(s_prov_status, LV_ALIGN_TOP_MID, 0, 200);
}

static void build_pair(lv_obj_t *scr)
{
    s_pg_pair = page(scr);
    lv_obj_t *title = label(s_pg_pair, rb_font_cn(), C_TEXT, "设备配对");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t *card = panel(s_pg_pair, 180, 76, C_PANEL, 14);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);
    s_pair_code = label(card, RB_FONT_NUM40, C_TEXT, "----");
    lv_obj_set_style_text_letter_space(s_pair_code, 8, 0);
    lv_obj_center(s_pair_code);
    lv_obj_t *h1 = wrap_label(s_pg_pair, C_MUTED, "在网页的“设备配对”中确认此配对码", 204);
    lv_obj_align(h1, LV_ALIGN_TOP_MID, 0, 124);
    s_pair_url = label(s_pg_pair, RB_FONT_ICON, C_TEXT, "");
    lv_obj_align(s_pair_url, LV_ALIGN_TOP_MID, 0, 170);
    s_pair_status = wrap_label(s_pg_pair, C_AMBER, "", 204);
    lv_obj_align(s_pair_status, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_t *h2 = label(s_pg_pair, rb_font_cn(), C_MUTED, "长按 确定 进入设置");
    lv_obj_align(h2, LV_ALIGN_TOP_MID, 0, 244);
}

void rb_ui_init(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_idle(scr);
    build_connect(scr);
    build_recording(scr);
    build_done(scr);
    build_settings(scr);
    build_prov(scr);
    build_pair(scr);
    build_bar(scr);   // 最后创建，保证在最上层

    s_toast = panel(scr, 212, LV_SIZE_CONTENT, C_PANEL_HI, 12);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    s_toast_label = wrap_label(s_toast, C_TEXT, "", 196);
    lv_obj_center(s_toast_label);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -22);
    show(s_toast, false);

    lv_screen_load(scr);
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

static const char *notice_text(rb_notice_t n)
{
    switch (n) {
    case RB_NOTICE_WIFI_OFFLINE: return "Wi-Fi 未连接，请稍候或重新配网";
    case RB_NOTICE_SERVER_OFFLINE: return "服务器未连接，无法开始录音";
    case RB_NOTICE_AUTH: return "设备未授权，请在网页上重新确认配对码";
    case RB_NOTICE_BUSY: return "服务器忙，请稍后再试";
    case RB_NOTICE_AUDIO: return "麦克风启动失败";
    case RB_NOTICE_ASR_ERROR: return "识别出错，录音仍在保存";
    case RB_NOTICE_SERVER_ERROR: return "服务器出错";
    case RB_NOTICE_NONE:
    default: return "";
    }
}

static void render_bar(const rb_app_t *app, const rb_ui_info_t *info, uint32_t now)
{
    bool rec = app->view == RB_VIEW_RECORDING || app->view == RB_VIEW_CONFIRM_STOP ||
               app->view == RB_VIEW_SAVING;
    show(s_bar_wifi, !rec);
    show(s_bar_srv, !rec && info->net.wifi_up);
    show(s_bar_title, !rec);
    show(s_bar_rec_dot, rec);
    show(s_bar_rec_time, rec);
    show(s_bar_cache, rec);

    if (!rec) {
        set_color(s_bar_wifi, info->net.wifi_up ? C_TEXT : C_MUTED);
        uint32_t srv = app->server_state > 0 ? C_GREEN : (app->server_state == 0 ? C_RED : C_MUTED);
        lv_obj_set_style_bg_color(s_bar_srv, lv_color_hex(srv), 0);
        const char *title = "";
        if (app->view == RB_VIEW_SETTINGS) title = "设置";
        else if (app->view == RB_VIEW_PROVISIONING) title = "配网";
        else if (app->view == RB_VIEW_PAIRING) title = "配对";
        else if (app->view == RB_VIEW_DONE) title = "录音结束";
        set_text(s_bar_title, title);
    } else {
        // 红点每 500 ms 闪烁；保存中常亮。
        bool on = app->view == RB_VIEW_SAVING || ((now / 500u) % 2u) == 0;
        lv_obj_set_style_bg_opa(s_bar_rec_dot, on ? LV_OPA_COVER : LV_OPA_20, 0);
        char t[16];
        rb_format_duration(t, sizeof(t), rb_app_elapsed_ms(app, now));
        set_text(s_bar_rec_time, t);
        char c[32] = "";
        if (!app->link_ok) {
            snprintf(c, sizeof(c), "断网缓存 %lus", (unsigned long)(info->backlog_ms / 1000u));
        }
        set_text(s_bar_cache, c);
    }

    // 右上角电量：读不到（-1）时不显示，避免误导。
    if (info->battery_soc < 0) {
        show(s_bar_batt, false);
    } else {
        int soc = info->battery_soc;
        const char *sym = soc > 80 ? LV_SYMBOL_BATTERY_FULL : soc > 55 ? LV_SYMBOL_BATTERY_3
                        : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY;
        char b[24];
        snprintf(b, sizeof(b), "%d%% %s", soc, sym);
        set_text(s_bar_batt, b);
        set_color(s_bar_batt, soc <= 10 ? C_RED : C_TEXT);
        show(s_bar_batt, true);
    }
}

static void render_idle(const rb_app_t *app, const rb_ui_info_t *info)
{
    const char *st;
    if (!info->net.wifi_up) st = "Wi-Fi 连接中…";
    else if (app->server_state > 0) st = "Wi-Fi 已连接 · 服务器在线";
    else if (app->server_state == 0) st = "Wi-Fi 已连接 · 服务器未连接";
    else st = "Wi-Fi 已连接 · 正在检测服务器";
    set_text(s_idle_status, st);
    set_color(s_idle_status, info->net.wifi_up && app->server_state == 0 ? C_AMBER : C_MUTED);
    set_text(s_idle_addr, info->net.server);
}

static void render_transcript(const rb_app_t *app, const rb_transcript_t *ts)
{
    uint32_t offset = rb_transcript_clamp_offset(ts, app->browse_offset);
    if (ts->version == s_last_ts_version && offset == s_last_offset) return;
    s_last_ts_version = ts->version;
    s_last_offset = offset;

    uint32_t count = rb_transcript_count(ts);
    int32_t last = (int32_t)count - 1 - (int32_t)offset;          // 视图中最下面一句
    int32_t first = last - (RB_UI_SLOTS - 1);
    for (int i = 0; i < RB_UI_SLOTS; i++) {
        int32_t k = first + i;
        const rb_sentence_t *s = (k >= 0 && count > 0) ? rb_transcript_get(ts, (uint32_t)k) : NULL;
        if (!s) {
            show(s_slot_row[i], false);
            continue;
        }
        if (s->speaker > 0) snprintf(s_slot_tag_buf[i], sizeof(s_slot_tag_buf[i]), "说话人%ld", (long)s->speaker);
        else snprintf(s_slot_tag_buf[i], sizeof(s_slot_tag_buf[i]), "说话人?");
        // 文本常驻于静态转写缓冲 / 静态标签缓冲：用 static 版本，不复制进 LVGL 堆。
        lv_label_set_text_static(s_slot_tag[i], s_slot_tag_buf[i]);
        set_color(s_slot_tag[i], speaker_color(s->speaker));
        lv_label_set_text_static(s_slot_text[i], s->text);
        show(s_slot_row[i], true);
    }
    bool partial = offset == 0 && ts->partial[0] != '\0';
    if (partial) lv_label_set_text_static(s_rec_partial, ts->partial);
    show(s_rec_partial, partial);
    show(s_rec_empty, count == 0 && !partial);

    show(s_rec_browse, offset > 0);
    if (offset > 0) {
        char b[48];
        snprintf(b, sizeof(b), "翻看 %lu/%lu · 5 秒后回到最新", (unsigned long)(last + 1), (unsigned long)count);
        set_text(lv_obj_get_child(s_rec_browse, 0), b);
    }
}

static void render_recording(const rb_app_t *app, const rb_transcript_t *ts, uint32_t now)
{
    render_transcript(app, ts);
    show(s_rec_confirm, app->view == RB_VIEW_CONFIRM_STOP);
    if (app->view == RB_VIEW_CONFIRM_STOP) {
        int32_t left = (int32_t)(app->deadline - now);
        char t[48];
        snprintf(t, sizeof(t), "其他键继续录音 · %ld", (long)(left > 0 ? left / 1000 + 1 : 0));
        set_text(s_rec_confirm_sub, t);
    }
    show(s_rec_saving, app->view == RB_VIEW_SAVING);
    if (app->view == RB_VIEW_SAVING) {
        static const char *const dots[] = {"正在保存", "正在保存.", "正在保存..", "正在保存..."};
        set_text(s_rec_saving_label, dots[(now / 400u) % 4u]);
    }
}

static void render_done(const rb_app_t *app, const rb_ui_info_t *info)
{
    uint32_t color = C_GREEN;
    const char *icon = LV_SYMBOL_OK;
    const char *title = "已保存";
    const char *note = app->asr_error ? "识别出过错，音频已完整保存" : "";
    switch (app->done_kind) {
    case RB_DONE_INTERRUPTED:
        color = C_AMBER;
        icon = LV_SYMBOL_WARNING;
        title = "录音已中断";
        note = "网络中断超过缓存时长，已上传的部分已保存";
        break;
    case RB_DONE_UNCONFIRMED:
        color = C_AMBER;
        icon = LV_SYMBOL_WARNING;
        title = "未收到保存确认";
        note = "录音已上传，请在网页上查看";
        break;
    case RB_DONE_FAILED:
        color = C_RED;
        icon = LV_SYMBOL_CLOSE;
        title = "录音异常结束";
        note = "已上传的部分会保存在服务器";
        break;
    case RB_DONE_SAVED:
    default:
        break;
    }
    lv_obj_set_style_bg_color(s_done_icon_bg, lv_color_hex(color), 0);
    set_text(s_done_icon, icon);
    set_text(s_done_title, title);
    set_text(s_done_note, note);
    char dur[16];
    rb_format_duration(dur, sizeof(dur), app->done_duration_ms > 0 ? (uint64_t)app->done_duration_ms : 0);
    char stats[64];
    if (app->done_sentences >= 0) {
        snprintf(stats, sizeof(stats), "时长 %s · %ld 句", dur, (long)app->done_sentences);
    } else {
        snprintf(stats, sizeof(stats), "时长 %s", dur);
    }
    set_text(s_done_stats, stats);
    char url[RB_SERVER_CAP + 8];
    snprintf(url, sizeof(url), "http://%s", info->net.server);
    set_text(s_done_url, url);
}

static void render_settings(const rb_app_t *app, const rb_ui_info_t *info)
{
    bool detail = app->settings_detail >= 0;
    show(s_set_list, !detail);
    show(s_set_detail, detail);
    if (!detail) {
        for (int i = 0; i < RB_SET_COUNT; i++) {
            bool sel = i == app->settings_sel;
            lv_obj_set_style_bg_color(s_set_rows[i], lv_color_hex(sel ? C_PANEL_HI : C_PANEL), 0);
            lv_obj_set_style_border_width(s_set_rows[i], sel ? 2 : 0, 0);
            lv_obj_set_style_border_color(s_set_rows[i], lv_color_hex(C_RED), 0);
            const char *t = SETTINGS_TEXT[i];
            if (i == RB_SET_REPROVISION && app->reprov_armed) t = "再按 确定 清除配网";
            set_text(s_set_labels[i], t);
            set_color(s_set_labels[i], i == RB_SET_REPROVISION && app->reprov_armed ? C_RED : C_TEXT);
        }
        return;
    }
    char body[320];
    if (app->settings_detail == RB_SET_SERVER) {
        set_text(s_set_detail_title, "服务器地址");
        snprintf(body, sizeof(body),
                 "%s\n\n网页：http://%s\n\n如需修改，请选择“重新配网”，在配网页面填写新的服务器地址。",
                 info->net.server, info->net.server);
    } else {
        set_text(s_set_detail_title, "设备信息");
        char batt[32];
        if (info->battery_soc >= 0 && info->battery_mv > 0) {
            snprintf(batt, sizeof(batt), "%d%%（%d.%02d V）", info->battery_soc, info->battery_mv / 1000,
                     (info->battery_mv % 1000) / 10);
        } else {
            snprintf(batt, sizeof(batt), "不可用");
        }
        snprintf(body, sizeof(body),
                 "设备 ID：%s\n固件：%s\nWi-Fi：%s\nIP：%s\n电量：%s\n可用内存：%lu KB（最大块 %lu KB）",
                 info->net.device_id, info->fw_version, info->net.ssid[0] ? info->net.ssid : "未配置",
                 info->net.ip[0] ? info->net.ip : "未连接", batt,
                 (unsigned long)(info->free_heap / 1024u), (unsigned long)(info->largest_block / 1024u));
    }
    set_text(s_set_detail_body, body);
}

static void render_prov(const rb_ui_info_t *info)
{
    const char *st;
    uint32_t color = C_AMBER;
    switch (info->net.portal) {
    case RB_PORTAL_JOINING: st = "正在连接 Wi-Fi…"; break;
    case RB_PORTAL_FAILED: st = "连接失败，请检查密码后重试"; color = C_RED; break;
    case RB_PORTAL_JOINED: st = "已连上，正在进入配对"; color = C_GREEN; break;
    case RB_PORTAL_WAITING:
    default:
        st = info->net.phone_joined ? "手机已连接，请在网页中填写" : "等待手机连接…";
        break;
    }
    set_text(s_prov_status, st);
    set_color(s_prov_status, color);
}

static void render_pair(const rb_ui_info_t *info)
{
    set_text(s_pair_code, info->net.pair_code[0] ? info->net.pair_code : "----");
    char url[RB_SERVER_CAP + 8];
    snprintf(url, sizeof(url), "http://%s", info->net.server);
    set_text(s_pair_url, url);
    const char *st;
    uint32_t color = C_AMBER;
    if (!info->net.wifi_up) st = "正在连接 Wi-Fi…";
    else if (info->net.server_ok == 0) { st = "服务器不可达，正在重试"; color = C_RED; }
    else if (!info->net.pair_code[0]) st = "正在获取配对码…";
    else { st = "等待网页确认…"; color = C_MUTED; }
    set_text(s_pair_status, st);
    set_color(s_pair_status, color);
}

void rb_ui_render(const rb_app_t *app, const rb_transcript_t *ts, const rb_ui_info_t *info,
                  uint32_t now)
{
    if (!app || !ts || !info) return;
    rb_view_t v = app->view;
    bool rec = v == RB_VIEW_RECORDING || v == RB_VIEW_CONFIRM_STOP || v == RB_VIEW_SAVING;
    if ((int)v != s_last_view) {
        show(s_pg_idle, v == RB_VIEW_IDLE);
        show(s_pg_connect, v == RB_VIEW_CONNECTING);
        show(s_pg_rec, rec);
        show(s_pg_done, v == RB_VIEW_DONE);
        show(s_pg_settings, v == RB_VIEW_SETTINGS);
        show(s_pg_prov, v == RB_VIEW_PROVISIONING);
        show(s_pg_pair, v == RB_VIEW_PAIRING);
        if (v == RB_VIEW_CONNECTING) s_last_ts_version = UINT32_MAX;   // 新录音：强制重绘转写区
        s_last_view = (int)v;
    }
    render_bar(app, info, now);
    switch (v) {
    case RB_VIEW_IDLE: render_idle(app, info); break;
    case RB_VIEW_CONNECTING: {
        static const char *const dots[] = {"正在连接服务器", "正在连接服务器.", "正在连接服务器..",
                                           "正在连接服务器..."};
        set_text(s_connect_label, dots[(now / 400u) % 4u]);
        break;
    }
    case RB_VIEW_RECORDING:
    case RB_VIEW_CONFIRM_STOP:
    case RB_VIEW_SAVING: render_recording(app, ts, now); break;
    case RB_VIEW_DONE: render_done(app, info); break;
    case RB_VIEW_SETTINGS: render_settings(app, info); break;
    case RB_VIEW_PROVISIONING: render_prov(info); break;
    case RB_VIEW_PAIRING: render_pair(info); break;
    }
    const char *n = notice_text(app->notice);
    show(s_toast, n[0] != '\0');
    if (n[0]) set_text(s_toast_label, n);
}
