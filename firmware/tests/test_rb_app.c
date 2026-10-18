// tests/test_rb_app.c —— 应用状态机主机测试：按键流程、二次确认、翻看自动返回、
// 失败降级（PRD R6）、配网/配对阶段切换、设置页、背光调暗/熄屏。
#include <assert.h>
#include <string.h>

#include "rb_app.h"

static rb_app_t app;
static uint32_t now_ms;

static uint32_t ev(rb_ev_type_t type, int32_t a, int64_t b, int32_t c)
{
    rb_event_t e = {.type = type, .a = a, .b = b, .c = c};
    return rb_app_handle(&app, &e, now_ms);
}

static uint32_t key(rb_ev_type_t k) { return ev(k, 0, 0, 0); }

static uint32_t tick_to(uint32_t t)
{
    uint32_t act = 0;
    while (now_ms < t) {
        now_ms += 250;
        act |= ev(RB_EV_TICK, 0, 0, 0);
    }
    return act;
}

static void ready_idle(void)
{
    rb_app_init(&app, RB_NET_READY, now_ms);
    ev(RB_EV_WIFI, 1, 0, 0);
    assert(app.view == RB_VIEW_IDLE);
}

static void start_recording(void)
{
    uint32_t act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONNECTING);
    assert(act & RB_ACT_SESSION_START);
    assert(act & RB_ACT_CLEAR_TRANSCRIPT);
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    assert(app.view == RB_VIEW_RECORDING && app.link_ok);
}

static void test_happy_path(void)
{
    now_ms = 1000;
    ready_idle();
    start_recording();
    tick_to(now_ms + 65000);
    assert(rb_app_elapsed_ms(&app, now_ms) == 65000);

    // 单击确定只进入确认，不结束。
    uint32_t act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONFIRM_STOP && !(act & RB_ACT_SESSION_STOP));
    // 其他键取消确认。
    key(RB_EV_KEY_DOWN);
    assert(app.view == RB_VIEW_RECORDING);
    // 确认窗口超时回到录音。
    key(RB_EV_KEY_OK);
    tick_to(now_ms + RB_CONFIRM_MS + 250);
    assert(app.view == RB_VIEW_RECORDING);
    // 再按确定 → 保存中。
    key(RB_EV_KEY_OK);
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
    uint32_t frozen = rb_app_elapsed_ms(&app, now_ms);
    tick_to(now_ms + 2000);
    assert(rb_app_elapsed_ms(&app, now_ms) == frozen);   // 计时已冻结
    key(RB_EV_KEY_OK);                                   // 保存中按键无效
    assert(app.view == RB_VIEW_SAVING);

    ev(RB_EV_SESS_SAVED, 0, 69000, 12);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_SAVED);
    assert(app.done_duration_ms == 69000 && app.done_sentences == 12);
    // 结束页超时回待机。
    tick_to(now_ms + RB_DONE_MS + 250);
    assert(app.view == RB_VIEW_IDLE);

    // 结束页任意键回待机。
    start_recording();
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_SAVED, 0, 1000, 0);
    assert(app.view == RB_VIEW_DONE && app.done_sentences == 0);
    key(RB_EV_KEY_UP);
    assert(app.view == RB_VIEW_IDLE);
}

static void test_browse(void)
{
    now_ms = 5000;
    ready_idle();
    start_recording();
    // 没有句子时翻看不动。
    key(RB_EV_KEY_UP);
    assert(app.browse_offset == 0);
    ev(RB_EV_TRANSCRIPT, 0, 0, 5);
    key(RB_EV_KEY_UP);
    key(RB_EV_KEY_UP);
    assert(app.browse_offset == 2);
    for (int i = 0; i < 10; i++) key(RB_EV_KEY_UP);
    assert(app.browse_offset == 4);   // 夹到 count-1
    key(RB_EV_KEY_DOWN);
    assert(app.browse_offset == 3);
    // 5 s 无按键后回到最新。
    tick_to(now_ms + RB_BROWSE_RETURN_MS - 500);
    assert(app.browse_offset == 3);
    tick_to(now_ms + 1000);
    assert(app.browse_offset == 0);
    // 句数减少（不会发生，但要安全）时夹取。
    key(RB_EV_KEY_UP);
    key(RB_EV_KEY_UP);
    ev(RB_EV_TRANSCRIPT, 0, 0, 1);
    assert(app.browse_offset == 0);
}

