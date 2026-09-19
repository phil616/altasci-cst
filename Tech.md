# Customer Service Terminal（CST）v1 技术方案（历史记录）

文档版本：1.0  
基准日期：2026-09-17  
目标平台：Windows 10 1809+ / Windows 11，x64  
实现语言：C++20  
UI 框架：Qt Widgets 6.11.2  

> Runtime v2 已按用户后续要求重设计。启动、环境、任务、权限、进程分离、Console、日志和配置兼容行为以 [Runtime v2 实现说明](docs/runtime-v2.md) 为准；下文保留 v1 历史背景，不再约束这些已替换部分。

> v1 原文：本文是实现规范，不是选型建议。本文中的技术栈、状态、行为、目录、接口和验收条件均为确定性要求。随附的 `cst-project.schema.json` 与 `cst-project.example.json` 是本文的一部分。

## 0. 给 Codex 的执行契约

Codex 必须直接持续编码，直到形成可构建、可测试、可打包、可运行的完整 Windows 应用，不得将工作拆成“第一阶段/第二阶段”后停下等待确认。

执行时必须遵守以下规则：

1. 以本文为唯一架构决策来源，不得自行更换语言、GUI 框架、构建系统、配置格式、状态机、进程模型、同步算法或打包方式。
2. 必须完成 GUI、领域逻辑、Win32 平台层、配置导入导出、Git 同步、凭据存储、日志、测试夹具、自动化测试、部署脚本和安装包配置。
3. 只有外部不可提供的材料可以留为构建输入：正式代码签名证书、真实 Git 凭据、真实客户项目配置。不得以这些材料缺失为由停止编码；开发构建输出未签名安装包，生产打包预设在缺少签名材料时明确失败。
4. 不允许用 TODO、空实现、假数据、仅打印日志的占位函数替代本文要求的行为。
5. 每完成一处功能立即继续完成其测试及相邻功能；不要请求用户逐阶段批准。
6. 若实现细节与本文矛盾，以本文为准；若操作系统客观限制导致无法满足，必须以“阻止启动并显示可执行诊断”的方式失败，不能静默降级。
7. Windows 首版必须完整可用。macOS 本次不实现，但平台相关能力必须通过本文规定的接口隔离，禁止 Win32 头文件泄漏到领域层或 UI 层。

## 1. 产品边界与确定性结论

CST 是安装在客户电脑上的本地桌面应用，同时承担四个角色：

- 配置驱动的客户操作面板；
- 顺序工作流执行器；
- 长期服务进程监督器；
- 以远程 Git 仓库为绝对权威的部署副本同步器。

首版只支持 Windows x64。一个 CST 安装可以保存多个项目配置，但任意时刻只能有一个“当前项目”，也只能运行一个项目。切换、导入、删除和同步项目时，当前项目必须处于 `Stopped` 或 `Failed` 且没有残留托管进程。

管理员页与用户页只是 UX 分区，不是身份验证或安全边界：

- 不设置管理员密码；
- 顶部一级导航固定为“用户”和“管理员”；
- 管理员可以编辑会以管理员权限执行的命令；
- 导入的配置被视为可信的可执行配置；
- 用户页不显示命令编辑能力。

CST 不作为 Windows Service 运行，不把项目留在后台。关闭窗口、正常退出、会话注销或 CST 崩溃时，所有由 CST 托管的项目进程都必须终止。

## 2. 固定技术栈

| 项目 | 固定选择 | 说明 |
|---|---|---|
| 语言 | C++20 | 不引入 Rust、C#、Python 或 Node 作为 CST 自身运行时 |
| GUI | Qt Widgets 6.11.2 | 使用原生桌面控件与布局，不使用 Qt WebEngine，不嵌网页 |
| Qt 模块 | Core、Gui、Widgets、Network、Test | 仅动态链接官方 Qt 模块 |
| 编译器 | MSVC 2022，v143，x64 | `/W4 /permissive- /utf-8 /EHsc`，警告视为错误仅对 CST 自有代码开启 |
| Windows SDK | 10.0.26100 | Win32 API、资源、manifest、签名工具 |
| 构建系统 | CMake 4.4.3 + Ninja Multi-Config | 根目录必须提供 `CMakePresets.json` |
| 安装器 | Qt Installer Framework 4.11.0 | 生成 x64 `.exe` 安装包 |
| Git 基准 | Git for Windows 2.55.0 | 作为集成测试版本；运行时允许管理员配置 `git.exe` 绝对路径，但最低版本为 2.45.0 |
| 配置 | JSON + JSON Schema Draft 2020-12 | Schema 版本固定为 1；未知字段拒绝 |
| 测试 | Qt Test + CTest | 不增加第三方测试框架 |
| 外部库 | 无 | 除 Qt 与 Windows SDK 外不引入运行时/源码第三方依赖 |

Qt 采用 LGPLv3 动态链接方式交付：Qt DLL 保持独立，安装包附带 Qt LGPLv3 文本、Qt 版权声明、所用模块列表和对应 Qt 源码获取地址；不得静态链接社区版 Qt。

