# recbean 录音豆

基于 FoloToy AI Passport（ESP32-C3）的录音豆：按设备「确定」键开始录音，屏幕实时显示区分说话人的转写文本；录完后在局域网 Web 页面回放、编辑、导出。语音识别使用豆包大模型流式语音识别（服务端中转，密钥不进设备）。

```text
设备 ──ws (IMA-ADPCM 16 kHz)──► recbean 服务 (Docker) ──wss──► 豆包流式 ASR
     ◄── 实时转写 / 说话人 ────┘  └─ SQLite + WAV + Web 编辑器 (http://<服务器>:8090)
```

| 目录 | 内容 |
|---|---|
| `firmware/` | 设备固件（上游 ai-passport `main@0b9e4c8` 基线 + 录音豆应用，来源见 `firmware/UPSTREAM.md`） |
| `server/` | FastAPI 服务端 + Vue 3 Web 编辑器，详见 [`server/README.md`](server/README.md) |
| `docker-compose.yml` | 服务端部署（宿主 8090 → 容器 8080，数据挂载 `./data`，凭据读 `.env`） |
| `deploy.sh` | 一键部署脚本 |

## 部署服务端

### 一键部署（推荐）

前提：本机能用 SSH 密钥免密登录目标主机，目标主机装有 Docker 与 Docker Compose v2。Windows 上在 Git Bash 中运行。

```bash
./deploy.sh              # 打包 server/ → 上传 → 旧镜像留作 recbean:prev → 重建启动 → 健康检查
./deploy.sh --status     # 查看容器状态与健康检查
./deploy.sh --rollback   # 回滚到上一版镜像 recbean:prev（不重建，数据不动）
./deploy.sh --help
```

默认部署到测试机 `root@192.168.2.144:/root/recbean`，可用环境变量改目标：

```bash
DEPLOY_HOST=root@10.0.0.5 DEPLOY_DIR=/opt/recbean DEPLOY_PORT=8090 ./deploy.sh
```

| 变量 | 默认值 | 说明 |
|---|---|---|
| `DEPLOY_HOST` | `root@192.168.2.144` | SSH 目标 |
| `DEPLOY_DIR` | `/root/recbean` | 远端目录 |
| `DEPLOY_PORT` | `8090` | 健康检查用的宿主端口，须与 `docker-compose.yml` 一致 |
| `SSH` / `SCP` / `TAR` | `ssh` / `scp` / `tar` | 客户端程序 |

脚本**不会上传或覆盖**远端的 `.env`（豆包凭据）和 `data/`（录音）。首次部署时若远端没有 `.env`，会从 `.env.example` 生成并提示，需要登录目标主机填写：

```bash
ssh root@192.168.2.144 "cd /root/recbean && vi .env && docker compose up -d"
# .env 里填 DOUBAO_API_KEY=<火山引擎新版控制台 API Key>
```

部署完成后访问 `http://<服务器>:8090`；`/api/health` 返回 `"asr_configured": true` 说明凭据已生效。

### 手动部署

```bash
# 把 docker-compose.yml、.env.example、server/ 拷到目标目录后：
cp .env.example .env && chmod 600 .env   # 填 DOUBAO_API_KEY
docker compose up -d --build
curl http://localhost:8090/api/health
```

### 注意

- 构建前端需要能访问 `registry.npmmirror.com`（`server/web/package-lock.json` 的下载源）。
- 设备默认连接 `192.168.2.144:8090`；换服务器后在设备「设置 → 服务器地址」中修改，或重新配网时填写。
- 当前只适合局域网：设备链路为明文 ws，Web 无登录，不要暴露到公网。

## 固件

构建、验证与烧录见 `firmware/AGENTS.md` 与 `firmware/docs/development/`；真机调试经验见 `.trellis/spec/guides/recbean-firmware-lessons.md`。
