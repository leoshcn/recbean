# recbean 录音豆

基于 FoloToy AI Passport（ESP32-C3）的录音豆：按设备「确定」键开始录音，屏幕实时显示区分说话人的转写文本；录完后在局域网 Web 页面回放、编辑、导出。语音识别使用豆包大模型流式语音识别（服务端中转，密钥不进设备）。也可以不用设备，直接在电脑浏览器里同时录麦克风和系统声音（Zoom、腾讯会议等），见[浏览器录音](#浏览器录音麦克风--系统声音)。

```text
设备 ──ws (IMA-ADPCM 16 kHz)──► recbean 服务 (Docker) ──wss──► 豆包流式 ASR
     ◄── 实时转写 / 说话人 ────┘  └─ SQLite + WAV + Web 编辑器 (http://<服务器>:8090)
浏览器 ──wss (PCM 16 kHz)──► 群晖反向代理 (HTTPS) ──► recbean 服务 /web/ws
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

## 浏览器录音（麦克风 + 系统声音）

Web 导航中的「浏览器录音」页（`/#/record`）可以不用设备，在 Windows 电脑上同时录下自己的麦克风和电脑正在播放的声音（Zoom、腾讯会议等对方的声音），实时显示带说话人的转写；停止后录音出现在「会议」列表中（带「浏览器」标签），回放、编辑、导出与设备录音相同。

### 前提：通过 HTTPS 访问（群晖 DSM 反向代理）

浏览器只允许在 HTTPS（或 localhost）页面中使用麦克风和屏幕共享，用 `http://192.168.2.144:8090` 打开录音页会提示“请通过 HTTPS（群晖反代地址）访问”，开始按钮不可用。服务端本身不处理证书，由群晖 DSM 反向代理负责 HTTPS，设备仍走原来的明文 `ws://…:8090`，不受影响。

在 DSM 7 中配置：

1. **控制面板 → 登录门户 → 高级 → 反向代理服务器 → 新增**。
2. 来源：协议 `HTTPS`，主机名填访问用的域名（如 `recbean.example.com`，或 NAS 已有证书的域名），端口选一个空闲端口（如 `8443`）。
3. 目的地：协议 `HTTP`，主机名 `192.168.2.144`，端口 `8090`。
4. **自定义标头 → 新增 → WebSocket**（会自动加上 `Upgrade` 和 `Connection` 两个标头）。**必须添加**，否则页面能打开但录音时提示“无法连接录音服务……”。
5. 保存后到 **控制面板 → 安全性 → 证书 → 设置**，给这条反代规则选择对应域名的证书（自签证书浏览器会报警，录音需要受信任的证书）。
6. 用 `https://recbean.example.com:8443/#/record` 访问。局域网内需要该域名能解析到群晖（路由器/DNS 或 hosts）。

### 使用

- 浏览器：Windows 上的 **Chrome 或 Edge**。Firefox、macOS 不支持录整台电脑的系统声音（此时只能录麦克风）。
- 勾选「麦克风」（可选择设备）和/或「系统声音」，点「开始录音」。
- 弹出共享窗口时选择 **「整个屏幕」并勾选「共享系统音频」**。只选标签页只能录到该标签页的声音；没勾选系统音频时页面会提示“未获取到系统声音”，可以重新选择或仅录麦克风。画面不会被录制，拿到声音后视频立即关闭。
- **建议佩戴耳机**：用外放时麦克风会把对方的声音再录一遍，造成重复。
- 录音中点浏览器的「停止共享」会继续录麦克风；网络短暂中断时页面自动重连并补传（最多缓存 60 s），超过服务端续录等待时间（`RESUME_TIMEOUT_S`，默认 30 s）则录音标记为「已中断」并保留已录部分。
- 录音期间不要刷新或关闭页面（会提示确认），刷新后无法续录。

## 固件

构建、验证与烧录见 `firmware/AGENTS.md` 与 `firmware/docs/development/`；真机调试经验见 `.trellis/spec/guides/recbean-firmware-lessons.md`。
