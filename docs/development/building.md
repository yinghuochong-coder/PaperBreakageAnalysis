# 构建、测试与 CI

## 1. 支持范围

当前工程只支持 Windows 10/11 x64、Visual Studio 2026 MSVC v145、C++20 和 CMake 4.2 以上版本。日常开发、IDE 和 CI 统一使用 `Visual Studio 18 2026` x64 generator，不提供 Ninja 预设。

Debug、Release 和静态分析均为真实相机构建：配置阶段固定创建 `paperbreak_camera_hikrobot`，生产服务固定装配支持发现、参数读写和取流的 MVS 提供者。工程不再提供关闭 Hikrobot 适配器的构建开关或单独的 Hikrobot 预设。模拟相机只作为 `BUILD_TESTING=ON` 时的自动化测试依赖，不安装到生产产物。

## 2. 依赖准备

按 `docs/architecture/dependencies.md` 安装：

- Qt 6.10.2 `msvc2022_64`；
- OpenCV 4.12.0 Windows SDK；
- Hikrobot MVS Development/Runtime 4.8.0.3；
- Visual Studio 2026 随附或批准版本的 vcpkg；
- Visual Studio 2026 C++ 工作负载中的 CMake 和 clang-format。

开源依赖由仓库根目录的 `vcpkg.json` 和固定 `builtin-baseline` 解析。`tests` feature 默认由提交的构建预设启用，zstd feature 默认关闭。禁止用全局 vcpkg classic 安装状态替代 manifest。

在开发者 PowerShell 或 CI 环境中注入逻辑路径：

```powershell
$env:VCPKG_ROOT = '<VS-or-approved-vcpkg-root>'
$env:PAPERBREAK_QT_ROOT = '<Qt-6.10.2-msvc2022_64-root>'
$env:OpenCV_DIR = '<OpenCVConfig.cmake-containing-directory>'
$env:PAPERBREAK_MVS_ROOT = '<MVS-Development-or-install-root>'
$env:PAPERBREAK_MVS_RUNTIME_DIR = '<MVS-Runtime-Win64-x64-directory>'
```

本机已在被 `.gitignore` 排除的 `CMakeUserPresets.json` 中为上述五项及 `VSINSTALLDIR` 设置实际安装路径，日常开发和 Codex 构建使用 `local-windows-vs2026-*` 预设即可，无需重复设置环境变量；`VSINSTALLDIR` 还用于定位 VS 随附的 `clang-format`。可移植的 `windows-vs2026-*` 预设仍只引用逻辑环境变量，供 CI 或其他开发机注入。不要把本机盘符、用户名或 SDK 绝对路径加入 `CMakePresets.json`、源码、默认配置或发布产物。

配置会逐项检查 MVS 头文件、x64 import library、Runtime DLL 及 DLL 文件版本，缺失或版本不匹配时直接失败。Runtime DLL 固定安装到 `bin`；不得把 Win32 Runtime 目录或开发机绝对路径写入提交预设。`hikrobot_adapter_unit` 使用伪 C API 验证 RAII/回调并调用真实 `MV_CC_GetSDKVersion()` 做 4.8.0.3 link smoke，不访问相机。实体相机发现、连接、取流与拔线测试不属于该测试。

从普通 PowerShell 构建前，先进入 VS 2026 x64 开发环境：

```powershell
& '<Visual-Studio-root>\Common7\Tools\Launch-VsDevShell.ps1' `
  -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

## 3. 配置、构建和测试

Debug：

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

Release：

