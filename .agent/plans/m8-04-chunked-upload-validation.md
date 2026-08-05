# M8-04：分块上传与校验 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：`docs/roadmap/development-roadmap.md` M8-04
- 关联需求：4.18 上位机通信、阶段 M8

## 目的与可观察结果

交付生产侧 Uplink v1 Qt Network 适配器和可供 M8-03 调度器注入的文件执行器。边缘端能够与参考模拟器建立 REST/WebSocket 会话、收发带确认的控制消息、幂等提交事件元数据，并对本地普通文件执行 SHA-256 流式校验、1 MiB 默认分块、服务端 checkpoint 查询、缺块续传和最终完成确认。网络中断或校验失败保留持久任务及 checkpoint，后续重试不重复创建逻辑文件。

## 范围

### 范围内

- 新增独立 `paperbreak_uplink_transport` Qt Network 适配器目标，不向领域公开头暴露 Qt 类型。
- 所有 HTTP/WebSocket I/O 有显式超时，`disconnect()` 可取消当前等待。
- 上传前校验普通文件、声明长度与 SHA-256；查询服务端已收分块，只发送缺块；逐块摘要与整文件摘要均为 SHA-256。
- 上传带宽使用配置的字节/秒上限节流，停止令牌可中断等待；不在采集回调中执行文件或网络 I/O。
- 把 edge config v2 的 uplink 公开行为迁移到明文 `http://`，移除 v1 不使用的凭据/证书强制要求，并增加分块、I/O 超时和上传速率上限配置。
- 使用参考模拟器完成真实回环 HTTP/WebSocket、重复确认、断点恢复、校验失败和取消测试。
- 更新协议、架构、错误码/配置文档和路线图证据。

### 范围外

- M8-05 Plant IO、TLS/鉴权、正式上位机业务系统、跨重启命令结果。
- 生产网络、实体相机、目标工控机网卡带宽或物理断电验收。
- 与本任务无关的控制台页面重构或后续 M9 工作。

## 当前基线

- `paperbreak_uplink` 已有传输无关 DTO、同步 `IUplinkTransport`、单线程 `UplinkRuntime` 和 M8-03 `PersistentUploadScheduler`。
- SQLite schema v4 的上传任务记录已保存逻辑 ID、相对路径、声明字节、checksum 和 checkpoint，但执行器尚未实现。
- `PaperBreakUplinkSimulator` 已实现冻结的 v1 REST/WebSocket 端点、分块幂等、断点状态和 SHA-256 完成校验。
- edge config v2 仍要求启用 uplink 时使用 HTTPS 和 credential，且没有分块、超时和速率字段。
- 工作区开始时干净，无需绕开用户修改。

## 前置条件与假设

- Qt 6.10.2 的 Core、Network、WebSockets 已是批准依赖，本任务不引入新依赖。
- v1 是明文无鉴权协议；`serverUrl` 表示 HTTP 基址，WebSocket URL 以会话响应为准。
- 默认分块 1 MiB、硬上限 4 MiB；默认上传上限 20 MiB/s、硬上限 1024 MiB/s；单次 I/O 默认 10 秒、范围 100 ms～60 秒。
- 调度器当前只有一个上传工作线程，执行器不再创建上传任务队列；传输适配器内部只保留单个同步在途操作和 WebSocket 事件线程。

## 设计说明

`QtUplinkTransport` 实现 `IUplinkTransport`。HTTP 请求使用每次调用局部的 Qt 网络管理器和有截止时间的事件循环；WebSocket 由适配器专用 Qt 事件线程持有，用固定单在途同步请求串行发送并等待 ack。命令回调只转交 M8-02 的双重有界队列。REST 上传由 M8-03 单工作线程执行，因此不会与相机采集回调耦合。

`ChunkedUploadExecutor` 将 `UploadJobRecord` 转换为协议请求：事件元数据任务走幂等 REST PUT；文件任务在事件根下解析并校验相对路径，流式计算或验证 SHA-256，创建/恢复上传，查询 `receivedChunks`，只发送缺失块，最后发送幂等 complete。checkpoint JSON 只保存有界的 `uploadId`、已确认块索引和文件摘要，不保存文件内容。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| WebSocket 同步操作 | UplinkRuntime | 适配器 Qt 事件线程 | 1 个在途 | 调用串行化，不扩容 | `disconnect()` 关闭 socket 并唤醒当前 ack 等待 | 调用方保留连接/失败指标 |
| 服务端命令回调 | 适配器 Qt 事件线程 | UplinkRuntime | 复用 M8-02 的 64 条/8 MiB 默认上限 | M8-02 拒绝最新并计数 | disconnect 后不再接收 | M8-02 `rejected_commands` |
| 文件上传 | M8-03 调度线程 | HTTP 适配器 | 1 个当前任务 | SQLite 未领取更多任务 | stop token 中断分块/节流，返回 RetryWait checkpoint | M8-03 调度器指标 |

### 持久化与恢复

不修改 SQLite schema。服务端 upload checkpoint 是断点事实源；本地 checkpoint 用于诊断和避免丢失 uploadId，但每次重试仍查询服务端并核对 totalBytes、chunkBytes、sha256。进程终止后 M8-03 把 `InProgress` 恢复为 `RetryWait`，下一次执行从服务端 `receivedChunks` 继续。

### 错误和降级

- `UPLINK_DISCONNECTED`、`UPLINK_TIMEOUT`、`UPLINK_SERVER_BUSY`、`UPLOAD_TRANSFER_FAILED`：可重试。
- `UPLOAD_CHECKSUM_MISMATCH`：可重试并保留 checkpoint；达到 M8-03 次数上限后人工处理。
- `UPLOAD_SOURCE_MISSING`、`UPLOAD_SOURCE_CHANGED`、`UPLOAD_REJECTED`、`UPLINK_PROTOCOL_ERROR`：永久失败或需人工处理，外部 HTTP 状态不作为唯一业务码。
- HTTP 507、429 和 5xx 视为可重试；协议冲突 409 和无效请求 4xx 视为永久拒绝，服务端结构化错误码优先。

