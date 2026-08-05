# M8-02：心跳、状态和命令 ExecPlan

## 元数据

- 状态：complete
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：M8-02 心跳、状态和命令
- 关联需求：4.2 配置审计、4.18 上位机通信、4.20 健康监测、阶段 M8

## 目的与可观察结果

在 M8-01 的同步传输端口之上交付一个可停止、资源有界的上行编排运行时。它自动协商 Uplink v1 会话，周期发送心跳和服务状态，断线后按有上限的指数退避重连；服务端命令在传输回调中只进入有界队列，随后复用 `SystemCommandService` 的参数校验、乐观并发、业务错误和审计路径执行。相同 `commandId` 不重复产生业务副作用，并重放有界缓存中的原结果。

## 范围

### 范围内

- `paperbreak_uplink` 的连接/心跳/状态/命令运行时、严格命令解析、指数退避、容量与指标。
- `SystemCommandService` 的 Uplink v1 命令映射、远程确认门禁、配置审计来源和命令审计。
- Mock 驱动的自动重连、心跳、状态、幂等、截止时间、协议版本、队列容量和停止测试。
- 更新 Uplink v1 文档、系统架构、错误码和路线图 M8-02 状态。

### 范围外

- M8-03 SQLite `upload_jobs`、上传优先级、重启恢复和持久命令结果。
- M8-04 Qt HTTP/WebSocket 真实边缘适配器、分块、断点续传、文件读取和限速。
- `service.restart` 的 SCM 控制路径；未声明该能力时稳定拒绝。
- 正式上位机、生产网络、实体相机和硬件联调。

## 当前基线

- 任务开始时 `git status --short` 无输出，工作区无已有修改。
- M8-01 已提供同步 `IUplinkTransport` 和有界 `MockUplinkTransport`，但没有工作线程、心跳循环、重连或命令执行器。
- Uplink v1 已固定 `command`/`command.result` 信封、`commandId`、截止时间、人工确认和能力协商语义。
- `SystemCommandService` 已集中实现本机 IPC 的配置、相机、事件和状态校验；配置仓库已有 `ConfigChangeSource::uplink`，但命令入口尚不能选择该审计来源。

## 前置条件与假设

- M8-02 使用 M8-01 的同步传输调用；正式传输适配器必须让 `disconnect()` 唤醒阻塞 I/O，并对单次 I/O 设置上限。当前只用 Mock 自动验证。
- 命令去重结果仅在本次进程生命周期内保留；跨服务重启的持久命令记录不属于 M8-03 上传任务范围，协议对端应重放未确认命令。
- 正式 v1 明文无鉴权；变更命令仍要求 `operatorConfirmed=true`，且审计运行时未装配时拒绝执行，不能把网络可达性误当作身份认证。

## 设计说明

`UplinkRuntime` 位于 `paperbreak_uplink`，只依赖标准库、领域错误与 `IUplinkTransport`。单工作线程拥有全部传输调用；传输命令回调只校验最外层信封并尝试写入固定容量队列。运行时用 `SessionHello.capabilities` 作为唯一允许命令集；命令严格解析后检查截止时间和人工确认，再调用注入的执行函数。

`SystemCommandService::handle_uplink_command` 把 Uplink v1 命令映射到现有服务命令，内部调用同一 dispatcher，因此复用字段白名单、配置 schema、相机能力回读、事件复核修订和停止令牌。配置写入上下文改为由入口选择：本机 IPC 使用 `local_ipc`，远程入口使用 `uplink`。所有远程变更命令在执行前写 `audit` 分类日志；缺少审计运行时或审计写入失败时拒绝执行。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 服务端命令 | 传输回调线程 | Uplink 单工作线程 | 默认 64 条/8 MiB，上限 4096 条/64 MiB | 任一上限满时拒绝最新命令并计数，不扩容 | 停止后拒绝新命令；已排队命令不再执行 | 深度、字节、高水位、拒绝、执行、重放 |
| 命令结果去重缓存 | Uplink 工作线程 | Uplink 工作线程 | 默认 1024 条/16 MiB，上限 4096 条/64 MiB | FIFO 淘汰最旧结果 | 停止时释放 | 保留数/字节、重放数、冲突数 |

工作线程使用停止令牌和条件变量；`request_stop()` 先禁止回调入队、请求停止并调用传输 `disconnect()`，`join(deadline)` 有明确截止。心跳/状态不进入额外队列。

### 持久化与恢复

本任务不引入持久化。连接、序号、退避和命令去重缓存均为进程内状态；服务重启后重新建立会话。事件上传持久化由 M8-03 实现。

### 错误和降级

- 断线/心跳/状态发送失败：进入 `disconnected`，本地业务继续，按初始值翻倍至最大值重连。
- 不支持协议版本：`UPLINK_PROTOCOL_VERSION_UNSUPPORTED`，仍按有上限退避，避免紧循环。
- 命令格式、machineId 或版本非法：`UPLINK_PROTOCOL_ERROR`，不执行。
- 未声明命令：`SYS_NOT_SUPPORTED`；未人工确认：`UPLINK_COMMAND_NOT_CONFIRMED`；过期：`UPLINK_COMMAND_EXPIRED`。
- 相同 commandId/相同内容：重放原结果；相同 ID/不同内容：`UPLINK_COMMAND_CONFLICT`，不执行。
- 命令队列满：拒绝最新并累计 `UPLINK_SERVER_BUSY` 语义指标，不阻塞传输回调。

## 实施步骤

