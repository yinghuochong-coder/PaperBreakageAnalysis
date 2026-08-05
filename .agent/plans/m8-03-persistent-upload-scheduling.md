# M8-03：持久化上传调度 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：`docs/roadmap/development-roadmap.md` M8-03
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.18、SQLite 上传任务管理；`docs/architecture/system-architecture.md` 9.5、12.6、13、14

## 目的与可观察结果

已提交事件或报警可以建立幂等的 SQLite 上传任务；任务按报警元数据、关键帧、manifest、低码率回放、原始文件的固定优先级领取。断网或进程停止不会丢失任务，重启会收回未完成的租约并继续执行。自动失败按带抖动的有上限指数退避重试，永久失败和需人工处理任务不再自动领取。任务数量和待上传字节同时受固定上限约束。

## 范围

### 范围内

- 将 schema v3 的简化 `upload_jobs` 迁移为可表达多个资源、幂等键、优先级、字节预算、租约、checkpoint 和失败分类的 schema v4。
- 为 `EventMetadataDatabase` 增加有界入队、原子领取、完成、失败、人工重试、重启恢复、查询和统计接口。
- 在 `paperbreak_uplink` 增加单工作线程持久上传调度器；执行器由调用方注入，以保持传输协议和 M8-04 分块实现解耦。
- Mock 自动化测试覆盖容量、磁盘字节上限、幂等、优先级、三类失败、退避抖动/上限、重启恢复、离线后补传及停止。
- 更新架构、协议说明、错误码（如需）、路线图状态和本计划证据。

### 范围外

- M8-04 的 HTTP/WebSocket 正式适配器、文件分块、断点续传、校验和带宽限速。
- 生产上位机、真实网络、实体相机、SCM 强杀或物理断电联调。
- 持久化远程命令结果、PLC/现场 IO、后续 M8/M9 任务。
- 自动扫描事件目录推断上传资源；调用方必须显式提交已验证的任务描述。

## 当前基线

- 任务开始时 `git status --short` 无输出，工作区无已有修改。
- `EventMetadataDatabase` 当前 schema 版本为 3；v1 创建的 `upload_jobs` 只有 `event_id` 唯一约束、状态、次数、下次时间和 checkpoint，尚无公开读写 API。
- `paperbreak_uplink` 已有 `IUplinkTransport`、Mock 和 M8-02 单线程 `UplinkRuntime`，但没有持久任务仓库或文件调度。
- `event_files` 保存已提交事件文件的相对路径、类型、摘要和大小；SQLite 不保存高速原始图像。

## 前置条件与假设

- 任务字节预算使用任务声明的待上传逻辑字节数，并在 SQLite 事务内求和；源文件存在性和逐块 checkpoint 校验属于执行器/M8-04。
- 事件资源任务引用现有 `events` 行；报警元数据允许没有 `event_id`，通过独立幂等键去重。
- 调度器不会直接持有 `IUplinkTransport`，避免与 M8-02 会话线程并发调用同一个传输对象。注入执行器必须在其实现中遵守传输所有权约束。
- 进程崩溃时 `InProgress` 任务在下次调度器启动时原子恢复为立即可领取的 `RetryWait`，不会创建新事件或新任务。

## 设计说明

storage 公开强类型上传 DTO，不暴露 SQLite 类型。`enqueue_upload_job` 在 `BEGIN IMMEDIATE` 内检查活动任务条数、待上传字节和幂等键；重复相同内容返回已有任务并标记 duplicate，不同内容稳定冲突。`claim_next_upload_job` 事务式选择到期的 `Pending/RetryWait` 任务并更新为 `InProgress`，排序为优先级降序、创建时间和 ID 升序。完成或失败更新必须匹配 `InProgress`，防止过期执行器覆盖其他状态。

