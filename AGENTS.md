# TrafficMonitor-AIUsage 协作规范

本文件适用于仓库内的人工协作与自动化代理。

## README 是功能说明入口

- 任何新增、修改或删除的功能、UI 行为、配置键、默认值、安全策略、数据源、
  构建方式或部署方式，都必须在同一次变更中同步更新 `README.md`。
- 不得只在提交信息、代码注释或聊天记录中描述公开行为；README 必须让新用户
  不阅读源码也能理解功能、限制、风险和配置方法。
- 配置模板 `AIUsage.ini`、代码默认值和 README 示例必须保持一致。

## 隐私与上传前检查

- 不得提交真实公网 IP、代理地址或端口、令牌、Cookie、账号、邮箱、订阅日期、
  本机用户名、绝对路径、日志或其他个人/机器信息。
- IPv4 示例使用 RFC 5737 地址（如 `203.0.113.10`），IPv6 示例使用 RFC 3849
  地址（如 `2001:db8::10`）；需要真实值的在线测试必须从环境变量读取。
- 发布前必须检查全部已跟踪文件和暂存差异，不得只检查 `AIUsage.ini`。
- 本机运行配置位于 TrafficMonitor 目录，不得复制回仓库模板或纳入提交。

## 构建、测试与部署

- CMake 缓存路径不一致时，删除明确确认的旧构建目录或使用新的构建目录；
  不覆盖来源不明的目录。
- Release 构建后运行 CTest，确认所有测试通过，再部署 DLL。
- 覆盖插件 DLL 前必须结束全部 TrafficMonitor 实例，并显式验证进程数为 0；
  若任一实例因权限不足无法结束，立即中止部署，不得继续启动新实例。
- 覆盖后只启动一次，并验证 TrafficMonitor 进程数严格等于 1。禁止在未确认旧
  实例退出时连续调用 `Start-Process`，否则旧 DLL、旧配置和任务栏占位会残留。
- 默认不创建 DLL 或目录备份，不生成 `_backup_*`；回退依赖 Git。只有用户明确
  要求时才创建额外备份。

## 同步 GitHub 前的本地验证（强制）

- **禁止推送未经本地完整验证的代码。** 每次 push / PR 到 GitHub 前，必须在本地跑通与
  CI 完全一致的流程；否则 CI 失败会向仓库所有者发送 GitHub 通知邮件，干扰正常邮件，
  **属严重问题，必须杜绝**。
- 本地验证步骤（与 `.github/workflows/build-and-release.yml` 对齐，逐 arch 验证 x64 至少）：
  1. 配置：`cmake -S . -B build/<arch> -G "Visual Studio 18 2026" -A <arch> -DCMAKE_BUILD_TYPE=Release`
  2. 构建**全部**目标（含测试）：
     `cmake --build build/<arch> --config Release --target AIUsage CodexUsageTests ProxyHelperTests`
     —— 不得只编部分目标；漏掉任一测试目标会导致 CTest 找不到 exe 而 `Not Run` 进而失败。
  3. 测试：`ctest --test-dir build/<arch> -C Release --output-on-failure`，必须 **0 失败**。
- 若 CI 仍因任何原因报错：先在本地复现、修复、重新验证通过后再推送，**不得依赖 CI
  反复试错**；尤其禁止在「本地没跑过完整流程」的状态下直接 push 指望 CI 兜底。
- 工作流里 Release/打 tag 步骤已做幂等处理（tag / release 已存在则跳过），但本地仍须
  确认主流程（构建+测试）全绿后再同步。

## Git 发布

- 发布前查看 `git status` 和完整 diff，只暂存本次范围内的文件。
- 不把构建目录、运行配置、凭据或日志加入版本控制。
- 提交前完成构建、测试、隐私扫描和 README 一致性检查。
- 从默认分支发布时使用独立 `agent/*` 分支并通过 PR 合并，除非用户明确要求
  直接更新默认分支。
