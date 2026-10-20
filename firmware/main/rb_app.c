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
    app->manual_off = false;
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
        if (app->manual_off || idle >= RB_OFF_IDLE_MS) return 0;
        if (idle >= RB_DIM_IDLE_MS) return RB_BACKLIGHT_DIM;
        return RB_BACKLIGHT_FULL;
    }
}

// 下发背光，并让电源状态跟随：熄屏即进入浅睡，点亮即退出浅睡。
static uint32_t update_backlight(rb_app_t *app, uint32_t now)
{
    uint8_t target = backlight_target(app, now);
    uint32_t act = 0;
    if (target != app->backlight) {
        app->backlight = target;
        act |= RB_ACT_BACKLIGHT;
    }
    if (target == 0 && app->power == RB_POWER_ACTIVE) {
        app->power = RB_POWER_LIGHT;
        app->light_since_ms = now;
        act |= RB_ACT_POWER_LIGHT_ENTER;
    } else if (target > 0 && app->power == RB_POWER_LIGHT) {
        app->power = RB_POWER_ACTIVE;
        act |= RB_ACT_POWER_LIGHT_EXIT;
    }
    return act;
}

// 决定深睡（终端）。低电时先点亮并显示“电量过低”，app 任务重绘、停留后再睡。
static uint32_t enter_deep(rb_app_t *app, rb_sleep_reason_t reason, uint32_t now)
{
    uint32_t act = RB_ACT_POWER_DEEP;
    if (reason == RB_SLEEP_LOW_BATT) {
        if (app->power == RB_POWER_LIGHT) act |= RB_ACT_POWER_LIGHT_EXIT;
        app->notice = RB_NOTICE_BATT_CUT;
        app->notice_deadline = now + RB_NOTICE_MS;
        if (app->backlight != RB_BACKLIGHT_FULL) {
            app->backlight = RB_BACKLIGHT_FULL;
            act |= RB_ACT_BACKLIGHT;
        }
        act |= RB_ACT_RENDER;
    }
    app->power = RB_POWER_DEEP;
    app->deep_reason = reason;
    return act;
}

int rb_app_filter_soc(int soc, int mv)
{
    if (soc == 0 && mv > RB_BATT_SANE_MV) return -1;
    return soc;
}

void rb_app_init(rb_app_t *app, rb_net_phase_t phase, uint32_t now)
{
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->phase = phase;
    app->server_state = -1;
    app->settings_detail = -1;
    app->done_sentences = -1;
    app->done_fail = -1;
    app->mode = RB_MODE_MEETING;
    app->backlight = RB_BACKLIGHT_FULL;
    app->view = home_view(app);
    app->last_activity_ms = now;
    app->power = RB_POWER_ACTIVE;
    app->deep_reason = RB_SLEEP_NONE;
    app->batt_soc = -1;
    app->batt_mv = -1;
}

bool rb_app_in_session(const rb_app_t *app)
{
    if (!app) return false;
    return app->view == RB_VIEW_CONNECTING || app->view == RB_VIEW_RECORDING ||
           app->view == RB_VIEW_CONFIRM_STOP || app->view == RB_VIEW_SAVING;
}

