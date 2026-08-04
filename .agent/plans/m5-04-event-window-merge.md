# M5-04：前后窗口冻结和事件合并 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-04
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.11、4.12；`docs/architecture/domain-model.md` 3.2、3.3、4

## 目的与可观察结果

在 `paperbreak_event` 中提供固定容量的事件窗口聚合器。首个候选立即以单调时间保护所有绑定相机的前窗口；后续候选在活动窗口或 `mergeGap` 内追加到同一 EventId 并扩展窗口；达到后窗口与合并截止点后，输出按相机和序号去重、可追溯的冻结帧。墙上时间仅作为展示锚点，系统时间跳变不改变窗口。超过 `maxEventSeconds` 的窗口被明确截断，缓存不足时保留所有可用证据并标记 `EVENT_BUFFER_INCOMPLETE`。

## 范围

### 范围内

- 事件窗口配置、活动快照、触发记录和冻结结果模型；
- 最多四路相机和固定数量活动事件；
- `[T-pre, T+post]` 单调时间边界、`mergeGap` 合并、跨相机窗口；
- `maxEventSeconds` 截断、候选重复幂等、乱序跨相机候选的窗口并集合并；
- 所有相机的前缓存立即租用，封闭时获取后窗口并按序去重；
- 缓存历史、序列缺口或租约容量不足的明确完整性标记；
- 单元测试、CMake、错误码/路线图和本计划证据。

### 范围外

- M5-05 关键帧和 JPEG；
- M5-06 事件目录、manifest 和原始帧写入；
- M5-07 SQLite、M5-08 保留策略、M5-09 服务/配置/IPC/UI 接线；
- 修改 M5-03 的候选判定、确认/拒绝语义；
- 实体相机、MVS SDK 或目标机性能验证。

## 当前基线

- M5-01 `MemoryRing::lease_window()` 返回闭区间的有界只读租约，窗口不足也会返回可用帧并标记 `complete=false`；租约容量耗尽时返回 `EVENT_BUFFER_INCOMPLETE`。
- M5-03 `CandidateEventSnapshot` 已提供不可变 EventId、候选触发双时钟和前缓存状态，但只标记后收集开始，不封闭后窗口或合并事件。
- 架构已把窗口冻结和合并归入 `paperbreak_event`，因此本任务不改变模块依赖方向。
- 工作区开始时 `git status --short` 为空。

## 前置条件与假设

- 各相机帧已先进入各自 `MemoryRing`，同一相机帧序号与单调接收时间严格递增；跨相机不要求全局帧序。
- 聚合器接收已经进入 Candidate 的触发及其 EventId；未合并时沿用该 ID，合并时返回首个活动事件的规范 ID，并保留后续来源候选 ID 供审计。M5-09 组合根接线时应使用返回的规范 ID 发布事件。
- 调用方周期性调用 `advance_time()`；精确合并边界保持可合并，超过边界后封闭。达到最大窗口末端后不得再扩展。
- `maxEventSeconds` 约束冻结窗口的起止跨度；初始 `pre+post` 不得超过该上限。链式合并触及上限时保留触发但截断后窗口并明确标记。

## 设计说明

新增 `EventWindowManager`，配置包含 1～4 个相机环、前/后时长、最大事件时长、合并间隔和最大活动事件数（固定上限 4）。内部互斥量保护固定上限活动聚合；不保存完成历史，也不创建线程或队列。

`start_or_merge()` 校验候选 ID、触发来源和双时钟字段。精确重复来源候选幂等返回；新候选与活动窗口在单调时间上重叠或间隔不超过 `mergeGap` 时，追加完整触发、来源 ID 并扩展起止边界，必要时合并被桥接的两个活动聚合。否则创建新活动事件并立即对全部相机租用 `[T-pre,T]`。窗口起点因乱序候选向前扩展时，补租新增前段。