## 3. 进程与权限模型

### 3.1 强制最高权限

`cst.exe` 的嵌入式 application manifest 必须设置：

```xml
<requestedExecutionLevel level="requireAdministrator" uiAccess="false" />
```

启动后的第一段业务代码必须完成：

1. 用 `OpenProcessToken` 与 `GetTokenInformation(TokenElevation)` 验证提升状态；
2. 用 `LookupPrivilegeValueW` 和 `AdjustTokenPrivileges` 尝试启用 `SeDebugPrivilege`；
3. 再次读取 `GetLastError()`，只有 `ERROR_SUCCESS` 才视为成功；
4. 任一步失败就显示阻塞错误对话框，说明“CST 必须以管理员权限运行并取得调试权限”，随后以非零状态退出；
5. 不提供普通权限只读模式或降级模式。

所有项目命令继承 CST 的提升令牌，这是有意设计。配置、源码及命令因此都属于可信输入。

### 3.2 单实例

每个 Windows 登录用户只允许一个 CST 实例：

- 互斥体名：`Local\\CST-<current-user-SID>`；
- 第二实例通过 `QLocalSocket` 向首实例发送 `activate`，首实例恢复并前置主窗口；
- 第二实例发送成功或超时后立即退出；
- 不允许两个实例同时操作同一个项目目录。

### 3.3 进程启动不使用 QProcess 作为托管核心

Qt 的 `QProcess` 继续用于测试辅助和非关键工具探测，但项目命令、Git 和帮助进程统一通过 `IProcessRunner` 运行。Windows 实现使用 `CreateProcessW`，原因是必须在子进程执行前将其放入 Job Object。

固定启动顺序：

1. 创建 stdout/stderr 匿名管道，只把必要句柄设为可继承；
2. 创建 Job Object；
3. 设置 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，不设置任何 breakaway 标志；
4. 创建 I/O completion port 并与 Job 关联；
5. 用 `CreateProcessW` 启动，标志固定为 `CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE`，并在 `STARTUPINFO` 中用 `STARTF_USESHOWWINDOW + SW_HIDE` 隐藏控制台；
6. 用 `AssignProcessToJobObject` 将尚未执行的子进程加入 Job；
7. 赋值失败时终止该子进程并返回失败；
8. 调用 `ResumeThread`；
9. 两个专用读取线程读取 stdout/stderr；优先按 UTF-8 严格解码，失败后按系统 OEM/ANSI 代码页（中文 Windows 常见 936/GBK）解码，再转成带时间戳的日志并通过 queued signal 送到 UI。ANSI 控制序列不会原样显示，基础颜色会映射为日志视图颜色。
10. completion-port 线程监听进程退出和活动进程数变为 0 的通知。

每条一次性命令和每个长期服务分别拥有 Job Object。长期服务启动的 `npm -> node -> vite`、`uv -> python -> uvicorn` 等后代默认都会留在同一 Job 中。

### 3.4 命令的两种精确语义

命令不是一个含义不清的字符串。配置只允许下面两种互斥模式：

- `exec`：`program + arguments[]`。CST 先把程序解析为绝对路径，再将绝对路径作为 `CreateProcessW.lpApplicationName`。`.cmd`、`.bat` 和 shell 内建命令不得使用此模式。
- `shell`：`script`。Windows 固定使用 `%SystemRoot%\\System32\\cmd.exe /D /S /C <script>`。只有需要批处理文件、管道、重定向、`&&` 或 shell 内建命令时使用。

命令可选字段 `activationScript` 指定一个 Windows 批处理激活脚本（例如 `{{PROJECT_DIR}}\\.venv\\Scripts\\activate.bat`）。该脚本会在命令执行前通过 `call` 生效；`exec` 模式会因此被包装为 cmd 命令，`shell` 模式会直接前置 `call`。准备命令和长期服务命令各自独立激活，不共享激活后的环境。

`exec` 的程序查找顺序固定为：绝对路径 → 项目配置的 `toolDirectories`（按数组顺序）→ CST 启动时继承的 `PATH`。不搜索当前工作目录；解析失败直接报错。`shell` 模式同样会把 `toolDirectories` 以及常见 Node/pnpm 用户目录前置到子进程 `PATH`，避免提升权限后继承环境缺少用户安装的命令。参数使用统一的 Windows CRT 反向引用算法生成命令行，并用单元测试覆盖空参数、空格、引号和尾部反斜杠。

环境变量合并顺序固定为：

1. `inheritSystem=true` 时复制 CST 启动环境；
2. 按 `envFiles[]` 顺序加载，后者覆盖前者；
3. `variables` 覆盖同名值；
4. 注入 `CST_PROJECT_ID`、`CST_PROJECT_DIR`、`CST_DATA_DIR`、`CST_LOG_DIR`；
5. Windows 环境键按不区分大小写合并，最终生成排序的 Unicode 双 NUL 结尾环境块。

