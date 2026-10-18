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
