# M5-06：事务式事件目录和 manifest ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-06
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.15、阶段 M5；`docs/architecture/system-architecture.md` 9.3、12.2、13

## 目的与可观察结果

为 M5-04 的冻结原始窗口和 M5-05 的已编码关键帧提供可靠的本地文件事实源。事件先在事件根目录内的唯一事务目录写入，逐文件写后校验，最后生成版本化 `manifest.json` 并通过同卷目录原子重命名公开。服务启动可扫描残留事务：完整且校验通过的事务继续提交；缺少清单、清单损坏或校验不匹配的事务移动到隔离区并写明损坏原因。所有持久化任务通过固定容量单工作线程队列执行，提交调用不等待磁盘，满载返回 Critical 业务错误。

## 范围

### 范围内

- `paperbreak_storage` 的事件持久化请求、manifest schema v1、事务写入器和只读校验接口；
- Windows/MSVC 下文件与目录耐久写入、同卷原子目录提交，以及可注入文件系统边界；
- 原始帧、JPEG 关键帧、事件元数据、长度与 SHA-256 校验；
- 默认容量 8、有硬上限的单工作线程运行时，满载/停止/排空和可观测快照；
- 启动残留扫描、完整事务恢复、损坏事务隔离和明确恢复结果；
- 写入点失败、磁盘满、权限拒绝、校验不匹配、进程残留和中文路径自动化测试；
- CMake、错误码说明、路线图完成证据。

### 范围外

- M5-07 SQLite 表、迁移和文件系统/数据库对账；
- M5-08 存储水位与保留删除；
- M5-09 服务组合根、配置、IPC、UI、复核和导出；
- M7 NVMe 块格式、视频预览和上传；
- 实体相机、断电和生产磁盘性能验收。

## 当前基线

- `src/storage` 只有链接 `common`、`event`、SQLite 的占位模块；尚无事件文件 API。
- M5-04 `FrozenEventWindow` 提供最多四路的冻结原始 `FrameView` 与触发记录；M5-05 提供可追溯关键帧描述和 JPEG 字节回调。
- `paperbreak_platform_windows` 已有配置文件的 Win32 同目录原子替换，但没有目录事务或事件二进制文件接口。
- 架构要求事件根/临时目录同卷、manifest 最后生成、文件刷新后原子 rename、正式目录不可原地修改；启动必须恢复或隔离残留事务。
- 工作区开始时 `git status --short` 为空；全仓已知 `src/console/src/preview_client.cpp` 存在与本任务无关的格式问题。

## 前置条件与假设

- 目标仅为 Windows 10/11 x64、MSVC 和 NTFS/受支持本地卷；最终目录原子可见性使用 `MoveFileExW` 的同卷目录重命名，文件句柄在提交前执行 `FlushFileBuffers`。
- M5-09 组合根在调用前提供事件状态、算法/配置/产线等快照；M5-06 不从可变全局状态推断这些字段。
- 事件 ID 采用既有 UUIDv7 安全字符；逻辑相机 ID 不直接作为路径，防止路径穿越且仍支持中文事件根路径。
- 进程突然终止/断电不能在当前会话真实执行；用构造残留目录覆盖启动恢复语义，真实断电耐久性保留为生产等价验证。

## 设计说明

`EventPersistenceRequest` 组合不可变的 manifest 元数据、`FrozenEventWindow` 和成功 JPEG；写入器校验事件 ID、相机/触发一致性、时间范围、数量和字节硬上限。原始帧写为 `raw/camera-N/frame-M.raw`，关键帧写为 `keyframes/keyframe-N.jpg`，路径映射和完整几何/时间/选择原因写入 manifest，不让外部 ID 参与路径拼接。

每个事件在 `<root>/.transactions/<eventId>.<unique>.pending` 内写入：先写 `event.json` 恢复元数据，再写原始帧和关键帧；每个文件通过读回流式 SHA-256 与长度校验；最后写包含所有规定字段和 `fileChecksums` 的 `manifest.json`，再原子移动到 `<root>/YYYY/MM/DD/<eventId>`。manifest 自身不纳入自校验映射。