`.env` 解析仅支持 UTF-8（允许 BOM）、空行、以 `#` 开头的注释和 `KEY=VALUE`；支持成对单/双引号，去除外层引号；不执行变量插值、不执行转义、不支持 `export`。格式不符即阻止启动。

### 3.5 停止策略

停止单个托管 Job 时：

1. 调用内置 `cst-signal-helper.exe <root-pid>`；该帮助程序先忽略自身控制事件，`AttachConsole(root-pid)` 后用 process group 0 向该隐藏控制台内所有进程发送 `CTRL_BREAK_EVENT`；
2. 等待任务的 `shutdownGraceMs`；
3. 尚未退出则 `TerminateJobObject(job, CST_EXIT_FORCED)`；
4. 等待 Job 活动进程数归零；
5. 关闭 Job 句柄。

如果 CST 被强制结束，最后一个 Job 句柄关闭会触发 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，作为全进程树兜底。停止是幂等操作。

## 4. 领域模型与顺序规则

### 4.1 聚合关系

```text
Project
├─ SourceSyncSpec（独立维护操作）
├─ RequiredPort[]
├─ ServiceTask[]（按 order 顺序启动）
│  ├─ PrepareCommand[]（一次性、严格串行）
│  ├─ ServiceCommand（恰好一条长期命令）
│  ├─ ReadinessProbe[]
│  └─ RestartPolicy
└─ UserAction[]（运行时动态生成按钮）
```

不再用一个泛化的 `task` 同时表示安装、长期服务和 Git 同步：

- `PrepareCommand` 是必须退出的一次性命令；
- `ServiceCommand` 是预期持续运行的命令；
- Git 同步是 `SourceSyncService` 的固定算法，不允许配置任意 Git 命令；
- 用户动作首版只允许打开 HTTP/HTTPS URL。

### 4.2 一键启动的严格顺序

所有任务按 `order` 升序执行，不并行。相同 `order` 为配置错误。

对每个任务：

1. 顺序执行 `prepareCommands`；只有进程正常退出且 exit code 位于 `successExitCodes` 才成功；
2. 任一准备命令失败、崩溃或超时，终止当前命令，停止此前已经启动的服务（任务逆序），项目进入 `Failed`；
3. 启动唯一的 `serviceCommand`；
4. 服务进程创建成功即视为任务进入 `Running`，不等待端口或就绪探针；
5. 当前任务进入 `Running` 后才启动下一个任务；
6. 所有任务进入 `Running` 后项目才进入 `Running`。

长期服务一旦进入 `Running`，任何非用户停止导致的退出都属于意外退出，无论 exit code 是否为 0；`successExitCodes` 对长期服务只用于日志展示，不改变重启判定。启动后 1500 ms 内退出视为启动失败，项目立即失败并输出诊断日志，不进入重启循环。

不设就绪探针机制，也不要求长期服务绑定端口。`requiredPorts` 只用于启动前端口占用检查和强制释放。

### 4.3 长期托管与自动重启

服务正常运行后若根进程或整个 Job 意外结束：

1. 清理原 Job 的残余进程；
2. 只重启 `serviceCommand`，不重新执行准备命令；
3. 退避时间依次为 1、2、4、8、15、30 秒，之后保持 30 秒；
4. 在滑动 10 分钟窗口内最多允许 5 次重启；
6. 达到限制或重启失败时，该任务进入 `Failed`，随后停止其他任务（逆序），项目进入 `Failed`；
7. 用户手动停止、关闭 CST 或开始退出时不触发自动重启。

配置中的 `restartPolicy` 固定要求 `mode=on_failure`，其余数值允许在 Schema 范围内由管理员填写；示例值即默认创建值。

### 4.4 停止与关闭

一键停止：

- 项目进入 `Stopping`；
- 禁止启动、同步、切换和编辑配置；
- 按任务 `order` 降序停止；
- 无论某个任务停止是否报错，都继续清理其余任务；
- 确认所有 Job 无活动进程后进入 `Stopped`。

点击窗口右上角关闭：

- 如果已经停止，立即退出；
- 否则拦截第一次 close event，显示不可交互的“正在停止所有项目…”覆盖层，执行完整停止流程，然后自动关闭；
- 不弹出“是否保持后台运行”，不缩入托盘；
- Windows 会话结束时立即开始停止；若系统不给足宽限期，Job 句柄关闭仍负责强杀。

## 5. 项目状态机

项目状态只能按下表转换：

| 当前状态 | 事件 | 下一状态 |
|---|---|---|
| `Stopped` | start | `Preflight` |
| `Failed` | start | `Preflight` |
| `Preflight` | checks passed | `ReclaimingPorts` |
| `Preflight` | check failed | `Failed` |
| `Preflight` | stop/close | `Stopping` |
| `ReclaimingPorts` | ports free | `Starting` |
| `ReclaimingPorts` | reclaim failed | `Failed` |
| `ReclaimingPorts` | stop/close | `Stopping` |
| `Starting` | all tasks ready | `Running` |
| `Starting` | stop/close | `Stopping` |
| `Starting` | task failed | `Stopping`，清理后 `Failed` |
| `Running` | stop/close | `Stopping` |
| `Running` | unrecoverable crash | `Stopping`，清理后 `Failed` |
| `Stopping` | all jobs empty | `Stopped` 或保留故障原因后 `Failed` |
| `Stopped`/`Failed` | sync | `Syncing` |
| `Syncing` | success | `Stopped` |
| `Syncing` | failure | `Failed`，旧工作副本仍可恢复 |
| `Syncing` | close | 取消 Git、删除 staging、恢复/保留有效 target 后退出 |

