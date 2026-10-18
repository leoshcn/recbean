// main/rb_app.h —— 录音豆应用状态机（纯 C，无 ESP-IDF/LVGL 依赖，可主机测试）。
//
// 输入：按键事件、网络/会话任务上报的事件、周期 TICK（都带单调毫秒时间 now）。
// 输出：rb_app_t 中的视图状态 + rb_app_handle() 返回的动作位（由 app 任务执行：
// 启停录音会话、清屏、重新配网、调背光、重绘）。
//
// 视图（design.md §7）：待机 / 连接中 / 录音 / 确认结束 / 保存中 / 结束 / 设置 / 配网 / 配对。
// 两种录音模式（10-18-dual-mode-notes-meeting）：
//   会议录音  待机单击确定 → 连接中 → started 后录音 → 确定 + 确认结束（下面 D4 的规则，不变）
//   录音笔记  待机按住上（PTT_DOWN）→ 立即进入录音页（会话任务先采音、后台连服务器）
//            松开（PTT_UP）：按住 < RB_PTT_MIN_MS 视为误触，取消并提示“按住说话”；
//            否则进保存中并 SESSION_STOP。笔记录音中其他按键一律忽略；不进连接中/确认页。
//            熄屏时按住上也直接开始（并点亮）；Wi-Fi 未连接时只提示。
//            连接阶段失败 → 结束页（未保存）；保存成功 → 结束页显示本段文字，RB_NOTE_DONE_MS 后回待机。
//   上键事件顺序：PTT_DOWN（按下）→ PTT_UP（松开）→ KEY_UP（单击，仅短按且滞后于松开）。
//   一次笔记按键（或熄屏时只负责点亮的按下）之后到达的那个 KEY_UP 单击会被吞掉。
// 按键（PRD D4）：
//   待机   单击确定 = 开始会议录音；按住上 = 录音笔记；长按确定 = 设置
//   录音   单击确定 = 进入“再按确定结束”确认（3 s 内再按确定结束，其他键或超时继续）
//          上/下 = 往旧/新翻看，最后一次按键 5 s 后自动回到最新
//   结束   任意键或 10 s 后回待机
//   设置   上/下 选择，确定 进入；“重新配网”需再按一次确定确认；长按确定返回
//   配网/配对  长按确定 = 设置（可查看设备信息或重新配网）
// 时间比较一律用 (int32_t)(a - b) 判断先后，可容忍 uint32 毫秒回绕（约 49 天）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RB_VIEW_IDLE = 0,
    RB_VIEW_CONNECTING,
    RB_VIEW_RECORDING,
    RB_VIEW_CONFIRM_STOP,
    RB_VIEW_SAVING,
    RB_VIEW_DONE,
    RB_VIEW_SETTINGS,
    RB_VIEW_PROVISIONING,
    RB_VIEW_PAIRING,
} rb_view_t;

// 网络任务的配网阶段。
typedef enum {
    RB_NET_PROV_AP = 0,     // 热点 RecBean-Setup 已开，等待手机提交表单
    RB_NET_PROV_JOINING,    // 已提交表单，正在用新凭据连接 Wi-Fi
    RB_NET_PAIRING,         // Wi-Fi 已连，等待 Web 端确认 4 位配对码
    RB_NET_READY,           // 已配网且有令牌，可以录音
} rb_net_phase_t;

// 会话失败/异常结束原因（RB_EV_SESS_FAILED / RB_EV_SESS_ENDED 的 a 字段）。
typedef enum {
    RB_FAIL_UNREACHABLE = 0, // 连不上服务器或握手/started 超时
    RB_FAIL_AUTH,            // 401/403 或 error.code=auth
    RB_FAIL_BUSY,            // error.code=busy
    RB_FAIL_AUDIO,           // 麦克风/codec 打开失败
    RB_FAIL_OVERFLOW,        // 断网超过缓存时长，本次录音被迫结束（服务端记 interrupted）
    RB_FAIL_SAVE_TIMEOUT,    // 发出 stop 后迟迟没有 saved
    RB_FAIL_SERVER,          // 其他服务端错误/断开且无法续传
} rb_fail_t;

