# M5-02：模拟检测器和人工触发 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-02
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.9、5.7

## 目的与可观察结果

提供与厂商相机实现解耦的模拟断纸触发器。调用者可为每路逻辑相机创建固定配置的检测器，在下一帧执行一次有界人工触发，或按固定单调时间周期、相邻帧平均灰度变化、ROI 内纸幅像素占比生成触发结果。人工结果必须精确标记 `triggerSource=ManualTest`，所有结果携带原帧相机 ID、序号和时间，供 M5-03 后续候选状态机消费。

## 范围

### 范围内

- `paperbreak_algorithm` 中供检测器实现使用的最小触发接口、结果、来源和 ROI 数据类型；
- 独立 `paperbreak_algorithm_mock` 目标及手动、固定周期、平均灰度变化、ROI 纸幅占比四种模式；
- 配置、帧布局、相机归属、单调时间顺序和像素格式校验；
- 人工请求容量 1、重复请求合并、下一有效帧一次性消费；
- 各模式及错误/边界的自动化单元测试；
- CMake、路线图状态和本 ExecPlan 完成证据。

### 范围外

- M5-03 候选事件状态机、连续帧确认、超时、并发事件和服务停止；
- M5-04 前后窗口冻结、事件合并，以及 M5-05 之后的关键帧、落盘和 SQLite；
- M6-01 完整 `IBreakDetector` 生命周期、热更新、插件 ABI/装载、正式 `DetectorInfo`；
- 配置 schema、服务组合根、IPC、人工触发按钮或 Qt 界面接线；这些由后续事件/算法配置任务统一落地；
- OpenCV、实体相机或 Hikrobot MVS SDK 调用。

## 当前基线

- `paperbreak_algorithm` 只有 `module_name()` 占位实现，当前不依赖相机模块；
- 架构已规定 `paperbreak_algorithm` 可依赖 `paperbreak_camera` 的只读 `FrameView`，`paperbreak_algorithm_mock` 只可依赖算法接口；
- `camera::FrameView` 已校验相机 ID、几何、步长和载荷大小并持有只读池化缓冲；
- M5-01 已提供每路固定内存环，但 M5-02 不接入缓存或事件状态机；
- 当前配置已有通用算法字段，但没有四种 mock 模式的完整参数；M5-09 才负责事件配置和生产装配；
- 工作区开始时 `git status --short` 为空。

## 前置条件与假设

- 每个检测器实例只处理一个配置的逻辑相机；自动检测状态仅由该相机的保序算法执行上下文调用。
- 人工请求可能来自另一控制线程，因此使用单个原子待处理标志；不建立队列，重复请求合并，不产生无界积压。
- 首版像素统计只接受 `Mono8`。Mono10/Mono12 和 Bayer 的位深、打包及去马赛克语义由 M6 正式算法决定；本任务对其返回稳定处理错误，不猜测格式。
- “纸幅像素”定义为 ROI 内灰度大于等于配置阈值的像素；纸幅占比低于配置的最小值时触发。
- 固定周期以帧的 `steady_clock` 时间为准；首帧建立基线，经过完整周期后的首个帧触发一次，时间大跳变不补发多次。

## 设计说明

`paperbreak_algorithm` 新增最小 `ITriggerDetector` 契约。它接收厂商无关的 `camera::FrameView`，返回 `TriggerResult`：是否触发、来源、原帧标识/时间、原因、平均灰度、灰度变化和纸幅占比等固定字段。该契约只服务 M5 候选入口，不宣称完成 M6 的插件生命周期接口。

`paperbreak_algorithm_mock` 提供 `MockTriggerDetector::create()`，先验证相机 ID、周期、归一化阈值和 ROI 尺寸，再以 RAII 返回实例。实例固定为四种自动评估模式之一，所有模式都可接受有界人工请求，人工请求在成功评估的下一帧覆盖自动来源：

- `manual`：`request_manual_trigger()` 把容量 1 的原子标志从空置改为待处理；重复请求报告合并。只有帧完成全部通用校验后才消费，结果来源为 `ManualTest`。
- `fixed_period`：首帧记录单调时刻；当当前帧与上次周期基线的间隔达到周期时触发并把基线更新为当前帧，避免追赶式突发。
- `mean_grayscale_change`：忽略步长填充，计算完整有效画面的归一化平均灰度；首帧只建立基线，之后绝对变化达到阈值时触发，并逐帧更新基线。
- `roi_paper_ratio`：验证 ROI 不越过当前帧，以阈值分割纸幅像素；占比严格低于最小纸幅占比时触发，结果记录 ROI 和实际占比。

所有模式校验逻辑相机一致、帧完整、序号递增且单调时间不回退；需要读取像素的灰度/ROI 模式额外要求 `Mono8` 和有效 stride/区域。失败使用 `ALGORITHM_PROCESS_FAILED` 并保留人工待处理请求；不抛出厂商错误码。

### 线程和队列

本任务不新增线程或跨线程队列。算法 `process()` 由每相机保序算法上下文串行调用；人工请求生产者为控制线程，消费者为算法上下文，通道是容量 1 的原子标志，满载策略为合并重复请求，停止时随检测器析构直接丢弃尚未消费请求。像素统计在算法执行上下文完成，不进入相机采集回调。

### 持久化与恢复

不适用。检测器状态和人工待处理标志均为进程内易失状态；事件持久化属于 M5-06/M5-07。

### 错误和降级

- `SYS_CONFIG_INVALID`：空相机 ID、无效周期/阈值或零尺寸 ROI；拒绝创建检测器。
- `ALGORITHM_PROCESS_FAILED`：错误相机、帧不完整、非 `Mono8`、时间回退或 ROI 越界；拒绝当前结果，保留实例和后续处理能力。
- 重复人工请求：不报失败，返回 `already_pending` 并保持一个待处理触发，防止控制请求无界积压。