状态变更只能由 `ProjectRuntimeService` 的单写入事件队列发起。UI 不得直接修改状态。

## 6. 端口占用与强制释放

### 6.1 枚举与冲突规则

Windows 实现使用：

- TCP IPv4/IPv6：`GetExtendedTcpTable`；
- UDP IPv4/IPv6：`GetExtendedUdpTable`；
- 进程父子关系：`CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)`；
- 服务 PID：`EnumServicesStatusExW(SC_ENUM_PROCESS_INFO)`。

TCP 只把 `LISTEN` 行视为占用；UDP 的绑定行视为占用。`0.0.0.0`/`::` 与同端口任意具体地址冲突；具体地址只与同地址及通配地址冲突。PID 0 的 TCP `TIME_WAIT` 不是占用者。

### 6.2 强制释放算法

在启动任何任务前，对全部 `requiredPorts` 顺序执行：

1. 枚举当前占用 PID；
2. 若 PID 属于 CST 的旧 Job，直接 `TerminateJobObject`；
3. 若 PID 对应 Windows Service，先 `ControlService(SERVICE_CONTROL_STOP)`，等待 2 秒；端口仍占用则终止服务进程；
4. 普通进程使用 Toolhelp 快照取得其后代，按叶到根顺序 `TerminateProcess`；最后终止占用 PID；每次终止都 `WaitForSingleObject` 确认退出；
5. 每 250 ms 重新枚举端口，最多持续 `portReclaimTimeoutMs`；
6. 若相同父 PID 反复生成新的占用者两次，向上提升一级，把该父进程及其全部后代作为终止目标；继续重复，最多提升 `maxAncestorEscalation` 层；
7. 不允许终止 PID 0、PID 4、CST 自身以及 `smss.exe`、`csrss.exe`、`wininit.exe`、`winlogon.exe`、`lsass.exe`。遇到这些系统边界、PPL 保护或任何无法取得终止权限的占用者时，必须终止启动流程并显示 PID、映像路径、端口和 Win32 错误；
8. 所有要求端口连续 500 ms 无占用后才进入 `Starting`。

这里没有“询问是否杀进程”的确认框。启动项目即代表授权 CST 释放配置中的所有端口。

### 6.3 启动后的端口验证

长期服务不再等待端口或运行额外探测。`requiredPorts` 只用于启动前的端口占用检查和强制释放，不再通过就绪机制决定任务是否运行成功。

## 7. Git 代码同步：远程仓绝对权威

### 7.1 明确语义

项目工作目录是可丢弃的部署副本，不是用户工作区。同步后：

- 本地已跟踪修改不保留；
- 未跟踪文件、忽略文件、构建产物和嵌套工作树不保留；
- 远端目标分支是唯一权威；
- 运行数据、日志、凭据、环境文件必须放在 CST 数据目录，不得放在 Git 工作目录中。

同步按钮只在 `Stopped` 或 `Failed` 且所有 Job 已空时启用。运行、启动、停止期间完全禁用；后端还必须再次检查，不能只依赖按钮状态。

### 7.2 不采用原地 reset/clean

实现使用事务式新克隆与目录交换，固定流程如下：

1. 获取 `ProjectOperationMutex` 和项目专属 `QLockFile`；锁路径为 `%ProgramData%\\CST\\locks\\<project-id>.lock`；
2. 确认项目未运行、无托管 Job、配置未被编辑；
3. 验证 `git.exe --version`，版本必须不低于 2.45.0；
4. 在工作目录同一父目录创建 `.<dirname>.cst-staging-<uuid>`；
5. 通过 CST AskPass 凭据环境运行：

   ```text
   git clone --branch <branch> --single-branch --recurse-submodules <repositoryUrl> <staging>
   ```

6. 在 staging 中运行：

   ```text
   git submodule sync --recursive
   git submodule update --init --recursive --force
   git status --porcelain=v1 --untracked-files=all
   git rev-parse HEAD
   ```

7. `git status` 必须为空，HEAD 必须可解析；否则删除 staging 并失败，现有目录不变；
8. 写入 `%ProgramData%\\CST\\state\\<project-id>\\sync-journal.json`，记录 target、staging、backup 和阶段；每次更新用 `QSaveFile` 原子提交；
9. 若 target 存在，用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 将其重命名为 `.<dirname>.cst-backup-<uuid>`；
10. 将 staging 重命名为 target；
11. 若第二次重命名失败，立即把 backup 恢复为 target；恢复失败时停止一切操作，显示三个绝对路径供实施人员处理；
12. 新 target 就位后删除 backup；删除失败不回滚成功同步，而是登记到 cleanup queue 并在下次启动重试；
13. 清除 journal，释放锁，记录新 HEAD。

