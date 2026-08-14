# Uplink v1 协议

## 1. 状态与范围

Uplink v1 是纸机断纸分析边缘工控机与上位机之间的参考协议，协议版本为整数 `1`。本协议冻结会话、状态/报警/命令、低帧率预览、事件元数据和事件文件断点上传。本仓库的 `PaperBreakUplinkSimulator` 是参考服务端，不是完整上位机业务系统；M8-01～M8-03 提供传输边界、运行时和持久调度，M8-04 的 `paperbreak_uplink_transport` 提供生产服务使用的 Qt REST/WebSocket 适配器和分块上传执行器。

协议正式采用明文 `http://` 与 `ws://`，无 TLS、无应用鉴权，参考服务端默认监听 `0.0.0.0:18080`。这意味着能够访问端口的主机可窃听数据、伪造设备或命令并实施中间人攻击。隔离 VLAN、防火墙、交换机 ACL 和物理访问控制可由现场按需采用，但不属于本项目验收门禁。不得把 SHA-256 文件校验描述为身份认证或抗中间人保护。

## 2. 通用约束

- JSON 使用 UTF-8，单个控制请求或 WebSocket 文本消息不超过 1 MiB。
- 未知字段只能放在可选的 `extensions` 对象中；其他位置出现未知字段时拒绝消息。
- `machineId`、`eventId`、`logicalFileId`、`requestId`、`messageId`、`commandId` 等外部标识只允许 ASCII 字母、数字、点、下划线和连字符，并受各字段长度上限约束。因此标识不能形成绝对路径或 `..` 路径逃逸。
- 人类可读信封时间、命令截止时间和错误时间使用带时区的 ISO 8601 UTC 文本；字段名以 `Ns` 结尾或本协议明确标为纳秒的字段使用规范十进制字符串。序号为无符号 64 位整数，并在每个设备会话内单调递增。
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

响应返回 `protocolVersion`、`requestId`、`sessionId`、`machineId`、`serverTime`、`heartbeatSeconds` 和会话专属 `webSocketUrl`。不包含版本 1 时返回 `UPLINK_PROTOCOL_VERSION_UNSUPPORTED`。传统服务端可以不确认扩展能力；支持 R0-02 严格契约的服务端在既有可选 `extensions` 中返回：

```json
{
  "extensions": {
    "paperbreak": {
      "capabilityContractVersion": 1,
      "acceptedCapabilities": [
        "event.lockByUtc",
        "status.timeSync",
        "preview.frame.time"
      ]
    }
  }
}
```

`acceptedCapabilities` 必须是请求 `capabilities` 的子集，并且每一项都必须是本协议冻结的能力。服务端可忽略请求中的未知可选能力；边缘端收到未知、未提供或本端不支持的已接受能力时返回/记录 `UPLINK_PROTOCOL_ERROR` 并拒绝建立严格会话，不能据此启用功能。

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

`messageType` 承载 `heartbeat`、`status`、`alarm`、`ack`、`command` 和 `command.result`。服务端下发命令时，payload 含 `commandId`、`commandType`、`deadline`、可选 `operatorConfirmed` 和 `body`。命令必须使用 `commandId` 去重并检查截止时间；涉及配置或状态变更时，body 还应携带适用的期望修订。

v1 命令集合：`system.requestStatus`、`config.replace`、`event.review`、`event.retryUpload`、`event.lockByUtc`、`camera.discover`、`camera.bind`、`camera.connect`、`camera.disconnect`、`camera.start`、`camera.stop`、`camera.updateConfig`、`camera.captureSnapshot`、`camera.softwareTrigger`、`service.restart`。边缘端仅执行握手后已协商的命令能力；未协商的已知命令返回 `SYS_NOT_SUPPORTED`，未知命令类型返回 `SYS_NOT_SUPPORTED`，均不进入业务 dispatcher。参考 GUI 对除状态刷新和自动 `event.lockByUtc` 外的命令执行二次确认。

M8-02 边缘运行时只允许单一工作线程调用同步传输。传输命令回调只尝试写入默认 64、最大 4096 条且默认 8 MiB、最大 64 MiB 的双重有界队列；任一上限满载都拒绝最新命令并累计 `UPLINK_SERVER_BUSY` 语义指标，不能阻塞网络回调。连接成功后立即发送一次 `status`，随后按协商的 1～3600 秒间隔依次发送 `heartbeat` 和 `status`。连接、心跳或状态发送失败后，从默认 1 秒开始指数退避，最大 1 分钟；具体运行时配置不得超过 1 小时。

