// tests/test_rb_app.c —— 应用状态机主机测试：按键流程、二次确认、翻看自动返回、
// 失败降级（PRD R6）、配网/配对阶段切换、设置页、背光调暗/熄屏；
// 两级省电与低电保护（10-20-power-battery-optimization：浅睡/深睡计时、唤醒按键语义、
// 待机页下键手动休眠、会话中不睡、配网页深睡、低电两次确认与先保存后深睡）。
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

// ---------------------------------------------------------------------------
// 录音笔记（按住上说话）
// ---------------------------------------------------------------------------

static uint32_t ptt_start(void)
{
    uint32_t act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_RECORDING && app.mode == RB_MODE_NOTE);
    assert(act & RB_ACT_SESSION_START);
    assert(act & RB_ACT_CLEAR_TRANSCRIPT);
    assert(app.ptt_held && !app.sess_started && !app.link_ok);
    return act;
}

static void test_note_happy_path(void)
{
    now_ms = 5000;
    ready_idle();
    // 按下立即进入录音页（不经过连接中），计时从按下开始。
    ptt_start();
    tick_to(now_ms + 500);
    assert(app.view == RB_VIEW_RECORDING && rb_app_elapsed_ms(&app, now_ms) == 500);
    // started：只标记已连上，不重置计时、不切换视图。
    uint32_t act = ev(RB_EV_SESS_STARTED, 0, 0, 0);
    assert(app.view == RB_VIEW_RECORDING && app.sess_started && app.link_ok);
    assert(act & RB_ACT_RENDER);
    assert(rb_app_elapsed_ms(&app, now_ms) == 500);
    assert(rb_app_ptt_waiting_release(&app));
    tick_to(now_ms + 2000);
    // 笔记中其他按键一律忽略（没有确认页、不翻看、长按不取消）。
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_RECORDING && !(act & (RB_ACT_SESSION_STOP | RB_ACT_SESSION_CANCEL)));
    act = key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_RECORDING && !(act & (RB_ACT_SESSION_STOP | RB_ACT_SESSION_CANCEL)));
    ev(RB_EV_TRANSCRIPT, 0, 0, 3);
    key(RB_EV_KEY_DOWN);
    assert(app.browse_offset == 0 && app.view == RB_VIEW_RECORDING);
    // 松开：进保存中并停止会话，计时冻结。
    uint32_t held = rb_app_elapsed_ms(&app, now_ms);
    act = key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
    assert(!(act & RB_ACT_SESSION_CANCEL));
    assert(!app.ptt_held && !rb_app_ptt_waiting_release(&app));
    tick_to(now_ms + 1000);
    assert(rb_app_elapsed_ms(&app, now_ms) == held);
    // 保存中随后到达的上键单击被吞掉；其他键也忽略。
    key(RB_EV_KEY_UP);
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_SAVING);
    ev(RB_EV_SESS_SAVED, 0, 2500, 1);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_SAVED && app.mode == RB_MODE_NOTE);
    assert(app.done_duration_ms == 2500 && app.done_sentences == 1);
    // 笔记结束页约 3 s 自动回待机。
    uint32_t done_at = now_ms;
    tick_to(done_at + RB_NOTE_DONE_MS - 250);
    assert(app.view == RB_VIEW_DONE);
    tick_to(done_at + RB_NOTE_DONE_MS);
    assert(app.view == RB_VIEW_IDLE);
}

static void test_note_done_dismiss(void)
{
    now_ms = 7000;
    ready_idle();
    ptt_start();
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    tick_to(now_ms + 1000);
    key(RB_EV_PTT_UP);
    ev(RB_EV_SESS_SAVED, 0, 1000, 1);
    assert(app.view == RB_VIEW_DONE);
    // 松开后滞后的单击不能把结束页关掉……
    key(RB_EV_KEY_UP);
    assert(app.view == RB_VIEW_DONE);
    // ……但用户真的再按一次（按下 → 松开 → 单击）就返回。
    key(RB_EV_PTT_DOWN);
    key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_DONE);
    key(RB_EV_KEY_UP);
    assert(app.view == RB_VIEW_IDLE);
    // 其他键也可提前返回。
    ptt_start();
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    tick_to(now_ms + 1000);
    key(RB_EV_PTT_UP);
    ev(RB_EV_SESS_SAVED, 0, 1000, 1);
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE);
}