由于每次使用全新 clone，原工作目录里的全部本地内容都会被替换，严格符合“远程仓绝对权威”。网络中断、鉴权失败或 clone 失败发生在交换前，因此不会损坏现有可运行副本。

### 7.3 同步崩溃恢复

应用启动时先扫描 sync journal：

- target 存在：把残余 staging/backup 加入清理队列，保留 target；
- target 不存在且 backup 存在：先恢复 backup 为 target；
- target 不存在、backup 不存在、staging 完整且 Git 校验通过：把 staging 提升为 target；
- 三者都无法形成有效工作副本：项目标记 `Failed` 并阻止启动。

### 7.4 Git 凭据

仓库 URL 必须是无 userinfo 的 `https://` URL，禁止 `https://user:token@host/...`。

管理员页将用户名和 PAT 写入 Windows Credential Manager：

- 类型：`CRED_TYPE_GENERIC`；
- target：配置中的 `credentialTarget`，格式固定为 `CST/git/<project-id>/<host>`；
- persistence：`CRED_PERSIST_LOCAL_MACHINE`；
- JSON 只保存 target 名称，不保存秘密。

安装包包含 `cst-git-askpass.exe`。Git 子进程环境固定设置：

```text
GIT_TERMINAL_PROMPT=0
GIT_ASKPASS=<absolute path>\\cst-git-askpass.exe
GIT_ASKPASS_REQUIRE=force
CST_CREDENTIAL_TARGET=<credential target name>
```

AskPass 根据 Git prompt 中的 `Username` 或 `Password` 返回对应值；其他 prompt 返回非零。stdout 只输出凭据值，任何日志写 stderr 且不得包含秘密。CST 日志脱敏 URL userinfo、Authorization、token、password、credential blob。

## 8. 配置、持久化与目录

### 8.1 固定目录

```text
%ProgramFiles%\CST\
  cst.exe
  cst-git-askpass.exe
  cst-signal-helper.exe
  Qt6*.dll / platforms / styles

%ProgramData%\CST\
  catalog.json
  projects\<project-id>\project.json
  data\<project-id>\env\*.env
  state\<project-id>\runtime.json
  state\<project-id>\sync-journal.json
  logs\<project-id>\cst.log
  logs\<project-id>\tasks\<task-id>.log
  locks\<project-id>.lock
```

源码工作目录由配置指定，示例为 `C:\\CSTProjects\\<project-id>`，必须是绝对路径且不得位于 `%ProgramFiles%\\CST`、`%ProgramData%\\CST` 或 Windows 系统目录内。

`catalog.json` 只保存项目 ID、显示名、配置路径、默认项目 ID 和最后选择 ID。所有 JSON 写入使用 `QSaveFile`；敏感凭据不进入文件。

### 8.2 配置加载与变量

启动时：恢复同步事务 → 读取 catalog → 读取默认项目 → 按随附 Schema 和运行时交叉约束验证 → 显示用户页。配置无效时进入管理员“配置问题”页，不允许运行。

字符串只展开下列占位符：

- `{{PROJECT_DIR}}`
- `{{DATA_DIR}}`
- `{{LOG_DIR}}`

三者分别固定映射为源码工作目录、`%ProgramData%\\CST\\data\\<project-id>`、`%ProgramData%\\CST\\logs\\<project-id>`。

未知占位符、递归展开或展开后产生相对工作目录都视为错误。shell script 也允许这些占位符，但 CST 只做字面替换和 cmd 引用，不做其他模板求值。

### 8.3 运行时交叉验证

JSON Schema 之外还必须检查：

- 项目、任务、命令、动作 ID 全局规则及各自集合唯一；
- task `order` 唯一；action `order` 唯一；
- 每个 `ownerTaskId` 必须存在；
- `serviceCommand.timeoutMs` 必须为 0；准备命令 timeout 必须大于 0；
- `exec` 的 program 不能是 `.cmd` 或 `.bat`；
- URL 只允许 `http`、`https`，必须是绝对 URL；
- Git URL 只允许 HTTPS 且不含用户名、密码、查询串或 fragment；
- 命令级工作目录、env 文件和 Git 可执行文件路径必须符合 Windows 绝对路径规则；
- 端口号与 protocol/address 组合唯一。

## 9. 桌面 UX 规范

### 9.1 总体外壳

主窗口最小尺寸 1040×680，默认 1180×760，支持 125%–250% DPI。使用 `QMainWindow`：

- 顶部：应用图标、当前项目名称、项目状态胶囊、一级分段导航“用户 / 管理员”；
- 内容区：`QStackedWidget`；
- 底部状态栏：版本、当前 Git HEAD 短哈希、最近一次操作结果；
- 不使用浏览器风格标签页，不使用巨型单页表单，不嵌 WebView。

