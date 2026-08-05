# AIUsage — TrafficMonitor Plugin

TrafficMonitor x64 任务栏插件，以环形仪表 + 信息块方式实时显示 Claude、Codex、Antigravity 的用量/配额和 Kiro Credits 额度。

Windows 11 的视觉语言以 50 像素高的
[`taskbar_preview.html`](docs/design/taskbar_preview.html) 为设计源：字号、字重、
配色、水平间距和元素层级都按它来。文字统一使用 `Segoe UI` 常规字重。

**纵向坐标不能照搬设计稿。** TrafficMonitor 把自己的任务栏窗口高度写死为
`TASKBAR_WND_HEIGHT`（`TaskBarDlg.h`，定义为 `DPI(32)`），`CalculateWindowSize()`
直接使用该常量，与任务栏实际高度、字体大小、`vertical_margin` 都无关；
`PluginInterface.h` 也没有让插件申请高度的接口。因此在 48 像素高的 Windows 11
任务栏上，插件真正拿到的绘制槽位只有 96-DPI 下的 32 像素，居中嵌在任务栏里。
按 50 像素固定框架定位会把 `5h` / `7d` 角标推到槽位上边缘之外、把来源名推到下边缘
之外，两者都会被宿主裁掉。

插件因此按宿主传入的项目矩形高度计算全部纵向坐标（`ComputeLayout`）：角标占据
从槽位顶部到角标基线之间的区域（`5h` / `7d` 无下伸部，墨迹全部落在该区域内），
圆环占据其下的剩余高度，信息区两行分别在各自的高度带内居中。槽位足够高时
（约 40 像素以上）圆环下方的 `Claude` / `Codex` 来源名会自动出现；在
TrafficMonitor 的 32 像素槽位上它不显示，改由信息区的 `Claude` / `Codex` 两列和
两组圆环之间的分隔线区分来源。

插件数值文字会采用 TrafficMonitor 当前任务栏预设传入的数值色；小标签使用插件的
主题感知弱化色，避免宿主把标签和值都设成同一深色后显得粗重。“背景透明”和
“自动适应 Windows 深色/浅色主题”会继续对 AIUsage 生效。圆环轨道、用量状态色、
分隔线和无数据状态由插件根据宿主传入的背景明暗切换深浅调色板。透明模式下插件
不会自行填充矩形背景。

浅色模式下的圆环轨道和分隔线比设计稿更深。设计稿画在 `#f6f6f6` 页面上，而插件
从不画在该颜色上：TrafficMonitor 会把配置的任务栏背景色作为色键从分层窗口中扣掉，
圆环背后露出的是 Windows 11 任务栏本身（浅色模式实测约 `RGB(240,242,244)`），
设计稿的 `#e6e6e6` 轨道与之只差约 10 级灰度，等于看不见。次级文字同样加深：
7-8 像素字号的抗锯齿覆盖率达不到满值，中灰会被冲淡。

文本抗锯齿使用灰度而非 ClearType。宿主的任务栏窗口是带色键的分层窗口，插件看不到
最终背景，ClearType 的子像素滤波会在小字上留下紫绿彩边且没有实心笔画。

即使 TrafficMonitor 的“渲染设置”选择 Direct2D，当前插件接口仍以 API 7 的
`DrawItem(HDC)` 交给宿主，TrafficMonitor 再通过 Direct2D/GDI 互操作合成；这与宿主
显示 Direct2D 并不矛盾。悬浮提示接口也只接受一段原生纯文本，不能设置圆角、背景、
字体层级或按钮。本插件因此只重排原生提示的信息层级，不挂钩 TrafficMonitor 窗口，
也不创建覆盖任务栏的自绘弹窗。

### Windows 11 任务栏视觉基准

![HTML exported Windows 11 taskbar visual specification](docs/design/taskbar-visual-spec-html.png)

上图由设计 HTML 在 736×50 视口中直接导出，不是手工重画的概念图。原批准稿
[`taskbar-visual-spec-win11.png`](docs/design/taskbar-visual-spec-win11.png) 保留为
对照。它描述的是 50 像素槽位下的完整形态：水平间距、字号和配色按它执行，纵向坐标
按下节的规则从实际槽位高度推导。