static void test_failures(void)
{
    now_ms = 10000;
    // Wi-Fi 未连接：不进入录音，提示。
    rb_app_init(&app, RB_NET_READY, now_ms);
    uint32_t act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE && app.notice == RB_NOTICE_WIFI_OFFLINE);
    assert(!(act & RB_ACT_SESSION_START));
    tick_to(now_ms + RB_NOTICE_MS + 250);
    assert(app.notice == RB_NOTICE_NONE);

    // 服务器不可达：会话报失败 → 回待机 + “服务器未连接”。
    ev(RB_EV_WIFI, 1, 0, 0);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_FAILED, RB_FAIL_UNREACHABLE, 0, 0);
    assert(app.view == RB_VIEW_IDLE && app.notice == RB_NOTICE_SERVER_OFFLINE);
    // 迟到的 started 被忽略。
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    assert(app.view == RB_VIEW_IDLE);

    // 会话任务没有回应：兜底超时取消会话。
    key(RB_EV_KEY_OK);
    act = tick_to(now_ms + RB_CONNECT_GUARD_MS + 250);
    assert(app.view == RB_VIEW_IDLE && (act & RB_ACT_SESSION_CANCEL));
    assert(app.notice == RB_NOTICE_SERVER_OFFLINE);

    // 未授权 / 忙 / 麦克风失败的提示。
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_FAILED, RB_FAIL_AUTH, 0, 0);
    assert(app.notice == RB_NOTICE_AUTH);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_FAILED, RB_FAIL_BUSY, 0, 0);
    assert(app.notice == RB_NOTICE_BUSY);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_ENDED, RB_FAIL_AUDIO, 0, 0);
    assert(app.view == RB_VIEW_IDLE && app.notice == RB_NOTICE_AUDIO);

    // 连接中长按确定取消。
    key(RB_EV_KEY_OK);
    act = key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_IDLE && (act & RB_ACT_SESSION_CANCEL));

    // 录音中断网：继续录音，状态显示缓存中；恢复后正常。
    start_recording();
    ev(RB_EV_SESS_LINK, 0, 0, 0);
    assert(app.view == RB_VIEW_RECORDING && !app.link_ok);
    ev(RB_EV_SESS_LINK, 1, 0, 0);
    assert(app.link_ok);
    // ASR 出错：只提示，不结束。
    ev(RB_EV_SESS_ASR_ERROR, 0, 0, 0);
    assert(app.view == RB_VIEW_RECORDING && app.notice == RB_NOTICE_ASR_ERROR && app.asr_error);
    // 断网超过缓存时长：结束页显示中断。
    ev(RB_EV_TRANSCRIPT, 0, 0, 7);
    tick_to(now_ms + 30000);
    ev(RB_EV_SESS_ENDED, RB_FAIL_OVERFLOW, 0, 0);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_INTERRUPTED);
    assert(app.done_duration_ms == 30000 && app.done_sentences == 7);

    // 保存超时：兜底进入“未确认”结束页并取消会话。
    key(RB_EV_KEY_OK);
    start_recording();
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_OK);
    act = tick_to(now_ms + RB_SAVE_GUARD_MS + 250);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_UNCONFIRMED);
    assert(act & RB_ACT_SESSION_CANCEL);
    // 会话任务自己报保存超时也一样。
    key(RB_EV_KEY_OK);
    start_recording();
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_ENDED, RB_FAIL_SAVE_TIMEOUT, 0, 0);
    assert(app.done_kind == RB_DONE_UNCONFIRMED);
    // 其他异常结束。
    key(RB_EV_KEY_OK);
    start_recording();
    ev(RB_EV_SESS_ENDED, RB_FAIL_SERVER, 0, 0);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_FAILED);
    // 会话外的 saved 被忽略。
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE);
    ev(RB_EV_SESS_SAVED, 0, 1, 1);
    assert(app.view == RB_VIEW_IDLE);
}