- [x] 1. 在 `paperbreak_uplink` 新增运行时公共模型、严格命令解析、退避/心跳/状态循环、有界命令队列和去重结果缓存。
- [x] 2. 为 `SystemCommandService` 增加 Uplink 命令入口和来源感知 dispatcher，覆盖状态、整配置替换、事件复核/重试及相机命令映射，并强制远程变更审计。
- [x] 3. 新增 M8-02 单元测试，覆盖正常心跳/状态、断线退避、协议协商、命令确认/截止、服务校验与审计、重复/冲突命令、队列容量和确定性关闭。
- [x] 4. 更新架构、协议、错误码和路线图证据，执行 Debug/Release 构建、CTest、静态分析、格式与差异检查。

## 验证计划

### 自动化测试

- Mock 在线时建立一次会话并周期发送心跳和状态；状态 JSON 来自注入的服务快照。
- 离线时不紧循环，退避从初始值指数增长且不超过最大值；恢复后自动重连。
- 会话协商到非 v1、非法心跳间隔或错误 machineId 时拒绝进入正常状态。
- 命令严格字段、能力、截止时间、人工确认、队列容量和停止后拒绝。
- `config.replace` 复用配置 schema/修订冲突并产生 `ConfigChangeSource::uplink` 审计；相机/事件命令复用既有服务校验。
- 同 commandId 相同内容只执行一次并重放结果；不同内容返回冲突。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
```

### 人工或硬件验证

- 环境：本任务只使用 Mock；未提供正式边缘 HTTP/WebSocket 适配器。
- 步骤：不执行真实上位机、生产网络或实体相机测试。
- 预期：M8-04 接入真实适配器后复用同一运行时。
- 证据保存位置：CTest 输出与本 ExecPlan 验证证据表。

## 回滚与恢复

移除新增运行时/测试并还原 `SystemCommandService` 的来源参数即可回到 M8-01。任务不迁移、不删除生产配置、SQLite 或事件数据；测试只使用临时目录。

## 验收标准

- [x] 自动连接、心跳、状态和有上限指数退避具备可观察测试证据。
- [x] 命令严格校验、截止时间、人工确认、能力协商和有界队列生效。
- [x] 重复 commandId 不重复产生业务副作用，内容冲突被拒绝。
- [x] 远程命令复用 `SystemCommandService` 业务校验，配置审计来源为 `uplink`，变更命令无审计时拒绝。
- [x] 工作线程具有停止令牌、传输断开和截止 join 路径。
- [x] Debug/Release 构建与 CTest 已执行并记录真实结果；未声称完成 M8-03/M8-04 或真实联调。

## 进度记录

- 2026-08-05：完成需求、架构、路线图、M8-00/M8-01、协议和现有服务命令基线检查；创建计划，状态 in-progress。
- 2026-08-05：完成运行时、服务命令映射、9 项定向测试、文档和全量验证；状态 complete。

## 决策记录

- DEC-001：编排和传输领域留在 `paperbreak_uplink`，服务用例映射留在 `paperbreak_service_core`，避免 uplink 反向依赖 IPC、相机或存储实现。
- DEC-002：只有 Uplink 工作线程调用同步传输；命令回调不执行服务命令或网络发送。
- DEC-003：命令结果使用有界进程内 FIFO 缓存；不把 M8-02 扩展为新的 SQLite schema 任务。
- DEC-004：明文无鉴权不等于可信身份；远程变更必须具有协议人工确认且审计设施可用。

## 意外发现

- 现有 `ConfigChangeSource::uplink` 已存在但尚未被命令入口使用，可在不改变配置 schema 的情况下接入。
- 现有 `event.retryUpload` 明确返回 M8 尚未接入；M8-02 只透传该稳定失败，不提前实现 M8-03。
- 全仓 `format-check` 仍被未修改的既有 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 阻塞；M8-02 修改的 C++ 文件已用相同 clang-format 定向检查通过。
- MSVC 静态分析首次指出来源感知 dispatcher 的既有大栈帧抑制注释放在了新薄包装器上；已把同一、说明充分的 C6262 定向抑制移动回实际 dispatcher，复验通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线检查 | 通过 | 工作区干净；M8-00/M8-01 已完成 |
| 2026-08-05 | `PaperBreakTests --gtest_filter=UplinkRuntime.*:SystemCommand.Uplink*` | 通过 | 9/9 |
| 2026-08-05 | Debug 配置、全量构建与 `ctest --preset local-windows-vs2026-debug` | 通过 | 28/28 |
| 2026-08-05 | Release 配置、全量构建与 `ctest --preset local-windows-vs2026-release` | 通过 | 28/28 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-static-analysis` | 通过 | `paperbreak_uplink`、`paperbreak_service_core` 及默认目标无分析警告 |
| 2026-08-05 | M8-02 C++ 文件定向 clang-format、`git diff --check` | 通过 | 无格式或空白错误 |
| 2026-08-05 | 全仓 `format-check` | 既有文件阻塞 | 未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp` |
| 2026-08-05 | 真实上位机、网络和硬件验证 | 未执行 | M8-04 正式传输适配器尚未实现；只完成 Mock 验证 |

## 完成摘要

已交付传输无关的 `UplinkRuntime`、条数/字节双重有界命令通道、会话/心跳/状态/指数退避、命令截止/确认/能力/幂等处理和完整指标快照。远程命令复用 `SystemCommandService`，配置审计准确标记为 `uplink`，所有变更命令均要求审计设施。Debug/Release 全量 CTest 与 MSVC 静态分析通过；真实 HTTP/WebSocket、持久上传、跨重启结果缓存和正式上位机联调仍属于后续任务。