水平常量（96-DPI，相对插件区域）：

| 项目 | 值 |
|------|-----|
| 插件左内边距 | `10` |
| 圆环盒（正方形，同时决定单元宽度） | 由槽位高度推导，上限 `28` |
| 圆环描边 / 环内数字 | 圆环盒的 `2.2/28` / `0.45` |
| 同组两环间距 | `6` |
| 两组圆环间距（分隔线居中） | `11` |
| 信息区分隔线左右留白 | `5 + 5` |
| 信息块间距 | `11` |
| 角标 / 来源名 / 信息标签 / 信息数值 字号 | `7 / 6.5 / 8 / 11` |

纵向坐标由 `ComputeLayout` 按宿主槽位高度推导，32 像素槽位下的实测结果：

| 项目 | 槽位内 y |
|------|----------|
| 槽位高度（`TASKBAR_WND_HEIGHT`） | `32` |
| `5h` / `7d` 角标带（顶部到角标基线） | `0 ~ 7` |
| 圆环（外径约 `23`，圆心 `19`） | `7 ~ 30` |
| 分隔线 | `4.5 ~ 29` |
| 信息标签 / 数值行中心 | `7.2 / 23.2` |
| `Claude` / `Codex` 来源名 | 不显示（槽位不足 40） |

TrafficMonitor 在插件左侧另有约 10 像素宿主间距，不计入上述插件内部坐标。

## Codex 重置卡与全局重置雷达

- 插件从 `~/.codex/auth.json` 只读获取 `tokens.access_token`，请求
  `https://chatgpt.com/backend-api/wham/rate-limit-reset-credits`。悬浮提示会显示
  可用重置卡总数，以及接口返回的每张可用卡的发放时间和本地过期时间；不会显示
  access token、refresh token、账号 ID 或重置卡完整 ID。该 `wham` 地址是 ChatGPT
  内部接口，并非稳定的公开 API；若服务端字段或鉴权方式变化，插件会在悬浮提示中
  报错并保持用量主接口独立工作。
- 可用卡中最早一张在 48 小时内过期时，`Codex` 星期栏优先改成紧凑的
  `2卡18h` / `1卡35m`。该值在深色任务栏使用亮黄到粉色渐变，在浅色任务栏使用
  深红到紫色渐变；过期预警优先于 7d 倒计时、新鲜度分钟数和星期。
