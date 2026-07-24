# AIUsage — TrafficMonitor Plugin

TrafficMonitor x64 任务栏插件，以环形仪表 + 信息块方式实时显示 Claude 和 Codex 的用量、Credits、订阅状态。

![Preview](docs/design/preview_final.png)

## 功能一览

| 显示项 | 数据来源 | 说明 |
|--------|----------|------|
| Claude 5h | `/api/oauth/usage` | 5 小时滑动窗口已用百分比 |
| Claude 7d | `/api/oauth/usage` | 7 天窗口已用百分比 |
| Codex 7d | `/backend-api/wham/usage` | 7 天窗口已用百分比 |
| Credits | `/api/oauth/usage` extra_usage | Claude 超额用量金额 |
| 5h 重置 | usage API resets_at | 下次 5h 窗口重置时间（本地时区） |
| 订阅状态 | `/api/oauth/profile` | 红点=已取消，绿点=正常 |

## 显示样式

基于 Windows 10 任务栏自绘（GDI + GDI+ 抗锯齿圆环），透明背景融合任务栏：

```
[55] [18] [4]  │  Credits  $34.20  │  5h重置  14:32  │  ●12月20日
 Claude环 Codex环    分隔线    信息块         信息块        红点+日期
```

- 圆环 24px，线宽 2.5px，进度从顶部顺时针
- Claude 橙色 `#c66c32`，Codex 青色 `#2ea8b1`
- 进度颜色：0-59% 原色 → 60-84% 橙色 → 85-100% 红色
- 默认隐藏百分比号，只显示数字

## 插件选项

编辑 `AIUsage.ini`（TrafficMonitor 配置目录下），所有选项均有中文注释：

```ini
[AIUsage]
ShowPctSign=0          # 圆环内是否显示 %
ShowCredits=1          # 显示 Credits 金额
ShowReset=1            # 显示 5h 重置时间
ShowSubscription=1     # 显示订阅状态和日期
ShowStatus=1           # 数据过期时显示红色提示
CustomSubExpiry=       # 自定义订阅到期日, 如 2026-12-20
ProxyServer=           # 代理地址, 如 http://127.0.0.1:7890
RequireProxy=1         # 1=无代理时阻止请求(默认), 0=允许直连
```

## 代理安全

插件默认要求代理才能发起 API 请求，防止真实 IP + 凭证直接暴露给 Claude/Codex 服务器。

- 启动时自动检测系统代理配置（IE 代理、系统代理、PAC 脚本）
- 支持在 `AIUsage.ini` 中显式指定代理地址
- 无代理时任务栏显示"代理未配置"红色警告，所有数据项显示 `--`
- 设置 `RequireProxy=0` 可关闭此保护（不推荐）

## 凭据来源

### Claude（优先级从高到低）
1. **MSIX Claude Desktop** — `%LOCALAPPDATA%\Packages\Claude_*\...\config.json`（DPAPI + AES-GCM 解密）
2. **经典安装** — `%APPDATA%\Claude\config.json`
3. **CLI** — `%USERPROFILE%\.claude\.credentials.json`（支持 token 刷新和写回）

只读桌面版令牌，不影响桌面版登录态。

### Codex
- `~/.codex/auth.json`（支持 token 刷新和写回）

## 数据刷新

- 后台线程每 60 秒刷新，UI 线程只读缓存
- Claude/Codex 独立请求，互不影响
- 429 限流指数退避（120s ~ 900s）
- 失败保留最后一次成功数据
- 超过 15 分钟未更新标记为过期
- Profile API 仅首次请求一次（获取订阅状态）

## 安全

- 日志不记录 token、Authorization、Cookie 等敏感信息
- 不修改或删除 Claude/Codex 官方凭据文件
- 默认要求代理，防止真实 IP 暴露
- Token 刷新沿用参考项目的安全写入方式

## 构建

**环境要求**：MSVC v143 (Build Tools) + Windows 10 SDK + CMake 3.21+

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

产物：`build/Release/AIUsagePreview.dll`

### 部署

1. 复制 `AIUsagePreview.dll` 到 `TrafficMonitor/plugins/`
2. 复制 `AIUsage.ini` 到 TrafficMonitor 配置目录
3. 重启 TrafficMonitor
4. 在显示设置中启用 "AI Usage Dashboard"

## 文件结构

```
├── CMakeLists.txt
├── AIUsage.ini                    # 插件配置模板
├── LICENSE
├── README.md
├── include/
│   └── PluginInterface.h          # TrafficMonitor 插件接口
├── src/
│   ├── CodexUsagePlugin.cpp       # 主插件 + GDI 自绘
│   ├── CodexUsageFetcher.cpp/h    # Codex 令牌读取 + API 请求
│   ├── CodexUsageCore.cpp/h       # Codex JSON 解析 + tooltip
│   ├── ClaudeUsageFetcher.cpp/h   # Claude API + profile 请求
│   ├── ClaudeCredentialReader.cpp/h  # Claude 凭据读取（DPAPI）
│   ├── DpapiHelper.cpp/h          # Windows DPAPI + AES-GCM 解密
│   ├── ProxyHelper.cpp/h          # 代理检测和会话创建
│   ├── JsonLite.cpp/h             # 轻量 JSON 解析器
│   └── CodexUsageVersion.h.in     # 版本模板
├── tests/
│   └── CodexUsageCoreTests.cpp    # 单元测试
└── docs/
    ├── README.md                  # 详细文档
    └── design/
        ├── taskbar_preview.html   # HTML 设计稿
        └── preview_*.png          # 设计截图
```

## 致谢

本项目在以下开源项目基础上开发，感谢原作者的贡献：

- **[HCLonely/TrafficMonitor_Codex_Plugin](https://github.com/HCLonely/TrafficMonitor_Codex_Plugin)** — 本项目的核心基础。提供了 TrafficMonitor 插件框架、CMake 工程结构、Codex 用量获取逻辑、WinHTTP 封装、JSON 解析器、后台刷新机制和单元测试。本项目的 Codex 功能完全沿用其代码。

- **[huanchong-99/claude-usage-assistant](https://github.com/huanchong-99/claude-usage-assistant)** — Claude 用量获取的参考实现。本项目的 Claude 凭据读取（DPAPI 解密、Chromium Local State 密钥、MSIX/Desktop/CLI 多来源）、OAuth token 刷新、usage API 请求、429 退避策略等核心逻辑均参考并移植自该项目的 `quota_card.py`。

- **[bemaru/trafficmonitor-ai-usage-plugin](https://github.com/bemaru/trafficmonitor-ai-usage-plugin)** — 提供了 `PluginInterface.h` 头文件和 TrafficMonitor 插件开发参考。

- **[zhongyang219/TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)** — TrafficMonitor 本体，提供了插件接口和任务栏嵌入框架。

## 许可证

MIT License