typedef enum {
    RB_EV_KEY_UP = 0,
    RB_EV_KEY_DOWN,
    RB_EV_KEY_OK,
    RB_EV_KEY_OK_LONG,
    RB_EV_TICK,
    RB_EV_NET_PHASE,        // a = rb_net_phase_t
    RB_EV_WIFI,             // a = 1 已拿到 IP / 0 断开
    RB_EV_SERVER,           // a = 1 探测在线 / 0 离线
    RB_EV_SESS_STARTED,     // 服务端回 started，录音正式开始
    RB_EV_SESS_FAILED,      // a = rb_fail_t；尚未开始录音就失败
    RB_EV_SESS_LINK,        // a = 1 续传恢复 / 0 链路中断（音频继续缓存）
    RB_EV_SESS_ASR_ERROR,   // 服务端报 asr_error：录音继续，只提示
    RB_EV_SESS_SAVED,       // b = duration_ms, c = sentences
    RB_EV_SESS_ENDED,       // a = rb_fail_t；录音开始后异常结束
    RB_EV_TRANSCRIPT,       // c = 当前保留的定稿句数（供翻看夹取）
    RB_EV_PTT_DOWN,         // 上键按下（按住说话开始）
    RB_EV_PTT_UP,           // 上键松开（按住说话结束）
} rb_ev_type_t;

// 录音模式。
typedef enum {
    RB_MODE_MEETING = 0,    // 会议录音：区分说话人，确定 + 确认结束
    RB_MODE_NOTE,           // 录音笔记：按住上说话，松开结束
} rb_mode_t;

typedef struct {
    rb_ev_type_t type;
    int32_t a;
    int64_t b;
    int32_t c;
} rb_event_t;

// 动作位（rb_app_handle 返回值）。
#define RB_ACT_RENDER           (1u << 0)  // 视图状态变化，需要重绘
#define RB_ACT_SESSION_START    (1u << 1)  // 启动录音会话（连接服务器、发 start）
#define RB_ACT_SESSION_STOP     (1u << 2)  // 正常结束：停止采音、发完缓存、发 stop、等 saved
#define RB_ACT_SESSION_CANCEL   (1u << 3)  // 放弃会话（超时/用户取消），立即断开
#define RB_ACT_REPROVISION      (1u << 4)  // 清除 Wi-Fi/服务器/令牌并重新打开配网热点
#define RB_ACT_CLEAR_TRANSCRIPT (1u << 5)  // 清空转写显示缓冲
#define RB_ACT_BACKLIGHT        (1u << 6)  // backlight 字段变化，需要下发

typedef enum {
    RB_NOTICE_NONE = 0,
    RB_NOTICE_WIFI_OFFLINE,     // “Wi-Fi 未连接”
    RB_NOTICE_SERVER_OFFLINE,   // “服务器未连接”
    RB_NOTICE_AUTH,             // “设备未授权，请重新配网”
    RB_NOTICE_BUSY,             // “服务器忙，请稍后再试”
    RB_NOTICE_AUDIO,            // “麦克风启动失败”
    RB_NOTICE_ASR_ERROR,        // “识别出错，录音仍在保存”
    RB_NOTICE_SERVER_ERROR,     // “服务器出错”
    RB_NOTICE_PTT_SHORT,        // “按住 上 键说话”（笔记按得太短，已丢弃）
} rb_notice_t;

typedef enum {
    RB_DONE_SAVED = 0,          // 收到 saved
    RB_DONE_INTERRUPTED,        // 断网超时，已上传部分由服务端存为 interrupted
    RB_DONE_UNCONFIRMED,        // 已发 stop 但没等到 saved
    RB_DONE_FAILED,             // 其他异常结束
} rb_done_kind_t;

// 设置页条目。
typedef enum {
    RB_SET_REPROVISION = 0,
    RB_SET_SERVER,
    RB_SET_INFO,
    RB_SET_BACK,
    RB_SET_COUNT,
} rb_settings_item_t;