## 实施步骤

- [x] 1. 扩展传输 DTO/checkpoint 和配置 v2，新增配置测试，证明只接受明文 HTTP、范围和序列化一致。
- [x] 2. 新增 Qt HTTP/WebSocket 正式适配器，覆盖会话、确认、命令、事件元数据、超时和可取消断开。
- [x] 3. 新增分块上传执行器，覆盖本地校验、缺块续传、重复块/complete、限速、停止和错误分类。
- [x] 4. 用参考模拟器增加回环集成测试与故障注入，执行 Debug/Release 构建、CTest、静态分析和格式检查。
- [x] 5. 更新协议、架构、配置文档、路线图及本计划的真实验证证据，不进入 M8-05。

## 验证计划

### 自动化测试

- 配置：启用 `http://` 成功，HTTPS/WS/凭据依赖和非法分块/超时/速率被拒绝，序列化回读一致。
- 传输：会话与 WebSocket 建立、心跳/状态 ack、重复 ack、命令回调、事件幂等、连接超时和 disconnect 取消。
- 上传：多块、服务端已有块续传、重复执行幂等、块/整文件校验错误、源文件变化、网络中断后恢复、限速和 stop token。

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

- 真实上位机、生产网络、目标 NIC 和实体相机并发带宽：未执行；仓库只提供参考模拟器与本机回环环境。
- 物理断电/进程强杀：未执行；自动化验证持久 checkpoint 等价路径，不宣称物理测试通过。

## 回滚与恢复

移除新增 transport 目标、实现和测试，并还原传输 DTO/配置/文档即可回到 M8-03。SQLite schema 不变化；任务和事件数据不删除。旧 HTTPS 配置在本任务迁移后将被明确拒绝，回滚前需恢复旧配置文件，不自动覆写用户配置。

## 验收标准

- [x] 文件按 1 MiB 默认/4 MiB 硬上限分块并逐块 SHA-256 校验。
- [x] 服务端 checkpoint 可恢复，重复块和重复 complete 不产生重复文件或事件。
- [x] 客户端先验证源文件，服务端完成时验证整文件，确认后 M8-03 才提交 Completed。
- [x] 每次 I/O 有上限，disconnect/stop 可取消；上传带宽有明确非零上限。
- [x] 配置 schema、解析、序列化和默认文件一致迁移到明文 v1。
- [x] Debug/Release 构建与 CTest 已执行并记录真实结果。

## 进度记录

- 2026-08-05：阅读需求、架构、路线图、协议、ADR、M8-01～M8-03 计划和相关源码；创建计划，状态 `in-progress`。
- 2026-08-05：完成配置迁移、正式 Qt 适配器、分块执行器、服务组合根装配、状态指标/控制台展示及参考模拟器故障测试。
- 2026-08-05：完成 Debug/Release 全量构建与 CTest、MSVC 静态分析、JSON Schema、格式和差异检查；状态更新为 `completed`。

## 决策记录

- DEC-001：复用 Qt Network/WebSockets，不引入新生产依赖。
- DEC-002：服务端状态是断点事实源；每次重试重新查询，避免只相信可能过期的本地 checkpoint。
- DEC-003：保持 M8-03 单上传工作线程，不增加上传线程池或进程内文件队列。
- DEC-004：预校验读盘与网络分块分别使用同一配置速率上限，避免整文件 SHA-256 阶段绕过资源保护。
- DEC-005：文件请求携带可选事件元数据，正式适配器先完成本地源校验，再幂等提交元数据和创建上传。

## 意外发现

- M8-03 调度器和上传任务仓库尚未在服务组合根装配；M8-04 需提供可直接注入的执行器，并在不扩大到 UI 重构的前提下评估最小生产装配。
- 生产安装树原先禁止 `Qt6WebSockets.dll`，与 M8-04 正式服务依赖冲突；门禁已改为要求安装该运行库，同时继续禁止模拟器专用 `Qt6HttpServer.dll`。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线检查 | 通过 | 工作区干净；M8-00～M8-03 已完成 |
| 2026-08-05 | `PaperBreakTests --gtest_filter='UplinkTransport.*'` | 通过 | 7/7；真实回环会话、取消、分块、限速、源变化、校验失败与断点恢复 |
| 2026-08-05 | Python Draft 2020-12 schema 验证 | 通过 | 默认配置和单元测试有效配置均符合 edge config v2 schema |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-debug`；`ctest --preset local-windows-vs2026-debug` | 通过 | 全量构建成功；28/28 CTest 通过，单元测试目标包含 334 项 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-release`；`ctest --preset local-windows-vs2026-release` | 通过 | 全量构建成功；28/28 CTest 通过 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-static-analysis` | 通过 | MSVC `/analyze` 全量构建无新增分析错误 |
| 2026-08-05 | `clang-format --dry-run --Werror`；`git diff --check` | 通过 | 修改的 C++ 文件格式一致，无空白错误 |

## 完成摘要

已交付正式 Qt Uplink v1 适配器、客户端源校验、服务端 checkpoint 缺块续传、逐块/整文件 SHA-256、幂等完成、可取消 I/O 和读盘/网络限速；生产服务装配持久调度并公开上行状态。edge config v2 已一致迁移到明文 v1 和新增有界传输字段。参考模拟器、Debug/Release 和静态分析验证通过；正式上位机、生产网络、物理断电、目标带宽和实体相机并发测试未执行。
