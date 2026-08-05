# Uplink v1 协议

## 1. 状态与范围

Uplink v1 是纸机断纸分析边缘工控机与上位机之间的参考协议，协议版本为整数 `1`。本协议冻结会话、状态/报警/命令、低帧率预览、事件元数据和事件文件断点上传。本仓库的 `PaperBreakUplinkSimulator` 是参考服务端，不是完整上位机业务系统；M8-01～M8-03 提供传输边界、运行时和持久调度，M8-04 的 `paperbreak_uplink_transport` 提供生产服务使用的 Qt REST/WebSocket 适配器和分块上传执行器。

协议正式采用明文 `http://` 与 `ws://`，无 TLS、无应用鉴权，参考服务端默认监听 `0.0.0.0:18080`。这意味着能够访问端口的主机可窃听数据、伪造设备或命令并实施中间人攻击。只能部署在隔离 VLAN 中，并使用主机防火墙、交换机 ACL 和物理访问控制限制来源。不得把 SHA-256 文件校验描述为身份认证或抗中间人保护。

## 2. 通用约束

- JSON 使用 UTF-8，单个控制请求或 WebSocket 文本消息不超过 1 MiB。
- 未知字段只能放在可选的 `extensions` 对象中；其他位置出现未知字段时拒绝消息。
- `machineId`、`eventId`、`logicalFileId`、`requestId`、`messageId`、`commandId` 等外部标识只允许 ASCII 字母、数字、点、下划线和连字符，并受各字段长度上限约束。因此标识不能形成绝对路径或 `..` 路径逃逸。
- 时间使用带时区的 ISO 8601 UTC 文本。序号为无符号 64 位整数，并在每个设备会话内单调递增。
- 请求以 `requestId` 幂等，WebSocket 控制消息以 `messageId` 幂等；文件分块键为 `(machineId,eventId,logicalFileId,chunkIndex)`。相同键和相同内容返回原确认，不同内容返回 HTTP 409。
- 成功响应为 JSON 对象。失败响应包含稳定的 `error.businessCode`、`message`、`module`、`operation`、`retryable` 和 `timestamp`。

## 3. REST

| 方法与路径 | 用途 | 主要成功状态 |
| --- | --- | --- |
| `POST /api/uplink/v1/sessions` | 协商协议、设备身份、软件版本与能力 | 201；幂等重放为 200 |
| `PUT /api/uplink/v1/devices/{machineId}/events/{eventId}` | 幂等提交事件元数据 | 202 |
| `POST /api/uplink/v1/devices/{machineId}/uploads` | 创建或恢复逻辑文件上传 | 201；已有相同上传为 200 |
| `GET /api/uplink/v1/devices/{machineId}/uploads/{uploadId}` | 查询状态和 `receivedChunks` | 200 |
| `PUT /api/uplink/v1/devices/{machineId}/uploads/{uploadId}/chunks/{chunkIndex}` | 提交分块；`Content-Range` 携带字节范围，`x-chunk-sha256` 携带小写十六进制摘要 | 202 |
| `POST /api/uplink/v1/devices/{machineId}/uploads/{uploadId}/complete` | 校验整文件并原子提交 | 200 |
| `GET /healthz` | 服务就绪和安全模式 | 200 |

会话请求至少包含：

```json
{
  "requestId": "session-001",
  "machineId": "EDGE-01",
  "productionLineId": "LINE-01",
  "softwareVersion": "0.1.0",
  "supportedProtocolVersions": [1],
  "capabilities": ["system.requestStatus", "camera.captureSnapshot"]
}
```

响应返回 `protocolVersion`、`requestId`、`sessionId`、`machineId`、`serverTime`、`heartbeatSeconds` 和会话专属 `webSocketUrl`。不包含版本 1 时返回 `UPLINK_PROTOCOL_VERSION_UNSUPPORTED`。