`advance_time()` 在当前单调时间超过合并截止点，或超过最大事件硬边界时封闭事件。每路相机租用尚未冻结的 `[firstTrigger,lastWindowEnd]` 可用范围，与已持有的前段租约合并，按 `(monotonicTime, sequenceNumber)` 排序并按序号去重。任何租约失败、不完整窗口或序列缺口都会使事件/相机 `complete=false` 并记录稳定业务码；最大时长截断使用独立标志，不与缓存完整性混淆。所有降级都不会丢弃已取得帧。完成结果由返回值移交调用方，不在管理器中形成无界完成队列。

墙上时间只保存首个触发的展示锚点和每个原始触发值，不参与窗口、合并或超时计算。

### 线程和队列

不新增线程或队列。公开方法可由最多四路调用线程并发进入，通过单互斥量串行化。活动事件数由 `maximum_active_events` 限制且硬上限为 4；每个事件的触发数硬上限为 16，达到上限时拒绝当前触发并返回 `EVENT_INVALID_TRANSITION`。通知/持久化留给后续任务，当前完成事件通过返回值直接转移所有权。`stop()` 冻结可用范围并拒绝后续候选，停止不依赖队列空位。

### 持久化与恢复

不适用。本任务仅持有进程内只读帧引用，不写磁盘或数据库。M5-06/M5-07 才定义事务提交与恢复；停止时返回的不完整冻结结果由未来组合根交给持久化层。

### 错误和降级

- `SYS_CONFIG_INVALID`：相机绑定、时长、上限或内存环状态非法，拒绝创建。
- `PIPELINE_FRAME_ORDER_VIOLATION`：候选字段非法或同一来源 ID 内容冲突，拒绝当前输入。
- `EVENT_INVALID_TRANSITION`：管理器停止、活动/触发固定容量耗尽，拒绝当前输入。
- `EVENT_BUFFER_INCOMPLETE`：租约失败、不完整或存在序列缺口；事件仍返回已取得证据并明确标损。
- `EVENT_NOT_FOUND`：查询未知活动 EventId。

## 实施步骤

- [x] 1. 新增窗口聚合公开模型、配置校验和有界活动容器。
- [x] 2. 实现候选幂等、重叠/间隔/跨相机合并、最大时长截断和前缓存立即保护。
- [x] 3. 实现精确边界封闭、逐相机后窗口租用、帧排序去重、完整性汇总和确定性停止。
- [x] 4. 新增定向单元测试并接入 CMake，覆盖路线图指定场景和容量/错误恢复。
- [x] 5. 更新路线图与计划，运行格式、Debug/Release 构建和非硬件 CTest；本任务复用既有错误码，未新增错误码表项。

## 验证计划

### 自动化测试

- 触发帧 N 和闭区间首尾边界；
- 墙上时间正反跳变不影响单调窗口；
- 活动窗口重叠、精确 `mergeGap`、超过间隔不合并和桥接合并；
- 跨相机触发共享规范 EventId，四路窗口分别按序冻结；
- `maxEventSeconds` 精确限制和截断标记；
- 前历史不足、后窗口不足、序列缺口和租约容量失败均保留可用帧并标损；
- 重复候选幂等、冲突输入、固定容量、停止和回调后无隐式历史。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行 `PaperBreakTests --gtest_filter=EventWindow*`、本任务 C++ 文件 clang-format dry-run、全仓 `format-check` 和 `git diff --check`。

### 人工或硬件验证

- 环境：不适用；使用固定帧池与构造帧。
- 步骤：不执行实体相机或 MVS SDK 测试。
- 预期：M5-09 生产接线后再进行人工触发不暂停采集和多相机端到端验证。
- 证据保存位置：本计划验证证据表。

## 回滚与恢复

本任务不修改持久数据。失败时移除新增窗口源文件和测试，恢复 event/test CMake 与文档状态即可；不得删除用户配置、缓存或事件目录。

## 验收标准