边缘端严格校验命令字段、协议版本、当前 `machineId`、RFC 3339 截止时间和握手能力。除 `system.requestStatus` 外，普通命令还必须具有 `operatorConfirmed=true` 且审计日志已装配。`event.lockByUtc` 是上位机自动协调命令，不要求 `operatorConfirmed` 或强制审计；字段缺省或为 `false` 均不因此拒绝，为 `true` 也不改变幂等键或权限。命令映射进入与本机 IPC 相同的 `SystemCommandService` dispatcher，配置替换使用同一 schema、修订冲突和原子存储逻辑，并把审计来源记录为 `uplink`。M8-03 起，已装配事件数据库时 `event.retryUpload` 将指定事件处于 `RetryWait`、`PermanentFailed` 或 `ManualIntervention` 的任务幂等地放回 `Pending`；没有可重试任务时返回 `requeuedJobs=0`，不创建事件或任务。`service.restart` 未声明能力时不增加 SCM 旁路。

命令结果缓存默认保留最近 1024、最大 4096 个 `commandId`，同时受默认 16 MiB、最大 64 MiB 字节上限约束，按 FIFO 淘汰最旧结果。相同 ID 和相同规范化内容重放原业务结果，`duplicate=true` 且不再次执行；相同 ID 与不同内容返回 `UPLINK_COMMAND_CONFLICT`。缓存只在当前边缘进程内有效，不替代上位机对未确认命令的持久重放。`command.result` 的 `result` 或 `error` 均为不超过 1 MiB 的 JSON 对象；现有服务命令若产生本机二进制附件，结果只报告 `binaryOmitted=true` 和字节数，二进制文件上传由 M8-03/M8-04 的可靠文件路径处理。

可靠上传任务保存在边缘 SQLite schema v4，不经命令结果缓存。任务按报警元数据、关键帧、manifest、低码率回放、原始文件的顺序领取，同时受未完成条数、未完成声明字节和全部历史条数上限约束；永久失败和人工处理仍占未完成预算，历史满载只淘汰最旧已完成行。同一幂等键及相同内容只保留一行；冲突内容返回 `UPLOAD_JOB_CONFLICT`。可重试失败按带抖动且有最大间隔的指数退避重新到期，达到最大尝试次数转人工处理；永久拒绝直接转 `PermanentFailed`。重启时遗留在途任务保留 ID 与 checkpoint 后恢复，不重复建立事件。

M8-04 执行器首先检查连接状态；离线立即返回 `UPLINK_DISCONNECTED`，不得打开、读取或散列源
文件。在线时以 manifest 声明的整文件 SHA-256 创建或恢复逻辑上传并读取服务端
`receivedChunks`，随后对源文件执行一次顺序读取：同一遍计算整文件和分块 SHA，只发送缺失
块。整文件摘要变化时不调用 complete，返回 `UPLOAD_SOURCE_CHANGED`，保留文件和 checkpoint
并转人工处理。每块携带独立 SHA-256；服务端完成确认前，边缘任务不得转为 `Completed`。
失败 checkpoint 保存有界的 `uploadId`、已确认块索引和摘要，重试时仍以服务端状态为事实源。
重复创建、重复块和重复 complete 都按同一幂等对象返回成功。适配器串行 HTTP 操作，读取与
网络分块阶段受限速控制，所有 I/O 有截止时间且可由停止令牌或 `disconnect()` 取消。

## 5. R0-02 能力与时间字段契约

### 5.1 冻结能力和会话模式

| 能力 | 方向 | 含义 |
| --- | --- | --- |
| `event.lockByUtc` | 上位机→边缘端命令 | 允许按外部 T0 锁定本机全部启用相机缓存并返回 `EventLockAck` |
| `status.timeSync` | 边缘端→上位机状态 | `status` 携带工控机和逐相机 `ClockSyncSnapshot` |
| `preview.frame.time` | 边缘端→上位机二进制预览 | 预览头携带校正采集 UTC、同步状态、不确定度和模型修订 |

能力名称区分大小写。严格 T0 模式只在边缘请求和服务端接受集合都包含
`event.lockByUtc` 时成立，节点协调状态为 `COORDINATED`。服务端未返回
`extensions.paperbreak`、契约版本不是 1、或未接受该能力时为 `LEGACY`；传统会话仍可使用
既有状态、命令、事件上传和分块续传，不能发送/执行 `event.lockByUtc`，也不能声称完成跨节点
锁定。不能根据软件版本、消息字段或服务端地址猜测能力。

