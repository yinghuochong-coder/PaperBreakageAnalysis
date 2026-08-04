# M5-03：候选事件状态机 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-03
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.10、4.11、4.12、7、13.3

## 目的与可观察结果

在 `paperbreak_event` 中提供最多四路逻辑相机共享的候选事件管理器。保序检测结果驱动 `Idle → Suspicious → Candidate → Confirmed/Rejected/Timeout`；首次进入 Candidate 时立即生成不可变 EventId、记录触发帧和双时钟、租用 M5-01 前缓存、标记后帧收集已开始并发布状态通知。重复检测结果和重复终态命令必须幂等，版本冲突、非法转换、超时和服务停止具有确定行为。

## 范围

### 范围内

- 候选事件状态、事件快照、版本和触发元数据模型；
- 每相机连续异常计数、Candidate/Confirmed 阈值、单调时间超时；
- Candidate 时生成 UUIDv7 形式 EventId，并立即持有前缓存租约；
- 多相机并发入口的串行化、重复帧/重复命令幂等和期望版本校验；
- Candidate、Confirmed、Rejected、Timeout 状态通知，以及回调异常隔离；
- 服务停止后拒绝新结果，并把尚未决策的 Candidate 确定性转为 Timeout；
- 单元测试、CMake、错误码文档、路线图和本计划证据。

### 范围外

- M5-04 精确后窗口冻结、跨相机窗口合并和 mergeGap；
- M5-05 关键帧/JPEG、M5-06 事件目录、M5-07 SQLite、M5-08 保留策略；
- M5-09 IPC/UI 的人工触发、确认、拒绝、查询和配置接线；
- M6 正式算法确认、插件生命周期和模型信号；
- 服务组合根、生产配置 schema 和实体相机集成。

## 当前基线

- M5-01 的 `MemoryRing::lease_window()` 可为单调时间闭区间返回有界只读租约，像素所有权仍来自固定相机池；
- M5-02 的 `TriggerResult` 已携带相机 ID、会话序号、相机帧号、双时钟、来源和原因；
- `paperbreak_event` 当前只实现内存环，无候选领域模型或 EventId 生成器；
- 领域文档规定 EventId 为 `EVT-` 加 UUIDv7，首次 Candidate 分配，终态保持同一 ID，状态/聚合变化递增版本；
- 错误码文档已有 `EVENT_VERSION_CONFLICT`、`EVENT_INVALID_TRANSITION` 和 `EVENT_BUFFER_INCOMPLETE`；
- 工作区开始时 `git status --short` 为空。

## 前置条件与假设

- 同一相机检测结果在进入事件管理器前已保序；管理器仍校验相机、序号和单调时间，精确重复序号按幂等重放处理，回退返回稳定错误。
- 候选阈值和确认阈值均以连续 `triggered=true` 的检测帧计数；确认阈值不得小于候选阈值。未触发结果在 Candidate 前复位 Suspicious，在 Candidate 后不自动拒绝，由显式拒绝或超时决定。
- M5-03 只建立“后帧收集已开始”的领域状态；实际后窗口租约和窗口封闭属于 M5-04，不在本任务复制像素或建立第二套帧容器。
- 事件管理器不创建线程。生产架构由单一事件管理执行器调用；为覆盖四路并发提交，公开方法内部仍使用互斥串行化。
- UUIDv7 随机位使用目标 MSVC 标准库的 `std::random_device` 熵源；同毫秒及墙上时间回拨时通过 74 位单调随机序列防止本进程内重复。数据库碰撞重试在 M5-07 接入唯一约束时完成。

## 设计说明

新增 `CandidateEventManager`。配置固定列出 1～4 路相机及其 `MemoryRing`，包含候选连续帧数、确认连续帧数、候选超时和前缓存时长。每路只保留一个当前观察状态或活动事件，空间上限由相机数固定，不保存无界历史。