- “今日是否全局重置”以 [Codex Runway](https://github.com/Licoy/codex-runway) 的
  匿名静态 v1 feed `https://codexreset.gitcdn.top/api/status.json` 为主基准，默认每
  60 分钟刷新。插件会严格校验 schema、监控状态、事件类型、置信度、固定说明、
  `@thsottiaux` 证据链接和适用范围，再按本机日期判断“是 / 否 / 未知”。同日已完成
  重置或同日未来计划为“是”；30 小时未成功更新、监控降级或存在无法确认的同日事件
  时 fail-to-unknown，不把旧信息继续当作确定结论。
- [codex-reset.com 的公开 forecast API](https://codex-reset.com/api/forecast) 和
  [Codex 雷达公开摘要](https://codex-reset-radar.pages.dev/current.json) 仍每
  15 分钟刷新，作为次级补充，保留 24/48 小时概率。Runway 有新鲜且明确的“是/否”
  时优先；Runway 不可用或为“未知”时，次级来源才决定公告窗口。次级开启信号超过
  12 小时会被忽略。
- 主源判断为“是”或主源未知而次级公告窗口开启时，四个圆环左侧出现渐变闪电 `⚡`。
  原生悬浮提示按“配额 / 今日全局重置 / 账户 / 状态”分段，显示主源结论、计划或最近
  重置时间、范围、置信度、补充概率、更新时间、重置卡和网络状态。
- 雷达和概率均来自独立社区站点，只能作为提醒，不能保证 OpenAI 一定重置，也不能替代
  账号自己的 5h/7d 服务端重置时间。

新增配置：

```ini
ShowResetCreditWarning=1    # 1=在 Codex 星期栏显示临期卡
ResetCreditWarningHours=48  # 最早可用卡进入多少小时后开始警告
ShowResetRadar=1            # 1=监控全局“速蹬窗口”
ResetRadarRefreshMinutes=15 # 允许 5-120 分钟
RunwayResetRefreshMinutes=60 # 主源刷新，允许 15-240 分钟
```

Codex 用量和重置卡接口继续复用插件现有的代理与出口 IP 检查；启用
`AllowedExitIPs` 时，`chatgpt.com` 会在真实请求前走相同的 fail-closed 预检。三个社区
重置接口是匿名公共数据，不发送 token、账号 ID 或 Cookie，因此不应用 `RequireProxy`
和 `AllowedExitIPs` 限制，避免公共站点不支持目标域名出口检查时被误判为“IP 屏蔽”；
如果已配置显式或系统代理，重置源仍沿用该代理路由。第三方源失败不会阻断
Claude/Codex 用量数据；Runway 缓存会随当前日期重新求值，超过 30 小时只显示“未知”，
次级开启信号只在 12 小时新鲜度范围内有效。

## 功能完整列表

### 数据源

| 数据 | 来源 | 说明 |
|------|------|------|
| Claude 5h 用量 | `/api/oauth/usage` | 5 小时滑动窗口已用百分比 |
| Claude 7d 用量 | `/api/oauth/usage` | 7 天窗口已用百分比 |
| Claude Credits | `/api/oauth/usage` `extra_usage` | 超额用量金额（$） |
| Claude 5h 重置时间 | `/api/oauth/usage` `resets_at` | 下次 5h 窗口重置的本地时间 |
| Claude 7d 重置时间 | `/api/oauth/usage` `resets_at` | 下次 7d 窗口重置日期 |
| Claude 订阅状态 | `/api/oauth/profile` | active/canceled/past_due 等 |
| Codex 5h 用量 | `/backend-api/wham/usage` | 5 小时窗口已用百分比 |
| Codex 7d 用量 | `/backend-api/wham/usage` | 7 天窗口已用百分比 |
| Codex 7d 重置时间 | `/backend-api/wham/usage` `reset_at` | 下次 7d 窗口重置时间 |
| Antigravity 模型配额 | Google `fetchAvailableModels` | 各模型剩余百分比与重置时间 |
| Antigravity 订阅档位 | Google `loadCodeAssist` | FREE/PRO/TEAMS 等 |
| Kiro Credits | Kiro `getUsageLimits` | 已用/总额度与剩余额度 |

### Antigravity 凭据

插件通过 Google OAuth 读取 Antigravity（Google Cloud Code）模型配额。**推荐方式**：双击 TrafficMonitor 目录下的 `ag-login.exe`（部署脚本会自动放置），浏览器自动打开 Google 登录页，授权完成后令牌自动写回 `AIUsage.ini` 的 `[Antigravity]` 段，之后插件自行刷新，无需任何手工维护。也可以手动填写：

| 配置键 | 必填 | 说明 |
|--------|------|------|
| `ClientId` | 否 | 默认使用公开的 Cloud Code installed-app 客户端，留空即用内置值 |
| `ClientSecret` | 否 | 已内置默认值（Google 视 installed-app secret 为非机密，参考项目同样明文内置），留空即用内置值 |
| `AccessToken` | 二选一 | 已签发的 access token（过期后需重新运行 ag-login.exe） |
| `RefreshToken` | 二选一（推荐） | 自动刷新 access token，无需手工维护 |
| `PrimaryModel` | 否 | 任务栏圆环的"主模型"（子串匹配），留空自动选择 |

主模型自动选择规则：优先 Gemini 3.x，其次 Claude，再其次 GPT；同优先级取剩余最少（最紧张）的模型。令牌只保存在本机配置文件中，不会上传到任何服务器。

### Kiro Credits 凭据

Kiro Credits 通过 Kiro IDE 的本地登录态读取，无需手动填凭据：

- 读取 `%USERPROFILE%\.aws\sso\cache\kiro-auth-token.json`（Kiro 登录时写入）
- 自动经 `prod.us-east-1.auth.desktop.kiro.dev/refreshToken` 刷新 access token
- 请求 `q.us-east-1.amazonaws.com/getUsageLimits` 获取额度
- 若令牌在其他位置，可在 `[Kiro]` 段的 `TokenPath` 覆盖
- 本机未登录 Kiro 时该来源显示 `--`
| Codex 重置卡 | `/backend-api/wham/rate-limit-reset-credits` | 可用总数、逐卡发放/过期时间 |
| 今日全局重置 | Codex Runway `api/status.json` | 本地日期的“是 / 否 / 未知”、计划、范围与证据 |
| 全局重置补充预测 | `codex-reset.com/api/forecast` + Codex 雷达公开摘要 | 次级公告窗口与 24/48h 概率 |

### 显示项（均可独立开关）

| 显示项 | 配置键 | 默认 | 说明 |
|--------|--------|------|------|
| Claude 5h 圆环 | 自动 | 始终显示 | 第一组 5h，用量色环和百分比数字 |
| Claude 7d 圆环 | 自动 | 始终显示 | 第一组 7d，用量色环和百分比数字 |
| Codex 5h 圆环 | 自动 | 始终显示 | 第二组 5h，用量色环和百分比数字 |
| Codex 7d 圆环 | 自动 | 始终显示 | 第二组 7d，用量色环和百分比数字 |
| Antigravity 圆环 | `ShowAntigravity` | 显示 | 第三组，主模型已用百分比，角标 `AG` |
| Kiro 圆环 | `ShowKiro` | 显示 | 第三组，Credits 已用百分比，角标 `Kiro` |
| Antigravity 档位块 | `ShowAntigravity` | 显示 | 信息块 `AG`，值为订阅档位（FREE/PRO/...） |
| Kiro 剩余额度块 | `ShowKiro` | 显示 | 信息块 `Kiro`，值为剩余额度，非百分比 |
| 百分比号 | `ShowPctSign` | 隐藏 | 0=只显示数字, 1=显示% |
| Credits 金额 | `ShowCredits` | 显示 | Claude 超额用量金额 |
| 5h 重置时间 | `ShowReset` | 显示 | 下次 5h 窗口重置时间 |
| 订阅状态 | `ShowSubscription` | 隐藏 | API 状态的红/绿点和文字 |
| 手动到期日期 | `ShowCustomExpiry` | 隐藏 | 独立显示 `CustomSubExpiry`，任务栏格式如 `8.13` |
| Claude 7d 重置星期 | `ShowClaude7dReset` | 显示 | 标签 `Claude`，值为一~日单字 |
| Codex 7d 重置星期 | `ShowCodex7dReset` | 显示 | 标签 `Codex`，值为一~日单字 |
| 7d 重置倒计时 | `Show7dCountdown` | 显示 | 不增加独立信息块；进入阈值后分别替换 Claude/Codex 星期值，默认 24 小时 |
| 重置卡临期警告 | `ShowResetCreditWarning` | 显示 | 最早可用卡 48h 内过期时替换 Codex 星期值并使用对比度渐变 |
| 全局重置标记 | `ShowResetRadar` | 显示 | 主源判断为“是”或主源未知且次级窗口开启时，在圆环左侧显示 `⚡` |
| 新鲜度提示 | `ShowStatus` | 显示 | Claude/Codex 分别判断；1 分钟黄线，5 分钟红线 |
| 自定义订阅日期 | `CustomSubExpiry` | 留空 | 可选的 `YYYY-MM-DD`，任务栏仅显示月日 |

### 圆环颜色逻辑

| 百分比 | 颜色 |
|--------|------|
| 0-59% | 绿色，正常 |
| 60-79% | 黄色，提醒 |
| 80-89% | 橙色，偏高 |
| 90-100% | 红色，紧张 |
| 无数据 | 灰色 |

圆环颜色只表达用量等级，不再表达 Claude/Codex 来源。四个圆环固定按
`Claude 5h、Claude 7d｜Codex 5h、Codex 7d` 排列，两组之间使用细分隔线，
每个圆环右上角有 `5h` / `7d` 角标；槽位高度足够时每组下方还会显示来源名称
（TrafficMonitor 的 32 像素槽位不足以显示，此时由信息区的 `Claude` / `Codex`
两列区分）。因此在黑白或色觉差异环境中同样能区分来源。

圆环数字和信息块数值跟随 TrafficMonitor 的数值文字颜色，圆环角标和信息块标签
跟随标签文字颜色。未勾选“指定每个项目的颜色”时，两者均跟随任务栏全局文字颜色。

### 订阅状态指示

- 红点：已取消 (canceled)、逾期 (past_due)、暂停 (paused)
- 绿点：正常 (active)、试用中 (trialing)

### 过期状态

- Claude、Codex、Antigravity、Kiro 分别记录最后成功更新时间
- 超过 1 分钟：对应圆环数字下显示黄色短横线
- 超过 5 分钟：短横线变为红色
- 超过 10 分钟：原有 Claude/Codex 星期值改为 `12分` 形式，超过 99 分钟固定为 `99+分`
- 正常的一分钟刷新请求进行期间提供短暂宽限，避免边界状态闪现
- 保留最后一次成功数据，不清零
- Tooltip 显示最后成功更新时间和最近错误

### 7d 重置倒计时

- Claude 与 Codex 分别使用自己的 7d 重置时间
- ISO 8601 时间统一按其 `Z`/时区偏移解析，再转换为 Windows 本地时间
- 默认在剩余 24 小时内显示倒计时，可通过 `CountdownShowBeforeHours` 调整
- 进入阈值后，原 Claude/Codex 星期值分别变为 `5h30m` 等具体倒计时；
  阈值外继续显示原星期值，不创建单独的“7d重置”信息块
- 显示 `5h30m` 倒计时格式
- 倒计时沿用普通数值颜色，不根据剩余时间变红

### 凭据读取

**Claude**（优先级从高到低）：
1. MSIX Claude Desktop `%LOCALAPPDATA%\Packages\Claude_*\...\config.json`（DPAPI + AES-GCM 解密）
2. 经典安装 `%APPDATA%\Claude\config.json`
3. CLI `%USERPROFILE%\.claude\.credentials.json`（支持 token 刷新和写回）

只读桌面版令牌，不影响桌面版登录态。

**Codex**：
- `~/.codex/auth.json`（支持 token 刷新和写回）

**Antigravity**：
- `AIUsage.ini` 的 `[Antigravity]` 段（推荐运行 `ag-login.exe` 一键授权写回，也可手动填 RefreshToken）
- 自动通过 `oauth2.googleapis.com/token` 刷新 access token，不修改本地文件

**Kiro**：
- 只读 `%USERPROFILE%\.aws\sso\cache\kiro-auth-token.json`（Kiro IDE 登录态）
- 自动经 Kiro 官方 auth 端点刷新 access token，不写回本地缓存

### 数据刷新逻辑

- 后台线程每 60 秒刷新
- UI 线程只读缓存，不阻塞任务栏
- Claude/Codex/Antigravity/Kiro 独立请求，互不影响
- 429 限流指数退避（120s → 900s）
- 失败保留最后一次成功数据
- Profile API 仅首次请求一次（获取订阅状态）

### 代理配置

- `ProxyServer`：显式指定代理地址（例如 `http://127.0.0.1:PORT`）
- 留空使用系统网络栈（支持 TUN/VPN）
- WinHTTP `AUTOMATIC_PROXY` 模式自动通过系统网络适配器路由
- `RequireProxy=1`：没有显式/系统代理时 fail-closed；配置了
  `AllowedExitIPs` 时，以出口 IP 校验作为 TUN/VPN 的放行条件
- `AllowedExitIPs`：逗号或分号分隔的精确公网 IPv4/IPv6 白名单。非空时，
  每次建立 Claude/Codex HTTP 会话前均通过同一路径查询公网 IP；检测失败或
  不匹配时，不发送令牌刷新和用量请求
- `ExitIpCheckUrl`：返回纯文本公网 IP 的 HTTPS 检测地址，默认
  `https://api.ipify.org/`；HTTP 和重定向会被拒绝
- `VerifyTargetHostExitIp=1`：在真实请求前访问同一目标域名的
  `/cdn-cgi/trace` 并读取 `ip=`，确保检测与 API 请求命中相同的域名分流规则；
  Claude、Codex 和令牌刷新域名会分别检测。
  只有 Cloudflare 前置的域名提供 `/cdn-cgi/trace`，Antigravity（Google）和
  Kiro（AWS）会返回 404。此时采用**宽松策略：默认放行**，不再回退到
  `ExitIpCheckUrl` 探测——因为不同域名走不同分流规则，回退探测会把
  误报的出口 IP 当成真实出口，反而导致误拦。仅当检测到的出口 IP 明确不在
  `AllowedExitIPs` 中时才会 fail-closed 拦截

推荐的 TUN 配置：

```ini
ProxyServer=
RequireProxy=1
AllowedExitIPs=你的代理公网IP
ExitIpCheckUrl=https://api.ipify.org/
VerifyTargetHostExitIp=1
```

> 应用层“先检测出口、再访问 API”可以防住 TUN 关闭、切换失败和大多数误直连，
> 但无法证明目标域名没有被代理软件的分流规则单独设置为直连。若账号风险要求
> 接近硬保证，请同时启用代理客户端的 kill switch，或使用 Windows 防火墙/WFP
> 将 `TrafficMonitor.exe` 限制到 TUN 接口；最确定的应用内方案仍是填写
> `ProxyServer`，让全部请求只走一个显式代理。

### 安全

- 日志不记录 token、Authorization、Cookie 等敏感信息
- 不修改或删除 Claude/Codex 官方凭据文件；Kiro 令牌缓存只读、不写回
- Antigravity 的 OAuth 凭据只存在于本机 `AIUsage.ini`，不记录到 Git
- Token 刷新沿用参考项目的安全写入方式

## 插件选项（AIUsage.ini）

插件优先使用 TrafficMonitor 通过 `EI_CONFIG_DIR` 提供的配置目录；若宿主版本未
发送该回调，则自动从当前 `TrafficMonitor.exe` 所在目录读取 `AIUsage.ini`。
宿主回调目录只有在其中真实存在 `AIUsage.ini` 时才允许覆盖当前有效配置，避免
空目录回调把到期日期、显示选项和出口 IP 锁静默重置为默认值。

```ini
[AIUsage]
ShowPctSign=0          # 圆环内是否显示 %
ShowCredits=1          # 显示 Credits 金额
ShowReset=1            # 显示 5h 重置时间
ShowSubscription=0     # 隐藏 API 订阅状态
ShowCustomExpiry=0     # 默认隐藏手动到期日期
ShowStatus=1           # 显示分来源新鲜度短线和过期分钟数
ShowClaude7dReset=1    # Claude 重置星期单字
ShowCodex7dReset=1     # Codex 重置星期单字
Show7dCountdown=1      # 阈值内替换 Claude/Codex 各自的星期值
CountdownShowBeforeHours=24
ShowResetCreditWarning=1
ResetCreditWarningHours=48
ShowResetRadar=1
ResetRadarRefreshMinutes=15
RunwayResetRefreshMinutes=60
ShowAntigravity=1        # 显示 Antigravity 圆环 + 档位块
ShowKiro=1               # 显示 Kiro 圆环 + 剩余额度块
CustomSubExpiry=       # 可选 YYYY-MM-DD；仓库模板必须留空
ProxyServer=           # 代理地址
RequireProxy=1         # 无代理时阻止；AllowedExitIPs 可作为 TUN 放行条件
AllowedExitIPs=        # 精确公网 IP 白名单，非空即启用 fail-closed
ExitIpCheckUrl=https://api.ipify.org/
VerifyTargetHostExitIp=1

[Antigravity]          # Google OAuth 凭据，仅存本机（双击 ag-login.exe 一键授权写回）
ClientId=              # 留空 = 内置默认 Cloud Code 客户端
ClientSecret=          # 留空 = 内置默认值（Google 视 installed-app secret 为非机密）
AccessToken=           # ag-login.exe 自动写回；也可手动填
RefreshToken=          # ag-login.exe 自动写回；推荐，用于自动续期
PrimaryModel=          # 主模型子串，留空自动选择

[Kiro]                 # Kiro IDE 登录态
TokenPath=             # 默认 %USERPROFILE%\.aws\sso\cache\kiro-auth-token.json
```

## 构建

**环境要求**：MSVC v143 (Build Tools) + Windows 10 SDK + CMake 3.21+

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

产物：`build/Release/AIUsagePreview.dll`

GitHub Actions 发版（tag `v*` / main 推送）自动附带以下产物：
- `AIUsage-{x86,x64,arm64}.zip`：插件 DLL + `AIUsage.ini` 模板
- `aiusage-server-linux-amd64` / `aiusage-server-linux-arm64`：Go 转发服务
  （静态二进制，部署到 Linux 服务器直接运行）
- `aiusage-server-windows-amd64.exe`：Go 转发服务 Windows 版
- `ag-login.exe`：Antigravity 一次性授权工具（放到 TrafficMonitor 目录双击即可）

### 部署

1. 关闭 TrafficMonitor
2. 直接覆盖 `TrafficMonitor/plugins/AIUsagePreview.dll`（默认不创建备份）
3. 首次部署时复制 `AIUsage.ini` 到 TrafficMonitor 配置目录；更新 DLL 时不要用
   仓库模板覆盖本机含出口 IP 等私有值的配置
4. 启动 TrafficMonitor
5. 在显示设置中启用 "AI Usage Dashboard"

部署时不能只发送一次结束命令就立即重启：必须确认所有 `TrafficMonitor.exe`
实例已经退出、进程数为 0，再启动一次并确认进程数为 1。残留实例会继续加载旧
DLL、保留旧任务栏占位，并使新旧显示区域叠加或错位。本地可使用
`deploy-local.ps1` 执行这一严格流程；脚本不会覆盖本机 `AIUsage.ini`。

### 可选的上游转发服务（server/）

`server/` 提供一个可选的 Go 转发服务：插件把 Claude/Codex 的 OAuth 凭据
一次性上传给该服务，由服务在自身网络出口（如家宽 IPv6）上代请求官方用量接口，
再回传给插件。这样插件所在机器的出口 IP 不会暴露给官方 API。

- 构建：`cd server && go build`；交叉编译 Linux 静态二进制：
  `GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build`（arm64 同理，产物约 7MB）
- 配置全部通过环境变量，至少需要 `AIUSAGE_TOKEN`（服务间共享的 bearer token）
- 环境变量：`AIUSAGE_LISTEN`（默认 `127.0.0.1:8444`）、`AIUSAGE_CACHE_USAGE`
  /`AIUSAGE_CACHE_CREDITS`/`AIUSAGE_CACHE_PROFILE`（缓存秒数）、
  `AIUSAGE_CLAUDE_CLIENT_ID`（默认公开的 Cloud Code 客户端）、
  `AIUSAGE_TLS_CERT`/`AIUSAGE_TLS_KEY`（可选 TLS）
- 上传的凭据只保存在服务进程内存，不落盘；所有端点都要求 bearer token 认证
- 测试：`go test ./...`

**Linux 上复用本机 CLI 登录态（默认开启）**：若服务器上跑过
`claude` / `codex` 登录，服务会直接读取并复用本地凭证，无需再上传：

- Claude：读 `~/.claude/.credentials.json`（`claudeAiOauth` 段），
  尊重 `CLAUDE_CONFIG_DIR` 环境变量
- Codex：读 `~/.codex/auth.json`（`tokens` 段），尊重 `CODEX_HOME` 环境变量；
  API Key 计费模式（无 `tokens`）没有订阅限额可查
- 令牌临近过期（<120s）或被上游 401 拒绝时，服务用其中的 refreshToken 自动
  续期并**原子写回原文件（0600）**——与 CLI 行为一致，登录一次即永久自续
- 优先级：本地凭证优先，插件上传的凭据作为回退；`AIUSAGE_USE_LOCAL_CREDS=0`
  可关闭本地复用，恢复纯转发模式
- 实现语义对齐 [huanchong-99/claude-usage-assistant](https://github.com/huanchong-99/claude-usage-assistant)
  （参考脚本归档于 `docs/reference/quota_card.py`）

默认插件仍是直连官方 API，不使用该服务；仅当部署该服务并配置插件指向它时才会
用到。

## 文件结构

```
├── CMakeLists.txt
├── AGENTS.md                      # 协作、README 同步及隐私规范
├── AIUsage.ini                    # 配置模板
├── deploy-local.ps1               # 零残留、单实例本地部署
├── LICENSE
├── README.md
├── gemini_bridge.user.js          # Gemini 用量监控脚本
├── include/
│   └── PluginInterface.h
├── src/
│   ├── CodexUsagePlugin.cpp       # 主插件 + GDI 自绘
│   ├── DashboardRenderer.cpp/h    # Direct2D/GDI 双路径渲染
│   ├── CodexUsageFetcher.cpp/h    # Codex 令牌 + API
│   ├── CodexUsageCore.cpp/h       # Codex JSON 解析
│   ├── ClaudeUsageFetcher.cpp/h   # Claude API + profile
│   ├── ClaudeCredentialReader.cpp/h  # Claude 凭据读取
│   ├── DpapiHelper.cpp/h          # DPAPI + AES-GCM
│   ├── ProxyHelper.cpp/h          # 代理检测
│   ├── AntigravityUsageFetcher.cpp/h  # Antigravity OAuth + 配额
│   ├── KiroCreditsFetcher.cpp/h   # Kiro Credits 额度
│   ├── JsonLite.cpp/h             # JSON 解析器
│   └── CodexUsageVersion.h.in
├── tests/
│   ├── CodexUsageCoreTests.cpp
│   └── ProxyHelperTests.cpp
└── server/                        # 可选 Go 转发服务（见“构建”一节）
    ├── main.go                    # HTTP 服务 + 上游转发 + 内存缓存
    ├── refresh.go                 # Claude token 刷新
    └── server_test.go             # 服务测试

docs/                              # 设计稿
```

## 协作与发布约束

- 所有新增、变更或移除的功能、显示行为、配置键、安全策略、构建和部署方式，
  必须在同一次提交中同步更新本 README；README 是公开功能说明的唯一入口。
- 仓库文件、示例、测试和提交内容不得包含真实公网 IP、代理端口、令牌、账号、
  邮箱、订阅日期、本机绝对路径或其他个人/机器信息。IP 示例只使用
  RFC 5737/RFC 3849 文档保留地址，机器相关配置保持为空。
- 本地发布默认直接覆盖 DLL，不创建 `_backup_*` 目录；需要回退时使用 Git 或
  GitHub Release。只有用户明确要求时才另行备份。
- 完整协作检查清单见 [`AGENTS.md`](AGENTS.md)。

## 致谢

- **[HCLonely/TrafficMonitor_Codex_Plugin](https://github.com/HCLonely/TrafficMonitor_Codex_Plugin)** — 核心基础，提供插件框架、Codex 逻辑、WinHTTP 封装、JSON 解析器
- **[huanchong-99/claude-usage-assistant](https://github.com/huanchong-99/claude-usage-assistant)** — Claude 凭据读取、DPAPI 解密、OAuth 刷新、usage API 参考
- **[bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin)** — PluginInterface.h
- **[zhongyang219/TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)** — 插件接口和任务栏嵌入框架
- **[Licoy/codex-runway](https://github.com/Licoy/codex-runway)** — 今日重置公开
  feed、schema 与悬浮信息层级参考。上游采用 AGPL-3.0；本插件未复制其 Swift/服务端
  源码，只独立实现公开 v1 数据格式的 Windows 消费端。
- **[wusimpl/AntigravityQuotaWatcher](https://github.com/wusimpl/AntigravityQuotaWatcher)** 与
  **[AntigravityQuotaWatcherDesktop](https://github.com/wusimpl/AntigravityQuotaWatcherDesktop)** —
  Antigravity 配额 API（loadCodeAssist / fetchAvailableModels）、Google OAuth
  流程与 Kiro Credits（AWS SSO 缓存 + getUsageLimits）实现参考。

## 许可证

MIT License