static void test_note_short_press(void)
{
    now_ms = 9000;
    ready_idle();
    ptt_start();
    now_ms += RB_PTT_MIN_MS - 1;
    uint32_t act = key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_IDLE);
    assert(act & RB_ACT_SESSION_CANCEL);
    assert(!(act & RB_ACT_SESSION_STOP));
    assert(app.notice == RB_NOTICE_PTT_SHORT);
    // 随后的单击被吞掉（待机页上键本来也无作用），不开始任何会话。
    act = key(RB_EV_KEY_UP);
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START));
    // 会话的迟到事件被忽略。
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    ev(RB_EV_SESS_FAILED, RB_FAIL_UNREACHABLE, 0, 0);
    assert(app.view == RB_VIEW_IDLE && app.notice == RB_NOTICE_PTT_SHORT);
    // 刚好达到最短时长 → 正常保存。
    ptt_start();
    now_ms += RB_PTT_MIN_MS;
    act = key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
}

static void test_note_screen_off(void)
{
    now_ms = 20000;
    ready_idle();
    tick_to(now_ms + RB_OFF_IDLE_MS + 250);
    assert(app.backlight == 0);
    // 熄屏时按住上直接开始并点亮。
    uint32_t act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_RECORDING && app.mode == RB_MODE_NOTE);
    assert(act & RB_ACT_SESSION_START);
    assert(app.backlight == RB_BACKLIGHT_FULL && (act & RB_ACT_BACKLIGHT));
    tick_to(now_ms + 1000);
    key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING);
    ev(RB_EV_SESS_SAVED, 0, 1000, 0);
    tick_to(now_ms + RB_NOTE_DONE_MS + 250);
    assert(app.view == RB_VIEW_IDLE);

    // 会议的“熄屏首次按键只点亮”不变：确定只点亮。
    tick_to(now_ms + RB_OFF_IDLE_MS + 250);
    assert(app.backlight == 0);
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START) && app.backlight == RB_BACKLIGHT_FULL);

    // 非待机页（设置页）熄屏时，上键的按下只点亮，随后的单击也不移动选择。
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_SETTINGS);
    tick_to(now_ms + RB_OFF_IDLE_MS + 250);
    assert(app.backlight == 0);
    uint8_t sel = app.settings_sel;
    act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_SETTINGS && !(act & RB_ACT_SESSION_START));
    assert(app.backlight == RB_BACKLIGHT_FULL);
    key(RB_EV_PTT_UP);
    key(RB_EV_KEY_UP);
    assert(app.settings_sel == sel);
    // 屏亮后再按上：正常移动选择。
    key(RB_EV_PTT_DOWN);
    key(RB_EV_PTT_UP);
    key(RB_EV_KEY_UP);
    assert(app.settings_sel != sel);
}

static void test_note_wifi_offline(void)
{
    now_ms = 30000;
    rb_app_init(&app, RB_NET_READY, now_ms);
    assert(app.view == RB_VIEW_IDLE && !app.wifi_up);
    uint32_t act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START));
    assert(app.notice == RB_NOTICE_WIFI_OFFLINE);
    act = key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_IDLE && !(act & (RB_ACT_SESSION_STOP | RB_ACT_SESSION_CANCEL)));
}

