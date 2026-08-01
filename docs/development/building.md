# 构建、测试与 CI

## 1. 支持范围

M0 工程只支持 Windows 10/11 x64、Visual Studio 2026 MSVC v145、C++20 和 CMake 4.2 以上版本。日常及 CI 使用 Ninja 单配置预设；另提供 Visual Studio 18 2026 x64 生成器预设用于 IDE 验证。

默认构建是 Mock-only，不查找 Hikrobot MVS SDK，也不依赖实体相机。`PAPERBREAK_ENABLE_HIKROBOT=ON` 在 M0 会明确失败，因为生产适配器属于 M3。

## 2. 依赖准备

按 `docs/architecture/dependencies.md` 安装：

- Qt 6.10.2 `msvc2022_64`；
- OpenCV 4.12.0 Windows SDK；
- Visual Studio 2026 随附或批准版本的 vcpkg；
- Visual Studio 2026 C++ 工作负载中的 CMake、Ninja 和 clang-format。

开源依赖由仓库根目录的 `vcpkg.json` 和固定 `builtin-baseline` 解析。`tests` feature 默认由提交的构建预设启用，zstd feature 默认关闭。禁止用全局 vcpkg classic 安装状态替代 manifest。

在开发者 PowerShell 或 CI 环境中注入逻辑路径：

```powershell
$env:VCPKG_ROOT = '<VS-or-approved-vcpkg-root>'
$env:PAPERBREAK_QT_ROOT = '<Qt-6.10.2-msvc2022_64-root>'
$env:OpenCV_DIR = '<OpenCVConfig.cmake-containing-directory>'
```

也可以在被 `.gitignore` 排除的 `CMakeUserPresets.json` 中设置这些值。不要把本机盘符、用户名或 SDK 绝对路径加入 `CMakePresets.json`、源码、默认配置或发布产物。

从普通 PowerShell 构建前，先进入 VS 2026 x64 开发环境：

```powershell
& '<Visual-Studio-root>\Common7\Tools\Launch-VsDevShell.ps1' `
  -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

## 3. 配置、构建和测试

Debug：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

Release：

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

测试预设默认排除 `hardware-integration` 标签。当前标签为：

| 标签 | 内容 | 默认运行 |
| --- | --- | --- |
| `unit` | common、Result、日志与版本 | 是 |
| `integration` | 服务/Qt/OpenCV smoke、配置失败诊断、安装路径扫描 | 是 |
| `simulation` | 不依赖硬件的模拟 lane 基线 | 是 |
| `hardware-integration` | 目标机、MVS 与实体相机 | 否 |

明确检查硬件测试登记但不执行设备操作时，可使用 `ctest --test-dir <build-dir> -L hardware-integration -V`；M0 占位测试只会报告 skipped，不能作为硬件通过证据。

## 4. 程序 smoke

服务控制台模式：

```powershell
out\build\windows-msvc-debug\src\service\PaperBreakEdgeService.exe --console
```

按 Enter 请求受控退出。自动化 smoke 使用有上限的 `--run-for-ms` 参数。SCM 注册、控制码和 Windows 服务状态上报属于 M1，M0 不实现。

Qt 客户端直接启动后创建最小系统托盘，右键菜单提供“退出界面”。自动测试使用 `QT_QPA_PLATFORM=offscreen` 验证 Qt 事件循环和确定性退出；托盘实际可见性必须在交互式 Windows 桌面人工观察。

两个程序均支持 `--version`，输出统一的软件版本、Git 提交/dirty 标记、UTC 构建时间、编译器和直接依赖版本。

## 5. 格式、静态分析与报告

```powershell
cmake --build --preset windows-msvc-debug --target format-check
cmake --preset windows-msvc-static-analysis
cmake --build --preset windows-msvc-static-analysis
ctest --preset windows-msvc-debug --output-junit out/test-results/debug-ctest.xml
```

格式检查使用 clang-format 的 `--dry-run --Werror`。静态分析预设对生产源码目标启用 MSVC `/analyze` 并跳过 GoogleTest/OpenCV smoke 目标，第三方头由 `/analyze:external-` 排除；普通 Debug/Release 不承担其额外构建成本。所有项目目标默认 `/utf-8 /W4 /WX /permissive-`。

## 6. 安装布局

```powershell
cmake --install out\build\windows-msvc-release `
  --prefix out\install\windows-msvc-release
```

当前安装布局包含 `bin`、`lib` 和 `include`，并通过 CMake/Qt 部署脚本复制服务所需的 vcpkg 动态库、Qt 动态库和 Qt 平台插件。它不制作安装器，也不替代 M9 的签名、完整许可证/SBOM 和企业部署物料。CTest 会从安装树启动两个程序的 `--version` smoke，并扫描产物，拒绝泄漏注入的 Qt、OpenCV 或 vcpkg 根路径。

## 7. CI 入口

`.ci/windows-build.ps1` 是 provider-neutral 的 Windows CI 入口，要求执行器预先设置三项逻辑路径并具备 `windows/x64/vs2026/msvc-v145/cmake-4.2/mock-only` 能力。脚本执行：

1. Debug/Release 配置、构建和默认非硬件 CTest；
2. JUnit XML 报告写入 `out/test-results/`；
3. clang-format 检查；
4. MSVC 静态分析。

仓库仍未指定远端 CI 平台、runner 注册和凭据，因此 M0 不提交绑定某一服务商的 workflow，也不声称自托管 runner 已上线。

## 8. M0 验证边界

M0 自动验证不访问 MVS SDK、相机、PLC 或上位机。Qt 6.10.2 `msvc2022_64` 与 v145、OpenCV 4.12.0 与 v145 的最小链接/启动由 smoke test 覆盖；真实取流、拔线、四路带宽及 7×24 小时测试均未执行，也不属于 M0。
