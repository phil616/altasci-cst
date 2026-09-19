# Runtime v2 使用与实现说明

v2 保留 Windows x64 / C++20 / Qt Widgets。关闭主窗口停止整个项目；任务的 Console 窗口可以分别打开、关闭，关闭 Console 不会停止任务。

## 从测试产物开始

完整解压便携包后运行 `cst.exe`，不要单独复制 EXE。目录中必须同时存在 `cst-runtime.exe`、`cst-signal-helper.exe` 和 Qt DLL。在任务的“命令”页选择运行方式，Console 的 mode 选择 `terminal` 即可交互；`pipes` 用于需要分离 stdout/stderr 的日志任务。

展开用户页“运行详情”，点击任务旁“打开 Console”。支持键盘输入、中文输入法提交、粘贴、Ctrl+C、窗口尺寸变化、VT 颜色、光标移动、清屏、进度回写和滚动历史；Ctrl+Shift+C 或“复制输出”复制文本。ConPTY 输出是合并终端流，不区分 stdout/stderr。兼容目标为 Python REPL、uv/npm 启动输出；尚不承诺所有全屏 TUI 的完整 VT/xterm 兼容。

## 命令配置

现有 `prepareCommands` / `serviceCommand` 字段继续保留，避免重写用户的命令与表单；`serviceCommand` 在 UI 中称为执行命令，`kind=task` 时它是一次性命令。v2 采用增量兼容格式，取代早期设计示例中的 `run`、`cwd` 字段；运行时通过 LaunchPlanner 生成 ProcessSpec。实际可导入示例见 [runtime-v2.example.json](../config/runtime-v2.example.json)。

| mode | program | 其他字段 | 实际调用 |
|---|---|---|---|
| exec | 原生 EXE 路径或最终 PATH 中的程序名 | arguments | 原生 argv，无 shell 解释 |
| python-venv | 留空 | venv、arguments | 环境内 Scripts/python.exe |
| uv | uv.exe 或明确路径 | toolArguments、arguments | uv run [工具选项] -- [目标命令及参数] |
| npm | node.exe 或明确路径 | npmCli 可选、npmAction、script、toolArguments、arguments | Node 调用原 npm CLI，保留 npm 生命周期与 scripts 语义 |
| shell | 留空 | script、activationScript 可选 | cmd /D /V:OFF /S /C |

npmAction 为 run/ci/install；run 时 script 填 package.json 的脚本名。npmCli 默认取该 Node 安装目录中的 node_modules/npm/bin/npm-cli.js，安装布局不同则显式指定。arguments 传到 npm 的 `--` 后，npm 自身仍按其脚本 shell 规则处理参数；不能把 npm script 当作完全无 shell 的原生 argv 接口。

uv 的 arguments 必须包含目标命令，例如 `["python", "-m", "app"]`；不要再写一次 run。venv 相对路径以命令 workingDirectory 为基准；相对 workingDirectory 以 source.workingDirectory 为基准。准备步骤可先创建后续需要的 venv、目录或环境文件，CST 在执行该命令时才检查和解析。

旧 activationScript 是兼容 shell 路径；不再提前把 python 固定到系统解释器。旧 exec + activationScript 遇到引号、百分号或换行参数会明确报错，避免错误展开。Python 环境优先迁移为 python-venv，工具模式不叠加激活脚本。

## 环境和任务

环境合并顺序为系统基础、项目环境、引用的具名环境（继承链）、任务环境、命令环境，随后添加工具目录、provider 环境和 CST 运行身份。具名环境位于 project.environments，任务用 environmentRef 引用。inheritSystem=false 保留明确的 SYSTEMROOT/WINDIR/COMSPEC/TEMP/TMP 系统运行变量，不回灌启动器 PATH。工具解析只使用该命令的最终 PATH，不猜 npm/pnpm 安装目录。

kind 默认 service；kind=task 成功退出为 Completed，timeoutMs 必须大于 0。长期服务 timeoutMs 为 0。prepareCommands 仍按顺序执行，不跨进程继承 shell 激活状态。

lifetime=root 为默认值，根进程退出结束 attempt；tree 允许根进程成功退出后，同一 Job 内的后代继续运行，Job 清空才结束。根异常退出不当作成功交接。跨 Job 的外部 daemon 不在托管范围。