static void test_note_failures(void)
{
    // 连接阶段失败（按着时）：进结束页（未保存），原因保留。
    now_ms = 40000;
    ready_idle();
    ptt_start();
    tick_to(now_ms + 3000);
    ev(RB_EV_SESS_FAILED, RB_FAIL_UNREACHABLE, 0, 0);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_FAILED);
    assert(app.done_fail == RB_FAIL_UNREACHABLE && !app.sess_started);
    // 之后松开、滞后的单击都不影响结束页。
    key(RB_EV_PTT_UP);
    key(RB_EV_KEY_UP);
    assert(app.view == RB_VIEW_DONE);
    tick_to(now_ms + RB_NOTE_DONE_MS);
    assert(app.view == RB_VIEW_IDLE);

    // 松开后（保存中）还没连上就失败：同样进结束页。
    ptt_start();
    tick_to(now_ms + 1000);
    uint32_t act = key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
    ev(RB_EV_SESS_FAILED, RB_FAIL_BUSY, 0, 0);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_FAILED && app.done_fail == RB_FAIL_BUSY);
    tick_to(now_ms + RB_NOTE_DONE_MS);

    // 松开后才连上（缓冲补发）：started 在保存中到达，随后 saved。
    ptt_start();
    tick_to(now_ms + 1000);
    key(RB_EV_PTT_UP);
    // 连接阶段耗时较长：保存兜底从 started 重新起算，不会在补发/等 saved 期间提前触发。
    tick_to(now_ms + 10000);
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    assert(app.view == RB_VIEW_SAVING && app.sess_started);
    act = tick_to(now_ms + RB_SAVE_GUARD_MS - 1000);
    assert(app.view == RB_VIEW_SAVING && !(act & RB_ACT_SESSION_CANCEL));
    ev(RB_EV_SESS_SAVED, 0, 1000, 1);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_SAVED);
    tick_to(now_ms + RB_NOTE_DONE_MS);

    // 一直连不上、会话任务也没上报：应用兜底超时 → 结束页 + 取消。
    ptt_start();
    act = tick_to(now_ms + RB_CONNECT_GUARD_MS);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_FAILED);
    assert(app.done_fail == RB_FAIL_UNREACHABLE && (act & RB_ACT_SESSION_CANCEL));
    key(RB_EV_PTT_UP);
    tick_to(now_ms + RB_NOTE_DONE_MS);
    assert(app.view == RB_VIEW_IDLE);

    // 已连上后长时间按住不触发兜底；录音中断网 → 缓冲；缓存溢出 → 中断结束页。
    ptt_start();
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    tick_to(now_ms + RB_CONNECT_GUARD_MS * 2);
    assert(app.view == RB_VIEW_RECORDING);
    ev(RB_EV_SESS_LINK, 0, 0, 0);
    assert(!app.link_ok);
    ev(RB_EV_SESS_ENDED, RB_FAIL_OVERFLOW, 0, 0);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_INTERRUPTED);
    assert(app.done_fail == RB_FAIL_OVERFLOW);
}

static void test_meeting_after_note(void)
{
    // 笔记之后再开会议：模式切回会议，会议路径与原来一致（含上键翻看与确认结束）。
    now_ms = 60000;
    ready_idle();
    ptt_start();
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    tick_to(now_ms + 1000);
    key(RB_EV_PTT_UP);
    ev(RB_EV_SESS_SAVED, 0, 1000, 1);
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_IDLE);

    start_recording();
    assert(app.mode == RB_MODE_MEETING && app.sess_started);
    ev(RB_EV_TRANSCRIPT, 0, 0, 5);
    // 上键单击的完整事件序列：按下、松开不影响会议录音，单击往旧翻看。
    uint32_t act = key(RB_EV_PTT_DOWN);
    assert(!(act & (RB_ACT_SESSION_START | RB_ACT_SESSION_STOP | RB_ACT_SESSION_CANCEL)));
    act = key(RB_EV_PTT_UP);
    assert(!(act & (RB_ACT_SESSION_START | RB_ACT_SESSION_STOP | RB_ACT_SESSION_CANCEL)));
    assert(!rb_app_ptt_waiting_release(&app));
    key(RB_EV_KEY_UP);
    assert(app.browse_offset == 1 && app.view == RB_VIEW_RECORDING);
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONFIRM_STOP);
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
    ev(RB_EV_SESS_SAVED, 0, 1000, 5);
    assert(app.view == RB_VIEW_DONE);
    // 会议结束页仍停留 10 s。
    uint32_t done_at = now_ms;
    tick_to(done_at + RB_NOTE_DONE_MS + 1000);
    assert(app.view == RB_VIEW_DONE);
    tick_to(done_at + RB_DONE_MS);
    assert(app.view == RB_VIEW_IDLE);
    // 会议连接中失败仍回待机并提示（不进结束页）。
    key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONNECTING && app.mode == RB_MODE_MEETING);
    ev(RB_EV_SESS_FAILED, RB_FAIL_UNREACHABLE, 0, 0);
    assert(app.view == RB_VIEW_IDLE && app.notice == RB_NOTICE_SERVER_OFFLINE);
    // 连接中按上键：不开始笔记。
    key(RB_EV_KEY_OK);
    act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_CONNECTING && !(act & RB_ACT_SESSION_START));
}

// ---------------------------------------------------------------------------
// 两级省电与电量（10-20-power-battery-optimization）
// ---------------------------------------------------------------------------

