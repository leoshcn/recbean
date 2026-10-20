# Journal - leoshcn (Part 1)

> AI development session journal
> Started: 2026-10-18

---



## Session 1: 录音豆 MVP：规划、服务端、固件、真机联调

**Date**: 2026-10-18
**Task**: 录音豆 MVP：规划、服务端、固件、真机联调
**Branch**: `feature/recbean-mvp`

### Summary

Trellis 规划录音豆 MVP（D1-D7）；服务端 FastAPI+Vue 部署到测试机 8090，真实豆包流式识别与说话人分离验证通过（speaker_id 字段确认，PCM/ADPCM CER 0%）；固件基于 ai-passport main@0b9e4c8 重写应用，真机修复 LVGL 内置池耗尽（改 CLIB malloc），配网/配对/录音上传真机跑通；亲子对话被拆成 4 人，新增 Web 合并说话人并部署；录后整段重聚类（方案 B）实测收益有限，暂缓。未验证：屏幕细节、真机断网演练、缺字全面检查、浏览器内合并操作。

### Git Commits

| Hash | Message |
|------|---------|
| `7feb5ad` | (see git log) |
| `d6be965` | (see git log) |
| `55444b5` | (see git log) |

### Status

[OK] **Completed**


## Session 2: 录音豆：一键部署脚本与 PR 合并

**Date**: 2026-10-18
**Task**: 录音豆：一键部署脚本与 PR 合并
**Branch**: `main`

### Summary

新增 deploy.sh（打包 server/、上传、旧镜像留作 recbean:prev、重建、健康检查，支持 --status/--rollback；远端 .env 与 data/ 不覆盖；解压加 --no-same-owner 修复 Windows 打包 uid 问题），根 README 补充项目说明与部署方式，测试机实跑通过；推送 feature/recbean-mvp，创建并合并 PR #1 到 main（7ce73c6）。

### Git Commits

| Hash | Message |
|------|---------|
| `9050027` | (see git log) |
| `7ce73c6` | (see git log) |

### Status

[OK] **Completed**


## Session 3: 录音笔记/会议录音双模式与共享热词

**Date**: 2026-10-18
**Task**: 录音笔记/会议录音双模式与共享热词
**Branch**: `feature/dual-mode-notes-meeting`

### Summary

设备按住上键记笔记、单击确定录会议；服务端按 kind 区分并按天聚合笔记，笔记关闭说话人区分；网页新增笔记/会议/热词入口；热词共享并经真实接口实测。服务端已部署测试机，固件应用分区烧录（保留 NVS），真机 AC1-AC5 通过。PR #2。

### Git Commits

| Hash | Message |
|------|---------|
| `0f29f5b` | (see git log) |

### Status

[OK] **Completed**


## Session 4: Web 浏览器独立录音

**Date**: 2026-10-20
**Task**: Web 浏览器独立录音
**Branch**: `feature/web-browser-recording`

### Summary

规划并实现浏览器录音：麦克风+整屏共享系统音频混音、/web/ws 续录、群晖 HTTPS 反代；Gateway 钩子化并修复 resume 失败丢会话；已部署测试机，用户实测一次成功

### Git Commits

| Hash | Message |
|------|---------|
| `105af3a` | (see git log) |

### Status

[OK] **Completed**


## Session 5: Web 端按品牌 VI 重设计 + 深色模式

**Date**: 2026-10-20
**Task**: Web 端按品牌 VI 重设计 + 深色模式
**Branch**: `feature/web-browser-recording`

### Summary

按 branding VI 重做 server/web：设计 token 浅/深两套、三态主题切换、BrandMark 组件与 favicon、侧边栏+移动抽屉布局、录音控制台（仅桌面）；check 修复 6 个样式/可访问性问题；部署 192.168.2.144 验收通过；新增 spec frontend/theming-and-brand.md

### Git Commits

| Hash | Message |
|------|---------|
| `0cb92d9` | (see git log) |

### Status

[OK] **Completed**