```powershell
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

测试预设默认排除 `hardware-integration` 标签。当前标签为：

| 标签 | 内容 | 默认运行 |
| --- | --- | --- |
| `unit` | common、Result、日志、版本、生命周期与 SCM 封装 | 是 |
| `integration` | 服务/Qt/OpenCV smoke、配置失败诊断、安装路径扫描 | 是 |
| `simulation` | 不依赖硬件的模拟 lane 基线 | 是 |
| `hardware-integration` | 目标机、MVS 与实体相机 | 否 |

明确检查硬件测试登记但不执行设备操作时，可使用 `ctest --test-dir <build-dir> -L hardware-integration -V`；M0 占位测试只会报告 skipped，不能作为硬件通过证据。

## 4. 程序与 Windows 服务

服务命令行模式互斥：

```powershell
PaperBreakEdgeService.exe --version
PaperBreakEdgeService.exe --validate-config --config '<config.json>'
PaperBreakEdgeService.exe --console --config '<config.json>'
PaperBreakEdgeService.exe --install --config '<config.json>'
PaperBreakEdgeService.exe --uninstall
```

`--validate-config` 执行完整 schema v2 校验：配置文件必须不超过 1 MiB，必须是合法 UTF-8 JSON 对象，并通过强类型、有限范围、未知/敏感字段、跨字段依赖和路径安全校验。根版本字段为 `configSchemaVersion`、`configRevision` 和 `modifiedAt`；完整格式、热应用/待重启分类及默认值见 `docs/config-schema.md`。配置路径必须显式传入，尚未固化生产环境默认路径。

console/SCM 启动通过 `ConfigRepository` 加载配置。主文件损坏时会从同目录 `.history` 中恢复最新有效快照；残留 `.paperbreak.tmp.*` 文件不会被当作配置加载。后续更新使用期望修订、同目录临时文件、刷新和原子替换，并默认保留最近 5 个有效历史快照。

控制台模式按 Ctrl+C 受控退出，并把控制台关闭、注销和系统关机信号转换为同一服务停止请求。自动化 smoke 可附加 `--run-for-ms 25`，取值范围为 0～60000 毫秒；该参数只用于控制台测试。退出码为：成功 `0`、命令行或配置错误 `2`、启动、关闭或 SCM 操作失败 `1`。

`--install` 和 `--uninstall` 必须在提升权限的 PowerShell 中运行，程序不会触发 UAC 自提升。安装命令先验证配置并保存其规范化绝对路径；配置文件及父目录必须允许 `NT AUTHORITY\LocalService` 读取。安装是幂等配置收敛，不立即启动服务；卸载同样幂等，运行中服务会先请求停止并最多等待 30 秒。

服务注册为自动启动的独立进程，内部 SCM 启动参数是 `--service --config <absolute-path>`，不供交互运行。SCM 宿主上报 START_PENDING、RUNNING、STOP_PENDING 和 STOPPED，在 pending 阶段每秒更新 checkpoint；接受停止、关机和预关机控制，但回调只提交容量为 1 的停止请求。异常退出恢复延迟为 5、15、60 秒，后续继续使用最后一项，稳定 24 小时后重置失败计数；非崩溃失败也应用该策略，正常停止不触发恢复。

查询注册结果可使用：

```powershell
sc.exe qc PaperBreakEdgeService
sc.exe qfailure PaperBreakEdgeService
sc.exe qfailureflag PaperBreakEdgeService
sc.exe queryex PaperBreakEdgeService
```

真实安装、启动、停止、Session 0 和异常恢复必须在隔离 Windows 测试机验证。默认 CTest 只测试可注入的 SCM 封装和手工启动内部模式时的拒绝路径，不修改真实服务数据库。

Qt 客户端直接启动后创建最小系统托盘，右键菜单提供“退出界面”。自动测试使用 `QT_QPA_PLATFORM=offscreen` 验证 Qt 事件循环和确定性退出；托盘实际可见性必须在交互式 Windows 桌面人工观察。

两个程序均支持 `--version`，输出统一的软件版本、Git 提交/dirty 标记、UTC 构建时间、编译器和直接依赖版本。

## 5. 格式、静态分析与报告

```powershell
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
ctest --preset local-windows-vs2026-debug --output-junit out/test-results/debug-ctest.xml


cmake --install out\build\local-windows-vs2026-debug --config Debug --prefix out\build\local-windows-vs2026-debug\test-install

```

格式检查使用 clang-format 的 `--dry-run --Werror`。静态分析预设对生产源码目标启用 MSVC `/analyze` 并跳过 GoogleTest/OpenCV smoke 目标，第三方头由 `/analyze:external-` 排除；普通 Debug/Release 不承担其额外构建成本。所有项目目标默认 `/utf-8 /W4 /WX /permissive-`。

## 6. 安装布局

```powershell
cmake --install out\build\local-windows-vs2026-debug --config Debug --prefix out\install\local-windows-vs2026-debug
cmake --install out\build\local-windows-vs2026-release --config Release --prefix out\install\local-windows-vs2026-release
```

当前安装布局包含 `bin`、`lib` 和 `include`，并通过 CMake/Qt 部署脚本复制服务所需的 vcpkg 动态库、Qt 动态库和 Qt 平台插件。`bin` 固定包含同为 4.8.0.3 的 `MvCameraControl.dll` 和 GigE 动态传输组件 `MVGigEVisionSDK.dll`；不复制未使用的 USB、采集卡、GUI Runtime 或模拟相机库。它不制作安装器，也不替代 M9 的驱动、签名、完整许可证/SBOM 和企业部署物料。CTest 会检查必需运行时文件，从安装树启动两个程序的 `--version` smoke，并扫描产物，拒绝泄漏注入的 Qt、OpenCV、MVS 或 vcpkg 根路径。

## 7. CI 入口

`.ci/windows-build.ps1` 是 Windows CI 入口，要求执行器预先设置 Qt、OpenCV、MVS Development/Runtime 和 vcpkg 五项逻辑路径，并具备 `windows/x64/vs2026/msvc-v145/cmake-4.2/mvs-4.8.0.3` 能力。脚本执行：

1. Debug/Release 配置、构建和默认非硬件 CTest；
2. JUnit XML 报告写入 `out/test-results/`；
3. clang-format 检查；
4. MSVC 静态分析。

仓库仍未指定远端 CI 平台、runner 注册和凭据，因此 M0 不提交绑定某一服务商的 workflow，也不声称自托管 runner 已上线。

## 8. M0 验证边界

M0 自动验证会链接并执行不打开设备的 MVS SDK smoke，但不连接、配置或取流实体相机，也不访问 PLC 或上位机。Qt 6.10.2 `msvc2022_64` 与 v145、OpenCV 4.12.0 与 v145 的最小链接/启动由 smoke test 覆盖；真实取流、拔线、四路带宽及 7×24 小时测试均未执行，也不属于 M0。
