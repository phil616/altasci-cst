# Customer Service Terminal

按 [Tech.md](Tech.md) 实现的 Windows x64 / C++20 / Qt Widgets 项目。当前处于实现与验收中；尚未完成 Windows 10/11 实机验收。

## 下载 Windows 测试产物

打开 [Windows Actions](https://github.com/phil616/altasci-cst/actions/workflows/windows.yml)，选择成功的运行，在 Artifacts 下载：

- `CST-windows-x64-portable-unsigned`：包含 Qt DLL 的程序目录，完整解压后启动 `cst.exe`。
- `CST-windows-x64-installer-unsigned`：Qt IFW 安装 EXE 及 SHA-256 校验值。
- `CST-windows-test-logs`：构建和自动测试诊断。

开发构建未签名。应用必须获得管理员和调试权限。项目配置中的命令会以提升权限执行；测试时使用专用测试项目和端口。不要把正在开发的工作目录作为同步目标：同步会完全替换本地副本。

请反馈 Windows 版本、产物中的提交号、操作步骤、预期/实际行为以及“诊断 / 日志”导出的诊断包。环境文件内容和凭据不进入诊断包。

## Windows 构建

需要 MSVC 2022 v143 x64、Windows SDK 10.0.26100、CMake 4.4.3、Ninja Multi-Config、动态 Qt 6.11.2、Qt IFW 4.11.0，以及用于集成测试的 Git for Windows 2.55.0。

在 x64 Native Tools 命令环境中设置：

```powershell
$env:CST_QT_ROOT = 'C:\Qt\6.11.2\msvc2022_64'
$env:CST_IFW_ROOT = 'C:\Qt\Tools\QtInstallerFramework\4.11'
$env:CST_TEST_GIT = 'C:\Program Files\Git\cmd\git.exe'
cmake --workflow --preset windows-debug
cmake --workflow --preset windows-release
cmake --workflow --preset windows-package
```

生产签名用 `windows-package-production` workflow，必须设置 `CST_SIGN_PFX`、`CST_SIGN_PFX_PASSWORD`、`CST_SIGN_TIMESTAMP_URL`；缺失时配置阶段失败。

## 本机通用测试

Linux 上的独立测试构建只验证通用服务与 Qt 界面，不生成替代平台应用，也不放宽 Windows 正式构建版本要求。

```sh
cmake -S tests -B build/portable -G Ninja
cmake --build build/portable
CST_TEST_GIT=/path/to/git-2.55.0 ctest --test-dir build/portable --output-on-failure
```

目前包含配置校验、环境与引用、状态转换、重启策略、配置持久化、服务顺序与失败清理、同步事务故障恢复、真实本地 Git 仓库和子模块同步，以及三种 DPI 缩放的 UI 测试。Windows 原生进程与管理员集成测试及逐条验收仍在补充，不能以通用测试通过代替完整验收。

## 平台实现参考

进程启动与句柄列表按 [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw) 和 [Job completion port](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port) 实现。安装包采用 [Qt IFW 离线安装器](https://doc.qt.io/qtinstallerframework/ifw-offline-installers.html) 与[组件脚本](https://doc.qt.io/qtinstallerframework/scripting.html)。