static void to_light(void)
{
    uint32_t act = tick_to(now_ms + RB_OFF_IDLE_MS + 250);
    assert(app.backlight == 0 && app.power == RB_POWER_LIGHT);
    assert(act & RB_ACT_POWER_LIGHT_ENTER);
}

// R1/R3：熄屏即浅睡；浅睡 15 min 后深睡；深睡后忽略事件，推迟（按键按住）后回到浅睡重新计时。
static void test_power_light_deep(void)
{
    now_ms = 100000;
    ready_idle();
    assert(app.power == RB_POWER_ACTIVE);
    uint32_t act = tick_to(now_ms + RB_OFF_IDLE_MS - 250);
    assert(app.power == RB_POWER_ACTIVE && !(act & RB_ACT_POWER_LIGHT_ENTER));
    to_light();
    uint32_t since = app.light_since_ms;
    act = tick_to(now_ms + 1000);
    assert(!(act & (RB_ACT_POWER_LIGHT_ENTER | RB_ACT_POWER_LIGHT_EXIT)));
    act = tick_to(since + RB_DEEP_IDLE_MS - 250);
    assert(app.power == RB_POWER_LIGHT && !(act & RB_ACT_POWER_DEEP));
    act = tick_to(since + RB_DEEP_IDLE_MS);
    assert(app.power == RB_POWER_DEEP && (act & RB_ACT_POWER_DEEP));
    assert(app.deep_reason == RB_SLEEP_IDLE && !(act & RB_ACT_POWER_LIGHT_EXIT));
    // 已决定深睡：其他事件一律忽略。
    assert(key(RB_EV_KEY_OK) == 0 && ev(RB_EV_TICK, 0, 0, 0) == 0 && ev(RB_EV_WAKE_KEY, 0, 0, 0) == 0);
    assert(app.power == RB_POWER_DEEP && app.view == RB_VIEW_IDLE);
    // 入睡前发现按键按住：放弃，回到浅睡并重新计时。
    act = ev(RB_EV_DEEP_ABORT, 0, 0, 0);
    assert(app.power == RB_POWER_LIGHT && app.light_since_ms == now_ms && app.deep_reason == RB_SLEEP_NONE);
    since = now_ms;
    act = tick_to(since + RB_DEEP_IDLE_MS - 250);
    assert(app.power == RB_POWER_LIGHT);
    act = tick_to(since + RB_DEEP_IDLE_MS);
    assert(app.power == RB_POWER_DEEP && (act & RB_ACT_POWER_DEEP));
    // 设置页同样熄屏进浅睡。
    ready_idle();
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_SETTINGS);
    to_light();
}

// R2：浅睡中 GPIO 唤醒 → 点亮；唤醒那次按键只点亮；按住上直接记笔记；超时后按键正常生效。
static void test_power_wake_key(void)
{
    now_ms = 200000;
    ready_idle();
    to_light();
    uint32_t act = ev(RB_EV_WAKE_KEY, 0, 0, 0);
    assert(app.power == RB_POWER_ACTIVE && app.backlight == RB_BACKLIGHT_FULL);
    assert((act & RB_ACT_POWER_LIGHT_EXIT) && (act & RB_ACT_BACKLIGHT) && (act & RB_ACT_RENDER));
    assert(ev(RB_EV_WAKE_KEY, 0, 0, 0) == 0);           // 重复/迟到的唤醒忽略
    now_ms += 200;
    act = key(RB_EV_KEY_OK);                             // 唤醒那次按键的单击：只点亮
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START));
    now_ms += 500;
    act = key(RB_EV_KEY_OK);                             // 再按确定：会议录音
    assert(app.view == RB_VIEW_CONNECTING && (act & RB_ACT_SESSION_START));
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_IDLE);

    // 唤醒后按住上：直接开始笔记录音。
    to_light();
    ev(RB_EV_WAKE_KEY, 0, 0, 0);
    now_ms += 50;
    act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_RECORDING && app.mode == RB_MODE_NOTE && (act & RB_ACT_SESSION_START));
    now_ms += 1000;
    key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING);
    ev(RB_EV_SESS_SAVED, 0, 1000, 0);
    tick_to(now_ms + RB_NOTE_DONE_MS + 250);
    assert(app.view == RB_VIEW_IDLE);

    // 设置页唤醒：上键按下只点亮，随后的单击不移动选择。
    key(RB_EV_KEY_OK_LONG);
    to_light();
    uint8_t sel = app.settings_sel;
    ev(RB_EV_WAKE_KEY, 0, 0, 0);
    act = key(RB_EV_PTT_DOWN);
    assert(app.view == RB_VIEW_SETTINGS && !(act & RB_ACT_SESSION_START));
    key(RB_EV_PTT_UP);
    key(RB_EV_KEY_UP);
    assert(app.settings_sel == sel);
    key(RB_EV_KEY_OK_LONG);
    assert(app.view == RB_VIEW_IDLE);

    // 唤醒按键没有产生事件（恢复前已松开）：保护窗口过后按键正常生效。
    to_light();
    ev(RB_EV_WAKE_KEY, 0, 0, 0);
    now_ms += RB_WAKE_GUARD_MS;
    act = key(RB_EV_KEY_OK);
    assert(app.view == RB_VIEW_CONNECTING && (act & RB_ACT_SESSION_START));

    // 按键组件未能暂停（入睡时键按住）时，浅睡中的按键事件同样退出浅睡、只点亮。
    key(RB_EV_KEY_OK_LONG);
    to_light();
    act = key(RB_EV_KEY_OK);
    assert(app.power == RB_POWER_ACTIVE && (act & RB_ACT_POWER_LIGHT_EXIT));
    assert(app.view == RB_VIEW_IDLE && !(act & RB_ACT_SESSION_START));
}

