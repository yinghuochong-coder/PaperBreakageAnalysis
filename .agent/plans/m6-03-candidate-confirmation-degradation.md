# M6-03：候选确认和降级 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M6-03
- 关联需求：需求 4.7、4.9、4.10、4.20、4.21、7、13.1～13.4、阶段 M6；架构 5.2、7～9、14、17、19；ADR-016

## 目的与可观察结果

把 M6-01/M6-02 检测器接入服务的有界事件运行时，并形成可测试的候选确认与故障降级合同：候选/确认分别应用置信阈值和连续帧，支持可选外部信号门控及决定后的冷却；算法队列满时丢弃最旧待检测帧并计数，单帧插件失败只丢该帧，连续失败或持续积压进入“仅人工触发”降级并报告稳定业务错误；配置候选构建失败不替换活动实例和配置。服务指标不再用 M4 占位值，而是呈现实际队列、耗时、跳帧、失败、候选、确认和降级状态。

M6-00 仍缺少冻结数据集、批准阈值和目标机预算，因此 classical 检测与本任务的默认策略继续标记为原型，不宣称正式断纸验收通过。

## 范围

### 范围内

- 扩展候选状态机：候选/确认置信阈值、独立连续计数、可选外部信号门控、单调时钟冷却；
- 服务运行时通过 `DetectorPluginRegistry`/`DetectorHost` 选择 Mock 或 classical 插件；
- 容量固定的算法帧队列采用 drop-oldest，记录深度、高水位、跳帧和积压连续次数；
- 捕获单帧算法错误；连续算法错误或持续积压后切换到 `manual-trigger-only`，人工触发继续可用；
- 重配置先完整构造候选管线，失败保留原管线；
- 公开运行时快照和监控指标，错误观察者接入算法积压/降级报警；
- 单元/集成测试、构建和路线图证据。

### 范围外

- M6-00 数据集、阈值审批、四路目标机性能和正式算法验收；
- M6-04 算法配置 UI、测试当前图像和结果叠加；
- 生产 PLC/Plant IO 协议、现场安全动作或 M8 上位机协议；
- 改变配置 schema 版本、增加外部模型制品或算法调参；
- 相机采集、事件存储、IPC 复核和预览的无关重构。

## 当前基线

- 工作树开始时干净；M6-00 为 blocked，M6-01/M6-02 已完成且均保持原型标记。
- `CandidateEventManager` 已有 Idle/Suspicious/Candidate/Confirmed/Rejected/Timeout、连续触发帧和候选时立即缓存租约，但不看 `confidence`、无冷却、无外部信号。
- `EventRuntime` 使用容量 64 的全局帧队列，满时返回 `ALGORITHM_QUEUE_FULL`；工作线程直接调用具体 `MockTriggerDetector`，尚未装载 `DetectorHost`/classical。
- 单帧 detector 失败已被事件线程报告并继续循环，但没有连续失败状态、积压降级或算法实际指标。
- 服务监控仍注册 `AlgorithmPlaceholderMetricSource`，全部算法指标标记不可用。

## 前置条件与假设

- M6-00 未批准降采样率和生产阈值；队列仅在实际满载时 drop-oldest，不主动定频抽样，所有默认阈值仍为原型。
- 首版批准的本地安全降级是 `manual-trigger-only`：停止自动视觉候选，继续采集、内存缓存、事件管理与受控人工触发；不得声称完成 PLC 安全停机。
- 连续失败和连续满载门限作为运行时有界保护参数提供，生产默认值固定且可在后续配置评审中调整；测试可注入更小门限。
- 外部信号只作为进程内状态输入和确认门控；生产 Plant IO 适配不在 M6-03 范围，自动化测试直接调用窄接口。
- 当前事件运行时仍是每相机保序的单工作线程组合；本任务不引入无界线程池，也不在相机回调执行检测、编码或 I/O。

## 设计说明

每个启用相机 lane 拥有一个 `DetectorHost` 和活动 `DetectorConfig`。编译期注册表内置 `mock-trigger` 与 `classical-vision`；配置 `type=mock` 映射到 Mock，其余使用插件 ID。禁用算法时自动视觉处理处于 disabled，但人工触发通过事件线程合成可追溯的 `ManualTest` 结果，因此不依赖具体插件私有 API。

