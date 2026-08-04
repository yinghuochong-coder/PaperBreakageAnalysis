# M5-08：存储水位和保留策略 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-08
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.17；`docs/architecture/system-architecture.md` 12.5、14、17

## 目的与可观察结果

在 `paperbreak_storage` 内提供可测试的磁盘水位和保留策略协调器。它能采样事件卷容量并稳定区分 Normal、Warning、Critical、Stop-save；Warning 优先清理已上传、明确允许删除、未锁定的最旧事件；天数和事件容量上限同样只能选择合规事件。删除先持久化状态，再把目录同卷移动到内部删除区，完成物理删除后落库；任一步失败可在后续维护轮次恢复。Stop-save 明确拒绝新的大文件，但不停止检测逻辑。

## 范围

### 范围内

- 数据库 schema 1→2 追加迁移和迁移前备份；
- 事件人工锁定、允许删除、删除状态、删除暂存路径和失败原因的持久化；
- Normal/Warning/Critical/Stop-save 水位判定、快照和大文件准入判断；
- 已上传且允许删除、未锁定事件的最旧优先有界清理；
- 保留天数和事件容量上限；
- 显式配置临时目录的有界过期清理；
- 删除中断/失败后的有界恢复；
- 定向自动化测试、文档和路线图证据。

### 范围外

- M5-09 服务组合根、配置、IPC、Qt 页面和报警登记接线；
- M7 NVMe 普通滚动缓存停止/恢复实现；
- M8 上传执行器和上传状态推进；
- 自动删除未上传、未明确允许删除、人工锁定、损坏或隔离证据；
- 实体相机、真实断电和生产 NVMe 性能测试。

## 当前基线

- M5-06 已提供不可变正式事件目录、`.transactions`、`.quarantine` 和原子提交；没有删除接口。
- M5-07 schema v1 已索引事件和文件，文件目录是事实源；`events.upload_state` 已存在，但没有人工锁定和删除状态。
- 数据库对账目前只把目录缺失标为 `Missing`，不会删除目录或记录。
- 服务尚未装配数据库和事件运行时，M5-09 才负责周期调度、报警和 IPC/UI。
- 任务开始时 `git status --short` 为空。

## 前置条件与假设

- 上传成功状态使用稳定字符串 `Uploaded`；M8 后续只负责推进该状态，本任务不实现上传。
- “已确认可删除”必须由独立布尔状态明确设置，不能从事件确认/误报状态猜测。
- 正式事件目录、`.deletions` 和数据库位于同一事件卷；移动到 `.deletions` 后才递归删除。
- 维护调用为同步且可能执行磁盘 I/O，只允许从后续存储工作路径调用，绝不能从相机采集回调调用。

## 设计说明

schema v2 新增 `event_retention` 一对一表，保存 `locked`、`deletion_allowed`、`deletion_state`、暂存相对路径、失败原因和时间。新事件索引会幂等补齐默认状态；旧 v1 数据迁移时批量补齐。公开数据库接口提供状态设置、有界清理候选、删除工作恢复、状态转换和保留占用查询，所有转换在既有互斥和单事务内执行。

`StoragePolicyManager` 使用可注入文件系统边界采样磁盘、同卷移动和删除，便于故障注入。每轮维护有固定事件/临时项扫描上限。水位按剩余字节由低到高判定 Stop-save、Critical、Warning、Normal；阈值必须满足 warning > critical > stop。Warning 及容量/天数超限时仅选择合规事件。Critical 报告应停止普通滚动缓存但仍允许正式事件；Stop-save 的 `large_writes_allowed=false`，调用方必须把未保存事件显式标损。

### 线程和队列

不新增线程或队列。数据库方法由互斥串行化；维护器为同步、有界操作，未来由 M5-09 既有存储工作线程周期调用。状态快照可并发只读；不在采集热路径聚合目录。

### 持久化与恢复

- schema 1→2 在单事务内创建并回填 `event_retention`，迁移前创建不可覆盖备份；0→2 顺序执行 v1、v2。
- 删除 CAS：`Active/DeleteFailed → DeletePending`，记录 `.deletions/...`；随后原子移动、递归删除，最后 `Deleted`。
- 崩溃后若原目录仍在则继续移动；暂存目录在则继续删除；两者均不存在则完成 `Deleted`。失败保留 `DeleteFailed` 和路径/错误，下一轮重试。
- 正式 manifest 不被改写；数据库记录和审计事实保留。

### 错误和降级

- 非法阈值/容量/期限：`SYS_CONFIG_INVALID`；
- Stop-save 大文件准入：`STORAGE_LOW_SPACE`，Critical，可在空间恢复后重试；
- 磁盘采样/移动/删除失败：`EVENT_DELETE_FAILED`，记录数据库失败状态并继续其他有界项；
- SQLite 失败沿用 `DATABASE_*` 业务码和原生扩展码；
- 维护报告逐项保留错误，不因单事件失败删除不合规事件。

## 实施步骤

- [x] 1. 阅读任务、需求、架构、M5-06/07 实现和测试，固定模块边界并创建计划。
- [x] 2. 实现 schema v2、保留状态数据库 API 和迁移/查询测试。
- [x] 3. 实现水位判定、准入、合规清理、可恢复删除与临时目录清理。
- [x] 4. 新增水位边界、锁定/未上传保护、最旧优先、天数/容量、故障恢复和 stop-save 测试。
- [x] 5. 运行任务文件格式、差异、Debug/Release 构建、CTest 和适用静态分析。
- [x] 6. 更新计划、路线图状态和完成证据，不开始 M5-09。

