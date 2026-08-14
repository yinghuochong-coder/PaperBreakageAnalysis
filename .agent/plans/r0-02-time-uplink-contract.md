# R0-02：时间与 Uplink 契约门禁 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-14
- 最后更新：2026-08-14
- 路线图条目：R0-02 时间与 Uplink 契约门禁
- 关联需求：EDGE-TS-001～004、EDGE-TS-010～014、EDGE-EVT-001～004、EDGE-EVT-010～017、EDGE-STAT-001～004、EDGE-PRV-001～004、EDGE-COMP-001～004

## 目的与可观察结果

在进入 T1、E3 和 O4 的生产代码前冻结时间领域类型、`TimeSyncRuntime` 模块边界、严格/传统能力协商以及统一 T0 的 Uplink v1 扩展。完成后，后续实现者无需重新决定字段类型、空值、单位、枚举、队列溢出、错误码或关闭顺序；仓库提供严格有效样例和预期拒绝样例的独立校验脚本。

## 范围

### 范围内

- 更新系统架构，定义时间运行时所有权、探针适配边界、不可变模型发布和关闭顺序。
- 在领域模型中冻结 `ClockSource`、`SyncState`、`FrameTimeMetadata`、`ClockModelSnapshot` 和 `ClockSyncSnapshot`。
- 扩展 Uplink v1 文档，冻结能力协商、`BREAK_EVENT_TRIGGERED`、`event.lockByUtc`、`EventLockAck`、状态和预览时间字段。
- 统一 JSON 纳秒字段与 C++/SQLite 物理类型，增加稳定业务错误码。
- 提供不依赖构建产物的 JSON 样例与严格校验脚本。

### 范围外

- 不实现 T1-01～T1-03 的帧结构、时间线程或 Windows/Hikrobot 探针。
- 不实现 E3 的事件锁定、持久 outbox、SQLite 幂等表或 Uplink dispatcher。
- 不实现 O4 的完整状态提供器和远程预览桥。
- 不改变 PBNVME、manifest、配置或 SQLite schema；这些属于 R0-03、D2 和 T1-03。
- 不提前开始 T1、E3 或 O4 的生产代码实现。

## 当前基线

- 工作区在任务开始时干净，R0-01 已完成。
- `docs/uplink-protocol-v1.md` 只定义基础 v1 会话、命令和预览头；没有已接受能力、统一 T0、ACK 或预览时间质量字段。
- `docs/architecture/system-architecture.md` 只区分单调/墙上/相机时间，没有 `TimeSyncRuntime` 所有权、探针边界和模型发布顺序。
- `docs/architecture/domain-model.md` 仍以 RFC 3339 毫秒文本作为所有外部时间的默认格式，没有为纳秒字段定义十进制字符串例外。
- 现有 C++ Uplink parser 保持既有 v1 行为；R0-02 是契约门禁，路线图明确首个生产代码任务为 T1-01。

## 前置条件与假设

- Uplink v1 的明文、无鉴权和既有 REST 分块上传语义不变。
- v1 顶层未知字段仍拒绝；新会话确认放入既有可扩展的 `extensions.paperbreak`，避免传统节点因顶层字段变化失效。
- 未安装实体相机/PTP/Grandmaster 验证环境；本任务不声称任何同步精度或硬件能力通过。
- Hikrobot MVS 时间能力的具体节点名和精度由 T1-03 在适配器内部确认，本计划只冻结无厂商类型的端口。

## 设计说明

`paperbreak_time` 将作为只依赖 common 的领域/运行时目标；Windows 系统时间探针和 Hikrobot 相机时间探针分别由平台与厂商适配器实现，MVS 类型不得越界。模型由单一时间线程生成并以不可变快照原子发布，采集侧只复制已发布快照并执行有界整数映射。