请求中的未知能力是可忽略声明，不会使 v1 会话失败。服务端 `acceptedCapabilities` 中出现未知、
未由请求提供或当前边缘端未实现的能力是无效选择，边缘端以 `UPLINK_PROTOCOL_ERROR` 拒绝该次
严格协商。`event.lockByUtc` 未协商却收到同名命令时返回 `SYS_NOT_SUPPORTED`；未知
`commandType` 同样返回 `SYS_NOT_SUPPORTED`，不得转发到业务 dispatcher。

### 5.2 纳秒十进制字符串

所有字段名以 `Ns` 结尾以及下文明确标为纳秒的 JSON 值只能使用字符串：

- 有符号值只接受 `0` 或 `-?[1-9][0-9]{0,18}`，解析后必须在
  `-9223372036854775808..9223372036854775807`；
- `uncertaintyNs`、`maximumObservedOffsetNs` 等非负值必须在
  `0..9223372036854775807`；
- 不接受 JSON 数字、空字符串、前导 `+`、多余前导零、指数或小数；
- 只有字段契约明确允许时才能使用 JSON `null`，且必须与对应 `Available=false` 一致；
- C++ 和 SQLite 纳秒值均为有符号 64 位整数；SQLite 列使用 `INTEGER` 和单位/范围
  `CHECK`。相机 ticks、频率、序号和 revision 不是纳秒字段，沿用各自无符号整数契约。

信封 `timestamp`、命令 `deadline` 和错误 `timestamp` 继续使用 RFC 3339 毫秒 UTC 文本，
不得用它们替代 T0 或校正采集纳秒。

## 6. `BREAK_EVENT_TRIGGERED` 立即通知

本机候选窗口成功获得保护后，边缘端立即登记持久通知并发送 `messageType=alarm`、
`payload.code=BREAK_EVENT_TRIGGERED`。不得等待后置窗口、JPEG、磁盘提交或文件上传。发送失败
不撤销本地事件；恢复连接后以 `eventId` 补发，上位机幂等处理。

payload 必需字段如下，除可选 `extensions` 外出现未知字段时返回 `UPLINK_PROTOCOL_ERROR`：

```json
{
  "code": "BREAK_EVENT_TRIGGERED",
  "eventId": "019c8c55-2f20-7a31-8b52-6e3b9ca1d88f",
  "triggerTimestampUtcNs": "1786671135123456789",
  "triggerMachineId": "EDGE-001",
  "triggerCameraId": "CAM01",
  "triggerSource": "ALGORITHM",
  "eventType": "PAPER_BREAK",
  "eventLevel": "ALARM",
  "syncState": "SYNCED",
  "uncertaintyNs": "500000",
  "clockModelRevision": 17,
  "preEventMs": 10000,
  "postEventMs": 10000,
  "configRevision": 93
}
```

字段约束：

- `eventId` 最多 128 字节；machine/camera ID 最多 64 字节，均遵循通用标识符规则；
- `triggerSource` 只能为 `ALGORITHM`、`MANUAL`、`PLANT_IO` 或 `EXTERNAL`；
- `eventType` 固定为 `PAPER_BREAK`，`eventLevel` 固定为 `ALARM`；
- `syncState` 使用冻结枚举；`uncertaintyNs` 可为 `null`，仅在无法估计时使用；
- 有可信校正帧时 T0 必须取该帧 `correctedCaptureUtcNs`。无法校正时可用该帧
  `receivedUtcNs` 建立降级事件，但状态必须为 `DEGRADED`/`UNSYNCED`，不得声称严格对齐；
- `clockModelRevision`、`configRevision` 为非负 64 位序号；`preEventMs` 和
  `postEventMs` 为 `0..600000` 的整数。

相同 `eventId` 的核心字段（T0、触发机器/相机/来源）相同是重放；任一核心字段不同返回/登记
`EVENT_LOCK_CONFLICT`，不得覆盖首次记录。

## 7. `event.lockByUtc` 与 `EventLockAck`

### 7.1 命令

命令文本仍使用第 4 节 v1 信封。payload 只允许 `commandId`、`commandType`、`deadline`、
可选 `operatorConfirmed`、`body` 和可选 `extensions`。`commandType` 固定为
`event.lockByUtc`；该自动命令不要求 `operatorConfirmed=true` 或强制审计。

body 必需字段如下，除可选 `extensions` 外未知字段稳定拒绝：

```json
{
  "eventId": "019c8c55-2f20-7a31-8b52-6e3b9ca1d88f",
  "triggerTimestampUtcNs": "1786671135123456789",
  "triggerMachineId": "EDGE-001",
  "triggerCameraId": "CAM01",
  "triggerSource": "ALGORITHM",
  "preEventMs": 10000,
  "postEventMs": 10000
}
```