检测入口先校验来源、相机、序号和单调时间。Idle 的首个异常进入 Suspicious；连续异常达到候选阈值时创建事件，生成 ID、版本置 1、记录首次异常和候选触发信息，立即租用 `[candidateTime-preEvent, candidateTime]`。租约不完整或获取失败不会丢弃 Candidate，而是在快照中明确标记前缓存不完整。Candidate 开始后达到确认阈值转为 Confirmed；显式 `confirm/reject` 使用 EventId 与 expectedVersion，成功后版本递增，重复相同终态命令幂等，过期不同命令返回冲突。

超时只使用传入的 `steady_clock` 时间。`advance_time()` 将到达截止时间的 Candidate 转为 Timeout。`stop()` 原子停止新观察，清除 Suspicious，并把所有 Candidate 转 Timeout；重复 stop 幂等。通知在状态提交后、管理器锁外同步调用；回调异常被捕获计数，不能破坏状态机或调用方。

### 线程和队列

不新增线程或跨线程队列。管理器使用一个互斥量串行化最多四路状态；状态和活动前缓存租约上限均为配置相机数。通知是同步回调且在锁外执行，无积压队列；回调慢会延长事件管理调用，因此生产装配只能投递到已有有界 `event.commands`/IPC 通道，M5-09 接线时遵守容量 256 和满载策略。停止不依赖普通队列空位，直接调用 `stop()`。

### 持久化与恢复

不适用。当前事件、版本和租约均为进程内状态；事件目录和 SQLite 持久化分别属于 M5-06/M5-07。服务停止会产生 Timeout 通知，后续持久化层接入时负责保存该事实。

### 错误和降级

- `SYS_CONFIG_INVALID`：相机为空/重复/超过四路、空环指针、非法阈值、超时或前缓存时长；拒绝创建。
- `PIPELINE_FRAME_ORDER_VIOLATION`：未知相机、序号/单调时间回退或检测结果字段不一致；拒绝当前结果，其他相机继续。
- `EVENT_NOT_FOUND`：终态命令找不到当前活动 EventId。
- `EVENT_VERSION_CONFLICT`：expectedVersion 过期且命令不是已完成终态的幂等重放。
- `EVENT_INVALID_TRANSITION`：目标终态不允许，或服务已停止后提交新检测结果。
- `EVENT_BUFFER_INCOMPLETE` 只记录为 Candidate 的前缓存保护状态；事件仍创建并通知，避免静默丢候选。

## 实施步骤

- [x] 1. 新增候选事件公开模型、UUIDv7 EventId 生成和配置校验，并补充 `paperbreak_event` 对算法接口的允许依赖。
- [x] 2. 实现多相机状态推进、Candidate 前缓存租约、版本化确认/拒绝、超时、通知隔离和确定性停止。
- [x] 3. 新增单元测试，覆盖全部状态、阈值边界、缓存保护、幂等/版本、四路并发、回调异常和停止。
- [x] 4. 更新错误码/路线图与计划，运行格式检查、Debug/Release 构建和非硬件 CTest。

## 验证计划

### 自动化测试

- Idle、Suspicious、Candidate、Confirmed 的连续帧精确边界，以及正常帧复位 Suspicious；
- Candidate 分配规范 EventId、保留首次/候选触发、立即持有前缓存租约并标记后收集；
- 显式确认/拒绝、重复相同命令幂等、过期版本冲突、非法 ID/转换；
- Candidate 截止前保持、精确截止点 Timeout，墙上时间跳变不影响超时；
- 重复检测序号不创建第二事件，回退/未知相机返回稳定错误且可恢复；
- 四路相机并发触发得到四个不同 ID，状态互不影响；
- 回调异常被隔离并计数；服务停止将 Candidate 终结、清除 Suspicious、拒绝新结果，重复停止幂等。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行 `PaperBreakTests --gtest_filter=EventCandidate*`、本任务 C++ 文件 clang-format dry-run、全仓 `format-check` 和 `git diff --check`。

### 人工或硬件验证

- 环境：不适用；使用构造帧、固定内存池和 M5-02 数据模型测试。
- 步骤：不执行实体相机或 MVS SDK 测试。
- 预期：M5-04/M5-09 后续接线后再验证完整前后窗口和 IPC/UI 端到端行为。
- 证据保存位置：本计划验证证据表。