视觉实现使用 Windows 系统字体和 `QPalette`，只对主操作、导航选中态和状态胶囊做局部 QSS，不建立仿网页的全局 CSS。左侧管理员导航固定宽度 220 px，导航行高 40 px，内容区外边距 24 px，表单纵向间距 12 px。主蓝色为 `#2563EB`，停止红色为 `#C62828`，处理中琥珀色为 `#B26A00`；悬停只调整 8% 明度。普通设置继续使用系统按钮、列表、表格、分隔线与对话框外观，不使用大面积圆角卡片、渐变背景或网页式顶部导航栏。

### 9.2 用户页

用户页只有四层信息：

1. 当前项目名称和一句说明；
2. 主操作按钮；
3. 动态动作按钮网格；
4. 可折叠“运行详情”。

主按钮状态固定：

| 项目状态 | 文本 | 颜色 | 可用性 |
|---|---|---|---|
| `Stopped`/`Failed` | 一键启动 | 主蓝色 | 可用 |
| `Preflight` | 一键停止 | 红色 | 可用；点击取消启动 |
| `ReclaimingPorts` | 一键停止 | 红色 | 可用；点击取消启动 |
| `Starting` | 一键停止 | 红色 | 可用；点击取消并清理已启动任务 |
| `Running` | 一键停止 | 红色 | 可用 |
| `Stopping` | 正在停止… | 琥珀色 | 不可用 |
| `Syncing` | 正在同步代码… | 琥珀色 | 不可用 |

用户点击“一键启动”后，按钮必须在同一轮 UI 事件中立即变为红色“一键停止”，不得等到全部服务启动完成后再改变。检查、释放端口和顺序启动期间，按钮始终可以取消本次启动。

动态动作由 `userActions` 运行时创建 `QPushButton`，按 `order` 排序放入两列 `QGridLayout`，窗口窄于 900 px 时变为一列。按钮使用系统默认浏览器调用 `QDesktopServices::openUrl`；`availableWhen=running` 的按钮只在项目 `Running` 时启用。打开失败必须显示错误，不改变项目状态。

“运行详情”显示任务列表、任务状态、重启次数和最近 5000 行日志；默认折叠，避免干扰普通用户。

### 9.3 管理员页

管理员页采用左侧本地导航 + 右侧页面 + 顶部面包屑，不把所有设置堆在同一面板。固定页面：

1. `项目 / 概览`：项目元数据、当前状态、默认项目；
2. `项目 / 任务与命令`：左侧任务列表，右侧任务详情；准备命令可上移/下移；命令模式字段互斥显示；
3. `项目 / 端口`：端口表格、任务归属、立即检测；
4. `项目 / 用户按钮`：按钮预览、排序、URL、可用条件；
5. `维护 / 代码同步`：仓库、分支、工作目录、Git 路径、凭据状态、测试认证、同步按钮和最近 HEAD；
6. `维护 / 环境与凭据`：toolDirectories、env 文件编辑入口、凭据写入/删除；凭据永不回显；
7. `诊断 / 日志`：按任务筛选、搜索、复制、打开日志目录、导出诊断包；
8. `配置 / 导入导出`：Schema 校验结果、导入、导出、设为默认、项目切换。

管理员编辑采用工作副本：字段变化先写入内存，底部显示“保存 / 放弃”。保存时完整验证并用 `QSaveFile` 替换配置。项目非停止状态时所有可影响运行的字段只读。

危险同步页固定显示说明：“同步会以远程分支替换整个本地项目目录，本地修改和生成文件不会保留。”这是信息说明，不再弹二次确认；点击同步即执行。

### 9.4 键盘与可访问性

- 所有功能可通过 Tab/Shift+Tab 到达；
- 主操作按钮是用户页默认按钮，但 Enter 不得在管理员表单中误触发启动；
- 颜色之外同时使用文字和图标表达状态；
- 控件设置 accessible name；
- 中文为首发语言，源代码字符串统一 UTF-8；
- 不使用小于 12 px 的正文，也不固定像素高度导致高 DPI 截断。

## 10. 应用架构与接口

采用分层单体，禁止 UI 直接调用 Win32 或 Git：

```text
UI (Qt Widgets)
  -> Application Services
      -> Domain
      -> Platform Interfaces
          -> Windows Implementations
```

核心接口固定为：

```cpp
class IProcessRunner;
class IManagedProcess;
class IPortManager;
class IPrivilegeService;
class ICredentialStore;
class IFileTransaction;
class IUrlLauncher;
class IClock;
```

主要应用服务固定为：

```cpp
class ProjectCatalogService;
class ProjectConfigService;
class ProjectRuntimeService;
class TaskSupervisor;
class PortReclaimService;
class SourceSyncService;
class DiagnosticExportService;
class LogService;
```

`ProjectRuntimeService` 持有串行 `OperationQueue`。`start`、`stop`、`sync`、`switchProject`、`saveConfig` 都进入同一队列；队列任意时刻只执行一个变更操作。耗时工作在专用 `QThread`/标准线程中完成，结果通过 queued signal 回到 UI，禁止阻塞 GUI 线程。

