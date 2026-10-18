# 实施清单

顺序：服务端 → 网页 → 固件 → 构建 → 部署服务端 → 真机验收（固件烧录需用户确认镜像哈希）。

## 服务端（server/）
1. `store.py`：SCHEMA 加 `kind` 列与索引；启动迁移；`create_recording(kind)`、`list_recordings(kind)`、按天查询辅助。
2. `doubao_asr.py`：`build_request_params(..., speaker_info=True)`；`engine_factory_from_settings` 工厂接收 `kind`。`asr.py` 未配置引擎同步签名。
3. `device_ws.py`：解析 `mode`；标题前缀；笔记段说话人固定为 1；模块 docstring 协议更新。
4. `api.py`：`kind` 过滤、`_public` 输出 `kind`；`/api/notes/days`、`/api/notes/days/{date}`、导出 TXT。
4b. 热词：`store.py` 增加 hotword 表和增删改查；新 `hotwords.py`（估算与选取）；`config.py` 读 `HOTWORD_TOKEN_BUDGET`；`models.py` 请求体；`api.py` 增加 `/api/hotwords*`；`device_ws.py` 开录时取生效热词传给 ASR 工厂；`doubao_asr.py` 写 `corpus.context`。
5. 测试：`tests/test_hotwords.py`（估算、额度截断、增删改查、排序校验、重复词）；`tests/test_gateway.py`（mode 缺省/note、标题、说话人）、`tests/test_api.py`（迁移、kind 过滤、按天聚合与时区边界、导出格式）、`tests/test_doubao.py`（请求参数）。
6. `server/README.md` 协议与接口说明；`.env.example` 加 `HOTWORD_TOKEN_BUDGET`。
6b. **真实接口实测**：仿照 `server/.localdata` 里的探测脚本，带不同数量的热词调用 `bigmodel_async`，确定 token 上限和超额时的行为，据此校准 `estimate_tokens`；再用一个专有名词对比配置热词前后的识别结果（AC12）。

验证：`cd server && .venv/Scripts/python -m pytest`

## 网页（server/web/）
7. `main.js` 路由；`App.vue` 导航；`api.js` 新接口；`util.js` 日期格式。
8b. 新 `views/Hotwords.vue`，`api.js` 加热词接口。
8. 新 `views/NoteDays.vue`、`views/NoteDay.vue`；`RecordingList.vue` 改为会议列表；`RecordingDetail.vue` 返回链接。

验证：`cd server/web && npm run build`

## 固件（firmware/）
9. BSP：`bsp_button.h/.c` 增加 `BSP_BTN_RELEASE`；`tests/test_bsp_button.c`。
10. `rb_app.h/.c`：mode、PTT 事件、`RB_PTT_MIN_MS`、`RB_NOTICE_PTT_SHORT`、笔记分支；`tests/test_rb_app.c` 覆盖：PTT 开始/熄屏开始/短按取消/松开保存/离线提示/笔记中其他键忽略/连接失败进 DONE/会议路径回归。
11. `rb_proto.h/.c`：`build_start` 带 mode；`tests/test_rb_proto.c`。
12. `rb_session.h/.c`：`rb_session_start(mode)`、笔记先采音、连接阶段保留 STOP。
13. `main.c`：按键映射、动作分发传 mode、PTT_UP 丢失兜底。
14. `rb_ui.c`：待机提示、录音页/完成页按 mode 文案。

验证：固件主机测试 + `tools/validate.sh` + `passport-build` 技能出合并镜像。

## 收尾
15. `./deploy.sh` 部署服务端（对外操作，先征得用户同意），`/api/health` 正常，老数据出现在会议列表。
16. `passport-device-test`：用户确认镜像后烧录，按 PRD AC1–AC5 真机验收。
17. 把真机经验写进 `.trellis/spec/guides/recbean-firmware-lessons.md`，服务端约定写进 backend spec。

## 风险点 / 回滚
- `rb_session.c` 连接阶段处理 STOP 是最容易出错的地方，主机测不到，重点真机测「按下马上松开 0.7 s」「服务器关掉时按住」两种情况。
- 服务端回滚：`./deploy.sh --rollback`。
