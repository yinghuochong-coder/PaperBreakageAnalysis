# M0：工程骨架 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：M0-01～M0-04（用户明确要求执行整个 M0）
- 关联需求：需求第 2、3、4.22、5.1、9、11、13、14、16 节；ADR-015

## 目的与可观察结果

从仅有设计文档的仓库建立 Windows x64/MSVC/C++20 最小工程。完成后，开发者可用提交的 CMake 预设配置并构建 Debug/Release，运行非硬件 CTest；服务可在 `--console` 模式受控退出，Qt 客户端可显示最小系统托盘；两个程序输出相同版本信息；日志、业务错误和 Result 基础经过自动化测试。

## 范围

### 范围内

- CMake 工程、VS 2026 Debug/Release 预设、vcpkg manifest、安装布局和版本元数据。
- M0 指定的库/程序/测试目标及架构要求的独立日志目标。
- 异步有界日志、模块分类、等级过滤、大小与日期轮转、敏感信息脱敏及确定性关闭。
- 稳定业务错误对象、保留原始码/上下文的 `Result<T>`。
- 服务控制台和 Qt 最小托盘程序，不包含 M1 生命周期/IPC 业务。
- GoogleTest/CTest 分类、默认排除硬件、格式/静态分析/报告和 provider-neutral Windows CI 脚本。
- 开发构建文档、路线图状态和本计划的实施证据。

### 范围外

- Windows SCM 注册与服务宿主、IPC、配置仓库、相机接口/Mock、采集、算法、事件、存储和上位机业务。
- Hikrobot SDK 适配和实体相机验证。
- 安装器与发布签名。

## 当前基线

- 已检查 `AGENTS.md`、`.agent/PLANS.md`、需求、系统架构、领域模型、错误码、路线图、依赖基线和 ADR-015。
- 仓库目前无源码、CMake、测试、CI 或 `.gitignore`；工作区开始时干净。
- 本机发现 CMake 4.2.3、VS 2026 18.6/v145、Qt 6.10.2、OpenCV 4.12.0；vcpkg 位于 VS 安装内但未在当前 PATH/VCPKG_ROOT 中。

## 前置条件与假设

- `VCPKG_ROOT`、`PAPERBREAK_QT_ROOT`、`OpenCV_DIR` 由环境或不提交的用户预设注入；验证命令可在当前 PowerShell 进程临时注入已发现路径。
- 默认构建为 Mock-only，不查找 MVS；启用 Hikrobot 开关但未提供 SDK 根目录时配置必须明确失败。
- 本任务不具备也不需要真实相机，不执行硬件测试。

## 设计说明

- 目标保持架构有向无环依赖；占位模块只公开模块标识，不提前定义后续业务接口。
- `paperbreak_common` 提供 `Error`、`Result<T>` 和生成的版本信息；`paperbreak_logging` 独立封装 spdlog。
- 日志运行时拥有专用有界异步线程池，不注册全局业务单例；关闭时先 flush，再释放 logger 和线程池。
- 日志文件名包含本地日期，单日内按大小轮转；跨日写入自动切换新日期文件。
- `Error` 保留稳定业务码、严重度、模块、操作、可选 native domain/code、白名单上下文和 UTC 时间。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `logging.async` | 调用日志门面的各 M0 程序/测试线程 | spdlog 专用后台线程 1 个 | 配置值，默认 8192 | `overrun_oldest`，保持调用方非阻塞 | `shutdown()` flush 后释放 logger/thread pool 并 join | spdlog overrun counter（后续 monitoring 接入） |

除日志外不新增工作线程或跨线程队列。

### 持久化与恢复

日志是可诊断输出，不是业务事实来源；轮转文件写入失败在初始化阶段返回 `LOG_INITIALIZATION_FAILED`。M0 不引入配置、数据库或事件持久化。

### 错误和降级

- 日志目录创建/打开失败：返回稳定业务码和原始文件系统上下文，不伪装成功。
- 缺少/版本错误依赖、平台/架构/编译器不符：CMake 配置阶段失败并给出修复入口。
- 无系统托盘环境的自动测试只验证 Qt 事件循环可启动和受控退出；实际可见托盘需交互式 Windows 会话人工观察。

## 实施步骤

- [x] 1. 创建 `.gitignore`、vcpkg manifest、CMake 模块/预设和生成版本信息；立即验证配置错误用例。
- [x] 2. 创建 common/logging 基础和所有 M0 目标边界；编译后修复所有 `/W4 /WX` 问题。
- [x] 3. 创建服务控制台入口和 Qt 最小托盘入口，增加版本及受控退出 smoke test。
- [x] 4. 创建 GoogleTest/CTest 分类、日志/Result 测试、OpenCV 链接 smoke、格式/静态分析/路径扫描与 CI 脚本。
- [x] 5. 编写开发构建说明，运行 Debug/Release、CTest、安装和路径泄漏验证，回写路线图与本计划证据。

## 验证计划

### 自动化测试

