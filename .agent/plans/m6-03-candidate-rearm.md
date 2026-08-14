# M6-03 跟进：候选事件恢复后重新布防 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-13
- 最后更新：2026-08-13
- 路线图条目：M6-03 候选确认和降级（独立跟进）
- 关联需求：需求 4.9～4.12、5、6、10、13；架构 5、7～9、12；配置 schema v6

## 目的与可观察结果

同一服务进程生命周期内，同一相机从首次异常到画面完成稳定恢复只创建一条自动来源 Candidate。Candidate 进入 Confirmed、Rejected 或 Timeout 后，该相机保持未布防；只有 cooldown 到期且检测器连续输出配置时长的 `triggered=false` 后才重新布防。人工测试仍可显式绕过锁存和冷却。四台相机的锁存、恢复和计数相互独立，既有事件窗口合并、16 条来源上限和持久化格式不变。

## 范围

### 范围内

- schema v6、`algorithm.rearmDurationMs`、v2～v5 内存迁移、严格校验、统一 v6 保存和回滚说明。
- 候选状态机正交重新布防锁存、恢复区间、抑制计数、人工绕过和快照字段。
- 每相机算法运行快照、IPC、监控与 Console 指标；Qt 算法配置页字段“重新布防稳定时间 (ms)”。
- 有界热重配置完成排空后导出/导入每相机重新布防种子，保留锁存和未到期 cooldown，但不继承正常累计时长。
- 单元、运行时集成、配置、IPC、Qt 客户端和页面测试，以及需求、架构、配置说明、IPC 和路线图证据。

### 范围外

- SQLite、manifest、事件目录、上传协议 schema 或既有事件窗口/16 条来源规则的修改。
- 跨服务进程持久化重新布防状态、算法阈值重标定、MVS 适配器修改、后续里程碑。
- 未经授权纳入或修改未跟踪的 `docs/mynote/图像帧处理流程.md`。
- 以 Mock/模拟测试替代实体相机和目标工控机验收结论。

## 当前基线

- 当前公开配置是 schema v5；v2～v4 迁移为 v5，算法字段包含 `confirmationDurationMs` 和 `cooldownMs`，但没有恢复稳定时长。
- `CandidateEventManager::CameraTracker` 以 `cooldown_until` 表示冷却；处理下一条结果时只要到期便清除，随后同一异常结果可再次进入 Suspicious/Candidate。
- Candidate 终态保留到下一条检测结果，再释放事件租约和回到 Idle；`CandidateEventState` 枚举及事件窗口/持久化契约已经稳定。
- 服务每启用相机有独立 Lane、DetectorHost、自动 latest-wins 槽和人工保留槽；全局结果队列容量 256，由单事件线程串行调用候选状态机。
- 热重配置先准备新管线，再停止旧管线、排空并复制 Lane 指标；当前未携带候选/冷却/锁存业务状态。
- IPC/Console 已贯通算法配置和固定 32 项运行指标。任务开始时 `git status --short` 无输出。

## 前置条件与假设

- 自动化测试使用内存帧和 Mock 检测器；本机无实体相机时硬件验收标记未执行。
- 严格正常仅指 `triggered=false`；`triggered=true` 即使置信度低于候选阈值仍立即重置恢复区间。
- 相邻正常结果允许的最大间隔是当前配置处理周期的两倍；超过后从当前结果重新计时。
- `rearmDurationMs=0` 仍要求观察到一条正常结果，并同时满足 cooldown；无结果、检测失败或服务重启不累计恢复。
- 活动 Candidate、终态、冷却中或已锁存相机在成功热重配置后均以锁存种子进入新管线；新启用相机从已布防状态开始。
- M6-00 继续 blocked，检测器仍为原型；本任务不改变算法验收结论。

## 设计说明

### 接口与数据结构

- `AlgorithmConfig` 和 `CandidateEventManagerConfig` 增加 `rearm_duration`；JSON/IPC/Qt 使用 `rearmDurationMs`，范围 `0..3600000`，默认 500。
- `CandidateCameraSnapshot` 增加 `rearm_pending`、`recovery_started_at`、`rearm_suppressed_results`，不改变 `CandidateEventState`。
- `CameraTracker` 保存锁存、恢复起点/最近正常结果和累计抑制数。任一终态转换都设置锁存并清空恢复区间，同时保留现有 cooldown 截止。
- 候选管理器提供固定最多四路的重新布防种子导出/导入接口；种子只携带是否应锁存及尚未到期的 cooldown 截止，不携带正常累计时长或事件持久化数据。
- 每相机 `AlgorithmLaneMetrics`、聚合运行快照、IPC JSON 和 Console 指标增加 `rearmPending` 与 `rearmSuppressedResults`。

### 状态转换