调度器启动先恢复遗留 `InProgress`，然后每次只领取并执行一个任务。成功提交 `Completed`；可重试失败增加 attempts 并计算 `min(maxDelay, initialDelay*2^(attempt-1))*[1-jitter,1+jitter]`；达到最大尝试转 `ManualIntervention`。永久失败转 `PermanentFailed`，显式人工类转 `ManualIntervention`。线程用停止令牌和条件变量，停止后不再领取新任务；正在执行的回调收到停止令牌，返回后持久化最终状态或恢复为可重试状态。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| SQLite `upload_jobs` | 事件/报警提交用例 | 上传调度线程 | 配置条数与待上传字节双上限，均有硬上限 | 原子拒绝新任务，重复幂等键不占容量 | 停止后不领取；在途执行收到停止令牌并落回可恢复状态 | 各状态数量、活动字节、领取/完成/失败/恢复 |
| 调度唤醒 | 入队调用/定时器/停止请求 | 上传调度线程 | 无数据队列，仅条件变量信号 | 信号可合并 | 停止立即唤醒 | 唤醒不单独计数 |

### 持久化与恢复

schema v4 迁移在事务内重建 `upload_jobs`，保留旧行并映射为 manifest 任务；迁移前沿用现有一致备份。所有状态转换和容量检查在数据库互斥及 SQLite 事务内完成。checkpoint 是有大小上限的 JSON 对象字符串。启动恢复不删除记录，只将遗留在途任务转为到期重试并记录稳定错误码。

### 错误和降级

- `UPLOAD_ENQUEUE_FAILED`：容量/活动字节预算满或 SQLite 写失败；不创建半任务。
- `UPLOAD_JOB_CONFLICT`：相同幂等键携带不同任务内容；不覆盖原任务。
- `UPLOAD_TRANSFER_FAILED`：可重试执行失败，保持 checkpoint 并退避。
- `UPLOAD_REJECTED`：永久失败，不再自动领取。
- `UPLOAD_RETRY_EXHAUSTED`：达到尝试上限，转人工处理。
- 数据库 BUSY/损坏沿用稳定数据库错误；调度器等待后重试仓库访问，不紧循环。

## 实施步骤

- [x] 1. 定义上传任务 DTO、状态/失败语义与 `EventMetadataDatabase` API；迁移 schema v3→v4，并实现事务式入队、领取、状态转换、恢复、查询和统计。
- [x] 2. 实现 `PersistentUploadScheduler` 的配置校验、单线程生命周期、优先领取、抖动退避、三类失败和指标。
- [x] 3. 增加数据库与调度器定向测试，覆盖迁移、边界、幂等、恢复、故障分类、断网补传及停止。
- [x] 4. 运行定向测试、Debug/Release 全量构建与 CTest、静态分析/格式与 diff 检查；修复本任务引入的问题。
- [x] 5. 更新相关文档、路线图任务状态及本计划的证据与完成摘要。

## 验证计划

### 自动化测试

- schema v3 旧任务迁移到 v4，字段/状态可继续领取。
- 入队条数和字节精确边界、超限拒绝、重复相同任务幂等、重复不同内容冲突。
- 五类任务排序稳定，领取与完成状态原子转换，非在途状态拒绝错误完成。
- 遗留 `InProgress` 重启恢复；事件表不新增重复事件。
- 可重试、永久和人工三类结果；指数倍增、抖动边界、最大间隔、次数耗尽。
- Mock 离线期间任务保留，恢复后按优先级补传；停止和 join 有确定截止。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

### 人工或硬件验证

- 不需要实体相机或 MVS SDK 行为验证。
- 正式上位机、真实网络断连、进程强杀和物理断电：未执行；原因是 M8-04 正式传输和生产环境不在本任务范围。自动化测试用临时 SQLite 与 Mock 执行器验证等价状态转换。

## 回滚与恢复

代码回滚后 schema v3 程序会稳定拒绝打开 v4 数据库，不会删除数据；迁移前备份保留在配置的备份目录，可在关闭数据库后用既有 `restore_backup` 显式恢复。任务实现不得覆盖备份或删除事件文件。调度器启动/停止失败时任务仍留在 SQLite，后续启动执行恢复。