// R10：待机页单击下 = 立即熄屏进浅睡，并从此刻计深睡；其他页面下键行为不变。
static void test_power_manual_sleep(void)
{
    now_ms = 300000;
    ready_idle();
    uint32_t act = key(RB_EV_KEY_DOWN);
    assert(app.backlight == 0 && app.power == RB_POWER_LIGHT && app.manual_off);
    assert((act & RB_ACT_BACKLIGHT) && (act & RB_ACT_POWER_LIGHT_ENTER));
    assert(app.light_since_ms == now_ms && app.view == RB_VIEW_IDLE);
    act = tick_to(now_ms + 5000);                        // TICK 不会把屏点亮
    assert(app.backlight == 0 && !(act & RB_ACT_POWER_LIGHT_EXIT));
    // 熄屏时再按下（按键组件未暂停的情况）：只亮屏，不反复切换。
    act = key(RB_EV_KEY_DOWN);
    assert(app.backlight == RB_BACKLIGHT_FULL && app.power == RB_POWER_ACTIVE && !app.manual_off);
    assert(act & RB_ACT_POWER_LIGHT_EXIT);
    // 调暗时单击下同样进入浅睡。
    tick_to(now_ms + RB_DIM_IDLE_MS + 250);
    assert(app.backlight == RB_BACKLIGHT_DIM);
    act = key(RB_EV_KEY_DOWN);
    assert(app.backlight == 0 && app.power == RB_POWER_LIGHT && (act & RB_ACT_POWER_LIGHT_ENTER));
    // GPIO 唤醒后，唤醒那次下键的单击只点亮。
    ev(RB_EV_WAKE_KEY, 0, 0, 0);
    act = key(RB_EV_KEY_DOWN);
    assert(app.backlight == RB_BACKLIGHT_FULL && app.power == RB_POWER_ACTIVE && !app.manual_off);
    assert(!(act & RB_ACT_POWER_LIGHT_ENTER) && !(act & RB_ACT_SESSION_START));
    // 再按下：再次休眠；15 min 后深睡（从按键时刻起算，而不是 120 s 自动熄屏）。
    now_ms += 2000;
    key(RB_EV_KEY_DOWN);
    uint32_t since = now_ms;
    assert(app.power == RB_POWER_LIGHT && app.light_since_ms == since);
    act = tick_to(since + RB_DEEP_IDLE_MS - 250);
    assert(app.power == RB_POWER_LIGHT);
    act = tick_to(since + RB_DEEP_IDLE_MS);
    assert(app.power == RB_POWER_DEEP && app.deep_reason == RB_SLEEP_IDLE);

    // 录音页：下键仍是往新翻看，不熄屏。
    ready_idle();
    start_recording();
    ev(RB_EV_TRANSCRIPT, 0, 0, 5);
    key(RB_EV_KEY_UP);
    key(RB_EV_KEY_UP);
    assert(app.browse_offset == 2);
    act = key(RB_EV_KEY_DOWN);
    assert(app.browse_offset == 1 && app.backlight == RB_BACKLIGHT_FULL && app.power == RB_POWER_ACTIVE);
    assert(!(act & RB_ACT_POWER_LIGHT_ENTER) && !app.manual_off);
    // 确认结束页：下键继续录音。
    key(RB_EV_KEY_OK);
    act = key(RB_EV_KEY_DOWN);
    assert(app.view == RB_VIEW_RECORDING && app.power == RB_POWER_ACTIVE && !(act & RB_ACT_POWER_LIGHT_ENTER));
    // 设置页：下键移动选择。
    ready_idle();
    key(RB_EV_KEY_OK_LONG);
    act = key(RB_EV_KEY_DOWN);
    assert(app.view == RB_VIEW_SETTINGS && app.settings_sel == RB_SET_SERVER);
    assert(app.backlight == RB_BACKLIGHT_FULL && !(act & RB_ACT_POWER_LIGHT_ENTER));
    // 结束页：下键回待机（不休眠）。
    ready_idle();
    start_recording();
    key(RB_EV_KEY_OK);
    key(RB_EV_KEY_OK);
    ev(RB_EV_SESS_SAVED, 0, 1000, 1);
    assert(app.view == RB_VIEW_DONE);
    act = key(RB_EV_KEY_DOWN);
    assert(app.view == RB_VIEW_IDLE && app.backlight == RB_BACKLIGHT_FULL && !(act & RB_ACT_POWER_LIGHT_ENTER));
    // 配网页：下键无作用，也不休眠。
    rb_app_init(&app, RB_NET_PROV_AP, now_ms);
    act = key(RB_EV_KEY_DOWN);
    assert(app.view == RB_VIEW_PROVISIONING && app.backlight == RB_BACKLIGHT_FULL);
    assert(!(act & RB_ACT_POWER_LIGHT_ENTER));
}