服务入口先将共享帧视图登记到有界内存环，再投递固定容量的待检测队列。满载时只移除最旧待检测帧、接收新帧、递增 skipped/drop-oldest 指标并限频报告积压，不损失事件前后窗缓存；达到连续门限后视觉管线进入 `manual-trigger-only`。算法处理失败同样只影响当前帧；成功处理清零连续失败计数，达到失败门限则进入同一确定性降级。降级不自动猜测恢复条件，只有成功事务式重配置才恢复自动检测。

候选管理器将 detector 的 `triggered/anomalous/confidence` 归一为两级资格：达到 candidate 阈值才计入候选连续帧，达到 confirmation 阈值才计入确认连续帧。外部信号策略为 `not-used` 或 `required-active`；后者只在连续确认条件与活动外部信号同时满足时 Confirmed。终态后从决定单调时间开始冷却，冷却内的自动结果被接受和计数但不会产生新候选。人工 confirm/reject 命令保持现有乐观并发语义。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `algorithm.frames[i]`（当前服务组合队列中的每相机 lane） | 相机帧投递回调 | 事件/算法工作线程 1 个 | 默认 8/相机，1～256 | drop-oldest，接收最新帧；计数并在持续满载时降级 | 停止接收后排空至线程退出；停止令牌不依赖队列槽 | depth/capacity/high-watermark/submitted/processed/skipped/backlog streak |

检测、候选、窗口和持久化仍不在相机回调执行。工作线程顶层和 DetectorHost 插件边界均捕获异常；析构、`request_stop` 和 `join` 保持确定性关闭路径。

### 持久化与恢复

不改变配置 schema、事件 manifest 或 SQLite schema。降级、连续计数和外部信号为进程内状态；重启从配置重建。事件候选仍在创建时立即租赁缓存，现有原子事件提交与恢复流程不变。

### 错误和降级

- 首次/持续队列满：`ALGORITHM_QUEUE_BACKLOG`（Warning/Error），旧待检测帧被显式跳过；
- 单帧插件失败/超时：保留 DetectorHost 原业务码并跳过该帧；
- 连续失败或持续积压：`ALGORITHM_DEGRADED`（Error），source 为相机或 algorithm，状态切换 `manual-trigger-only`；
- 降级时自动视觉帧不生成结果，人工触发仍可形成候选；
- 配置/插件创建、初始化或更新失败：返回原稳定错误，旧管线、旧实例、旧修订与接收状态不变；
- 外部信号相机未知或时间回退：`SYS_CONFIG_INVALID`/`PIPELINE_FRAME_ORDER_VIOLATION`，不改变其他相机状态。

## 实施步骤

- [x] 1. 扩展 `CandidateEventManager` 的阈值、确认计数、外部信号与冷却语义，并补边界/组合/时间测试。
- [x] 2. 将 `EventRuntime` lane 切换到编译期注册表和 `DetectorHost`，实现配置到插件参数映射与失败保留旧管线。
- [x] 3. 实现队列 drop-oldest、单帧失败隔离、连续失败/积压降级、人工触发保底及运行时快照。
- [x] 4. 用实际算法指标源替换占位源，接入报警/日志错误观察者并补服务集成测试。
- [x] 5. 运行定向测试、Debug/Release 全量构建与 CTest、静态分析/格式/差异检查，更新计划和路线图证据。

## 验证计划

### 自动化测试