创建上传请求包含 `requestId`、`eventId`、`logicalFileId`、`fileName`、`contentType`、`totalBytes`、`chunkBytes` 和整文件 `sha256`。默认建议分块为 1 MiB，硬上限 4 MiB；单文件硬上限 64 GiB；服务端同时活动上传上限 32。最后一块可以小于声明分块，其余分块长度必须精确匹配。空间不足返回 HTTP 507，服务端不得自动删除已收事件文件。

## 4. WebSocket

连接路径为 `/api/uplink/v1/sessions/{sessionId}/stream`。文本信封固定包含：

```json
{
  "protocolVersion": 1,
  "messageType": "status",
  "messageId": "msg-001",
  "machineId": "EDGE-01",
  "sequence": 42,
  "timestamp": "2026-08-05T01:02:03.004Z",
  "payload": {},
  "extensions": {}
}
```

`messageType` 承载 `heartbeat`、`status`、`alarm`、`ack`、`command` 和 `command.result`。服务端下发命令时，payload 含 `commandId`、`commandType`、`deadline`、`operatorConfirmed` 和 `body`。命令必须使用 `commandId` 去重并检查截止时间；涉及配置或状态变更时，body 还应携带适用的期望修订。

v1 命令集合：`system.requestStatus`、`config.replace`、`event.review`、`event.retryUpload`、`camera.discover`、`camera.bind`、`camera.connect`、`camera.disconnect`、`camera.start`、`camera.stop`、`camera.updateConfig`、`camera.captureSnapshot`、`camera.softwareTrigger`、`service.restart`。边缘端仅执行握手 `capabilities` 声明的命令；未声明 `service.restart` 时返回稳定的“不支持”，不要求为此增加 SCM 控制路径。参考 GUI 对除状态刷新外的命令执行二次确认。

M8-02 边缘运行时只允许单一工作线程调用同步传输。传输命令回调只尝试写入默认 64、最大 4096 条且默认 8 MiB、最大 64 MiB 的双重有界队列；任一上限满载都拒绝最新命令并累计 `UPLINK_SERVER_BUSY` 语义指标，不能阻塞网络回调。连接成功后立即发送一次 `status`，随后按协商的 1～3600 秒间隔依次发送 `heartbeat` 和 `status`。连接、心跳或状态发送失败后，从默认 1 秒开始指数退避，最大 1 分钟；具体运行时配置不得超过 1 小时。

边缘端严格校验命令字段、协议版本、当前 `machineId`、RFC 3339 截止时间和握手能力。除 `system.requestStatus` 外，命令还必须具有 `operatorConfirmed=true` 且审计日志已装配。命令映射进入与本机 IPC 相同的 `SystemCommandService` dispatcher，配置替换使用同一 schema、修订冲突和原子存储逻辑，并把审计来源记录为 `uplink`。M8-03 起，已装配事件数据库时 `event.retryUpload` 将指定事件处于 `RetryWait`、`PermanentFailed` 或 `ManualIntervention` 的任务幂等地放回 `Pending`；没有可重试任务时返回 `requeuedJobs=0`，不创建事件或任务。`service.restart` 未声明能力时不增加 SCM 旁路。

命令结果缓存默认保留最近 1024、最大 4096 个 `commandId`，同时受默认 16 MiB、最大 64 MiB 字节上限约束，按 FIFO 淘汰最旧结果。相同 ID 和相同规范化内容重放原业务结果，`duplicate=true` 且不再次执行；相同 ID 与不同内容返回 `UPLINK_COMMAND_CONFLICT`。缓存只在当前边缘进程内有效，不替代上位机对未确认命令的持久重放。`command.result` 的 `result` 或 `error` 均为不超过 1 MiB 的 JSON 对象；现有服务命令若产生本机二进制附件，结果只报告 `binaryOmitted=true` 和字节数，二进制文件上传由 M8-03/M8-04 的可靠文件路径处理。