外部纳秒字段使用规范十进制字符串，空值必须与显式 available 标志一致。会话请求继续声明能力；严格节点要求服务端在 `extensions.paperbreak.acceptedCapabilities` 中确认 `event.lockByUtc`。未知请求能力可被传统服务端忽略，但服务端确认未知、未提供或不支持的能力时边缘端稳定拒绝。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空 |
| --- | --- | --- | ---: | --- | --- |
| `time.control` | 配置/健康/服务编排 | `TimeSyncRuntime` | 16 条 | 拒绝最新普通控制并返回 `SYS_BUSY`；stop 独立 | 关闭入口、唤醒探针、发布最终停止快照并 join |
| `time.model.latest` | 时间线程 | 采集/事件/状态 | 系统 1 槽 + 每相机 1 槽，最多 4 | 原子覆盖旧模型；历史帧持有原快照值 | 停止相机和事件映射后释放 |
| `event.lock.requests` | Uplink 命令入口 | 事件锁定引擎 | 64 条且 1 MiB | 拒绝最新请求，返回 `UPLINK_SERVER_BUSY`；禁止 drop-oldest | 先拒绝新命令，已接受请求形成 ACK 或明确停止失败 |
| `event.trigger.outbox` | 事件管理 | Uplink 可靠通知 | 4096 条且 16 MiB 持久预算 | 拒绝新通知登记并触发 Critical；不得撤销本地事件 | 先停止生产，提交事务；网络发送可留待重启 |
| `preview.remote.latest[i]` | `PreviewRuntime` | Uplink 预览工作线程 | 每相机 1 个槽，最多 4 | latest-wins，覆盖只计数 | 断线/停止立即清空，不排空 |

容量是后续配置 schema 的初始基线；R0-02 不创建线程或队列。

### 持久化与恢复

本任务不改变生产持久化。契约要求 E3 后续将触发通知 outbox 和外部锁定幂等结果写入有界 SQLite 表；写入失败不得伪报通知或撤销本地事件。具体 schema、迁移、备份和对账属于 E3/D2。

### 错误和降级

- 未协商 `event.lockByUtc`：`SYS_NOT_SUPPORTED`，不进入事件队列。
- 版本、未知结构或服务端确认非法能力：`UPLINK_PROTOCOL_VERSION_UNSUPPORTED` 或 `UPLINK_PROTOCOL_ERROR`。
- 无可用 UTC→单调模型：`TIME_MAPPING_UNAVAILABLE`；事件可返回 `FAILED`，本机既有事件不撤销。
- 锁定通道或事件容量满：分别返回 `UPLINK_SERVER_BUSY` 或 `EVENT_LOCK_CAPACITY_FULL`，禁止覆盖旧请求。
- 状态或预览超过协议上限：拒绝整条状态/当前预览，不截断 JSON/JPEG，不反压采集。

## 实施步骤

- [x] 1. 更新系统架构和领域模型，冻结模块依赖、类型、所有权、探针、发布、通道及关闭语义。
- [x] 2. 更新 Uplink v1 协议，冻结严格/传统协商、统一 T0 消息、ACK、状态和预览字段。
- [x] 3. 更新稳定错误码及需求追踪，解决 RFC 3339 与纳秒十进制字符串的适用范围冲突。
- [x] 4. 新增严格有效和预期拒绝 JSON 样例及独立 PowerShell 校验脚本。
- [x] 5. 完成代码差异复核；用户追加验证要求后执行契约校验、Debug 配置、构建和全量 CTest，并回写计划和路线图。

## 验证计划

### 自动化测试

- 独立脚本严格检查样例的字段白名单、协议版本、能力子集、纳秒十进制字符串、枚举、空值/available 一致性和 ACK 聚合约束。
- 预期拒绝样例覆盖未知字段、未知协议版本和未提供/未知能力确认，并固定业务错误码。
- 后续 T1/E3/O4 生产实现必须把这些样例接入 C++ 单元/集成测试；本任务不提前修改生产 parser。

### 构建与测试命令

完整门禁原应执行：