// 时长参数（毫秒）。
#define RB_CONFIRM_MS        3000   // “再按确定结束”窗口
#define RB_BROWSE_RETURN_MS  5000   // 翻看后自动回到最新
#define RB_CONNECT_GUARD_MS  12000  // 连接中兜底超时（会话任务自身超时更短）
#define RB_SAVE_GUARD_MS     50000  // 等 saved 的兜底超时（会话任务自身：补发 ≤20 s + 等 saved 25 s）
#define RB_DONE_MS           10000  // 结束页停留
#define RB_NOTE_DONE_MS      3000   // 笔记结束页停留
#define RB_PTT_MIN_MS        600    // 笔记按住短于此视为误触（会话任务据此推迟发 start，见 rb_session.c）
#define RB_NOTICE_MS         4000   // 提示条显示时长
#define RB_REPROV_ARM_MS     3000   // 重新配网二次确认窗口
#define RB_DIM_IDLE_MS       30000  // 待机类页面调暗
#define RB_OFF_IDLE_MS       120000 // 待机类页面熄屏
#define RB_DIM_BUSY_MS       60000  // 录音/配网页调暗（不熄屏）
#define RB_BACKLIGHT_FULL    100
#define RB_BACKLIGHT_DIM     20

typedef struct {
    rb_view_t view;
    rb_net_phase_t phase;
    bool wifi_up;
    int8_t server_state;        // -1 未知, 0 离线, 1 在线
    // 录音
    rb_mode_t mode;             // 当前/最近一次录音的模式
    bool ptt_held;              // 笔记：上键仍按着（等待 PTT_UP）
    bool swallow_up_click;      // 吞掉下一次 KEY_UP 单击（属于刚处理过的那次上键按下）
    bool sess_started;          // 本次会话已收到 started（笔记：false = 还在连服务器）
    uint32_t rec_start_ms;      // 会议：收到 started 的时刻；笔记：按下上键的时刻
    uint32_t rec_end_ms;        // 用户确认结束的时刻（计时停止）
    bool link_ok;               // 录音中链路是否正常（false = 断网缓存中）
    bool asr_error;             // 本次录音收到过 asr_error
    uint32_t ts_count;          // 转写缓冲中的定稿句数（RB_EV_TRANSCRIPT 更新）
    uint32_t browse_offset;     // 0 = 跟随最新；n = 往回翻了 n 句
    uint32_t browse_deadline;   // 翻看自动返回时刻（browse_offset > 0 时有效）
    uint32_t deadline;          // 当前视图的超时时刻（连接/确认/保存/结束页）
    // 结束页
    rb_done_kind_t done_kind;
    int64_t done_duration_ms;
    int32_t done_sentences;     // -1 = 未知
    int8_t done_fail;           // 异常结束原因 rb_fail_t；-1 = 无
    // 设置
    uint8_t settings_sel;       // rb_settings_item_t
    int8_t settings_detail;     // -1 列表；RB_SET_SERVER / RB_SET_INFO 详情页
    bool reprov_armed;          // 已按过一次“重新配网”，等待确认
    uint32_t reprov_deadline;
    // 提示条
    rb_notice_t notice;
    uint32_t notice_deadline;
    // 背光
    uint32_t last_activity_ms;  // 最近一次按键或视图切换
    uint8_t backlight;          // 当前背光 0..100
} rb_app_t;

// 初始化。phase 来自 NVS 中的配网状态；now 为当前毫秒。
void rb_app_init(rb_app_t *app, rb_net_phase_t phase, uint32_t now);

// 处理一个事件，返回 RB_ACT_* 位。纯函数式：只改 app，不做 I/O。
uint32_t rb_app_handle(rb_app_t *app, const rb_event_t *ev, uint32_t now);

// 录音计时（毫秒）：录音中为 now - rec_start；确认结束后冻结。
uint32_t rb_app_elapsed_ms(const rb_app_t *app, uint32_t now);

// 是否属于录音会话进行中的视图（连接中/录音/确认结束/保存中）。
bool rb_app_in_session(const rb_app_t *app);

// 笔记录音中、仍在等上键松开：app 任务据此在 TICK 时读按键电平，补发丢失的 PTT_UP。
bool rb_app_ptt_waiting_release(const rb_app_t *app);
