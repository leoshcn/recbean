// main/rb_app.c —— 应用状态机实现。转移规则见 rb_app.h 顶部说明。
#include "rb_app.h"

#include <string.h>

// a 是否已到达/超过 b（容忍回绕）。
static bool reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static rb_view_t home_view(const rb_app_t *app)
{
    switch (app->phase) {
    case RB_NET_PROV_AP:
    case RB_NET_PROV_JOINING:
        return RB_VIEW_PROVISIONING;
    case RB_NET_PAIRING:
        return RB_VIEW_PAIRING;
    case RB_NET_READY:
    default:
        return RB_VIEW_IDLE;
    }
}

// 切换视图并把它当作一次“活动”：新页面出现时屏幕应当亮起。
static uint32_t set_view(rb_app_t *app, rb_view_t view, uint32_t now)
{
    app->view = view;
    app->last_activity_ms = now;
    if (view == RB_VIEW_SETTINGS) {
        app->settings_sel = RB_SET_REPROVISION;
        app->settings_detail = -1;
        app->reprov_armed = false;
    }
    return RB_ACT_RENDER;
}

static uint32_t show_notice(rb_app_t *app, rb_notice_t notice, uint32_t now)
{
    app->notice = notice;
    app->notice_deadline = now + RB_NOTICE_MS;
    app->last_activity_ms = now;
    return RB_ACT_RENDER;
}

static rb_notice_t notice_for_fail(int32_t reason)
{
    switch ((rb_fail_t)reason) {
    case RB_FAIL_AUTH: return RB_NOTICE_AUTH;
    case RB_FAIL_BUSY: return RB_NOTICE_BUSY;
    case RB_FAIL_AUDIO: return RB_NOTICE_AUDIO;
    case RB_FAIL_SERVER: return RB_NOTICE_SERVER_ERROR;
    case RB_FAIL_UNREACHABLE:
    default:
        return RB_NOTICE_SERVER_OFFLINE;
    }
}

static uint8_t backlight_target(const rb_app_t *app, uint32_t now)
{
    uint32_t idle = now - app->last_activity_ms;
    switch (app->view) {
    case RB_VIEW_CONNECTING:
    case RB_VIEW_CONFIRM_STOP:
    case RB_VIEW_SAVING:
        return RB_BACKLIGHT_FULL;
    case RB_VIEW_RECORDING:
    case RB_VIEW_PROVISIONING:
    case RB_VIEW_PAIRING:
        return idle >= RB_DIM_BUSY_MS ? RB_BACKLIGHT_DIM : RB_BACKLIGHT_FULL;
    case RB_VIEW_IDLE:
    case RB_VIEW_DONE:
    case RB_VIEW_SETTINGS:
    default:
        if (idle >= RB_OFF_IDLE_MS) return 0;
        if (idle >= RB_DIM_IDLE_MS) return RB_BACKLIGHT_DIM;
        return RB_BACKLIGHT_FULL;
    }
}

static uint32_t update_backlight(rb_app_t *app, uint32_t now)
{
    uint8_t target = backlight_target(app, now);
    if (target == app->backlight) return 0;
    app->backlight = target;
    return RB_ACT_BACKLIGHT;
}

void rb_app_init(rb_app_t *app, rb_net_phase_t phase, uint32_t now)
{
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->phase = phase;
    app->server_state = -1;
    app->settings_detail = -1;
    app->done_sentences = -1;
    app->backlight = RB_BACKLIGHT_FULL;
    app->view = home_view(app);
    app->last_activity_ms = now;
}

bool rb_app_in_session(const rb_app_t *app)
{
    if (!app) return false;
    return app->view == RB_VIEW_CONNECTING || app->view == RB_VIEW_RECORDING ||
           app->view == RB_VIEW_CONFIRM_STOP || app->view == RB_VIEW_SAVING;
}

static bool recording_like(const rb_app_t *app)
{
    return app->view == RB_VIEW_RECORDING || app->view == RB_VIEW_CONFIRM_STOP ||
           app->view == RB_VIEW_SAVING;
}