类型、标识符、T0 和窗口范围与第 6 节相同。边缘端在进入锁定队列前完成严格解析、能力、
`machineId`、deadline 和 checked arithmetic 校验；请求起止时间任一溢出时返回
`UPLINK_PROTOCOL_ERROR`。相同 `commandId` 的规范内容重放现有命令结果；同 ID 不同内容返回
`UPLINK_COMMAND_CONFLICT`。不同 commandId 但相同 eventId/规范核心内容重放持久
`EventLockAck`，`duplicate=true`；相同 eventId 的核心内容冲突返回 `EVENT_LOCK_CONFLICT`。

外部锁定入口默认 64 条、总计 1 MiB，满载拒绝最新命令并返回 `UPLINK_SERVER_BUSY`，禁止
drop-oldest。服务停止后新命令返回 `SYS_SERVICE_STOPPING`。解析或排队不得阻塞相机回调、
采集线程或 Uplink 心跳。

### 7.2 `EventLockAck`

成功执行的 `command.result.result` 必须是以下严格对象：

```json
{
  "eventId": "019c8c55-2f20-7a31-8b52-6e3b9ca1d88f",
  "machineId": "EDGE-002",
  "duplicate": false,
  "lockStatus": "PARTIAL",
  "requestedStartUtcNs": "1786671125123456789",
  "requestedEndUtcNs": "1786671145123456789",
  "actualStartUtcNs": "1786671126123456789",
  "actualEndUtcNs": "1786671145123456789",
  "syncState": "DEGRADED",
  "uncertaintyNs": "3000000",
  "clockModelRevision": 22,
  "cameras": [
    {
      "cameraId": "CAM01",
      "status": "PARTIAL",
      "frameCount": 1180,
      "firstCaptureUtcNs": "1786671126123456789",
      "lastCaptureUtcNs": "1786671145100000000",
      "sequenceGaps": 2,
      "errorCode": "EVENT_BUFFER_INCOMPLETE"
    }
  ]
}
```

`lockStatus` 只能为 `COMPLETE`、`PARTIAL`、`FAILED`；逐相机 `status` 只能为
`LOCKED`、`PARTIAL`、`FAILED`。`actualStartUtcNs`、`actualEndUtcNs`、逐相机首末时间和
`uncertaintyNs` 允许为 `null`；无错误的 `errorCode` 必须为 `null`，不能使用空字符串。
`cameras` 线上格式最多 6 项且 cameraId 唯一，包含接收时所有已启用相机的结果。当前边缘产品
目标仍按项目级四相机上限装配，因此不会发送超过本机实际支持数的项目；上位机/模拟器保留
六项解析能力以兼容既有容量基线。帧计数和缺口为非负
64 位整数。

只有每个启用相机都为 `LOCKED`、实际范围覆盖请求范围、没有序号缺口、映射可用且整体状态为
`SYNCED` 时才能返回 `COMPLETE`。至少有一台相机保留可用证据但不满足完整条件时返回
`PARTIAL`；没有任何相机证据时返回 `FAILED`。整体实际范围是所有已锁定证据的最小/最大 UTC；
没有证据时两者均为 `null`。T0 迟到但证据仍在内存/NVMe 时必须锁定实际证据，不得仅因迟到
失败。

## 8. 状态时间字段

协商 `status.timeSync` 后，每条 `status.payload` 必须包含工控机级 `timeSync`，每个
`cameras[]` 项必须包含相同结构的逐相机 `timeSync`。该结构严格为：

```json
{
  "available": true,
  "currentUtcNs": "1786671136123456789",
  "clockSource": "PTP_HARDWARE",
  "offsetNs": "125000",
  "offsetAvailable": true,
  "uncertaintyNs": "500000",
  "uncertaintyAvailable": true,
  "maximumObservedOffsetNs": "640000",
  "maximumObservedOffsetAvailable": true,
  "lastSynchronizedUtcNs": "1786671136000000000",
  "lastSynchronizedUtcNsAvailable": true,
  "syncState": "SYNCED",
  "grandmasterIdentity": "00-1D-C1-FF-FE-12-34-56",
  "grandmasterAvailable": true,
  "modelRevision": 17,
  "lastErrorCode": null
}
```

每个 `*Available` 必须与对应值是否非 null 完全一致。`available=false` 时
`currentUtcNs=null`、来源/状态为 `UNKNOWN`、revision 为 0；真实数值 0 必须用
available=true 表示。Grandmaster 未知或路径不支持时必须为 null/false，不得虚构。