## 验证计划

### 自动化测试

- schema 1→2 原子迁移、备份、重复打开及旧事件默认不可删除；
- 三个精确水位边界和非法阈值拒绝；
- Warning 仅按最旧顺序删除 `Uploaded + deletionAllowed + !locked` 事件；
- 未上传、锁定、未允许和损坏事件不删除；
- 保留天数和容量上限触发清理且每轮有界；
- 移动失败、删除失败、移动后中断和删除后未落库可在后续轮次恢复；
- Stop-save 拒绝大文件，恢复到 Critical/Warning 后重新允许正式事件写入；
- 中文/空格路径与显式临时目录过期清理。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug --output-on-failure
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release --output-on-failure
```

另运行定向 GTest、任务文件 clang-format、`git diff --check`、全仓 `format-check` 和 storage 静态分析；既有阻断单独记录。

### 人工或硬件验证

- 环境：本任务可用临时目录和故障注入自动验证核心行为。
- 未执行：实体相机、生产 NVMe、真实断电/进程强杀和服务周期调度。
- 后续：M5-09 装配后验证报警/UI/事件未保存状态；M9 前完成生产等价磁盘与强杀恢复验证。

## 回滚与恢复

代码回滚不删除数据库、备份、正式事件或 `.deletions` 内容。schema v2 不自动降级；旧程序恢复需关闭数据库并使用迁移前 v1 备份。删除失败项由新版本继续恢复或人工审计，不自动清理锁定/未上传证据。

## 验收标准

- [x] 精确水位判定和 Stop-save 大文件门禁有测试。
- [x] 只删除已上传、明确允许且未锁定的最旧事件。
- [x] 天数、容量和临时清理均有固定上限。
- [x] 删除先落库，移动/删除/落库中断均可恢复并有故障注入测试。
- [x] schema v2 迁移、备份和兼容性有自动化证据。
- [x] Debug/Release 构建与非硬件 CTest 已实际运行。
- [x] 未实现 M5-09 或修改无关文件。

## 进度记录

- 2026-08-04：阅读任务、需求、架构和 M5-06/07 基线；创建计划，状态 in-progress。
- 2026-08-04：完成 schema v2、存储水位、合规保留清理、可恢复删除、临时清理和全部验证，状态 completed。

## 决策记录

- DEC-001：使用独立 `event_retention` 表而不修改不可变 manifest；删除资格与事件业务状态分离。
- DEC-002：采用数据库状态→同卷暂存→物理删除→完成状态的可恢复协议，不直接递归删除正式目录。
- DEC-003：M5-08 提供同步、有界协调器和准入契约；周期线程、报警与服务装配留在 M5-09。
- DEC-004：事件容量统计使用已验证事件文件字节与 manifest 字节之和；目录簇/文件系统元数据不作为业务容量，实际卷余量由独立磁盘采样兜底。
- DEC-005：失败删除一旦首次合规声明，恢复不再受后来目录对账产生的 Missing 状态阻断；路径仍必须严格匹配 `.deletions/<EventId>.deleting`。

## 意外发现

- M5-07 的 `upload_jobs` 尚无任务记录，且上传执行属于 M8；测试通过 manifest 的 `uploadState=Uploaded` 构造已上传事件，不提前实现上传状态机。
- M5-07 对账可能在删除暂存后先把正式目录标为 Missing；删除重试因此必须区分首次资格声明和已声明工作的恢复。
- 全仓格式检查仍首先报告未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp`，本任务文件单独检查通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 任务开始时工作区为空。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter=StorageMetadataDatabase.*:StoragePolicy.*` | 通过 | 13/13；8 项数据库和 5 项策略测试覆盖迁移、水位、资格保护、最旧优先、天数/容量、临时清理和删除恢复。 |
| 2026-08-04 | Debug 配置、全量构建、`ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | `/W4 /WX` 构建成功，非硬件 CTest 23/23；通用单元入口 230/230。 |
| 2026-08-04 | Release 配置、全量构建、`ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | `/W4 /WX` 构建成功，非硬件 CTest 23/23。 |
| 2026-08-04 | static-analysis 配置，构建 `paperbreak_storage` | 通过 | 默认 MSVC 静态分析通过。 |
| 2026-08-04 | 任务 C++ 文件 clang-format dry-run；`git diff --check` | 通过 | M5-08 新增/修改文件通过。 |
| 2026-08-04 | 全仓 `format-check` | 阻断（既有） | 未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 第 6 行格式问题。 |
| 2026-08-04 | 实体相机、真实强杀/断电、生产 NVMe 水位/性能 | 未执行 | 使用临时目录和注入文件系统自动验证；生产调度与报警接线属于 M5-09。 |

## 完成摘要

M5-08 已交付 schema v2 保留状态、四级存储水位、StopSave 大文件门禁、只针对已上传且明确允许且未锁定事件的最旧优先清理、天数/容量限制、显式临时目录清理，以及“先落库、再暂存、后删除”的崩溃可恢复协议。13 项定向测试、Debug/Release 全量构建和两套 23/23 CTest、storage 静态分析均通过；M5-09 服务/配置/报警/IPC/UI 接线和生产硬件验证保持在后续任务范围。