## 11. 仓库结构

```text
CST/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ LICENSES/
├─ README.md
├─ cmake/
│  ├─ DeployQt.cmake
│  └─ Signing.cmake
├─ config/
│  ├─ cst-project.schema.json
│  └─ cst-project.example.json
├─ resources/
│  ├─ app.manifest
│  ├─ cst.rc
│  ├─ icons/
│  └─ styles/
├─ src/
│  ├─ domain/
│  ├─ application/
│  ├─ infrastructure/common/
│  ├─ infrastructure/windows/
│  ├─ ui/widgets/
│  ├─ ui/pages/user/
│  ├─ ui/pages/admin/
│  └─ app/main.cpp
├─ helpers/
│  ├─ git-askpass/main.cpp
│  └─ signal-helper/main.cpp
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  └─ fixtures/
├─ installer/
│  ├─ config/config.xml
│  └─ packages/com.cst.app/
└─ scripts/
   ├─ build.ps1
   ├─ test.ps1
   └─ package.ps1
```

CMake targets固定为：

- `cst_domain`：纯领域模型，不链接 Widgets/Win32；
- `cst_application`：状态机与服务；
- `cst_platform_windows`：Win32 实现，链接 `Advapi32 Iphlpapi Ws2_32 Shell32 User32`；
- `cst_ui`：Widgets 页面；
- `cst`：GUI executable；
- `cst-git-askpass`；
- `cst-signal-helper`；
- `cst-test-port-owner`：测试用监听/子进程/重生夹具，不进安装包。

## 12. 日志与诊断

日志格式为单行 UTF-8 JSON Lines，字段固定为：

```json
{"ts":"2026-09-17T12:00:00.123Z","level":"info","projectId":"...","taskId":"backend","operationId":"...","event":"process.stdout","message":"..."}
```

规则：

- CST 主日志 10 MiB × 5；每任务日志 20 MiB × 5；
- 写入在单独日志线程完成；
- UI 只保存最近 5000 行；
- stdout/stderr 每行标记 channel；无法解码的字节用 U+FFFD 替换并记录 `decodeError=true`；
- token、password、Authorization、URL userinfo 和 Credential blob 必须脱敏；
- 每个用户操作生成 UUID `operationId`，贯穿 UI、状态机、子命令与同步日志。

诊断包是 zip，包含脱敏后的配置、catalog、runtime、最近日志、CST 版本、Windows 版本、Git 版本、端口快照和任务状态；不包含 env 文件内容、凭据、项目源码。实现先把这些文件复制到受控临时目录，再调用 Windows 自带 `%SystemRoot%\\System32\\tar.exe -a -c -f <output.zip> -C <temp> .`，成功后删除临时目录；不得调用 PowerShell 脚本或引入压缩库。

## 13. 构建、部署与签名

### 13.1 预设与命令

必须提供 `windows-debug`、`windows-release`、`windows-package` 三个 CMake workflow preset。脚本调用预设，不复制构建逻辑。

```powershell
cmake --workflow --preset windows-debug
cmake --workflow --preset windows-release
cmake --workflow --preset windows-package
```

`windows-debug` 和 `windows-release` 都执行 configure → build → test；`windows-package` 在 release 测试通过后执行安装树生成、`windeployqt`、签名和 Qt IFW `binarycreator`。

### 13.2 部署

`windeployqt` 必须从所选 Qt 6.11.2 MSVC x64 安装运行，复制 Core/Gui/Widgets/Network 所需 DLL、platforms、styles 和 MSVC runtime，不复制 WebEngine/QML。

安装位置固定为 `%ProgramFiles%\\CST`，开始菜单项名为“Customer Service Terminal”，桌面快捷方式默认创建。应用数据不随普通升级删除。

开发包允许未签名，并在文件名加 `-unsigned`。生产预设固定读取：

```text
CST_SIGN_PFX
CST_SIGN_PFX_PASSWORD
CST_SIGN_TIMESTAMP_URL
```

缺少任一变量时生产预设失败；存在时用 Windows SDK `signtool.exe` 先签所有 EXE/DLL，再签安装包，并在打包完成后执行 `/pa /all` 验证。

## 14. 测试要求

### 14.1 单元测试

必须覆盖：

- JSON Schema 等价运行时校验及所有交叉约束；
- Windows 参数引用；
- `.env` 解析与覆盖顺序；
- 状态机所有合法/非法转换；
- 顺序执行、失败短路、逆序停止；
- 重启滑动窗口与退避；
- 地址/端口冲突判断；
- URL scheme 限制；
- 日志脱敏；
- sync journal 的每个崩溃阶段恢复；
- 占位符展开和路径限制。

### 14.2 Windows 集成测试

管理员标签 `admin` 的测试必须使用 `cst-test-port-owner`：

- 监听 TCP/UDP IPv4/IPv6；
- 生成多级子进程并让叶进程监听；
- 父进程自动重生监听子进程；
- 验证 CST 能杀叶节点、后代和必要上游，最终释放端口；
- 验证系统保护边界会导致明确失败而非死循环；
- 验证 Job close 杀死全部后代；
- 验证关闭窗口后端口全部释放。