- 下一检测结果继续释放终态 Candidate，但不会因 cooldown 到期自动恢复可触发状态。
- 锁存期间：正常结果启动/延续恢复区间；异常结果清空恢复区间，非人工异常每条只递增抑制数。
- 正常结果若与上一正常结果间隔大于两周期，从当前结果重新计时。当本结果同时满足 cooldown 和恢复时长时清除锁存；本结果不创建 Candidate。
- 人工结果可绕过锁存/cooldown。若已有活动 Candidate，人工结果只参与其判定；否则可显式创建来源。人工 Candidate 进入终态后按相同规则重新锁存。
- 外部确认只作用于活动 Candidate 的判定，不改变锁存或恢复区间。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `algorithm.frames[i].automatic` | 帧分发 | 相机算法 worker | 1 | latest-wins；既有策略不变 | 最多排空最新 1 帧 | sampled/missed 指标不变 |
| `algorithm.frames[i].manual` | 人工请求后的首帧 | 相机算法 worker | 1 | 已有请求合并；既有策略不变 | 优先且最多排空 1 帧 | 人工处理计数不变 |
| `algorithm.results` | 最多四个 worker | 单事件线程 | 256 | 拒绝并按既有策略降级来源 Lane | 关闭生产后有界排空并完成排序 | 新增逐相机锁存/抑制快照 |

重新布防状态只由现有单事件线程内的候选管理器更新，不引入新线程、队列或阻塞操作。热重配置在旧结果生产端停止并有界排空后导出种子，再在新管线上应用。

### 持久化与恢复

- v2～v5 在内存补入 `rearmDurationMs=500` 并升级为 v6；任何保存、配置历史和 IPC 更新统一序列化 v6。
- 服务重启从已布防开始，不持久化 CameraTracker 状态。事件数据库、manifest、事件目录和上传协议均不变。
- 旧程序不能读取 v6；回滚旧二进制前必须从配置历史恢复最后一份 v5 文件，不能只手工修改版本号。

### 错误和降级

- 非法 `rearmDurationMs` 返回既有稳定配置错误；抑制自动异常是正常行为，不报警、不返回新错误码、不进入 trigger capacity。
- 检测失败/无结果不证明恢复；现有算法失败、积压和结果队列拒绝策略不变。
- 新管线构建或启动失败保留旧管线及状态；成功切换才消费导出的种子。

## 实施步骤

- [x] 1. 升级配置模型、schema、解析/迁移/序列化和默认文件，覆盖 v2～v6 严格测试、边界与回环。
- [x] 2. 在候选状态机实现锁存、恢复区间、人工绕过、抑制计数和种子接口，覆盖全部状态与精确时间边界。
- [x] 3. 在服务运行时贯通配置、逐相机快照/指标、热重配置种子和运行时集成场景。
- [x] 4. 扩展算法 IPC DTO、系统命令 JSON、Qt `AlgorithmClient`、指标描述符及配置页保存/展示测试。
- [x] 5. 更新需求、架构、配置说明、IPC 和路线图 M6-03 跟进证据，不改持久化协议文档。
- [x] 6. 运行定向测试、Debug/Release 配置/构建/CTest、格式检查、MSVC 静态分析和 `git diff --check`，记录既有阻断及硬件未执行项。

## 验证计划

### 自动化测试

- 状态机：持续异常跨多轮 cooldown 仅一条自动 Candidate；499/500 ms；异常/低置信 triggered 重置；两周期间隔；冷却期累计；三种终态；人工绕过且不并存；四路隔离；外部信号不解锁；零时长首条正常。
- 运行时：单路持续异常无 trigger-capacity 且 `triggerCount=1`；不足/达到恢复时长；活动窗口内合并与冻结后新事件；四路每相机一条并合并；成功/失败热重配置保持锁存。
- 配置/客户端：v2～v5 迁移、v6 严格字段、0/3600000 边界、超界拒绝、序列化回环、IPC 读写、Qt 映射/页面保存和指标值。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行任务相关 clang-format/仓库格式检查、MSVC 静态分析和 `git diff --check`，只记录实际结果。

### 人工或硬件验证

- 环境：目标 Windows 工控机；单台及最多四台 MV-CS020-60GM；持续异常/正常画面切换能力。
- 步骤：单路持续异常；不足 500 ms 和至少 500 ms 正常后再次异常；四路同时持续异常；观察来源 Candidate、规范事件合并、triggerCount、锁存和抑制指标。
- 预期：持续异常每相机最多一条自动来源；满足稳定恢复后下一次异常才创建新来源；四路正确合并。
- 证据：后续硬件记录；本任务无实体相机时明确标记“未执行”，不以 Mock 代替。

## 回滚与恢复

- 代码回滚不删除或修改既有事件数据。回滚旧程序前从配置历史恢复最后一个 v5 文件。
- 热重配置失败继续保留旧管线及锁存状态；候选种子只在新管线成功启动后应用。
- SQLite、manifest、事件目录和上传协议不迁移，无数据回滚步骤。

## 验收标准

- [x] 同一相机持续异常只创建一条自动 Candidate，三种终态均锁存，恢复边界和人工绕过符合锁定规则。
- [x] 四相机状态、计数和热重配置相互独立；旧窗口合并、16 条上限与事件文件格式保持不变。
- [x] schema v6、v2～v5 迁移、IPC、Qt 配置页和全部新快照/指标贯通。
- [x] 相关自动化测试及 Debug/Release 验证完成，或对实际阻断如实记录。
- [x] 需求、架构、配置、IPC、路线图和本计划证据同步；未修改未跟踪笔记。
- [x] 实体相机验收未执行时明确记录，未作硬件结论。

