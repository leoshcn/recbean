# 设计：录音笔记 / 会议录音双模式

## 1. 总体

一段笔记 = 一条 `recording`（`kind='note'`，有自己的 WAV 和句子）。「当日笔记」是**展示层按天聚合**，不把多段音频拼成一个文件——这样续传、去重、WAV 落盘、逐句编辑全部复用现有逻辑，删除单段也简单。

```text
待机 ──按住「上」──► 笔记录音(立即采音, 后台连 ws) ──松开──► 保存中 ──saved──► 完成(显示本段文字) ──3 s──► 待机
     └─单击「确定」─► 连接中 ──started──► 会议录音 ──确定+确认──► 保存中 ──► 完成        （不变）
```

## 2. 协议（向后兼容）

- 设备 → 服务 `start` 增加可选字段 `"mode":"note"|"meeting"`；缺省或未知值按 `meeting`。
- 其它消息不变。笔记模式下服务端仍会推 partial/final，设备照常显示。
- `rb_proto_build_start(buf, cap, fw, mode)` 增加参数；`rb_proto.h` 头注释与 `server/README.md`、`device_ws.py` 模块 docstring 同步更新。

## 3. 固件

### 3.1 BSP 按键
- `bsp_btn_ev_t` 末尾新增 `BSP_BTN_RELEASE`，注册 `BUTTON_PRESS_UP` 回调。追加在枚举末尾，不改变已有值；同步 `tests/test_bsp_button.c` 的桩。

### 3.2 事件映射（`main.c:on_key`）
- 「上」`PRESS` → `RB_EV_PTT_DOWN`；「上」`RELEASE` → `RB_EV_PTT_UP`；「上」`CLICK` 仍发 `RB_EV_KEY_UP`（会议翻看用）。
- 新事件追加到 `rb_ev_type_t`，`rb_app_handle` 把它们当按键处理。

### 3.3 状态机（`rb_app.c`）
- `rb_app_t` 新增 `mode`（`RB_MODE_MEETING`/`RB_MODE_NOTE`）与 `ptt_held`。
- 待机 + `PTT_DOWN`：Wi-Fi 离线 → 提示；否则 `mode=NOTE`，`rec_start_ms=now`，直接进 `RB_VIEW_RECORDING`，动作 `SESSION_START|CLEAR_TRANSCRIPT`。**熄屏时也执行**（只对 PTT 放开“首次按键只点亮”规则）。
- 笔记录音中：`PTT_UP` → 时长 < `RB_PTT_MIN_MS`(600) 则 `SESSION_CANCEL`、回待机、提示 `RB_NOTICE_PTT_SHORT`；否则进 `SAVING` + `SESSION_STOP`。其他按键（含随后到达的 UP CLICK）忽略。
- 笔记模式不进入 `CONNECTING` / `CONFIRM_STOP`；`SESS_STARTED` 在笔记 RECORDING 中只置 `link_ok`。连接阶段失败（`SESS_FAILED`）→ 进 DONE(FAILED)（因为用户已经说了话，要明确告诉他没保存）。
- `PTT_UP` 丢失兜底：笔记录音中每个 TICK 检查 `ptt_held`，若 BSP 已报松开但事件丢了，以 `bsp` 侧状态为准——实现上由 `main.c` 在 TICK 时读取按键电平补发 `PTT_UP`（`bsp_button_read_mv` 不在「上」键窗口时）。
- DONE 视图在笔记模式显示「已记入今日笔记」+ 本段定稿文字（转写缓冲里的句子拼接，截断到屏幕能放下的长度）。
- 会议模式全部路径不变（`mode=MEETING`）。

### 3.4 会话任务（`rb_session.c`）
- `rb_session_start(mode)`：笔记模式**先 `rb_audio_start()` 再连 ws**，帧从 seq 0 进环形缓冲；started 后按现有逻辑从缓冲最旧帧开始上传。
- 连接/等 started 阶段收到 `BIT_CMD_STOP`：记住 `stopping`，不中断连接；started 后把缓冲发完再发 stop、等 saved（复用录音阶段的 stopping 分支）。当前这两个等待循环只看 CANCEL，需要把 STOP 位保留到录音循环。
- 连接阶段缓冲溢出 → 上报 FAILED(UNREACHABLE)，`rb_audio_stop()`。
- `start` 消息带 mode。

### 3.5 UI（`rb_ui.c`）
- 待机页三行提示：「按 确定 会议录音」「按住 上 记笔记」「长按 确定 设置」（布局需在 240 宽屏上实测）。
- 录音页按 `mode` 切换标题/底部提示：笔记显示「正在听… 松开结束」，未连上服务器时显示「连接中」小标记但计时照走。
- 完成页按 `mode` 切换文案。

## 4. 服务端