bool rb_app_ptt_waiting_release(const rb_app_t *app)
{
    return app && app->mode == RB_MODE_NOTE && app->ptt_held && app->view == RB_VIEW_RECORDING;
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
    app->done_fail = -1;                            // 异常结束时由调用方随后写入原因
    app->deadline = now + (app->mode == RB_MODE_NOTE ? RB_NOTE_DONE_MS : RB_DONE_MS);
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

// 新会话共用的状态清理。
static void reset_session_state(rb_app_t *app, rb_mode_t mode)
{
    app->mode = mode;
    app->notice = RB_NOTICE_NONE;
    app->ts_count = 0;
    app->browse_offset = 0;
    app->asr_error = false;
    app->sess_started = false;
    app->done_fail = -1;
}

// 上键按下：待机时开始录音笔记（熄屏也开始，按住是明确的意图；误触由最短按住时长兜底）。
static uint32_t ptt_down(rb_app_t *app, bool was_off, uint32_t now)
{
    // 每次上键单击之前必有一次按下：在这里重置吞单击标记，保证它只作用于本次按键。
    app->swallow_up_click = false;
    app->ptt_held = false;
    if (app->view != RB_VIEW_IDLE) {
        if (!was_off) return 0;
        // 熄屏时这次按下只负责点亮：吞掉随后的单击，保持“首次按键只点亮”。
        app->swallow_up_click = true;
        return RB_ACT_RENDER;
    }
    app->swallow_up_click = true;   // 待机页上键单击本无作用；吞掉它更不会误触结束页等
    if (!app->wifi_up) return show_notice(app, RB_NOTICE_WIFI_OFFLINE, now);
    reset_session_state(app, RB_MODE_NOTE);
    app->ptt_held = true;
    app->link_ok = false;           // 还没连上服务器：录音页显示“连接中”，计时照走
    app->rec_start_ms = now;
    app->rec_end_ms = now;
    app->deadline = now + RB_CONNECT_GUARD_MS;   // 一直等不到 started 的兜底
    return set_view(app, RB_VIEW_RECORDING, now) | RB_ACT_SESSION_START | RB_ACT_CLEAR_TRANSCRIPT;
}

// 上键松开：结束笔记（太短则视为误触丢弃）。
static uint32_t ptt_up(rb_app_t *app, uint32_t now)
{
    if (!app->ptt_held) return 0;
    app->ptt_held = false;
    app->swallow_up_click = true;   // 短按时随后还会到一个单击，不能让它操作下一页
    if (app->mode != RB_MODE_NOTE || app->view != RB_VIEW_RECORDING) return 0;
    if (now - app->rec_start_ms < RB_PTT_MIN_MS) {
        uint32_t act = set_view(app, home_view(app), now) | RB_ACT_SESSION_CANCEL;
        return act | show_notice(app, RB_NOTICE_PTT_SHORT, now);
    }
    app->rec_end_ms = now;
    app->browse_offset = 0;
    app->deadline = now + RB_SAVE_GUARD_MS;
    return set_view(app, RB_VIEW_SAVING, now) | RB_ACT_SESSION_STOP;
}

static uint32_t handle_key(rb_app_t *app, rb_ev_type_t key, uint32_t now)
{
    // 熄屏时第一次按键只负责点亮，不触发任何操作（避免误开始录音）。
    // 例外：待机页按住上直接开始录音笔记（ptt_down）。
    // 浅睡被 GPIO 唤醒时屏幕已先点亮：随后到达的第一个按键事件属于那次唤醒按键，同样只点亮。
    bool was_off = app->backlight == 0;
    if (app->wake_guard) {
        if (!reached(now, app->wake_guard_deadline)) was_off = true;
        app->wake_guard = false;
    }
    app->last_activity_ms = now;
    app->manual_off = false;
    uint32_t act = update_backlight(app, now);
    if (key == RB_EV_PTT_DOWN) return act | ptt_down(app, was_off, now);
    if (key == RB_EV_PTT_UP) return act | ptt_up(app, now);
    if (key == RB_EV_KEY_UP && app->swallow_up_click) {
        app->swallow_up_click = false;
        return act | (was_off ? RB_ACT_RENDER : 0);
    }
    if (was_off) return act | RB_ACT_RENDER;
    // 笔记录音/保存中其他按键一律忽略（结束只靠松开上键）。
    if (app->mode == RB_MODE_NOTE && rb_app_in_session(app)) return act;

    switch (app->view) {
    case RB_VIEW_IDLE:
        if (key == RB_EV_KEY_OK_LONG) return act | set_view(app, RB_VIEW_SETTINGS, now);
        if (key == RB_EV_KEY_DOWN) {
            // 手动休眠：立即熄屏进浅睡，深睡倒计时从此刻起算（light_since_ms）。
            app->manual_off = true;
            return act | update_backlight(app, now);
        }
        if (key != RB_EV_KEY_OK) return act;
        if (!app->wifi_up) return act | show_notice(app, RB_NOTICE_WIFI_OFFLINE, now);
        reset_session_state(app, RB_MODE_MEETING);
        app->ptt_held = false;
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
            // 手机端配网有进展（提交表单/加入成功等）也算活动，重新计配网页深睡倒计时。
            app->last_activity_ms = now;
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
        if (app->mode == RB_MODE_NOTE) {
            // 笔记从按下就在录音（计时不重置）；started 只表示连上了，缓冲的音频开始补发。
            if (app->sess_started) return 0;
            app->sess_started = true;
            app->link_ok = true;
            // 连上之前就已松开：保存兜底从 started 起算（会话任务此后还有补发 ≤20 s +
            // 等 saved 25 s，加上之前的连接阶段会超过从松开起算的 RB_SAVE_GUARD_MS）。
            if (app->view == RB_VIEW_SAVING) app->deadline = now + RB_SAVE_GUARD_MS;
            return RB_ACT_RENDER;
        }
        if (app->view != RB_VIEW_CONNECTING) return 0;
        app->sess_started = true;
        app->rec_start_ms = now;
        app->rec_end_ms = now;
        app->link_ok = true;
        return set_view(app, RB_VIEW_RECORDING, now);

    case RB_EV_SESS_FAILED: {
        if (app->view == RB_VIEW_CONNECTING) {
            uint32_t act = set_view(app, home_view(app), now);
            return act | show_notice(app, notice_for_fail(ev->a), now);
        }
        // 笔记在连接阶段失败：用户已经说了话，进结束页明确告诉他没保存。
        uint32_t act = enter_done(app, RB_DONE_FAILED, now);
        app->done_fail = (int8_t)ev->a;
        return act;
    }

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

    case RB_EV_SESS_ENDED: {
        if (app->view == RB_VIEW_CONNECTING) {
            uint32_t act = set_view(app, home_view(app), now);
            return act | show_notice(app, notice_for_fail(ev->a), now);
        }
        rb_done_kind_t kind = RB_DONE_FAILED;
        if (ev->a == RB_FAIL_OVERFLOW) kind = RB_DONE_INTERRUPTED;
        else if (ev->a == RB_FAIL_SAVE_TIMEOUT) kind = RB_DONE_UNCONFIRMED;
        uint32_t act = enter_done(app, kind, now);
        app->done_fail = (int8_t)ev->a;
        return act;
    }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 电源 / 电量
// ---------------------------------------------------------------------------

// 低电保护触发：会话中先按正常流程结束并保存（保存完成或超时后由 TICK 进入深睡），
// 连接中直接取消；不在会话中立即深睡。
static uint32_t battery_cut(rb_app_t *app, uint32_t now)
{
    uint32_t act = 0;
    app->batt_cut = true;
    switch (app->view) {
    case RB_VIEW_CONNECTING:
        act |= set_view(app, home_view(app), now) | RB_ACT_SESSION_CANCEL;
        break;
    case RB_VIEW_RECORDING:
    case RB_VIEW_CONFIRM_STOP:
        if (app->mode == RB_MODE_NOTE) {
            app->ptt_held = false;
            app->swallow_up_click = true;
            if (now - app->rec_start_ms < RB_PTT_MIN_MS) {   // 太短：同误触，丢弃
                act |= set_view(app, home_view(app), now) | RB_ACT_SESSION_CANCEL;
                break;
            }
        }
        app->rec_end_ms = now;
        app->browse_offset = 0;
        app->deadline = now + RB_SAVE_GUARD_MS;
        act |= set_view(app, RB_VIEW_SAVING, now) | RB_ACT_SESSION_STOP;
        act |= show_notice(app, RB_NOTICE_BATT_CUT, now);
        break;
    case RB_VIEW_SAVING:
        act |= show_notice(app, RB_NOTICE_BATT_CUT, now);
        break;
    default:
        break;
    }
    if (!rb_app_in_session(app)) act |= enter_deep(app, RB_SLEEP_LOW_BATT, now);
    return act;
}

static uint32_t handle_battery(rb_app_t *app, int32_t soc, int32_t mv, uint32_t now)
{
    app->batt_soc = (int8_t)(soc >= 0 && soc <= 100 ? soc : -1);
    app->batt_mv = (int16_t)(mv > 0 && mv < 32768 ? mv : -1);
    bool cut = (app->batt_soc >= 0 && app->batt_soc <= RB_BATT_CUT_SOC) ||
               (app->batt_mv > 0 && app->batt_mv < RB_BATT_CUT_MV);
    if (!cut) app->batt_cut_hits = 0;
    else if (app->batt_cut_hits < UINT8_MAX) app->batt_cut_hits++;

    uint32_t act = 0;
    if (app->batt_soc >= RB_BATT_WARN_RESET) {
        app->batt_warned = false;               // 充上电了，下次再低时重新提示
        app->batt_warn_pending = false;
    } else if (app->batt_soc >= 0 && app->batt_soc <= RB_BATT_WARN_SOC && !app->batt_warned) {
        app->batt_warned = true;
        // 浅睡中不为一条提示点亮屏幕：等用户唤醒后再显示。
        if (app->power == RB_POWER_ACTIVE) act |= show_notice(app, RB_NOTICE_BATT_LOW, now);
        else app->batt_warn_pending = true;
    }
    if (!app->batt_cut && app->batt_cut_hits >= RB_BATT_CONFIRM) act |= battery_cut(app, now);
    return act;
}

// 浅睡中按键脚 GPIO 唤醒：点亮回到活动态；随后到达的第一个按键事件只算点亮。
static uint32_t handle_wake_key(rb_app_t *app, uint32_t now)
{
    if (app->power != RB_POWER_LIGHT) return 0;   // 迟到的唤醒：已被按键事件点亮
    app->manual_off = false;
    app->last_activity_ms = now;
    app->wake_guard = true;
    app->wake_guard_deadline = now + RB_WAKE_GUARD_MS;
    return update_backlight(app, now) | RB_ACT_RENDER;
}

// 深睡被推迟（按键按住）：回到浅睡/活动态，重新开始计时；按键事件随后照常处理。
static uint32_t handle_deep_abort(rb_app_t *app, uint32_t now)
{
    if (app->power != RB_POWER_DEEP) return 0;
    app->power = app->backlight == 0 ? RB_POWER_LIGHT : RB_POWER_ACTIVE;
    app->light_since_ms = now;
    // 浅睡中不改活动时刻（否则会把屏幕点亮）；配网页等活动态重新计时。
    if (app->power == RB_POWER_ACTIVE) app->last_activity_ms = now;
    app->deep_reason = RB_SLEEP_NONE;
    if (app->batt_cut) {
        app->batt_cut = false;                 // 下一次读数仍低时再触发
        app->batt_cut_hits = 0;
    }
    return RB_ACT_RENDER;
}

// TICK 中的深睡判定（在背光/浅睡更新之后）。
static uint32_t check_deep(rb_app_t *app, uint32_t now)
{
    if (app->power == RB_POWER_DEEP || rb_app_in_session(app)) return 0;
    if (app->batt_cut) return enter_deep(app, RB_SLEEP_LOW_BATT, now);
    if (app->power == RB_POWER_LIGHT && now - app->light_since_ms >= RB_DEEP_IDLE_MS) {
        return enter_deep(app, RB_SLEEP_IDLE, now);
    }
    if ((app->view == RB_VIEW_PROVISIONING || app->view == RB_VIEW_PAIRING) &&
        app->phase != RB_NET_PROV_JOINING && now - app->last_activity_ms >= RB_PROV_DEEP_MS) {
        return enter_deep(app, RB_SLEEP_PROV, now);
    }
    return 0;
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
        if (app->mode == RB_MODE_NOTE && !app->sess_started && reached(now, app->deadline)) {
            // 兜底：会话任务自身的连接/started 超时更短，正常不会走到这里。
            act |= enter_done(app, RB_DONE_FAILED, now) | RB_ACT_SESSION_CANCEL;
            app->done_fail = RB_FAIL_UNREACHABLE;
            break;
        }
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
    act |= update_backlight(app, now);
    if (app->batt_warn_pending && app->power == RB_POWER_ACTIVE) {
        app->batt_warn_pending = false;
        act |= show_notice(app, RB_NOTICE_BATT_LOW, now);
    }
    return act | check_deep(app, now);
}

uint32_t rb_app_handle(rb_app_t *app, const rb_event_t *ev, uint32_t now)
{
    if (!app || !ev) return 0;
    // 已决定深睡：等待 app 任务执行，期间只接受“放弃深睡”。
    if (app->power == RB_POWER_DEEP) {
        return ev->type == RB_EV_DEEP_ABORT ? handle_deep_abort(app, now) : 0;
    }
    uint32_t act;
    switch (ev->type) {
    case RB_EV_BATTERY:
        return handle_battery(app, ev->a, ev->c, now);
    case RB_EV_WAKE_KEY:
        return handle_wake_key(app, now);
    case RB_EV_DEEP_ABORT:
        return 0;
    case RB_EV_KEY_UP:
    case RB_EV_KEY_DOWN:
    case RB_EV_KEY_OK:
    case RB_EV_KEY_OK_LONG:
    case RB_EV_PTT_DOWN:
    case RB_EV_PTT_UP:
        return handle_key(app, ev->type, now);
    case RB_EV_TICK:
        return handle_tick(app, now);
    default:
        act = handle_net(app, ev, now);
        return act | update_backlight(app, now);
    }
}