// R4：连接、录音、确认结束、保存中不熄屏、不进任何一级睡眠（会议录音 > 20 min）。
static void test_power_session_pinned(void)
{
    now_ms = 400000;
    ready_idle();
    start_recording();
    uint32_t act = tick_to(now_ms + 25u * 60u * 1000u);
    assert(app.view == RB_VIEW_RECORDING && app.power == RB_POWER_ACTIVE && app.backlight == RB_BACKLIGHT_DIM);
    assert(!(act & (RB_ACT_POWER_LIGHT_ENTER | RB_ACT_POWER_DEEP)));
    key(RB_EV_KEY_OK);
    act = tick_to(now_ms + RB_CONFIRM_MS - 250);
    assert(app.view == RB_VIEW_CONFIRM_STOP && app.power == RB_POWER_ACTIVE);
    key(RB_EV_KEY_OK);
    act = tick_to(now_ms + RB_SAVE_GUARD_MS - 1000);
    assert(app.view == RB_VIEW_SAVING && app.power == RB_POWER_ACTIVE && app.backlight == RB_BACKLIGHT_FULL);
    assert(!(act & (RB_ACT_POWER_LIGHT_ENTER | RB_ACT_POWER_DEEP)));
    // 连接中直到兜底超时都不睡。
    ready_idle();
    key(RB_EV_KEY_OK);
    act = tick_to(now_ms + RB_CONNECT_GUARD_MS - 250);
    assert(app.view == RB_VIEW_CONNECTING && !(act & (RB_ACT_POWER_LIGHT_ENTER | RB_ACT_POWER_DEEP)));
    // 笔记按住很久也不睡。
    ready_idle();
    ptt_start();
    ev(RB_EV_SESS_STARTED, 0, 0, 0);
    act = tick_to(now_ms + RB_DEEP_IDLE_MS + RB_OFF_IDLE_MS);
    assert(app.view == RB_VIEW_RECORDING && app.power == RB_POWER_ACTIVE);
    assert(!(act & (RB_ACT_POWER_LIGHT_ENTER | RB_ACT_POWER_DEEP)));
}