Git 集成测试使用本地 bare repository，不依赖公网：创建两个提交和子模块，修改现有工作副本并添加 ignored/untracked 文件，再执行同步，断言最终目录与远端 HEAD 一致且本地文件消失。还要在交换两次 rename 之间注入失败，验证旧目录恢复。

### 14.3 UI 测试

使用 Qt Test 验证：

- 用户/管理员一级导航；
- 动态按钮数量、顺序、禁用条件；
- 主按钮文本、颜色属性和状态转换；
- 运行时编辑控件只读；
- close event 等待停止后退出；
- 125%、150%、200% DPI 下关键控件无截断。

## 15. 完成定义与验收清单

只有下列条件全部满足，Codex 才能声称项目完成：

- [ ] Windows 10 1809+ 与 Windows 11 x64 可启动；非提升状态严格拒绝；
- [ ] 可以创建、编辑、校验、导入、导出、切换并设定默认项目；
- [ ] 准备命令严格顺序执行，长期服务进程创建成功即进入 Running；
- [ ] 多个任务严格按 order 启动、逆序停止；
- [ ] 服务崩溃按规定重启，超限后整体失败并清理；
- [ ] 端口占用者被强制清理，重生者触发上游升级，无法清理时禁止启动；
- [ ] 所有托管子孙进程属于 Job，关闭 CST 后不残留；
- [ ] 主按钮按状态变色并在 Running 时显示“一键停止”；
- [ ] 右上角关闭会停止所有项目；
- [ ] 动态 HTTP/HTTPS 按钮由配置实时渲染并使用默认浏览器；
- [ ] Git 同步在运行时被前后端双重禁止；
- [ ] 同步以全新远程副本替换本地目录，不保留任何本地变化；
- [ ] 同步失败或交换崩溃不会留下不可恢复的半目录；
- [ ] Token 只进入 Credential Manager，从不进入 JSON、命令行或日志；
- [ ] 日志轮转、脱敏、诊断包可用；
- [ ] 所有单元测试和非管理员集成测试通过；提升环境下管理员测试通过；
- [ ] `windows-package` 生成包含 Qt 运行库的可安装 EXE；
- [ ] 仓库内没有 TODO、空实现、占位页面或未接线按钮。

## 16. 官方参考资料（已于基准日期核验）

### Qt 与构建

- [Qt 6.11 支持平台](https://doc.qt.io/qt-6/supported-platforms.html)
- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- [Qt Widgets 布局动态增删控件](https://doc.qt.io/qt-6/qboxlayout.html)
- [QDesktopServices 打开系统 URL](https://doc.qt.io/qt-6/qdesktopservices.html)
- [QJsonDocument](https://doc.qt.io/qt-6/qjsondocument.html)
- [QSaveFile 原子保存](https://doc.qt.io/qt-6/qsavefile.html)
- [QLockFile](https://doc.qt.io/qt-6/qlockfile.html)
- [QNetworkAccessManager](https://doc.qt.io/qt-6/qnetworkaccessmanager.html)
- [Qt 6 CMake 入门](https://doc.qt.io/qt-6/cmake-get-started.html)
- [Qt Windows 部署与 windeployqt](https://doc.qt.io/qt-6/windows-deployment.html)
- [Qt Installer Framework 4.11](https://doc.qt.io/qtinstallerframework/index.html)
- [CMake 4.4.3 下载](https://cmake.org/download/)
- [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
- [JSON Schema Draft 2020-12](https://json-schema.org/draft/2020-12)

### Windows

- [Application manifests](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests)
- [AdjustTokenPrivileges](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges)
- [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [JOBOBJECT_EXTENDED_LIMIT_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information)
- [GetExtendedTcpTable](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable)
- [GetExtendedUdpTable](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedudptable)
- [Tool Help 进程快照](https://learn.microsoft.com/en-us/windows/win32/toolhelp/taking-a-snapshot-and-viewing-processes)
- [TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)
- [EnumServicesStatusExW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexw)
- [ControlService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-controlservice)
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [CredWriteW / Windows Credential Manager](https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credwritew)

### Git

- [Git for Windows 当前发行版](https://git-scm.com/install/windows)
- [git clone](https://git-scm.com/docs/git-clone)
- [Git credentials 与 AskPass](https://git-scm.com/docs/gitcredentials)
- [git reset（破坏性语义参考）](https://git-scm.com/docs/git-reset)
- [git clean（删除语义参考）](https://git-scm.com/docs/git-clean)

## 17. 明确不做的内容

首版不实现：macOS 二进制、Windows Service、后台托盘常驻、远程控制、自动更新、WebView、插件系统、脚本市场、多用户权限、管理员密码、SSH Git、HTTP Git、容器编排、并行任务、项目运行后继续同步。

这些排除项不得被 Codex 当作“后续阶段”写入首版计划；本次实现只需保证平台接口边界可替换，以便将来增加 macOS 实现。
