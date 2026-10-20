# Web 端界面按品牌 VI 重设计并支持深色模式

## Goal

把 `server/web`（Vue 3 + Vite 管理端）按 `branding/recbean-brand-identity.html` 的录音豆 VI 重新设计：简洁、现代、有科技感；支持浅色/深色两套主题；布局改为侧边栏导航，录音页做成“控制台”。只改视觉与模板结构，不改功能。

## Background (repo evidence)

- 前端 `server/web`：Vue 3.5 + vue-router 4（hash 路由，`src/main.js`），无 UI 库/CSS 框架。Docker 构建时打包 `web/dist`（`server/Dockerfile:14,20`）。
- 现状样式：`src/style.css` 43 行，硬编码 hex、蓝色主色 `#2563eb`、无 CSS 变量、无响应式（仅 `views/NoteDay.vue:201` 一处 `@media (hover: none)`）。顶栏导航在 `src/App.vue`，品牌为纯文字。
- 7 个页面各一个 `<style>`，共 28 处硬编码 hex：`views/Hotwords.vue`、`NoteDay.vue`、`NoteDays.vue`、`Pairing.vue`、`Record.vue`（524 行）、`RecordingDetail.vue`、`RecordingList.vue`。
- 说话人颜色硬编码在 `src/util.js:40-44`（`COLORS` + `speakerColor`），通过内联 style 使用（`Record.vue:77`、`RecordingDetail.vue:40,69`）。
- 录音页状态机（`Record.vue:103`）：`idle → acquiring (→ needSys) → connecting → recording ⇄ reconnecting → stopping → saved | interrupted`；**无暂停态**。
- `index.html` 无 favicon、无 theme-color。
- `NoteDay.vue:16` 的“手机长按复制”提示说明笔记页曾在手机上使用。
- VI 要点（`branding/recbean-brand-identity.html`）：
  - 色板（oklch）：豆绿 Bean `78% 0.2 148`（深底标志/强调）、深豆绿 Deep `52% 0.15 152`（浅底链接与主按钮）、墨绿 Ink `22% 0.035 165`（深色背景）、雾白 Mist `96.5% 0.012 150`（深底文字）、录音红 Signal `64% 0.21 27`（仅 REC 状态点）。中性色 bg `98.6% 0.004 155` / surface 白 / fg `20% 0.02 165` / muted `48% 0.015 165` / border `91% 0.008 160`。
  - 字体：系统字体栈（SF Pro / Segoe UI Variable / 苹方 / 微软雅黑）；数字与时间码等宽 + `tabular-nums`。圆角 10/16px；间距 8/12/20/32/56/96。
  - 标志 SVG：`<symbol id="rb-mark">`、`rb-mark-sm`（≤24px 加粗切缝）、`rb-icon`、`rb-icon-sm`。
  - 规则：浅底用深豆绿、深底用豆绿；豆绿不直接放白底；标志不加描边/渐变/投影、不改角度、不换成柱状声波或麦克风；动态状态 IDLE 静止切缝 / REC 切缝左流周期 1.6s / PAUSED 切缝拉直。
- `.trellis/spec/frontend/*` 仍为空模板，无既有前端约定。

## Requirements

- **R1 设计 token**：`style.css` 用 CSS 变量定义 VI 色板、中性色、字体、圆角、间距，浅/深两套；全局与各页面样式只引用变量，移除硬编码颜色（含 `util.js` 说话人颜色，改为随主题变化的 8 色 token）。
- **R2 品牌标识**：侧边栏使用 VI 标志 + 字标“录音豆”；`index.html` 加 favicon（`rb-icon-sm`）与 `theme-color`（浅/深）。
- **R3 深色模式**：深色以墨绿 Ink 为底、雾白文字、豆绿为强调。默认跟随系统（`prefers-color-scheme`）；侧边栏提供三态切换（跟随系统 / 浅色 / 深色），选择存 localStorage（读写 try/catch，失败回退跟随系统）；首屏渲染前应用主题，无闪屏。
- **R4 组件统一**：按钮（主/次/危险/链接）、输入框、下拉、卡片、徽标、列表项、进度条/电平表、空状态、错误/警告提示按 token 统一；焦点态可见；录音红只用于录音中状态。
- **R5 布局重排**：顶栏导航改为左侧侧边栏（标志、5 个导航项：笔记 / 会议 / 浏览器录音 / 热词 / 设备配对，主题切换）；各页面按新视觉重排信息层级（页面标题区 + 内容卡片）。路由、API、业务逻辑不变。
- **R6 录音控制台**：`Record.vue` 重做为控制台：大号圆形录音按钮、VI 动态切缝作状态指示、等宽大字时长、REC 红点、音源选项与双电平表、实时转写区。切缝状态映射：`recording` → 左流动画；`acquiring/needSys/connecting/reconnecting/stopping` → 切缝拉直；`idle/saved/interrupted` → 静止标准标志。尊重 `prefers-reduced-motion`（关闭流动动画）。仅面向桌面浏览器。
- **R7 移动端适配（录音页除外）**：< 768px 时侧边栏收起为顶部窄栏（标志 + 菜单按钮），点开抽屉导航；卡片/列表单列；可点击区域 ≥ 40px；无横向滚动。录音页在窄屏仅显示“请在桌面浏览器使用录音”提示。

## Acceptance Criteria

- [ ] AC1（R1）：`server/web/src` 下 `.vue`/`.css`/`util.js` 中除 token 定义外无硬编码颜色；浅/深两套主题下正文对比度 ≥ 4.5:1。
- [ ] AC2（R2）：侧边栏显示 VI 标志 + 字标；浏览器标签页显示录音豆 favicon。
- [ ] AC3（R3）：三态按钮可在跟随系统/浅色/深色间切换，刷新后保持；“跟随系统”下随 OS 主题实时变化；刷新无浅色闪屏；禁用 localStorage 时页面仍正常（跟随系统）。
- [ ] AC4（R4/R5）：7 个页面在两种主题下视觉一致、无未适配的残留样式；当前导航项高亮正确（会议详情 `/r/:id` 高亮“会议”）。
- [ ] AC5（R6）：录音中切缝持续左流、REC 红点可见、时长等宽不抖动；连接/重连/保存中切缝拉直；空闲/已保存为静止标志；开启“减少动态效果”时无动画。
- [ ] AC6（R7）：375px 宽度下除录音页外各页可完整使用（导航抽屉、播放、复制、编辑、删除），无横向滚动；录音页显示桌面提示。
- [ ] AC7（R5）：所有页面功能行为与改造前一致（录音开始/停止/重连、转写编辑、说话人改名/合并、热词增删排序、配对确认、导出、删除）。
- [ ] AC8：`npm run build` 通过。

## Out of Scope

- 后端、固件、API、路由改动。
- 手机端浏览器录音体验。
- 引入 UI 组件库或 CSS 框架、Web 字体文件（继续用系统字体栈）。
- 新增功能（如暂停录音）。