- [x] 精确冻结 `[T-pre,T+post]`，墙上时间跳变不影响结果；
- [x] 活动窗口/`mergeGap` 内候选保留同一规范 EventId 和全部原始触发；
- [x] 跨相机窗口、触发帧 N、首尾边界和帧顺序均可自动验证；
- [x] `maxEventSeconds`、活动事件和触发数均有硬上限；
- [x] 缓存不足保留可用帧并标记 `EVENT_BUFFER_INCOMPLETE`；
- [x] 无线程、无无界队列、无采集暂停和 I/O；
- [x] Debug/Release 构建及非硬件 CTest 已实际运行并记录；
- [x] 未实现 M5-05 及之后功能。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、领域模型、M5-01/M5-03 实现与测试；创建计划，状态 in-progress。
- 2026-08-04：完成有界窗口聚合、所有相机前租约、后窗口冻结、规范 EventId 合并、桥接、最大时长、完整性标记和 9 项定向测试。
- 2026-08-04：Debug/Release 全量构建、两套 CTest 23/23 和通用 unit 207/207 通过；记录既有全仓格式阻断，计划状态改为 completed。

## 决策记录

- DEC-001：M5-04 使用独立 `EventWindowManager` 聚合已成立候选，不改变 M5-03 判定状态机；M5-09 组合根接线时消费返回的规范 EventId。
- DEC-002：前窗口在新事件成立时对全部绑定相机立即租用；后窗口在封闭时从持续滚动的环获取，租约不足按不完整事件降级。
- DEC-003：完成事件只通过返回值转移，不保存进程内无界历史；活动事件和每事件触发均设硬上限。

## 意外发现

- 首次格式化命令误选了 Visual Studio 随附的 ARM64 `clang-format.exe`，在 x64 主机上无法运行；改用同一工具链的 `VC/Tools/Llvm/x64/bin/clang-format.exe` 后，本任务文件 dry-run 通过。
- 在线合并必须同时限制活动事件数和单事件触发数；实现固定为最多 4 个活动事件、每事件 16 条触发，避免长时间异常形成无界元数据。
- CMake 继续报告既有 `SQLite::SQLite3` 目标弃用开发者警告；本任务未修改存储依赖。
- 全仓 `format-check` 继续首先报告未修改的 `src/console/src/preview_client.cpp` 格式不一致，与 M5-01～M5-03 记录相同；本任务未越界修复。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 实施前工作区为空。 |
| 2026-08-04 | `PaperBreakTests --gtest_filter=EventWindow*`（Debug/Release） | 通过 | 各 9/9；覆盖精确窗口、合并边界、墙上时间跳变、乱序桥接、跨相机/四线程、最大时长、缓存不足、容量和停止。 |
| 2026-08-04 | `PaperBreakTests` 通用 unit 过滤器（Debug/Release） | 通过 | 各 207/207。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-debug`、全量构建、`ctest --preset local-windows-vs2026-debug` | 通过 | MSVC `/W4 /WX`；23/23。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-release`、全量构建、`ctest --preset local-windows-vs2026-release` | 通过 | MSVC `/W4 /WX`；23/23。 |
| 2026-08-04 | 本任务 C++ 文件 `clang-format --dry-run --Werror` | 通过 | 新增头文件、实现和测试无格式差异。 |
| 2026-08-04 | 全仓 `format-check` | 受既有问题阻断 | 未修改的 `src/console/src/preview_client.cpp` 格式不一致。 |
| 2026-08-04 | `git diff --check` | 通过 | 无空白错误。 |
| 2026-08-04 | 实体相机、服务配置、IPC/UI、关键帧/落盘 | 未执行/未实现 | M5-04 使用构造帧和固定内存池；生产接线属于 M5-09，关键帧和持久化属于 M5-05～M5-07。 |

## 完成摘要

M5-04 已提供固定最多四路相机的有界事件窗口聚合器，完成单调时间前后冻结、规范 EventId 合并、精确 mergeGap、乱序桥接、跨相机关联、最大时长、缓存不足标损和确定性停止。Debug/Release 构建、两套 23/23 CTest、通用 unit 207/207 和 9 项定向测试通过；服务配置/IPC/UI 接线、关键帧、落盘和实体相机验证保持在后续任务范围。