## 实施步骤

- [x] 1. 在 `paperbreak_algorithm` 中新增最小触发契约和稳定来源字符串转换，并建立对只读相机帧接口的公开依赖。
- [x] 2. 新建 `paperbreak_algorithm_mock`，完成四种模式、配置验证、有界人工请求和像素/时间校验。
- [x] 3. 接入单元测试，覆盖人工来源与一次性/合并、周期边界、灰度阈值/步长、ROI 占比/边界和错误后恢复。
- [x] 4. 更新路线图与计划，运行格式、Debug/Release 构建和非硬件 CTest。

## 验证计划

### 自动化测试

- 人工请求在下一有效帧触发且精确输出 `ManualTest`；重复请求合并、消费后可再次请求；
- 无人工请求不触发，非法帧不消费待处理请求；
- 周期首帧不触发、边界前不触发、精确边界触发、长时间跳跃只触发一次；
- 灰度首帧建立基线、阈值上下及精确边界，并验证忽略 stride 填充；
- ROI 纸幅占比的零/部分/完整纸幅、精确阈值和 ROI 越界；
- 空相机、无效阈值/周期/ROI 配置拒绝；错误相机、非 Mono8、不完整帧和单调时间回退返回稳定错误且后续可恢复。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行定向 `PaperBreakTests --gtest_filter=AlgorithmMockTrigger*`、本任务 C++ 文件 clang-format dry-run、全仓 `format-check` 和 `git diff --check`。

### 人工或硬件验证

- 环境：不适用；本任务完全使用构造的只读帧，不访问相机或 MVS SDK。
- 步骤：不执行实体相机测试。
- 预期：M5-03/M5-09 后续装配后再验证 IPC/UI 人工请求到事件链的端到端行为。
- 证据保存位置：本计划验证证据表。

## 回滚与恢复

本任务不修改持久数据。失败时可移除新增触发契约、mock 目标和测试并恢复 CMake 列表；既有 `module_name()` API 保留。不得删除用户配置或事件数据。

## 验收标准

- [x] 四种模式均可确定性生成或不生成触发；
- [x] 人工触发精确标记 `triggerSource=ManualTest`，且请求通道容量固定、重复请求不增长；
- [x] mock 实现只链接算法接口，不依赖 Hikrobot、相机 mock、存储或 UI；
- [x] 帧来源、时间、像素和 ROI 错误返回稳定业务错误且不破坏后续处理；
- [x] 自动化测试覆盖全部模式和关键边界；
- [x] Debug/Release 构建及非硬件 CTest 已实际运行并记录；
- [x] 未实现 M5-03 或 M6-01 之后的功能。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、M5-01、帧视图、错误码和测试基线；创建计划，状态 in-progress。
- 2026-08-04：完成 M5 最小触发契约、独立 mock 目标、四种触发路径和 8 项定向测试。
- 2026-08-04：Debug/Release 全量构建与两套 CTest 23/23 通过；记录既有全仓格式阻断，计划状态改为 completed。

## 决策记录

- DEC-001：M5 使用最小 `ITriggerDetector`，不提前实现 M6 的初始化、热更新、重置、信息查询和插件装载契约。
- DEC-002：每实例绑定一个逻辑相机，自动状态由单一保序消费者维护；仅人工请求用容量 1 原子标志跨线程传递。
- DEC-003：像素统计首版只接受明确布局的 `Mono8`；未知位深/打包不做猜测转换。
- DEC-004：本任务使用显式独立配置，不修改生产配置 schema；M5-09 再统一接入服务配置和 UI。

## 意外发现

- 当前 PowerShell PATH 未包含 Visual Studio 自带 `clang-format.exe`；首次格式命令在配置/编译前停止。定位到 VS LLVM x64 22.1.3 后，本任务文件检查通过。
- 全仓 `format-check` 仍首先报告未修改的 `src/console/src/preview_client.cpp` 格式不一致，与 M5-01 记录相同；本任务未越界修复。
- CMake 配置继续报告既有 `SQLite::SQLite3` 目标弃用开发者警告，不影响构建或测试，本任务未修改存储依赖。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 实施前工作区为空。 |
| 2026-08-04 | `PaperBreakTests --gtest_filter=AlgorithmMockTrigger*` | 通过 | 8/8；覆盖四种触发、人工请求容量、阈值/时间/ROI 边界及错误恢复。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-debug`、全量构建、`ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | MSVC `/W4 /WX`；23/23。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-release`、全量构建、`ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | MSVC `/W4 /WX`；23/23。 |
| 2026-08-04 | 本任务文件 `clang-format --dry-run --Werror` | 通过 | VS LLVM x64 clang-format 22.1.3，6 个 C++/头文件无格式差异。 |
| 2026-08-04 | 全仓 `format-check` | 受既有问题阻断 | 未修改的 `src/console/src/preview_client.cpp` 格式不一致。 |
| 2026-08-04 | `git diff --check` | 通过 | 无空白错误。 |
| 2026-08-04 | 实体相机/服务 IPC/UI 人工触发 | 未执行 | M5-02 不接入服务与 UI，也不需要 MVS；不宣称硬件或端到端事件链通过。 |

## 完成摘要

M5-02 已提供厂商实现无关的最小触发契约和独立 mock 检测器，支持人工、固定周期、平均灰度变化和 ROI 纸幅占比触发。人工请求有固定容量并精确输出 `ManualTest`，像素/来源/顺序错误可恢复。8 项定向测试和 Debug/Release 两套 23/23 CTest 通过；M5-03 状态机、生产配置及 IPC/UI 接线保持在后续任务范围。