- 候选阈值、确认阈值和连续帧在精确边界生效，低置信帧分别重置对应计数；
- 外部信号 required 时仅在视觉连续条件和活动信号同时满足后确认；冷却边界按单调时间精确恢复；
- 队列满时提交不阻塞且 drop-oldest，跳帧/高水位/积压指标准确；
- 检测器单次异常后后续帧仍处理；连续异常进入 manual-only 且只报告一次降级转换；
- 降级后人工触发仍产生事件，自动结果不再产生候选；
- 无效插件/参数重配置失败后，旧配置和旧运行状态继续处理；
- classical 与 mock 可通过相同宿主边界选择，指标含实际耗时、队列、跳帧、候选和确认。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug --output-on-failure
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release --output-on-failure
git diff --check
```

### 人工或硬件验证

- 自动化测试使用内存帧/模拟检测器，不依赖实体相机。
- 实体相机、冻结数据集、四路 240 frame/s 目标机性能、真实断纸准确性、生产 PLC 外部信号和 7×24 小时稳定性均未执行；缺少 M6-00 批准输入及目标硬件/现场协议。

## 回滚与恢复

回退候选状态机扩展、服务算法宿主接入、指标源和测试即可恢复 M6-02/M5-09 基线。没有持久化格式或用户数据迁移，不删除生产配置/事件；重配置失败始终继续使用旧管线。

## 验收标准

- [x] 连续帧、候选/确认阈值、冷却和可选外部信号组合有确定合同与测试；
- [x] 算法队列有明确容量、drop-oldest 策略、跳帧计数和积压报警；
- [x] 单次插件异常不终止工作线程或采集服务；
- [x] 连续失败/积压进入可观察的 `manual-trigger-only`，人工触发仍可用；
- [x] 配置失败保留旧实例、旧配置和可用运行状态；
- [x] 服务输出实际算法耗时、队列、跳帧、失败、候选/确认和状态指标；
- [x] Debug/Release 构建和非硬件 CTest 已运行并如实记录；
- [x] M6-00 保持 blocked/原型，未进入 M6-04。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、计划规范和 M6-00～M6-02 记录；检查候选、DetectorHost、事件运行时、监控、配置和测试基线，确认工作树干净，创建计划并进入 in-progress。
- 2026-08-04：完成两级阈值/连续帧、外部信号、截止时间与冷却状态机；将 Mock/classical 接入统一宿主，并实现事务式配置候选管线。
- 2026-08-04：完成每相机容量 8 的待检测队列、drop-oldest、跳帧/积压计数、单次失败隔离、连续失败/积压降级和仅人工触发保底；内存环在算法入队前登记，过载只跳过检测。
- 2026-08-04：替换算法占位指标，接入算法日志及积压/降级报警；补齐 19 项定向测试与错误码、路线图证据。
- 2026-08-04：完成 Debug/Release 全量构建与 CTest、MSVC 静态分析、任务文件格式和差异检查，计划转为 completed；M6-00 继续 blocked，未启动 M6-04。

## 决策记录

- DEC-001：批准降级状态采用 `manual-trigger-only`；它保留采集、缓存和人工触发，不把未实现的 PLC 安全动作伪装为成功。
- DEC-002：队列满采用架构基线的 drop-oldest，不使用未经 M6-00 批准的周期降采样；每次实际跳帧均计数。
- DEC-003：外部信号仅提供窄的进程内确认门控；生产 Plant IO 适配留在批准协议里程碑，不扩展本任务。
- DEC-004：配置更新以完整候选管线构建成功为提交点，避免部分 lane 已切换而其他 lane 失败。
- DEC-005：帧先进入有界内存环，再进入可丢弃的算法队列；内存环容量增加一个算法队列延迟窗口，既保留配置的前置历史，也不让算法积压破坏事件缓存。
- DEC-006：错误观察者在释放事件运行时队列锁后接收积压/降级通知，允许观察者安全查询运行时而不发生锁重入。

## 意外发现

- 现有 `EventRuntime` 的“事件帧队列”同时承担内存环登记与算法执行入口；若直接 drop-oldest 会同时破坏事件缓存。实现将线程安全的有界内存环登记提前到入队侧，并按已规划的算法队列预算扩展环容量，因此跳帧仅影响检测且无需拆分工作线程。
- 现有配置已含 candidate/confirmation 阈值、连续帧和冷却字段，无需在 M6-03 改 schema；外部信号策略不写入生产配置，避免提前进入 M6-04/M8。
- 服务配置事务原本只把 `/event` 变更交给事件运行时；若不扩展相关路径，算法和 Plant IO 配置虽能落盘却不会重建检测管线。现已将 `/algorithm` 与 `/plantIo` 纳入同一事务式应用器。
- 人工触发要求“请求后的精确下一帧”；drop-oldest 不能淘汰该目标帧。队列因此保护已选定的人工目标，极端容量下改为跳过新到的非目标帧。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | 初始工作树与文档/源码基线检查 | 通过 | 工作树干净；M6-03 尚无计划或实现，算法监控为占位。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter=EventCandidateState.*:EventRuntimeIntegration.*` | 19/19 通过 | 12 项候选状态机、7 项服务运行时集成；全部使用内存帧和模拟/编译期插件。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-debug`、`cmake --build --preset local-windows-vs2026-debug`、`ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过，24/24 | Debug `/W4 /WX` 全量构建与非硬件 CTest。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-release`、`cmake --build --preset local-windows-vs2026-release`、`ctest --preset local-windows-vs2026-release --output-on-failure` | 通过，24/24 | Release `/W4 /WX` 全量构建与非硬件 CTest。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-static-analysis`、`cmake --build --preset local-windows-vs2026-static-analysis --target paperbreak_event paperbreak_service_core PaperBreakEdgeService` | 通过 | 任务涉及目标的 MSVC 静态分析无错误。 |
| 2026-08-04 | 任务 C/C++ 文件 `clang-format --dry-run --Werror`；`git diff --check` | 通过 | 仅检查本任务 9 个 C/C++ 文件及全部差异。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 未通过（既有问题） | 未修改的 `src/console/main.cpp:467、468、515、516` 不符合 clang-format；与 M6-02 记录一致，本任务未越界修改。 |
| 2026-08-04 | 实体相机/冻结数据集/目标机四路性能/真实 PLC/7×24 | 未执行 | 缺少 M6-00 批准输入、目标硬件和现场协议；不得据此宣称生产算法验收。 |