static void test_provisioning_and_settings(void)
{
    now_ms = 50000;
    rb_app_init(&app, RB_NET_PROV_AP, now_ms);
    assert(app.view == RB_VIEW_PROVISIONING);
    key(RB_EV_KEY_OK);                       // 配网页单击无效
    assert(app.view == RB_VIEW_PROVISIONING);
    ev(RB_EV_NET_PHASE, RB_NET_PROV_JOINING, 0, 0);
    assert(app.view == RB_VIEW_PROVISIONING);
    ev(RB_EV_NET_PHASE, RB_NET_PAIRING, 0, 0);
    assert(app.view == RB_VIEW_PAIRING);
    // 配对页长按进设置，设置“返回”回到配对页。
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_SETTINGS && app.settings_sel == RB_SET_REPROVISION);
    // 在设置页时阶段变化不拽走画面。
    ev(RB_EV_NET_PHASE, RB_NET_READY, 0, 0);
    assert(app.view == RB_VIEW_SETTINGS);
    key(RB_EV_KEY_UP);
    assert(app.settings_sel == RB_SET_BACK);  // 向上回绕
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE);         // 阶段已 READY → 待机

    // 待机长按进设置；详情页任意键返回列表。
    key(RB_EV_KEY_OK_LONG);
    key(RB_EV_KEY_DOWN);
    assert(app.settings_sel == RB_SET_SERVER);
    key(RB_EV_KEY_OK);
    assert(app.settings_detail == RB_SET_SERVER);
    key(RB_EV_KEY_DOWN);
    assert(app.settings_detail == -1 && app.settings_sel == RB_SET_SERVER);
    key(RB_EV_KEY_DOWN);
    key(RB_EV_KEY_OK);
    assert(app.settings_detail == RB_SET_INFO);
    key(RB_EV_KEY_OK);
    // 重新配网需二次确认；超时或移动取消确认。
    key(RB_EV_KEY_DOWN);
    key(RB_EV_KEY_DOWN);
    assert(app.settings_sel == RB_SET_REPROVISION);
    uint32_t act = key(RB_EV_KEY_OK);
    assert(app.reprov_armed && !(act & RB_ACT_REPROVISION));
    tick_to(now_ms + RB_REPROV_ARM_MS + 250);
    assert(!app.reprov_armed);
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_DOWN);
    assert(!app.reprov_armed);
    key(RB_EV_KEY_UP);
    key(RB_EV_KEY_OK);
    act = key(RB_EV_KEY_OK);
    assert(act & RB_ACT_REPROVISION);
    assert(app.view == RB_VIEW_PROVISIONING && app.phase == RB_NET_PROV_AP && !app.wifi_up);
    // 长按确定从设置返回。
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_SETTINGS);
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_PROVISIONING);
}

static void test_backlight(void)
{
    now_ms = 90000;
    ready_idle();
    assert(app.backlight == RB_BACKLIGHT_FULL);
    uint32_t act = tick_to(now_ms + RB_DIM_IDLE_MS + 250);
    assert(app.backlight == RB_BACKLIGHT_DIM && (act & RB_ACT_BACKLIGHT));
    tick_to(now_ms + (RB_OFF_IDLE_MS - RB_DIM_IDLE_MS));
    assert(app.backlight == 0);
    // 熄屏时第一次按键只点亮，不开始录音。
    act = key(RB_EV_KEY_OK);
    assert(app.backlight == RB_BACKLIGHT_FULL && (act & RB_ACT_BACKLIGHT));
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START));
    // 调暗时按键正常生效。
    tick_to(now_ms + RB_DIM_IDLE_MS + 250);
    assert(app.backlight == RB_BACKLIGHT_DIM);
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONNECTING && app.backlight == RB_BACKLIGHT_FULL);
    // 录音中只调暗，不熄屏。
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    tick_to(now_ms + RB_OFF_IDLE_MS * 2);
    assert(app.view == RB_VIEW_RECORDING && app.backlight == RB_BACKLIGHT_DIM);
    // 结束页出现时亮屏。
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_OK);
    tick_to(now_ms + RB_SAVE_GUARD_MS - 1000);
    assert(app.view == RB_VIEW_SAVING && app.backlight == RB_BACKLIGHT_FULL);   // 保存中保持全亮
    ev(RB_EV_SESS_SAVED, 0, 1, 1);
    assert(app.backlight == RB_BACKLIGHT_FULL);
}

static void test_wraparound(void)
{
    now_ms = 0xFFFFF000u;   // 约 4 秒后 uint32 毫秒回绕
    ready_idle();
    start_recording();
    tick_to(0xFFFFF000u + 2000);
    now_ms = 2000;          // 回绕后（tick_to 用 < 比较，这里手动推进）
    ev(RB_EV_TICK, 0, 0, 0);
    assert(rb_app_elapsed_ms(&app, now_ms) == 2000u + 0x1000u);
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONFIRM_STOP);
    now_ms += 1000;
    ev(RB_EV_TICK, 0, 0, 0);
    assert(app.view == RB_VIEW_CONFIRM_STOP);
    now_ms += RB_CONFIRM_MS;
    ev(RB_EV_TICK, 0, 0, 0);
    assert(app.view == RB_VIEW_RECORDING);
}

int main(void)
{
    test_happy_path();
    test_browse();
    test_failures();
    test_provisioning_and_settings();
    test_backlight();
    test_wraparound();
    return 0;
}