- common：版本字段、Result 成功/失败、业务码/native code/details 共存。
- logging：等级过滤、敏感字段脱敏、大小轮转、不可写/非法目录诊断。
- integration：服务 `--console --run-for-ms` 受控退出、Qt offscreen 启动、两程序版本输出一致、OpenCV 链接启动。
- simulation/hardware-integration：建立标签；模拟基线通过，硬件占位测试在明确执行时报告 skipped；默认预设排除 hardware-integration。
- configure：错误架构、缺失依赖和启用 MVS 但缺失根目录均产生明确失败。
- install：扫描可部署文本配置与运行产物，不出现注入的 SDK 根路径。

### 构建与测试命令

```powershell
cmake --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug
cmake --preset windows-vs2026-release
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-release
```

另运行安装目标、格式检查和 VS 2026 静态分析预设；若工具缺失则记录为限制，不伪报成功。

### 人工或硬件验证

- 环境：交互式 Windows 10/11 桌面。
- 步骤：启动 `PaperBreakEdgeConsole.exe`，观察系统托盘图标与退出菜单。
- 预期：托盘可见，退出客户端不关联任何后台服务控制。
- 证据保存位置：本任务不自动截图；最终记录为“未执行人工可见性验证”。
- 实体相机/MVS：不适用且未执行。

## 回滚与恢复

本任务仅新增工程骨架和更新相关文档，无用户数据/schema。失败时可按新增文件逐项移除并恢复路线图 M0 状态；不得使用破坏性 Git 命令覆盖用户修改。

## 验收标准

- [x] Debug/Release 的 VS 2026 x64 预设均可配置、构建。
- [x] 所有 M0 目标存在且依赖边界清晰。
- [x] 服务 console smoke 和 Qt client smoke 通过。
- [x] 默认 CTest 运行 unit/integration/simulation 且排除 hardware-integration。
- [x] 日志、错误/Result、统一版本信息测试通过。
- [x] 依赖失败与错误架构诊断测试通过，安装树路径扫描通过。
- [x] 开发构建与 CI 说明齐全。

## 进度记录

- 2026-08-01：完成需求、架构、路线图、依赖与本机工具链基线检查；状态 `in-progress`。
- 2026-08-01：完成 M0-01～M0-04 实现、Debug/Release/VS 2026 构建、非硬件测试、安装树、格式和静态分析验证；状态 `completed`。

## 决策记录

- DEC-001：用户直接要求“执行 M0”，视为明确授权合并执行 M0-01～M0-04；仍不进入 M1。
- DEC-002：不创建特定托管平台 workflow，提供 provider-neutral PowerShell CI 入口，因为仓库尚无远端/CI 平台信息。
- DEC-003：默认 Mock-only 构建不查找 MVS；MVS 开关仅验证缺失依赖诊断，不建立后续适配器。

## 意外发现

- 当前 PowerShell 未设置 `VCPKG_ROOT`、`PAPERBREAK_QT_ROOT`、`OpenCV_DIR`；所需工具/SDK 实际已安装在本机固定位置，可临时注入以验证提交的逻辑入口。
- 本机 OpenCV 包的有效配置目录位于 `build/x64/vc16/lib`，而不是 SDK 根目录；预设继续只接受外部注入的 `OpenCV_DIR`，未固化本机路径。
- MSVC 静态分析会进入 vcpkg/GoogleTest 外部头文件；静态分析预设关闭测试构建并使用 `/analyze:external-`，确保生产目标自身仍以分析告警为错误。
- Qt 部署脚本依赖 `find_package(Qt6)` 在调用方作用域生成的变量，因此依赖发现封装由函数调整为宏；安装树随后完成独立启动验证。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | 基线工具发现 | 已完成 | CMake 4.2.3；VS 18.6；MSVC 14.51；Qt/OpenCV 已安装；环境变量待注入 |
| 2026-08-01 | 旧 Ninja Debug 预设（现已删除） | 通过 | 修订前 Debug 构建成功；默认非硬件 CTest 10/10 通过 |
| 2026-08-01 | 旧 Ninja Release 预设（现已删除） | 通过 | 修订前 Release 构建成功；默认非硬件 CTest 10/10 通过 |
| 2026-08-01 | VS 2026 x64 配置、构建、CTest | 通过 | MSVC v145 构建成功；默认非硬件 CTest 10/10 通过 |
| 2026-08-01 | `.ci/windows-build.ps1` | 通过 | Debug/Release、JUnit 报告、格式检查和生产目标静态分析均成功 |
| 2026-08-01 | Release 安装树验证 | 通过 | Qt、MSVC、fmt/spdlog 运行时已部署；路径扫描及两个安装后程序的 `--version` 启动检查通过 |
| 2026-08-01 | 硬件标签占位测试 | 跳过 | 占位 GoogleTest 明确报告 1 skipped；未连接 MVS SDK 或实体相机 |
| 2026-08-01 | 构建预设修订及 `.ci/windows-build.ps1` | 通过 | 只保留 VS 2026 Debug/Release/静态分析预设；Debug/Release 各 10/10，格式检查及静态分析通过 |

## 完成摘要

已完成 M0 工程骨架：建立仅使用 Visual Studio 2026 x64 generator 的 Debug/Release/静态分析构建与安装基线、全部目标边界、异步日志与错误/Result/版本基础、服务控制台与 Qt 托盘入口，以及默认不依赖硬件的测试和 CI 入口。自动化验收已通过。交互式 Windows 托盘可见性未人工观察，MVS SDK 和实体相机未测试；这些限制不属于 M0 的自动化硬件无关门禁。