restartPolicy.mode 支持 never/on_failure/always，删除固定 1500 ms 判据，按配置预算及退避执行；resetAfterSeconds=0 表示不因存活时间重置预算。服务在 never 模式退出，或 on_failure 模式成功退出，会结束项目并显示服务退出原因，而不会把长期服务当作一次性 Completed。必要服务失败时清理整个项目。

任务按 order 与 dependsOn 拓扑顺序串行启动。依赖条件可选 completed/started/ready，completed 必须指向一次性任务，ready 必须指向配置就绪检查的服务。readiness.type 支持 none/tcp/http，未配置探针时 Running 仅表示存活；Ready 表示配置探针通过，探针不能证明端口一定属于当前任务，也不等同持续健康监测。

新项目端口策略默认为 fail；旧项目缺省保留 reclaimConfigured。普通运行无需管理员权限；回收其他用户/系统服务端口可能被系统拒绝并显示 PID/Win32 错误。本版不自动触发 UAC 提权；确需旧版强制回收行为时可显式以管理员身份运行。

## 生命周期与模块边界

- LaunchPlanner 负责环境、路径、provider 和 argv；TaskSupervisor 只编排、监督与取消。
- cst-runtime 独立进程持有 TaskSupervisor、项目锁、Job、ConPTY/pipes 和过程日志。GUI 通过 RuntimeClient 的版本化本地 IPC 请求操作、订阅输出及控制输入。
- ProjectRuntimeService 保留 GUI 状态归约、配置与维护操作入口；同步使用同一项目文件锁，宿主运行时不能绕过锁替换目录。
- 每个 attempt 独立 Job 和会话。保留挂起创建、加入 Job 后恢复、kill-on-close；停止带宽限期，最后强制清理 Job，进程退出与输出排空分别处理。
- GUI 关闭取消准备和退避重启，逆序停止，清理成功后退出宿主。宿主监视 GUI 进程句柄，GUI 崩溃后也清理任务；宿主崩溃由 Job 句柄关闭清理后代。
- IPC 以当前用户 ACL、随机启动 token 和协议版本握手保护，变更请求带 requestId。状态快照携带输出序列，客户端使用 afterSeq 补读；8 MiB 环形缓冲覆盖旧数据时发送 Gap，GUI 和日志队列同样有上限。
- 一个 GUI 会话对应一个宿主；不提供脱离主窗口常驻或多客户端控制。Console 重开使用保留的屏幕模型，不重启任务。

过程日志在宿主写入，固定 projectId/runId/taskId/attemptId；GUI 不用当前操作重新标记旧输出。原始终端帧只用于有界内存显示，不写诊断日志，也不记录输入。pipes 日志和 CST 状态事件可搜索和导出；ConPTY 问题反馈请使用“复制输出”，并自行检查敏感内容。

## 配置升级与数据位置

v1/v2 配置都可读取。v1 加载到内存时升级版本号，保留原命令顺序、退出码、端口和关闭语义；首次将已有 v1 文件保存为 v2 时生成 `.v1.bak`。此前固定 1500 ms 判据不再保留。未知激活脚本不会自动猜测为 venv。

新安装使用 `%LOCALAPPDATA%\CST`。若检测到 `%ProgramData%\CST\catalog.json`，继续使用该旧目录，避免让已有项目消失；若旧目录对当前用户不可写，需要调整其权限或用旧版导出配置后迁移到新的用户目录。凭据仍在 Windows Credential Manager。

## 验证

新增 LaunchTests、RuntimeTests、TerminalTests 和 Windows RuntimeWindowsTests。Windows 流水线安装测试用 Python 3.12、Node 22、uv 0.12.17，验证真实 venv 创建、参数边界、uv 项目环境、npm pre/post、ConPTY 输入和尺寸、根退出后的后代清理。软件不自带这些生态工具，客户机器仍需安装并配置工具路径。

Windows 10/11 实机验收仍需用户完成，尤其是不同工具安装布局、中文字体/输入法、全屏程序和关闭时的系统行为。构建与自动测试结果以对应提交的 Actions 运行记录为准。