```powershell
powershell -NoProfile -File tools/validate-r0-02-contract.ps1
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

用户随后要求继续构建以完成任务；以上校验、Debug 配置、构建和 CTest 已执行并归档结果。

### 人工或硬件验证

- 环境：目标网卡、PTP/NTP 服务、Grandmaster 和 MV-CS020-60GM 未用于本任务。
- 步骤：无。
- 预期：硬件探针能力和同步精度保持“待硬件/系统联调”。
- 证据保存位置：本 ExecPlan 与后续 T1/V5 验证记录。

## 回滚与恢复

若契约审查不通过，恢复本任务修改的文档、样例和校验脚本即可；本任务不迁移 schema、不写生产数据且不改变运行时，禁止删除现有事件、配置或验证资料。

## 验收标准

- [x] `TimeSyncRuntime` 所有权、依赖、探针、模型发布和关闭顺序明确且不泄漏 MVS/Win32 类型。
- [x] 五个时间契约类型的字段、C++/SQLite/JSON 类型、枚举、空值和不可变性已冻结。
- [x] Uplink 严格/传统协商、事件通知、锁定命令、ACK、状态和预览时间字段已冻结。
- [x] 所有纳秒 JSON 字段使用规范十进制字符串；C++/SQLite 使用有符号 64 位纳秒整数。
- [x] 未知字段、版本和非法能力确认具有稳定拒绝语义；传统 v1 会话和上传保持兼容。
- [x] 新通道均记录条数/字节容量、满载策略、指标方向和关闭行为。
- [x] 已新增严格样例和校验脚本，独立校验已通过。
- [x] 未实现后续生产代码、未修改无关文件；Debug 配置、构建和全量 CTest 已通过。

## 进度记录

- 2026-08-14：创建计划，完成需求、架构、协议、领域模型、错误码和现有 Uplink parser 基线检查，状态 `in-progress`。
- 2026-08-14：完成契约文档、错误码、验证向量和独立校验脚本的编码及只读差异复核；按用户要求未运行校验、构建或测试，状态 `implemented-unverified`。
- 2026-08-14：用户要求继续构建；独立契约校验、Debug 配置、Debug 构建和最终全量 CTest 通过，状态更新为 `completed`。

## 决策记录

- DEC-001：R0-02 只冻结契约和验证向量，不提前实现路线图明确归属 T1/E3/O4 的生产代码。
- DEC-002：会话能力确认使用 `extensions.paperbreak`，不向 Uplink v1 顶层增加传统严格 parser 无法识别的字段。
- DEC-003：未知请求能力允许服务端忽略；服务端确认未知、未提供或本端不支持的能力视为协议错误，从而同时满足传统兼容与严格拒绝。
- DEC-004：所有含义为纳秒的 JSON 字段使用字符串；RFC 3339 继续用于人类可读信封、截止时间和日志时间。
- DEC-005：边缘运行时通道遵循项目级最多四相机约束；`EventLockAck` 线上解析上限保留六项以兼容既有上位机/模拟器容量基线，本机只发送实际支持相机。

## 意外发现

- 当前 Uplink 文档写“时间统一为 ISO 8601”，与新增需求的纳秒十进制字符串存在直接冲突，需要按字段用途划分。
- 当前命令通用规则要求所有非状态命令 `operatorConfirmed=true`，但 `event.lockByUtc` 明确是自动协调例外。
- 当前预览 DTO 只有 RFC 3339 `timestamp`，无法表示采集校正时间是否可用或同步质量。
- 活动增量需求/路线图保留六相机联调基线，但仓库 `AGENTS.md` 和系统架构产品目标仍为最多四相机；本任务没有扩大运行时硬件上限，只在 ACK 线上格式保留六项解析兼容。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-14 | 需求与现有实现只读检查 | 完成 | 已确认契约缺口和后续任务边界 |
| 2026-08-14 | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate-r0-02-contract.ps1` | 通过 | 有效样例通过；拒绝向量返回稳定业务错误码 |
| 2026-08-14 | `cmake --preset local-windows-vs2026-debug` | 通过 | 使用 Visual Studio 自带 CMake 的绝对路径执行；仅有可选 `WrapVulkanHeaders` 缺失警告 |
| 2026-08-14 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | 首次链接因工作区 Debug 控制台进程占用可执行文件失败；正常关闭该进程后重试通过 |
| 2026-08-14 | `ctest --preset local-windows-vs2026-debug` | 通过 | 首轮 31/32，既有 Uplink 心跳时序断言偶发失败；直接全量 GoogleTest、隔离 `unit` 和最终全量 CTest 重跑均通过，最终 32/32 |
| 2026-08-14 | 实体相机、PTP/Grandmaster 与上位机联调 | 未执行 | 本任务为契约门禁；硬件能力与同步精度继续标记为待 T1/V5 验证 |

## 完成摘要

R0-02 的契约编码已完成：系统架构和领域模型冻结时间运行时、探针、不可变快照、稳定枚举和
关闭顺序；Uplink v1 冻结严格/传统能力协商、统一 T0 通知/命令/ACK、状态与预览时间字段；
错误码、有效样例、拒绝向量和独立 PowerShell 校验脚本已新增。独立契约校验、Debug 配置、
Debug 构建和最终全量 CTest（32/32）均通过，任务标记为 `completed`；实体硬件与同步精度未验证，
也未开始 T1-01。