可靠上传任务保存在边缘 SQLite schema v4，不经命令结果缓存。任务按报警元数据、关键帧、manifest、低码率回放、原始文件的顺序领取，同时受未完成条数、未完成声明字节和全部历史条数上限约束；永久失败和人工处理仍占未完成预算，历史满载只淘汰最旧已完成行。同一幂等键及相同内容只保留一行；冲突内容返回 `UPLOAD_JOB_CONFLICT`。可重试失败按带抖动且有最大间隔的指数退避重新到期，达到最大尝试次数转人工处理；永久拒绝直接转 `PermanentFailed`。重启时遗留在途任务保留 ID 与 checkpoint 后恢复，不重复建立事件。

M8-04 执行器在网络操作前流式核对本地长度和整文件 SHA-256，创建或恢复逻辑上传后总是读取服务端 `receivedChunks`，只发送缺失块。每块携带独立 SHA-256；服务端完成确认前，边缘任务不得转为 `Completed`。失败 checkpoint 保存有界的 `uploadId`、已确认块索引和摘要，重试时仍以服务端状态为事实源。重复创建、重复块和重复 complete 都按同一幂等对象返回成功。适配器串行 HTTP 操作，预校验读盘与网络分块阶段默认均限速 20 MiB/s，所有 I/O 有截止时间且可由停止令牌或 `disconnect()` 取消。

## 5. JPEG 预览二进制帧

二进制消息由以下三段连续组成：

1. 4 字节无符号小端整数，表示 JSON 头长度；
2. UTF-8 JSON 头，包含 `protocolVersion=1`、`messageType=preview.frame`、`messageId`、`machineId`、`cameraId`、`sequence`、`timestamp` 和 `jpegBytes`；
3. 精确为 `jpegBytes` 的 JPEG 数据。

JSON 头上限 64 KiB，JPEG 上限 2 MiB，每设备最多接收 5 fps。预览是容量 1 的最新帧覆盖槽，不持久化、不确认、不重传，丢失预览不得影响可靠事件上传。

## 6. 参考模拟器

`PaperBreakUplinkSimulator` 同一可执行文件支持 GUI 与 `--headless`，并支持 `--listen`、`--port`、`--workspace`、`--max-devices`、`--workspace-limit-gib`、`--scenario`、`--run-for-ms`、`--version` 和 `--smoke-test`。默认工作区为 `%LOCALAPPDATA%/PaperBreak/UplinkSimulator`，默认设备上限 16、空间上限 20 GiB。

工作区 SQLite `user_version=1` 保存设备、会话、消息、事件、上传 checkpoint、分块和命令。分块写入 `.partial`，每块验证 SHA-256；完成时流式校验整文件并同卷原子重命名到 `events`。启动时复核 checkpoint、已记录分块长度与摘要；不一致文件移动到 `.quarantine`，不删除证据。存储任务队列容量 128，每设备命令队列容量 64；网络事件线程串行产生响应，未形成无界待响应集合；GUI 快照和每设备预览只保留最新值。停止顺序为拒绝新接入、关闭 WebSocket、排空有界存储任务、提交 SQLite/WAL checkpoint，并在 10 秒截止内 join 工作线程。

场景文件使用 `schemaVersion: 1`，每个设备可设置 `rejectConnections`、`disconnectWebSockets`、`responseDelayMs`、`failNextRequests`、`duplicateAcknowledgements`、`replayCommands`、`forceChecksumMismatch` 和 `disconnectAfterChunk`。GUI 和无界面模式使用同一个 `FaultProfile` 模型。

## 7. Edge 配置约束

M8-04 已同步迁移 edge config v2 schema、解析校验、序列化和默认配置：启用 uplink 时只接受 `http://` 基址，会话流只接受服务端返回的 `ws://` URL，凭据与证书引用必须为空。配置同时定义 64 KiB～4 MiB 分块、100～60000 ms 单次 I/O 截止和 1～1024 MiB/s 上传上限；这些传输设置变更需要重启服务。
