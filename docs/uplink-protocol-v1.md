# Uplink v1 协议

## 1. 状态与范围

Uplink v1 是纸机断纸分析边缘工控机与上位机之间的参考协议，协议版本为整数 `1`。本协议冻结会话、状态/报警/命令、低帧率预览、事件元数据和事件文件断点上传。本仓库的 `PaperBreakUplinkSimulator` 是参考服务端，不是完整上位机业务系统；M8-01 已提供传输无关的 `IUplinkTransport` 和测试 Mock，真实 HTTP/WebSocket 边缘适配器及可靠上传仍属于 M8-02～M8-04。

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

## 7. 已知迁移点

现有 edge config v2 在启用 uplink 时仍校验 HTTPS URL。M8-00/M8-01 不实现真实边缘传输适配器，也不改动该公开配置行为；在 M8-04 适配器接入时，必须把配置 schema、解析校验和默认配置同步迁移到 `http://`/`ws://`，不得只修改其中一处。
