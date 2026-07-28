# AIUsage — TrafficMonitor Plugin

TrafficMonitor x64 任务栏插件，以环形仪表 + 信息块方式实时显示 Claude 和 Codex 的用量、Credits、订阅状态。

信息块按各自最大文字宽度排版，上下标签与数值水平居中，并相对圆环整体上移
2 个逻辑像素。插件宽度只由显示开关决定，不随冷启动占位符、刷新结果或倒计时
状态改变；右侧仅保留 2 个逻辑像素内边距，避免 TrafficMonitor 重启后反复保存
偏移或在托盘前留下额外空白。

![Preview](docs/design/preview_final.png)

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

### 显示项（均可独立开关）

| 显示项 | 配置键 | 默认 | 说明 |
|--------|--------|------|------|
| Claude 5h 圆环 | 自动 | 始终显示 | 橙色环，百分比数字 |
| Claude 7d 圆环 | 自动 | 始终显示 | 橙色环，百分比数字 |
| Codex 7d 圆环 | 自动 | 始终显示 | 青色环，百分比数字 |
| 百分比号 | `ShowPctSign` | 隐藏 | 0=只显示数字, 1=显示% |
| Credits 金额 | `ShowCredits` | 显示 | Claude 超额用量金额 |
| 5h 重置时间 | `ShowReset` | 显示 | 下次 5h 窗口重置时间 |
| 订阅状态 | `ShowSubscription` | 隐藏 | API 状态的红/绿点和文字 |
| 手动到期日期 | `ShowCustomExpiry` | 隐藏 | 独立显示 `CustomSubExpiry`，任务栏格式如 `8.13` |
| Claude 7d 重置星期 | `ShowClaude7dReset` | 显示 | 标签 `Claude`，值为一~日单字 |
| Codex 7d 重置星期 | `ShowCodex7dReset` | 显示 | 标签 `Codex`，值为一~日单字 |
| 7d 重置倒计时 | `Show7dCountdown` | 显示 | 不增加独立信息块；进入阈值后分别替换 Claude/Codex 星期值，默认 24 小时 |
| 新鲜度提示 | `ShowStatus` | 显示 | Claude/Codex 分别判断；1 分钟黄线，5 分钟红线 |
| 自定义订阅日期 | `CustomSubExpiry` | 留空 | 可选的 `YYYY-MM-DD`，任务栏仅显示月日 |

### 圆环颜色逻辑

| 百分比 | 颜色 |
|--------|------|
| 0-59% | Claude 橙色 `#c66c32` / Codex 青色 `#2ea8b1` |
| 60-84% | 警告橙色 |
| 85-100% | 红色 |
| 无数据 | 灰色 |

### 订阅状态指示

- 红点：已取消 (canceled)、逾期 (past_due)、暂停 (paused)
- 绿点：正常 (active)、试用中 (trialing)

### 过期状态

- Claude 和 Codex 分别记录最后成功更新时间
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

### 数据刷新逻辑

- 后台线程每 60 秒刷新
- UI 线程只读缓存，不阻塞任务栏
- Claude/Codex 独立请求，互不影响
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
  Claude、Codex 和令牌刷新域名会分别检测

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
- 不修改或删除 Claude/Codex 官方凭据文件
- Token 刷新沿用参考项目的安全写入方式

## 插件选项（AIUsage.ini）

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
CustomSubExpiry=       # 可选 YYYY-MM-DD；仓库模板必须留空
ProxyServer=           # 代理地址
RequireProxy=1         # 无代理时阻止；AllowedExitIPs 可作为 TUN 放行条件
AllowedExitIPs=        # 精确公网 IP 白名单，非空即启用 fail-closed
ExitIpCheckUrl=https://api.ipify.org/
VerifyTargetHostExitIp=1
```

## 构建

**环境要求**：MSVC v143 (Build Tools) + Windows 10 SDK + CMake 3.21+

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

产物：`build/Release/AIUsagePreview.dll`

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
│   ├── CodexUsageFetcher.cpp/h    # Codex 令牌 + API
│   ├── CodexUsageCore.cpp/h       # Codex JSON 解析
│   ├── ClaudeUsageFetcher.cpp/h   # Claude API + profile
│   ├── ClaudeCredentialReader.cpp/h  # Claude 凭据读取
│   ├── DpapiHelper.cpp/h          # DPAPI + AES-GCM
│   ├── ProxyHelper.cpp/h          # 代理检测
│   ├── JsonLite.cpp/h             # JSON 解析器
│   └── CodexUsageVersion.h.in
├── tests/
│   ├── CodexUsageCoreTests.cpp
│   └── ProxyHelperTests.cpp
└── docs/
    └── design/                    # 设计稿
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

## 许可证

MIT License