## 完成摘要

M6-03 已完成：候选确认合同、算法宿主接入、显式跳帧与计数、失败隔离、连续异常/积压降级、配置失败回滚以及实际监控指标均已实现并通过自动化验证。M6-00 仍为 blocked，classical 和阈值继续保留原型标记；未启动 M6-04。

## 纠正实施记录：每相机独立算法 Lane（2026-08-08）

- 状态：completed；本节仅追加纠正记录，不改写 2026-08-04 的历史验证证据。
- 纠正原因：复核发现既有实现虽按相机计算队列容量，实际仍使用全局 deque、单一算法线程和全局降级/连续计数；`algorithm_snapshot(cameraId)` 也返回聚合指标，不满足单相机阻塞与故障隔离目标。
- 已确认设计：每个启用相机独占有界 `algorithm.frames[CAMxx]`、串行 worker、DetectorHost、人工触发状态和指标；容量固定的 `algorithm.results` 由单一事件线程消费。结果按单调时间、相机 ID、序号稳定排序，以所有 Lane 队列/在途帧的最早单调时间为安全水位。
- 生命周期约束：启动先创建事件线程、再创建 Lane worker，并在统一启动门前等待；任一候选线程准备失败即取消并回收已创建线程。停止先禁止提交并排空 Lane，再排空结果入口和冻结窗口，最后停止 JPEG/持久化。重配置完整构造候选检测器、Lane、队列和启动门后的线程组，成功后才排空旧线程组并切换。
- 接口约束：新增 `partially-degraded`、逐 Lane 快照和结果入口指标；IPC 兼容现有 JSON 并追加 `consecutiveBacklogEvents`、`resultQueueRejected`；不修改配置 schema 或事件持久化格式。
- 验证限制：只使用模拟检测器和内存帧；实体相机、目标工控机四路吞吐、冻结数据集准确率、PLC 和 7×24 小时测试仍未执行。

### 纠正结果与验证证据

- 已完成每启用相机独占 Lane、容量 256 的有界结果入口、安全水位排序、单线程事件处理、Lane 私有降级/连续计数、`partially-degraded` 聚合状态、启动/停止/重配置线程组切换、逐相机 IPC/Console/监控指标。
- 8 项 `EventRuntimeLanes` 定向测试覆盖四路并行与相机内保序、单 Lane 积压隔离、逐 Lane 失败降级及人工触发、三种聚合状态与重配置恢复、安全水位、结果入口满载、线程准备失败回滚和重复启停；另更新 IPC、客户端状态及监控测试。
- `EventRuntimeLanes.*` 在 Debug 下重复 20 轮，共 160 次定向测试全部通过。
- `cmake --build --preset local-windows-vs2026-debug` 与 `ctest --preset local-windows-vs2026-debug --output-on-failure` 通过，CTest 28/28；Debug `PaperBreakTests` 共 364 项，其中 363 项通过、1 项硬件基线按设计跳过。
- `cmake --build --preset local-windows-vs2026-release` 与 `ctest --preset local-windows-vs2026-release --output-on-failure` 通过，CTest 28/28；直接运行完整 Release `PaperBreakTests` 时发现既有 `UplinkSimulator.ServesSessionWebSocketPreviewAndPersistentResumableUpload` 时序波动，隔离重复 3 次为 2 次通过、1 次失败，不属于本次 Lane 改动。
- `cmake --build --preset local-windows-vs2026-static-analysis --target paperbreak_service_core PaperBreakEdgeService PaperBreakEdgeConsole` 通过；任务 C++ 文件 clang-format 检查和 `git diff --check` 通过。