uint32_t rb_app_elapsed_ms(const rb_app_t *app, uint32_t now)
{
    if (!app) return 0;
    if (app->view == RB_VIEW_RECORDING || app->view == RB_VIEW_CONFIRM_STOP) {
        return now - app->rec_start_ms;
    }
    if (app->view == RB_VIEW_SAVING || app->view == RB_VIEW_DONE) {
        return app->rec_end_ms - app->rec_start_ms;
    }
    return 0;
}

static uint32_t enter_done(rb_app_t *app, rb_done_kind_t kind, uint32_t now)
{
    if (app->view == RB_VIEW_RECORDING || app->view == RB_VIEW_CONFIRM_STOP) {
        app->rec_end_ms = now;   // 异常结束时也冻结计时
    }
    app->done_kind = kind;
    app->done_duration_ms = (int64_t)(app->rec_end_ms - app->rec_start_ms);
    app->done_sentences = (int32_t)app->ts_count;   // 先用本地句数，saved 到达后覆盖
    app->deadline = now + RB_DONE_MS;
    app->browse_offset = 0;
    return set_view(app, RB_VIEW_DONE, now);
}

// ---------------------------------------------------------------------------
// 按键
// ---------------------------------------------------------------------------

static uint32_t key_settings(rb_app_t *app, rb_ev_type_t key, uint32_t now)
{
    if (key == RB_EV_KEY_OK_LONG) return set_view(app, home_view(app), now);
    if (app->settings_detail >= 0) {
        app->settings_detail = -1;      // 详情页：任意键回列表
        return RB_ACT_RENDER;
    }
    if (key == RB_EV_KEY_UP || key == RB_EV_KEY_DOWN) {
        int sel = app->settings_sel + (key == RB_EV_KEY_UP ? -1 : 1);
        if (sel < 0) sel = RB_SET_COUNT - 1;
        if (sel >= RB_SET_COUNT) sel = 0;
        app->settings_sel = (uint8_t)sel;
        app->reprov_armed = false;
        return RB_ACT_RENDER;
    }
    // OK 单击
    switch ((rb_settings_item_t)app->settings_sel) {
    case RB_SET_REPROVISION:
        if (!app->reprov_armed) {
            app->reprov_armed = true;
            app->reprov_deadline = now + RB_REPROV_ARM_MS;
            return RB_ACT_RENDER;
        }
        app->reprov_armed = false;
        app->phase = RB_NET_PROV_AP;    // 乐观切换；网络任务随后上报真实阶段
        app->wifi_up = false;
        app->server_state = -1;
        return set_view(app, RB_VIEW_PROVISIONING, now) | RB_ACT_REPROVISION;
    case RB_SET_SERVER:
    case RB_SET_INFO:
        app->settings_detail = (int8_t)app->settings_sel;
        return RB_ACT_RENDER;
    case RB_SET_BACK:
    default:
        return set_view(app, home_view(app), now);
    }
}

