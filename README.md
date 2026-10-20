# Codex Token HUD · C / C++ 试作版

参考 [Antigrative Dashboard](https://github.com/YOU-SHOULD-KNOW-ME/antigrative-dashboard)，为 Windows Codex 桌面聊天框增加本地统计条。

C23 负责有界 JSON 与数值校验；C++23 负责 Codex CLI、会话日志增量读取、Windows 进程生命周期和 CDP。聊天框属于 Web 渲染界面，`widget/inline.js` 是必要的 DOM 薄层。没有 Electron、Node 或 Python 常驻采集器，不修改 `app.asar`、登录文件或系统配置。

## 数据与边界

- 账户额度：调用现有 Codex CLI 的 `account/rateLimits/read`，复用 CLI 当前登录。查询会联网；不发起模型请求。CLI 和桌面若登录不同账户，额度以 CLI 账户为准。
- 显示服务端实际返回的额度组、时间窗口、剩余百分比和重置时间。未提供的窗口不显示占位；到重置时间不会自行填成 100%。
- 会话 Token：只匹配聊天框当前的本地会话 UUID。首读最多 2 MiB，随后增量读取；列表最多覆盖最近 7 个有日志日期中的 32 个会话。较早会话未匹配时显示不可用，可通过 `--session` 显式指定。
- 缓存率 = 累计已缓存输入 / 累计输入。Codex 的输入已包含缓存，输出已包含思考，不重复相加。
- 上下文为最近请求 `total_tokens / model_context_window` 的估算。不是累计输入比例，也不等同于 Codex 扣除保留基线后的剩余百分比。
- 速度 = 相邻 Token 事件的累计输出增量 / 事件时间差，显示最近有效采样，并在新事件到达后更新；包含工具等待，不等于逐 Token 流式解码速度。无有效事件时显示 —。历史日历/模型图表未实现。
- 重置额度仅在 Codex 主额度组的任一窗口剩余 ≤1%、存在可用重置次数、用户点击并确认时允许；原生后端会重新查询并再次校验。不会自动消耗重置次数。
- 状态栏位于模型选择器左侧；完全访问与模型选择器缩为 12px。控件只更新数值，不随采集切换宽窄布局。
- 鼠标悬停统计项满 1 秒后展开详情，移开会取消等待；点击和键盘聚焦可立即打开。
- 聊天切换复用同一控件，保留最多 32 个会话快照；未变化数值不重复改写。首次打开未缓存聊天需要一次采集。
- 只向当前本地调试通道发送白名单数值；不发送聊天正文、邮箱、账户 ID 或凭据。没有第三方遥测，不写统计副本。

## 使用

便携包中的 EXE、`widget` 和脚本必须放在一起。需要已安装并登录 ChatGPT 的 Codex CLI；实测接口版本为 `0.162.1`。

## 安装与运行

### 1. 一键安装到固定目录

```powershell
.\Start-Codex-Dashboard.ps1 -Install
```
默认安装至 `%USERPROFILE%\.codex\dashboard`，包含主程序、微件、启动与停止脚本及便捷批处理入口 (`start.cmd` / `stop.cmd`)。

### 2. 正常启动 / 挂载

```powershell
# 在安装目录或发布包中执行
.\Start-Codex-Dashboard.ps1
```

### 3. 守护进程 (Watchdog) 与自动恢复

当 Codex 客户端退出或意外关闭时，守护进程会自动重新拉起 Codex（带回环调试参数），并尝试重新挂载仪表盘。活动任务是否继续由 Codex 管理：

```powershell
# 前台监视模式（显示重启日志）
.\Start-Codex-Dashboard.ps1 -Watchdog

# 后台静默守护模式（常驻后台）
.\Start-Codex-Dashboard.ps1 -Watchdog -Daemon
```

### 4. 停止并撤下组件

```powershell
.\Stop-Codex-Dashboard.ps1
```
干净停止 Watchdog 守护进程与 C/C++ 采集器，并通过 CDP 撤下聊天框 DOM 控件，不影响当前会话内容。

### 5. 单次验证真实额度（输出脱敏 JSON）

```powershell
.\codex-dashboard.exe --once
```

原生 CLI 优先跟随 npm 的 Codex 安装，再从 PATH 定位，也可 `--codex "路径\codex.exe"`。`--codex-home` 仅选择会话存储目录，不替换 CLI 的登录环境。

## 构建

Windows，CMake ≥ 3.24，支持 C23 / C++23 的编译器。已用 MinGW GCC 16.2 构建。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

运行编译目录的 EXE 时补 `--widget "源码路径\widget\inline.js"`，或把 `widget` 复制到 EXE 旁。

CDP JSON 使用随源码提供的 nlohmann/json 3.12.0 单头文件，MIT 许可；最终 EXE 无需额外 C++ 运行库安装。Windows WinHTTP 是系统组件。

## 宿主兼容与验收

已按 Codex Owl 源码定位 `app://-` 页面、`data-codex-composer-root` 与本地会话 route scope。没有稳定公开挂载 API，后续客户端更新可能需要调整 DOM 适配。

已在 Windows Codex 桌面端验证实际挂载、更新、停止与重挂载。构建和回归范围见 `VALIDATION.md`；`widget/preview.png` 只使用虚构数据。

客户端正常退出后，使用安装目录 `start.cmd` 重开并挂载；脚本用注册应用激活接口传递本地调试参数，已运行却未开入口时会明确报告。升级安装前先运行 `stop.cmd`。守护模式是可选项，本次未启用自动重启或开机启动。

## 许可

本项目采用 AGPL-3.0-only，完整文本见 LICENSE。third_party/nlohmann 保留其 MIT 许可证。
