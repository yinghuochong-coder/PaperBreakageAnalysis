# M1-01：服务核心生命周期 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-01
- 关联需求：需求 4.1；架构 4.1、7、10.3、13、15

## 目的与可观察结果

交付可测试的 `ServiceRuntime` 与控制台/Windows 控制宿主边界。服务支持基础配置预检、控制台运行、启动取消、失败回滚、按阶段有界关闭和稳定错误；所有自动化测试均不依赖 SCM、MVS SDK 或实体相机。

## 范围

### 范围内

- `paperbreak_service_core` 生命周期状态机、组件接口、停止原因和关闭阶段；
- 基础 JSON 配置文件校验及 `--config`/`--validate-config` CLI；
- 控制台宿主和 `SetConsoleCtrlHandler` 的最小 RAII 适配；
- 单元、集成测试以及公开 CLI 文档；
- 路线图状态和本计划验证证据。

### 范围外

- Windows SCM 注册、安装、状态上报和恢复策略（M1-02）；
- 完整强类型配置、schema、原子存储和回滚（M1-03）；
- IPC、相机、事件、上传等后续业务实现；
- 实体相机、MVS SDK 或真实 Windows 服务关机测试。

## 当前基线

- M0 已完成，服务入口仅提供 `--console`、`--version` 和测试用 `--run-for-ms`；
- 日志运行时已采用固定容量异步队列并支持显式 `shutdown()`；
- 相机、管线、事件、存储和上传目标仍是占位实现；
- 2026-08-01 已运行本地 Debug 配置、构建和默认 CTest，10/10 通过；工作区开始时无未提交修改。

## 前置条件与假设

- 基础配置校验只覆盖 1 MiB 上限、JSON 语法、根对象和 `schemaVersion == 1`；
- `--console` 与 `--validate-config` 都要求显式 `--config <path>`；
- 默认总关闭时限为 30 秒，测试通过运行时选项注入更短时限；
- 生命周期参与者必须遵守传入的单调时钟截止时间；运行时不使用强杀或分离线程掩盖失控组件；
- Win32 callback trampoline 所需的原子非拥有指针属于平台适配状态，不保存业务状态。

## 设计说明

`ServiceRuntime` 是已注入组件的唯一所有权根。组件按注册顺序启动；正常关闭按配置入口、采集、处理、事件、上传、IPC、日志排序，同阶段逆启动顺序。启动失败按已启动组件的逆序回滚。`request_stop()` 只设置停止原因和启动停止令牌，实际 `request_stop/join` 在宿主控制线程执行。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| Windows 控制请求 | Win32 控制回调 | ConsoleHost 主线程 | 1 个合并槽 | 首个请求生效，后续幂等合并 | 唤醒主线程并进入运行时关闭 | M1-06 接入 |

本任务不新增业务工作线程或业务跨线程队列。日志线程继续使用 M0 已定义的固定容量和关闭语义。

### 持久化与恢复

不适用。本任务只读配置，不写入配置、事件或数据库。

### 错误和降级

- CLI/基础配置错误退出码为 2，运行时启动/关闭失败退出码为 1；
- 启动阶段错误统一为 `SYS_SERVICE_START_FAILED`，details 保留组件、阶段和原始业务码；
- 停止超时为 `SYS_SHUTDOWN_TIMEOUT`，details 记录未完成组件/阶段；
- Win32 回调捕获所有异常并返回控制码处理失败，不允许异常越过 ABI 边界。

## 实施步骤

- [x] 1. 创建基础配置校验目标、接口和错误映射，并补充边界测试。
- [x] 2. 创建服务核心目标，实现状态机、启动取消、逆序回滚和分阶段关闭。
- [x] 3. 创建 Windows 控制适配目标和控制请求合并槽，确保回调无异常泄漏。
- [x] 4. 重构服务入口和控制台宿主，固定 CLI 组合、退出码和日志关闭路径。
- [x] 5. 更新 CMake、CTest、测试数据和开发文档。
- [x] 6. 执行 Debug、CTest、格式检查、静态分析和 `git diff --check`，修复发现的问题。
- [x] 7. 更新路线图、计划状态和验证证据。

## 验证计划

### 自动化测试

- 生命周期状态、幂等、顺序、取消、回滚、异常和超时；
- Win32 控制码映射、请求合并和异常屏障；
- 合法/非法/超大/截断配置及中文空格路径；
- CLI 成功、缺参、冲突、非法配置和控制台定时停止。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
git diff --check
```

### 人工或硬件验证

- 未执行：本任务不安装 SCM 服务，不访问 MVS SDK 或实体相机；真实 SCM/System Shutdown 集成由 M1-02 验证。

## 回滚与恢复

新增代码均为独立目标和源文件，无数据格式写入。失败时可移除新目标并恢复服务入口、测试与文档到任务前状态；不得删除用户配置或构建环境数据。

## 验收标准

- [x] `ServiceRuntime` 与 Win32 宿主分离，公开接口不包含 Win32 类型；
- [x] 重复操作、启动取消、失败回滚、关闭顺序和停止超时测试通过；
- [x] Win32 回调不执行耗时关闭且异常不越界；
- [x] CLI 基础配置校验和退出码符合约定；
- [x] Debug、默认非硬件 CTest、格式检查和静态分析通过；
- [x] 文档、路线图和验证证据完成更新。

## 进度记录

- 2026-08-01：创建计划，状态 `in-progress`；确认 Debug 基线 10/10 测试通过。
- 2026-08-01：完成实现和验证，状态 `completed`；默认非硬件 CTest 扩展为 14/14 通过。

## 决策记录

- DEC-001：基础校验限制为 JSON/根对象/schemaVersion，不提前实施 M1-03。
- DEC-002：配置路径必须通过 `--config` 显式传入，不固化 SCM 默认路径。
- DEC-003：总关闭时限默认 30 秒，所有阶段共享同一 `steady_clock` 截止时间。
- DEC-004：正常关闭不强杀或 detach；组件违反截止时间契约属于实现缺陷。

## 意外发现

- Visual Studio 已安装 clang-format，但当前 PowerShell 的 PATH 未包含其目录；首次 `format-check` 因找不到工具失败。将 VS LLVM x64 bin 临时加入 PATH 后检查通过，未修改提交的环境配置。
- Win32 控制回调注册状态改用原子 `shared_ptr`，保证注销时已进入回调的调用仍持有安全生命周期，不把非拥有裸指针留给并发回调。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | M1-01 修改前 Debug configure/build/CTest | 通过 | 10/10 非硬件测试通过 |
| 2026-08-01 | `cmake --preset local-windows-vs2026-debug` | 通过 | VS 2026/v145 x64 配置成功 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | `/W4 /WX` Debug 构建成功 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 14/14；单元测试含 22 个核心用例；排除硬件标签 |
| 2026-08-01 | Debug `format-check` | 通过 | 临时将 VS LLVM x64 bin 加入 PATH 后通过 |
| 2026-08-01 | Static Analysis configure/build | 通过 | MSVC `/analyze` 生产目标构建成功 |
| 2026-08-01 | `git diff --check` | 通过 | 无空白错误；仅 Git LF→CRLF 提示 |

## 完成摘要

新增基础配置预检、宿主无关的服务生命周期核心和 Windows 控制台控制适配层；服务 CLI 现要求显式配置路径，支持校验模式、受控控制台停止和稳定退出码。新增生命周期、配置、Win32 回调及 CLI 集成测试。未执行 SCM 安装、真实 Windows 服务关机、MVS SDK 或实体相机测试，这些不属于 M1-01。