公开读取/校验只接受已提交的年月日目录，不读取 `.transactions` 或 `.quarantine`。恢复器扫描数量有固定上限；有效 manifest 及全部校验通过的事务继续提交，无法完整恢复者写 `recovery.json` 后移动到 `.quarantine`，不删除残留证据。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `event.persist` | 后续事件组合根 | `EventPersistenceRuntime` 单工作线程 | 默认 8 个事件，硬上限 64 | 拒绝新事件，返回 Critical `EVENT_WRITE_FAILED`，不阻塞调用方 | 停止接收后排空已接受事务；`join` 使用单调截止时间，超时保留事务目录 | depth/capacity/high-watermark/submitted/completed/rejected/writeFailures/callbackFailures |

### 持久化与恢复

- schemaVersion 固定为 1；不实现迁移，高版本读取返回 `EVENT_SCHEMA_UNSUPPORTED`。
- 文件采用 `CREATE_NEW`、完整写入、`FlushFileBuffers`；提交不覆盖已存在正式事件。
- SHA-256 以小写十六进制记录，写后重新读取计算，长度或摘要不一致返回 `EVENT_CHECKSUM_FAILED`。
- 任一失败保留事务目录；完整残留可幂等提交，缺失/损坏残留隔离并记录稳定损坏原因。
- 本任务不删除正式、残留或隔离数据；M5-07 后以正式目录为文件事实源建立索引。

### 错误和降级

- `EVENT_WRITE_FAILED`：非法写入阶段、磁盘满、权限拒绝、目录冲突、队列满或停止接收；Critical、可重试，保留事务路径与原生 Win32 码（可得时）。
- `EVENT_CHECKSUM_FAILED`：写后长度/SHA-256 不一致；Critical，不提交，保留残留。
- `EVENT_RECOVERY_FAILED`：扫描、验证、隔离或恢复提交失败；Critical，不把残留宣称为正式事件。
- `EVENT_SCHEMA_UNSUPPORTED`：manifest schema 高于 1；Error、不可重试且隔离。

## 实施步骤

- [x] 1. 在 `paperbreak_storage` 增加事件数据模型、manifest 序列化/解析、SHA-256 和严格输入/路径/上限校验；用纯数据单元测试验证必需字段、稳定路径、重复物理帧和非法输入。
- [x] 2. 增加可注入文件系统边界与 Windows 耐久实现，实现事务目录、逐文件写后校验、manifest 最后写和原子目录提交；用实际中文临时路径验证最终目录可见、内容可回放和校验。
- [x] 3. 实现有界单线程 `EventPersistenceRuntime`、快照、回调异常隔离和截止时间停止；测试容量拒绝不阻塞、失败后继续和确定性排空。
- [x] 4. 实现残留扫描、有效事务恢复和损坏事务隔离；逐写入点注入失败，并覆盖磁盘满、权限拒绝、摘要不匹配、完整 manifest 残留和不支持 schema。
- [x] 5. 更新构建、错误码/路线图/ExecPlan，运行任务文件格式检查、Debug/Release 全量构建、两套非硬件 CTest、`git diff --check` 并记录限制。

## 验证计划

### 自动化测试

- manifest 至少字段、raw/keyframe 文件索引、理由和 SHA-256/长度可重算；
- 中文/空格事件根、事务期正式目录不可见、提交后只读验证与原始像素回放；
- event.json、原始帧、关键帧、manifest、最终 rename 各阶段故障注入；
- 磁盘满/权限拒绝映射稳定业务错误并保留事务；
- 写后字节损坏触发 `EVENT_CHECKSUM_FAILED`；
- 有 manifest 的进程残留恢复提交，无/坏 manifest 隔离并生成 recovery 标记；
- 队列精确容量、满载 Critical、工作失败隔离、回调异常和停止排空。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行任务文件 `clang-format --dry-run --Werror`、`git diff --check`；全仓 `format-check` 若仍被既有文件阻断则如实记录。

### 人工或硬件验证

- 环境：无需实体相机；本机 NTFS 实际临时目录由自动化覆盖。
- 未执行：真实进程强杀、物理断电、磁盘拔除、四相机满帧率并发写入和生产 ACL；需要生产等价环境，不能由单元故障注入替代。
- 预期：后续 M5-09 接线及 M9 前验证不阻塞采集，且重启后残留事务被恢复或隔离。
- 证据保存位置：后续生产验证记录；自动化临时目录在测试结束时清理，不作为硬件证据。

## 回滚与恢复