- **迁移**：`Store.__init__` 中检查 `PRAGMA table_info(recording)`，无 `kind` 列则 `ALTER TABLE recording ADD COLUMN kind TEXT NOT NULL DEFAULT 'meeting'`；新建表 SCHEMA 也带 `kind` 列，加索引 `(kind, started_at)`。
- `create_recording(..., kind)`；`list_recordings(kind=None)`。
- `Gateway.start`：解析 `mode` → `kind`；标题前缀「会议」/「笔记」；`asr_factory(kind)`，笔记时 `enable_speaker_info=False`（并去掉 `ssd_version`）。笔记段 `SpeakerMap` 固定映射到 1。`UnconfiguredAsrEngine`/测试假引擎的工厂签名一起改。
- 新接口：
  - `GET /api/recordings?kind=meeting|note`
  - `GET /api/notes/days` → `[{date:"2026-10-18", count, duration_ms, first_text, live}]`，日期按 `settings.timezone`（环境变量 `TZ_NAME`，默认 `Asia/Shanghai`）计算（Python 侧分组，数据量小）。
  - `GET /api/notes/days/{date}` → `{date, segments:[{id, started_at, duration_ms, status, live, sentences:[...]}]}`，按 `started_at` 升序。
  - `GET /api/notes/days/{date}/export.txt` → 标题「笔记 YYYY-MM-DD」，每段 `HH:MM 文字`（段内句子直接拼接）。
- 单段编辑/删除/音频复用 `/api/recordings/{id}/...`。

## 4.1 热词

- **表**：`hotword(id INTEGER PK, word TEXT NOT NULL UNIQUE, position INTEGER NOT NULL, created_at INTEGER NOT NULL)`，排序用 `ORDER BY position, id`。
- **额度**：新模块 `app/hotwords.py`，提供 `estimate_tokens(word)` 和 `select_active(words, budget)`。按列表顺序累加，第一个超额的词及其后面的词都不生效（不跳过它去塞后面更短的词，用户看到的顺序就是优先级）。估算规则先保守地取：每个汉字 1 token，每段连续的字母或数字 1 token，每个词再加 1 个分隔开销；实现时用真实接口校准（implement.md 6b）。`Settings.hotword_token_budget` 从 `HOTWORD_TOKEN_BUDGET` 读取，默认 100。
- **接口**：
  - `GET /api/hotwords` → `{budget, used, items:[{id, word, tokens, active}]}`
  - `POST /api/hotwords` `{words:[...]}` → 批量添加，跳过重复词，返回新增数和跳过数
  - `PATCH /api/hotwords/{id}` `{word}`；`DELETE /api/hotwords/{id}`
  - `PUT /api/hotwords/order` `{ids:[...]}`，必须是当前全部 id 的一个排列
- **接入 ASR**：`Gateway.start` 建会话时读取生效热词，传给 `asr_factory(kind, hotwords)`。`build_request_params(..., hotwords)` 在列表非空时写入 `request["corpus"] = {"context": json.dumps({"hotwords":[{"word":w}, ...]}, ensure_ascii=False)}`。日志只记录热词个数，不记录内容。

## 5. 网页

- 新增 `/hotwords`（Hotwords.vue），导航放在「会议」和「设备配对」之间。顶部是额度条、多行输入框和「添加」按钮；列表每行显示词、token 数、生效/未生效标记，带上移、下移、编辑、删除。
- 路由：`/` → 重定向 `/notes`；`/notes`（NoteDays.vue）、`/notes/:date`（NoteDay.vue）、`/meetings`（原 RecordingList.vue，带 `kind=meeting`）、`/r/:id`（不变，返回链接改到 `/meetings`）、`/pairing`。
- NoteDay.vue：段卡片列表；每段一个小播放按钮（共享一个 `<audio>`，切换 src）；句子点「编辑」走 `updateSentence`；「删除本段」走 `deleteRecording`；「复制全文」用 `navigator.clipboard`（局域网 http 下可能不可用，退回选中文本 textarea）；有 live 段时 3 s 轮询。
- 空状态文案：笔记「还没有笔记。在设备待机页按住『上』键说话。」；会议「还没有会议录音。在设备上单击『确定』开始。」

## 6. 兼容与回滚

- 新服务端 + 老固件：start 无 mode → meeting，行为不变。
- 新固件 + 老服务端：老服务端忽略 mode，笔记被当成会议存下来（不丢数据）。所以**先部署服务端再烧固件**。
- 回滚：服务端 `./deploy.sh --rollback`；`kind` 列留在库里对老代码无害（老 INSERT 不写该列，取默认值）。

## 7. 风险

- 热词额度的真实上限和超额时的行为未知。如果超额会让整次识别失败，估算就必须偏保守；实测结果写进 `.trellis/spec/backend/doubao-asr-and-deploy.md`。

- 按键松开事件依赖 ADC 轮询，三键共用一个 ADC 引脚；按住「上」时误判为别的键的概率需真机验证。
- 笔记先采音后连接：服务端不可达时最多缓冲 8 s，超时后丢弃并提示；这段时间内存占用与会议录音相同（环形缓冲已静态分配）。
- 待机页三行中文提示在小屏上的排版需要真机看效果。