## 回滚与恢复

本任务不修改持久数据。失败时移除新增状态机源文件和测试，恢复 event/test CMake 与文档状态即可；不得删除用户配置、缓存或事件数据。

## 验收标准

- [x] 六种状态及规定转换可观察且边界确定；
- [x] Candidate 立即获得不可变 EventId、触发元数据、前缓存保护状态和后收集标志；
- [x] 连续帧、超时、拒绝、并发相机、重复触发、版本和停止测试全部通过；
- [x] 管理器固定最多四路，不新增无界队列、动态线程或持久业务单例；
- [x] Debug/Release 构建和非硬件 CTest 已实际运行并记录；
- [x] 未实现 M5-04 及之后功能。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、领域模型、M5-01/M5-02 实现与测试；创建计划，状态 in-progress。
- 2026-08-04：完成候选领域模型、UUIDv7、最多四路状态串行化、前缓存租约、通知、版本命令、超时和停止实现，并新增 9 项定向测试。
- 2026-08-04：Debug/Release 全量构建与两套非硬件 CTest 23/23 通过，unit 入口 198/198；记录既有全仓格式阻断，计划状态改为 completed。

## 决策记录

- DEC-001：M5-03 管理器直接持有 M5-01 前缓存租约；后窗口只记录已开始，不提前实现 M5-04 的窗口完成/合并。
- DEC-002：连续异常总数同时驱动 Candidate 和 Confirmed 阈值；Candidate 必定先提交并通知，再允许到达 Confirmed。
- DEC-003：每相机只有一个当前事件，终态由通知交给后续持久化层；管理器不保存无界事件历史。
- DEC-004：通知锁外同步执行并隔离异常，不在本任务新增通知线程或队列。

## 意外发现

- 首次定向测试发现 `steady_clock` 纪元附近的饱和减法先计算“当前时间减最小时间”会造成有符号 duration 溢出，使前缓存窗口被误判不完整；改为先与 `min + duration`/`max - duration` 比较后修复，精确边界测试通过。
- CMake 继续报告既有 `SQLite::SQLite3` 目标弃用开发者警告；本任务未修改存储依赖。
- 全仓 `format-check` 继续首先报告未修改的 `src/console/src/preview_client.cpp` 格式不一致，与 M5-01/M5-02 记录相同；本任务未越界修复。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 实施前工作区为空。 |
| 2026-08-04 | `PaperBreakTests --gtest_filter=EventCandidate*`（Debug/Release） | 通过 | 各 9/9；覆盖六态、阈值、前缓存、UUIDv7、超时、幂等/版本、四路并发、回调异常、配置和停止。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-debug`、全量构建、`ctest --preset local-windows-vs2026-debug` | 通过 | MSVC `/W4 /WX`；23/23，Debug unit 198/198。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-release`、全量构建、`ctest --preset local-windows-vs2026-release` | 通过 | MSVC `/W4 /WX`；23/23。 |
| 2026-08-04 | 本任务文件 `clang-format --dry-run --Werror` | 通过 | 新增头文件、实现和测试无格式差异。 |
| 2026-08-04 | 全仓 `format-check` | 受既有问题阻断 | 未修改的 `src/console/src/preview_client.cpp` 格式不一致。 |
| 2026-08-04 | `git diff --check` | 通过 | 无空白错误。 |
| 2026-08-04 | 实体相机、服务配置、IPC/UI、完整后窗口 | 未执行/未实现 | M5-03 使用构造帧和固定内存池；后窗口/合并属于 M5-04，端到端接线属于后续任务。 |

## 完成摘要

M5-03 已提供固定最多四路的候选事件管理器，完成六态转换、连续帧阈值、单调超时、UUIDv7 EventId、Candidate 前缓存保护、后收集开始标志、通知隔离、版本化确认/拒绝、重复幂等和确定性停止。Debug/Release 构建、两套 23/23 CTest 和 9 项定向测试通过；M5-04 的精确后窗口/合并及服务 IPC/UI 接线保持在后续任务范围。