static uint32_t handle_key(rb_app_t *app, rb_ev_type_t key, uint32_t now)
{
    // 熄屏时第一次按键只负责点亮，不触发任何操作（避免误开始录音）。
    bool was_off = app->backlight == 0;
    app->last_activity_ms = now;
    uint32_t act = update_backlight(app, now);
    if (was_off) return act | RB_ACT_RENDER;

    switch (app->view) {
    case RB_VIEW_IDLE:
        if (key == RB_EV_KEY_OK_LONG) return act | set_view(app, RB_VIEW_SETTINGS, now);
        if (key != RB_EV_KEY_OK) return act;
        if (!app->wifi_up) return act | show_notice(app, RB_NOTICE_WIFI_OFFLINE, now);
        app->notice = RB_NOTICE_NONE;
        app->ts_count = 0;
        app->browse_offset = 0;
        app->asr_error = false;
        app->link_ok = true;
        app->deadline = now + RB_CONNECT_GUARD_MS;
        return act | set_view(app, RB_VIEW_CONNECTING, now) |
               RB_ACT_SESSION_START | RB_ACT_CLEAR_TRANSCRIPT;

    case RB_VIEW_CONNECTING:
        if (key != RB_EV_KEY_OK_LONG) return act;
        return act | set_view(app, home_view(app), now) | RB_ACT_SESSION_CANCEL;

    case RB_VIEW_RECORDING:
        if (key == RB_EV_KEY_OK) {
            app->deadline = now + RB_CONFIRM_MS;
            return act | set_view(app, RB_VIEW_CONFIRM_STOP, now);
        }
        if (key == RB_EV_KEY_UP) {
            if (app->browse_offset + 1 < app->ts_count) app->browse_offset++;
            app->browse_deadline = now + RB_BROWSE_RETURN_MS;
            return act | RB_ACT_RENDER;
        }
        if (key == RB_EV_KEY_DOWN) {
            if (app->browse_offset > 0) app->browse_offset--;
            app->browse_deadline = now + RB_BROWSE_RETURN_MS;
            return act | RB_ACT_RENDER;
        }
        return act;

    case RB_VIEW_CONFIRM_STOP:
        if (key == RB_EV_KEY_OK) {
            app->rec_end_ms = now;
            app->browse_offset = 0;
            app->deadline = now + RB_SAVE_GUARD_MS;
            return act | set_view(app, RB_VIEW_SAVING, now) | RB_ACT_SESSION_STOP;
        }
        return act | set_view(app, RB_VIEW_RECORDING, now);   // 其他键：继续录音

    case RB_VIEW_SAVING:
        return act;   // 保存中不响应按键，等待 saved 或超时

    case RB_VIEW_DONE:
        return act | set_view(app, home_view(app), now);

    case RB_VIEW_SETTINGS:
        return act | key_settings(app, key, now);

    case RB_VIEW_PROVISIONING:
    case RB_VIEW_PAIRING:
        if (key == RB_EV_KEY_OK_LONG) return act | set_view(app, RB_VIEW_SETTINGS, now);
        return act;
    }
    return act;
}

// ---------------------------------------------------------------------------
// 网络 / 会话事件
// ---------------------------------------------------------------------------