// R5：配网/配对页 10 min 无操作直接深睡；按键与配网进展重新计时；加入 Wi-Fi 中不睡。
static void test_power_provisioning_deep(void)
{
    now_ms = 500000;
    rb_app_init(&app, RB_NET_PROV_AP, now_ms);
    uint32_t start = now_ms;
    uint32_t act = tick_to(start + RB_PROV_DEEP_MS - 250);
    assert(app.power == RB_POWER_ACTIVE && app.backlight == RB_BACKLIGHT_DIM && !(act & RB_ACT_POWER_DEEP));
    act = tick_to(start + RB_PROV_DEEP_MS);
    assert(app.power == RB_POWER_DEEP && (act & RB_ACT_POWER_DEEP) && app.deep_reason == RB_SLEEP_PROV);

    rb_app_init(&app, RB_NET_PROV_AP, now_ms);
    start = now_ms;
    tick_to(start + RB_PROV_DEEP_MS / 2);
    key(RB_EV_KEY_OK);                                   // 配网页单击无作用，但算操作
    start = now_ms;
    act = tick_to(start + RB_PROV_DEEP_MS - 250);
    assert(app.power == RB_POWER_ACTIVE);
    ev(RB_EV_NET_PHASE, RB_NET_PROV_JOINING, 0, 0);      // 手机提交了表单
    act = tick_to(now_ms + RB_PROV_DEEP_MS + 5000);
    assert(app.power == RB_POWER_ACTIVE && !(act & RB_ACT_POWER_DEEP));   // 加入中不睡
    ev(RB_EV_NET_PHASE, RB_NET_PROV_AP, 0, 0);           // 加入失败回到热点：重新计时
    start = now_ms;
    act = tick_to(start + RB_PROV_DEEP_MS - 250);
    assert(app.power == RB_POWER_ACTIVE);
    act = tick_to(start + RB_PROV_DEEP_MS);
    assert(app.power == RB_POWER_DEEP && app.deep_reason == RB_SLEEP_PROV);

    rb_app_init(&app, RB_NET_PAIRING, now_ms);
    start = now_ms;
    act = tick_to(start + RB_PROV_DEEP_MS);
    assert(app.view == RB_VIEW_PAIRING && app.power == RB_POWER_DEEP && app.deep_reason == RB_SLEEP_PROV);
}

static uint32_t batt(int32_t soc, int32_t mv) { return ev(RB_EV_BATTERY, soc, 0, mv); }

