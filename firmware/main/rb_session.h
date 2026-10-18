// main/rb_session.h —— 录音会话任务：WebSocket 连接、start/stop/resume、音频帧上传、断线续传。
//
// 协议见 rb_proto.h（design.md §3 + server/README.md）。会话结果通过 rb_bus 上报：
//   RB_EV_SESS_STARTED / FAILED / LINK / ASR_ERROR / SAVED / ENDED，以及 partial/final 文本。
// 失败降级（PRD R6）：
//   - 开始时连不上/无 started：FAILED(UNREACHABLE)，不进入录音；
//   - 令牌被拒（error auth）：FAILED/ENDED(AUTH)，并请求网络任务清令牌重新配对；
//   - 录音中断网：继续采音进环形缓冲，LINK(0)；websocket 自动重连后发 resume，
//     从服务端给出的 next_seq 重发，LINK(1)；
//   - 缓存溢出（断网超过 ~10 s）：停止采音，ENDED(OVERFLOW)，服务端把已收部分存为 interrupted；
//   - 发 stop 后 15 s 无 saved：ENDED(SAVE_TIMEOUT)。
// 下面的接口只置命令位并返回，可在 app 任务中调用。
#pragma once

#include "esp_err.h"

#include <stdint.h>

esp_err_t rb_session_init(void);

void rb_session_start(void);   // 开始新录音
void rb_session_stop(void);    // 正常结束：停采音、发完缓存、发 stop、等 saved
void rb_session_cancel(void);  // 立即放弃（app 兜底超时/用户取消）

// 尚未上传的音频时长（毫秒），供录音页显示“断网缓存 Ns”。
uint32_t rb_session_backlog_ms(void);