static uint32_t handle_net(rb_app_t *app, const rb_event_t *ev, uint32_t now)
{
    switch (ev->type) {
    case RB_EV_NET_PHASE: {
        rb_net_phase_t phase = (rb_net_phase_t)ev->a;
        if (phase == app->phase) return 0;
        app->phase = phase;
        // 用户在设置/结束页时不把画面拽走；待机与配网相关页面跟随阶段切换。
        if (app->view == RB_VIEW_IDLE || app->view == RB_VIEW_PROVISIONING ||
            app->view == RB_VIEW_PAIRING) {
            rb_view_t next = home_view(app);
            if (next != app->view) return set_view(app, next, now);
        }
        return RB_ACT_RENDER;
    }
    case RB_EV_WIFI:
        app->wifi_up = ev->a != 0;
        if (!app->wifi_up) app->server_state = -1;
        return RB_ACT_RENDER;
    case RB_EV_SERVER:
        app->server_state = ev->a ? 1 : 0;
        return RB_ACT_RENDER;
    case RB_EV_TRANSCRIPT:
        app->ts_count = ev->c > 0 ? (uint32_t)ev->c : 0;
        if (app->ts_count == 0) app->browse_offset = 0;
        else if (app->browse_offset >= app->ts_count) app->browse_offset = app->ts_count - 1;
        return RB_ACT_RENDER;
    default:
        break;
    }

    if (!rb_app_in_session(app)) return 0;   // 会话已结束后的迟到事件一律忽略

    switch (ev->type) {
    case RB_EV_SESS_STARTED:
        if (app->view != RB_VIEW_CONNECTING) return 0;
        app->rec_start_ms = now;
        app->rec_end_ms = now;
        app->link_ok = true;
        return set_view(app, RB_VIEW_RECORDING, now);

    case RB_EV_SESS_FAILED:
        if (app->view == RB_VIEW_CONNECTING) {
            uint32_t act = set_view(app, home_view(app), now);
            return act | show_notice(app, notice_for_fail(ev->a), now);
        }
        return enter_done(app, RB_DONE_FAILED, now);

    case RB_EV_SESS_LINK:
        if (!recording_like(app)) return 0;
        app->link_ok = ev->a != 0;
        return RB_ACT_RENDER;

    case RB_EV_SESS_ASR_ERROR:
        app->asr_error = true;
        return show_notice(app, RB_NOTICE_ASR_ERROR, now);

    case RB_EV_SESS_SAVED: {
        if (app->view == RB_VIEW_CONNECTING) return 0;
        uint32_t act = enter_done(app, RB_DONE_SAVED, now);
        if (ev->b > 0) app->done_duration_ms = ev->b;
        if (ev->c >= 0) app->done_sentences = ev->c;
        return act;
    }

    case RB_EV_SESS_ENDED:
        if (app->view == RB_VIEW_CONNECTING) {
            uint32_t act = set_view(app, home_view(app), now);
            return act | show_notice(app, notice_for_fail(ev->a), now);
        }
        if (ev->a == RB_FAIL_OVERFLOW) return enter_done(app, RB_DONE_INTERRUPTED, now);
        if (ev->a == RB_FAIL_SAVE_TIMEOUT) return enter_done(app, RB_DONE_UNCONFIRMED, now);
        return enter_done(app, RB_DONE_FAILED, now);

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// TICK
// ---------------------------------------------------------------------------

static uint32_t handle_tick(rb_app_t *app, uint32_t now)
{
    uint32_t act = 0;
    if (app->notice != RB_NOTICE_NONE && reached(now, app->notice_deadline)) {
        app->notice = RB_NOTICE_NONE;
        act |= RB_ACT_RENDER;
    }
    switch (app->view) {
    case RB_VIEW_CONNECTING:
        if (reached(now, app->deadline)) {
            act |= set_view(app, home_view(app), now) | RB_ACT_SESSION_CANCEL;
            act |= show_notice(app, RB_NOTICE_SERVER_OFFLINE, now);
        }
        break;
    case RB_VIEW_RECORDING:
        if (app->browse_offset > 0 && reached(now, app->browse_deadline)) {
            app->browse_offset = 0;
        }
        act |= RB_ACT_RENDER;   // 计时每秒变化
        break;
    case RB_VIEW_CONFIRM_STOP:
        if (reached(now, app->deadline)) act |= set_view(app, RB_VIEW_RECORDING, now);
        act |= RB_ACT_RENDER;
        break;
    case RB_VIEW_SAVING:
        if (reached(now, app->deadline)) {
            act |= enter_done(app, RB_DONE_UNCONFIRMED, now) | RB_ACT_SESSION_CANCEL;
        }
        break;
    case RB_VIEW_DONE:
        if (reached(now, app->deadline)) act |= set_view(app, home_view(app), now);
        break;
    case RB_VIEW_SETTINGS:
        if (app->reprov_armed && reached(now, app->reprov_deadline)) {
            app->reprov_armed = false;
            act |= RB_ACT_RENDER;
        }
        break;
    default:
        break;
    }
    return act | update_backlight(app, now);
}

uint32_t rb_app_handle(rb_app_t *app, const rb_event_t *ev, uint32_t now)
{
    if (!app || !ev) return 0;
    uint32_t act;
    switch (ev->type) {
    case RB_EV_KEY_UP:
    case RB_EV_KEY_DOWN:
    case RB_EV_KEY_OK:
    case RB_EV_KEY_OK_LONG:
        return handle_key(app, ev->type, now);
    case RB_EV_TICK:
        return handle_tick(app, now);
    default:
        act = handle_net(app, ev, now);
        return act | update_backlight(app, now);
    }
}