## 进度记录

- 2026-08-13：读取 ExecPlan 规范、需求、架构、路线图及相关源码/测试；确认工作树干净并创建独立跟进计划，状态 `in-progress`。
- 2026-08-13：完成 schema v6、候选重新布防状态、热重配置种子、运行指标、IPC、Qt 页面和自动化测试；同步需求、架构、配置、IPC 与路线图。
- 2026-08-13：完成 Debug 单元与 Release 构建/定向验证，记录运行中 Debug 实例造成的全量构建/单实例 smoke 阻断，以及既有存储性能和静态分析阻断；状态更新为 `completed`。
- 2026-08-13：用户停止旧 Debug 进程后完成全量重建，Debug/Release 服务均成功校验 schema v6；同步更新算法页 smoke 的 34 张指标卡断言，Debug CTest 最终 30/30。

## 决策记录

- DEC-001：重新布防作为正交状态，不扩展 `CandidateEventState`，避免改变事件决策和持久化语义。
- DEC-002：热重配置种子只继承锁存和未到期 cooldown，不继承正常累计时长，因为检测器参数可能变化。
- DEC-003：锁存期间自动异常仅累计抑制数，人工来源由既有 `manual_test` 语义显式绕过。

## 意外发现

- 当前 cooldown 到期会在处理同一条结果前直接清除，导致持续异常周期性重新触发；现有双单槽和单事件线程边界足以在不新增队列的情况下修复。
- 验证期间发现用户正在运行 Debug 服务和控制台；未终止这些进程，因此 Debug 全量链接分别被两个 exe 文件锁阻断，服务/Qt smoke 也受 IPC/单实例互斥影响。
- 旧进程停止后发现其 Debug 二进制仍是 schema v5 版本；全量重建消除 `SYS_CONFIG_SCHEMA_UNSUPPORTED`。重建后的算法页 smoke 还暴露旧的 32 张指标卡断言，更新为 34 后通过。
- Release 性能测试一次测得 97.48 MiB/s，低于既有 100 MiB/s 门槛；全仓 MSVC 静态分析仍被未修改的 `src/storage/src/nvme_cache.cpp:73` C28020 阻断，均未越界修改。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-13 | `git status --short` | 通过 | 任务开始时无输出 |
| 2026-08-13 | Debug/Release `PaperBreakTests` 定向测试 | 通过 | Candidate、配置、IPC、Qt 客户端、指标、热重配置及单/四相机场景共 45 项通过 |
| 2026-08-13 | `ctest --preset local-windows-vs2026-debug -R '^unit$'` | 通过 | 421 项：420 通过，1 项 Release-only 性能测试按设计跳过 |
| 2026-08-13 | Debug 全量 build/CTest | 通过 | 旧进程停止后全量构建成功；修正 34 张指标卡 smoke 断言后 CTest 30/30 |
| 2026-08-13 | Debug/Release `--validate-config` | 通过 | 两个服务均以退出码 0 接受 `configSchemaVersion=6` |
| 2026-08-13 | Release configure/build | 通过 | `/W4 /WX` 全量构建成功；最终变更后重复构建成功 |
| 2026-08-13 | Release 任务相关 `PaperBreakTests` | 通过 | 45/45，通过 Candidate、配置、客户端、指标、单/四相机持续异常及热重配置场景 |
| 2026-08-13 | `ctest --preset local-windows-vs2026-release` | 部分通过 | 初次 29/30；当时运行中的 Debug 控制台导致 `qt_console_smoke` 单实例冲突；重建后该 smoke 已在 Release 单独通过 |
| 2026-08-13 | Release CTest 排除 Qt smoke | 既有性能阻断 | 28/29；存储吞吐 97.48 MiB/s，低于既有 100 MiB/s 门槛；另外 417 个单测通过 |
| 2026-08-13 | `format-check`、`git diff --check` | 通过 | 全仓 C++ 格式及差异空白检查通过 |
| 2026-08-13 | MSVC `/analyze` | 任务源通过 | event、config、console model 目标通过；service-core 与 Console 源直接 `ClCompile` 通过；全依赖被未修改的 NVMe C28020 阻断 |
| 2026-08-13 | 实体相机/目标工控机验收 | 未执行 | 未作硬件结论，Mock 结果不替代硬件证据 |

## 完成摘要

已完成候选事件恢复后重新布防：同相机终态后保持锁存，cooldown 与连续严格正常时长同时满足才解除；自动异常只累计抑制，人工可显式绕过。配置升级为 schema v6，服务、IPC、监控、Qt 配置页及 34 项 Console 指标同步；热重配置继承锁存和未到期 cooldown，不继承正常累计时长。SQLite、manifest、事件目录、上传协议、事件窗口与 16 条来源上限未修改。Debug 全量构建和 CTest 最终通过，Debug/Release 服务均已验证可读取 v6；剩余限制仅为已记录的既有 Release 存储性能门槛和既有 NVMe 静态分析告警。实体相机和目标工控机验收未执行。