## 验收标准

- [x] `upload_jobs` 使用 SQLite 且支持每事件多个资源和独立报警元数据任务。
- [x] 五类任务严格按规定优先级和稳定次序领取。
- [x] 未完成任务数量和声明上传字节同时有配置上限及硬上限。
- [x] 可重试失败使用带抖动的有上限指数退避，达到次数上限转人工处理。
- [x] 重启恢复在途任务且幂等入队不重复创建事件或任务。
- [x] 永久失败与需人工处理不会自动重试，并可显式恢复人工任务。
- [x] 工作线程具有确定性停止路径，不进行相机回调工作。
- [x] 定向测试、Debug/Release 构建和自动化 CTest 通过；未执行的硬件/生产验证明确记录。

## 进度记录

- 2026-08-05：阅读需求、架构、路线图、M8-02 计划和相关源码；创建计划，状态 `in-progress`。
- 2026-08-05：完成 schema v4 迁移、持久任务仓库、单线程调度器、人工重试命令、文档和 7 项上传调度定向测试。
- 2026-08-05：完成 Debug/Release 全量构建与 CTest、MSVC 静态分析、修改文件格式检查和差异检查；状态更新为 `completed`。

## 决策记录

- DEC-001：持久仓库放在 storage，调度策略放在 uplink，并以执行回调隔离正式传输。原因是 SQLite 已由 storage 封装，而 M8-04 尚未定义分块适配器；影响是 M8-03 可完整验证调度可靠性，但不声称完成真实网络传输。
- DEC-002：容量统计所有未完成任务，包括永久失败和需人工处理记录；只有 `Completed` 历史可按配置上限裁剪。磁盘上限按声明的逻辑上传字节统计，以避免失败任务仍占用事件文件时绕过资源上限。
- DEC-003：达到自动尝试上限进入 `ManualIntervention`，永久协议/业务拒绝进入 `PermanentFailed`；两者都需要显式操作才可回到队列。

## 意外发现

- schema v1 已创建 `upload_jobs`，但 `UNIQUE(event_id)` 只能保存每事件一项资源，必须通过版本迁移重建表。
- M8-02 运行时单线程独占会话；M8-03 不应让另一个内部线程直接并发调用同一个 `IUplinkTransport`。
- 仓库级 `format-check` 会在本任务未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 报既有格式差异；本任务所有修改的 C++ 文件已用同一 clang-format 执行并通过 `--dry-run --Werror`。
- 原有 UplinkRuntime 测试以瞬时连接尝试数判断三次失败，在全量测试负载下会采样到正在连接状态；改为等待稳定的 `reconnect_failures` 指标后消除了时间窗口。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线检查 | 通过 | `git status --short` 无输出 |
| 2026-08-05 | 上传调度与人工重试定向测试 | 通过 | 8/8（其中新增上传调度测试 7 项） |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-debug` + `ctest --preset local-windows-vs2026-debug` | 通过 | 全量构建成功；28/28 CTest，通过的核心单测为 326 项 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-release` + `ctest --preset local-windows-vs2026-release` | 通过 | 全量构建成功；28/28 CTest |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-static-analysis` | 通过 | MSVC 静态分析全量构建成功 |
| 2026-08-05 | 修改文件 clang-format dry-run | 通过 | 所有本任务修改的 C++ 源文件和头文件无格式错误 |
| 2026-08-05 | 仓库 `format-check` | 基线阻塞 | 仅未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 报格式差异 |
| 2026-08-05 | `git diff --check` | 通过 | 无空白错误 |

## 完成摘要

M8-03 已完成：SQLite schema v4 可持久表达多资源上传任务，提供幂等入队、条数/字节双上限、固定优先级领取、三类失败、人工恢复和重启回收；`PersistentUploadScheduler` 提供有上限抖动退避与确定性停止。正式网络适配、分块、校验和限速仍属于 M8-04；未执行真实上位机、实体相机、进程强杀或物理断电验证。