// R7/R8：低电提示一次；保护需连续两次满足；会话中先保存再深睡；读数过滤。
static void test_power_battery(void)
{
    assert(rb_app_filter_soc(0, 3700) == -1);
    assert(rb_app_filter_soc(0, RB_BATT_SANE_MV) == 0);
    assert(rb_app_filter_soc(0, 3400) == 0);
    assert(rb_app_filter_soc(0, -1) == 0);
    assert(rb_app_filter_soc(57, 3900) == 57);
    assert(rb_app_filter_soc(-1, 3900) == -1);

    now_ms = 600000;
    ready_idle();
    uint32_t act = batt(80, 3950);
    assert(app.notice == RB_NOTICE_NONE && app.batt_soc == 80 && app.batt_mv == 3950);
    act = batt(5, 3650);
    assert(app.notice == RB_NOTICE_BATT_LOW && (act & RB_ACT_RENDER));
    tick_to(now_ms + RB_NOTICE_MS + 250);
    assert(app.notice == RB_NOTICE_NONE);
    batt(4, 3640);
    assert(app.notice == RB_NOTICE_NONE);                 // 只提示一次
    batt(RB_BATT_WARN_RESET, 3700);                       // 充电回升
    batt(5, 3650);
    assert(app.notice == RB_NOTICE_BATT_LOW);
    // 单次低电不触发；中间恢复则重新计数。
    act = batt(2, 3500);
    assert(!(act & RB_ACT_POWER_DEEP) && app.power == RB_POWER_ACTIVE);
    batt(3, 3500);
    act = batt(2, 3500);
    assert(!(act & RB_ACT_POWER_DEEP));
    act = batt(1, 3480);
    assert((act & RB_ACT_POWER_DEEP) && app.power == RB_POWER_DEEP && app.deep_reason == RB_SLEEP_LOW_BATT);
    assert(app.notice == RB_NOTICE_BATT_CUT && app.backlight == RB_BACKLIGHT_FULL && (act & RB_ACT_RENDER));
    // 推迟后需重新连续两次。
    ev(RB_EV_DEEP_ABORT, 0, 0, 0);
    assert(app.power == RB_POWER_ACTIVE && !app.batt_cut);
    act = batt(1, 3480);
    assert(!(act & RB_ACT_POWER_DEEP));
    act = batt(1, 3480);
    assert(act & RB_ACT_POWER_DEEP);

    // 电压低于阈值也触发（SOC 无效时）。
    ready_idle();
    batt(-1, RB_BATT_CUT_MV - 50);
    act = batt(-1, RB_BATT_CUT_MV - 60);
    assert((act & RB_ACT_POWER_DEEP) && app.deep_reason == RB_SLEEP_LOW_BATT);
    // 无效读数（-1/-1）不算低电。
    ready_idle();
    batt(-1, -1);
    act = batt(-1, -1);
    assert(!(act & RB_ACT_POWER_DEEP) && app.batt_cut_hits == 0);

    // 会议录音中：先结束并保存，saved 后深睡。
    ready_idle();
    start_recording();
    tick_to(now_ms + 5000);
    batt(2, 3400);
    act = batt(2, 3400);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP) && !(act & RB_ACT_POWER_DEEP));
    assert(app.notice == RB_NOTICE_BATT_CUT && app.power == RB_POWER_ACTIVE);
    act = tick_to(now_ms + 3000);
    assert(app.view == RB_VIEW_SAVING && !(act & RB_ACT_POWER_DEEP));
    ev(RB_EV_SESS_SAVED, 0, 5000, 2);
    assert(app.view == RB_VIEW_DONE && app.done_kind == RB_DONE_SAVED);
    act = tick_to(now_ms + 250);
    assert((act & RB_ACT_POWER_DEEP) && app.deep_reason == RB_SLEEP_LOW_BATT && app.notice == RB_NOTICE_BATT_CUT);
    // 保存超时同样深睡。
    ready_idle();
    start_recording();
    key(RB_EV_KEY_OK);                                    // 确认结束页中触发
    batt(1, 3300);
    act = batt(1, 3300);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP));
    act = tick_to(now_ms + RB_SAVE_GUARD_MS + 250);
    assert((act & RB_ACT_POWER_DEEP) && app.view == RB_VIEW_DONE);
    // 笔记录音中：结束并保存（按住上键的松开不再起作用）。
    ready_idle();
    ptt_start();
    now_ms += 1000;
    batt(2, 3400);
    act = batt(2, 3400);
    assert(app.view == RB_VIEW_SAVING && (act & RB_ACT_SESSION_STOP) && !app.ptt_held);
    key(RB_EV_PTT_UP);
    assert(app.view == RB_VIEW_SAVING);
    // 连接中：取消，立即深睡。
    ready_idle();
    key(RB_EV_KEY_OK);
    batt(2, 3400);
    act = batt(2, 3400);
    assert((act & RB_ACT_SESSION_CANCEL) && (act & RB_ACT_POWER_DEEP) && app.view == RB_VIEW_IDLE);
    // 浅睡中低电：先唤醒屏幕显示提示再深睡。
    ready_idle();
    to_light();
    batt(2, 3400);
    act = batt(2, 3400);
    assert((act & RB_ACT_POWER_DEEP) && (act & RB_ACT_POWER_LIGHT_EXIT) && (act & RB_ACT_BACKLIGHT));
    assert(app.backlight == RB_BACKLIGHT_FULL && app.notice == RB_NOTICE_BATT_CUT);
    // 浅睡中的低电提示不点亮屏幕，唤醒后再显示。
    ready_idle();
    to_light();
    act = batt(5, 3650);
    assert(app.notice == RB_NOTICE_NONE && app.backlight == 0 && app.power == RB_POWER_LIGHT);
    assert(!(act & RB_ACT_POWER_LIGHT_EXIT));
    ev(RB_EV_WAKE_KEY, 0, 0, 0);
    tick_to(now_ms + 250);
    assert(app.notice == RB_NOTICE_BATT_LOW);
}

int main(void)
{
    test_power_light_deep();
    test_power_wake_key();
    test_power_manual_sleep();
    test_power_session_pinned();
    test_power_provisioning_deep();
    test_power_battery();
    test_note_happy_path();
    test_note_done_dismiss();
    test_note_short_press();
    test_note_screen_off();
    test_note_wifi_offline();
    test_note_failures();
    test_meeting_after_note();
    test_happy_path();
    test_browse();
    test_failures();
    test_provisioning_and_settings();
    test_backlight();
    test_wraparound();
    return 0;
}
