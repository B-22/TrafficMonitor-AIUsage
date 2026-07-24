# AIUsage — TrafficMonitor Plugin

TrafficMonitor x64 任务栏插件，以环形仪表 + 信息块方式实时显示 Claude 和 Codex 的用量、Credits、订阅状态。

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
| 订阅到期 | `ShowSubscription` | 显示 | 日期 + 红/绿点状态 |
| Claude 7d 重置星期 | `ShowClaude7dReset` | 隐藏 | 显示周一~周日 |
| Codex 7d 重置星期 | `ShowCodex7dReset` | 隐藏 | 显示周一~周日 |
| 7d 重置倒计时 | `Show7dCountdown` | 隐藏 | 穿透显示，>24h 显示周几+时间，<24h 显示倒计时，<2h 红色 |
| 过期提示 | `ShowStatus` | 显示 | 数据 >15min 未更新时红色提示 |
| 自定义订阅日期 | `CustomSubExpiry` | 空 | 手动设置订阅到期日，格式 2026-12-20 |

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

- 数据超过 15 分钟未更新时自动显示
- 显示格式：`过期 18m` 或 `过期 2h16m`
- 红色文字
- 保留最后一次成功数据，不清零
- Tooltip 显示最后成功更新时间和最近错误

### 7d 重置倒计时

- 自动选择最近的 7d 重置时间（Claude 或 Codex）
- >24h：显示 `周三 14:32` 格式
- <24h：显示 `5h30m` 倒计时格式
- <2h：红色文字警告

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

- `ProxyServer`：显式指定代理地址（如 `http://127.0.0.1:7890`）
- 留空使用系统网络栈（支持 TUN/VPN）
- WinHTTP `AUTOMATIC_PROXY` 模式自动通过系统网络适配器路由

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
ShowSubscription=1     # 显示订阅状态和日期
ShowStatus=1           # 过期时显示红色提示
ShowClaude7dReset=0    # Claude 7d 重置星期
ShowCodex7dReset=0     # Codex 7d 重置星期
Show7dCountdown=0      # 7d 重置倒计时穿透显示
CustomSubExpiry=       # 自定义订阅到期日
ProxyServer=           # 代理地址
RequireProxy=0         # 0=不阻止(默认), 1=无代理时阻止
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
2. 复制 `AIUsagePreview.dll` 到 `TrafficMonitor/plugins/`
3. 复制 `AIUsage.ini` 到 TrafficMonitor 配置目录
4. 启动 TrafficMonitor
5. 在显示设置中启用 "AI Usage Dashboard"

## 文件结构

```
├── CMakeLists.txt
├── AIUsage.ini                    # 配置模板
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
│   └── CodexUsageCoreTests.cpp
└── docs/
    ├── CONTRIBUTING.md            # 协作规范
    └── design/                    # 设计稿
```

## 致谢

- **[HCLonely/TrafficMonitor_Codex_Plugin](https://github.com/HCLonely/TrafficMonitor_Codex_Plugin)** — 核心基础，提供插件框架、Codex 逻辑、WinHTTP 封装、JSON 解析器
- **[huanchong-99/claude-usage-assistant](https://github.com/huanchong-99/claude-usage-assistant)** — Claude 凭据读取、DPAPI 解密、OAuth 刷新、usage API 参考
- **[bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin)** — PluginInterface.h
- **[zhongyang219/TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)** — 插件接口和任务栏嵌入框架

## 许可证

MIT License