代码回滚仅移除 M5-06 新增 API/实现/测试和 CMake 项，不触碰用户事件数据。schema v1 正式目录为不可变文件事实；旧程序不识别时保持原样。失败事务和隔离目录必须保留，由修复后版本重新扫描或人工审计，禁止自动删除。

## 验收标准

- [x] 同卷唯一临时目录中完成所有文件，逐文件长度和 SHA-256 写后校验，manifest 最后生成且目录原子提交。
- [x] manifest schema v1 包含需求 4.15 的全部最低字段，并能定位/回放每个原始帧和关键帧。
- [x] 默认容量 8 的非阻塞事件队列满载时返回 Critical，所有工作线程有确定性停止路径。
- [x] 正式目录提交前不由公开读取 API 暴露；正式目录不覆盖、不原地修改。
- [x] 启动残留有效时恢复提交，无效时隔离并明确标损，不删除证据。
- [x] 路线图指定故障注入、磁盘满、权限、校验、残留和中文路径测试通过。
- [x] Debug/Release 构建和非硬件 CTest 已实际运行；未执行硬件/断电验证明确列出。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、错误码、M5-04/M5-05 契约和 storage/platform 基线；创建计划，状态 in-progress。
- 2026-08-04：完成 schema v1、事务写入、SHA-256、Windows 文件系统边界、启动恢复、有界运行时和 10 项定向测试；Debug/Release 全量验证通过，状态 completed。

## 决策记录

- DEC-001：写入 `event.json` 作为 manifest 前的恢复元数据，最终 manifest 仍包含全部最低字段并最后生成；无 manifest 的残留不猜测缺失证据，明确隔离标损。
- DEC-002：相机 ID 与事件 ID 不直接成为任意文件路径；事件 ID 严格验证，原始相机目录使用稳定序号，兼顾可追溯性和路径安全。
- DEC-003：本任务提供运行时和文件事实源，不接入服务报警登记；满载和写入失败通过 Critical `Error` 及完成回调交给 M5-09 组合根报警。
- DEC-004：不引入新生产依赖；SHA-256 在 storage 内部实现并用已批准的 JSON 库生成 manifest。

## 意外发现

- 全仓 `format-check` 仍首先报告未修改的 `src/console/src/preview_client.cpp`；任务文件单独检查通过。
- 默认 MSVC 静态分析在构建 `paperbreak_event` 依赖时被未修改的 `src/event/src/key_frame.cpp` 第 257/290 行两条 C6011 阻断，尚未进入 `paperbreak_storage`；不在 M5-06 越界修复。
- 配置阶段继续提示既有 `SQLite::SQLite3` target 已弃用；M5-06 未修改 SQLite 依赖，目标迁移留在 M5-07 依赖/迁移工作范围评审。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 开始实施时工作区为空。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter=StorageEventStore.*` | 通过 | 10/10，覆盖校验、事务、SHA-256、故障注入、恢复、中文路径和队列。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | Debug 全量 `/W4 /WX` 构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-debug` | 通过 | 非硬件 23/23；Debug 通用 unit 217/217。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-release` | 通过 | Release 全量 `/W4 /WX` 构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-release` | 通过 | 非硬件 23/23。 |
| 2026-08-04 | 任务文件 `clang-format --dry-run --Werror`；`git diff --check` | 通过 | M5-06 C++ 文件格式与差异空白检查通过。 |
| 2026-08-04 | 全仓 `format-check` | 阻断（既有） | 首个报告文件为未修改的 `src/console/src/preview_client.cpp`。 |
| 2026-08-04 | `local-windows-vs2026-static-analysis --target paperbreak_storage` | 阻断（既有） | 在进入 storage 前，`src/event/src/key_frame.cpp` 两条 C6011 以 `/WX` 失败。 |
| 2026-08-04 | 实体相机、真实进程强杀、物理断电、生产磁盘性能/ACL | 未执行 | 当前使用构造帧、受控残留和 I/O 故障注入；不能代替生产等价验证。 |

## 完成摘要

M5-06 已交付不可覆盖的 schema v1 事件目录事务、逐文件 SHA-256/长度写后校验、manifest 最后写、Windows 同卷原子提交、严格正式目录读取验证、残留恢复/隔离和默认容量 8 的单线程持久化运行时。10 项定向测试、Debug/Release 全量构建和两套非硬件 CTest 通过。SQLite、服务/IPC/UI/报警接线、实体相机、进程强杀、物理断电和生产性能保持在后续任务或生产等价验证范围。