每相机最近帧时间字段冻结为 `receivedUtcNs`（十进制字符串或无最近帧时为 null）、
`correctedCaptureUtcNs`（字符串或 null）、`correctedCaptureUtcNsAvailable`、`syncState`、
`uncertaintyNs`（字符串或 null）、`uncertaintyAvailable` 和 `clockModelRevision`。没有最近帧时，
两个时间均为 null，两个 available 均为 false，状态为 `UNKNOWN`，revision 为 0。完整状态其他
字段由 O4-01 实现，但不得改变这里的名称和空值语义。状态序列化超过 1 MiB 时以
`STATUS_PAYLOAD_TOO_LARGE` 拒绝整条状态并报警，不截断 JSON。

传统会话可以继续发送既有 status；上位机必须将缺失时间结构显示为不可用/LEGACY，不得填 0
或推断同步。

## 9. JPEG 预览二进制帧

二进制消息由以下三段连续组成：

1. 4 字节无符号小端整数，表示 JSON 头长度；
2. UTF-8 JSON 头，基础字段包含 `protocolVersion=1`、`messageType=preview.frame`、`messageId`、`machineId`、`cameraId`、`sequence`、`timestamp` 和 `jpegBytes`；
3. 精确为 `jpegBytes` 的 JPEG 数据。

协商 `preview.frame.time` 后，头还必须包含：

```json
{
  "correctedCaptureUtcNs": "1786671135123456789",
  "correctedCaptureUtcNsAvailable": true,
  "syncState": "SYNCED",
  "uncertaintyNs": "500000",
  "uncertaintyAvailable": true,
  "clockModelRevision": 17
}
```

校正时间和不确定度为 null 时对应 available 必须为 false。传统会话保持基础头，接收方将
采集校正时间明确标为不可用。协商时间能力后缺少任一时间字段、出现未知非 `extensions` 字段、
枚举未知或 null/available 不一致均为 `UPLINK_PROTOCOL_ERROR`。

`timestamp` 是预览消息发送 UTC，不是采集 T0。JSON 头上限 64 KiB，JPEG 上限 2 MiB，
每设备协商总帧率不得超过 5 fps。预览是每相机容量 1 的最新帧覆盖槽，由固定工作线程公平
轮转；不持久化、不确认、不重传。过大帧返回/计数 `PREVIEW_FRAME_TOO_LARGE` 并只丢当前帧；
断网、慢消费和丢失预览不得影响采集、算法、事件或可靠上传。

## 10. 参考模拟器

`PaperBreakUplinkSimulator` 同一可执行文件支持 GUI 与 `--headless`，并支持 `--listen`、`--port`、`--workspace`、`--max-devices`、`--workspace-limit-gib`、`--scenario`、`--run-for-ms`、`--version` 和 `--smoke-test`。默认工作区为 `%LOCALAPPDATA%/PaperBreak/UplinkSimulator`，默认设备上限 16、空间上限 20 GiB。

工作区 SQLite `user_version=1` 保存设备、会话、消息、事件、上传 checkpoint、分块和命令。分块写入 `.partial`，每块验证 SHA-256；完成时流式校验整文件并同卷原子重命名到 `events`。启动时复核 checkpoint、已记录分块长度与摘要；不一致文件移动到 `.quarantine`，不删除证据。存储任务队列容量 128，每设备命令队列容量 64；网络事件线程串行产生响应，未形成无界待响应集合；GUI 快照和每设备预览只保留最新值。停止顺序为拒绝新接入、关闭 WebSocket、排空有界存储任务、提交 SQLite/WAL checkpoint，并在 10 秒截止内 join 工作线程。

场景文件使用 `schemaVersion: 1`，每个设备可设置 `rejectConnections`、`disconnectWebSockets`、`responseDelayMs`、`failNextRequests`、`duplicateAcknowledgements`、`replayCommands`、`forceChecksumMismatch` 和 `disconnectAfterChunk`。GUI 和无界面模式使用同一个 `FaultProfile` 模型。

## 11. Edge 配置约束

M8-04 已同步迁移 edge config v2 schema、解析校验、序列化和默认配置：启用 uplink 时只接受 `http://` 基址，会话流只接受服务端返回的 `ws://` URL，凭据与证书引用必须为空。配置同时定义 64 KiB～4 MiB 分块、100～60000 ms 单次 I/O 截止和 1～1024 MiB/s 上传上限；这些传输设置变更需要重启服务。
