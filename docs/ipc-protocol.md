# 本机 IPC v1 协议

## 1. 边界

`PaperBreakEdgeService` 通过 `QLocalServer` 监听固定名称
`PaperBreakEdgeService.Ipc`。Windows 下仅已认证的本机用户可执行请求：普通用户只能查询，
提升后的管理员可执行 `system.reloadConfig` 和 `alarm.acknowledge`。无法确认身份、匿名或远程连接不会进入业务命令处理。

IPC v1 不持久化请求和推送。客户端断线重连后必须重新查询状态；服务端不重放断线期间的推送。

## 2. 帧格式

每帧按以下顺序编码，两个整数均为无符号 32 位网络字节序（大端）：

```text
uint32 headerLength
uint32 binaryLength
headerLength bytes UTF-8 JSON
binaryLength bytes optional binary payload
```

固定上限：

| 项目 | 上限 |
| --- | ---: |
| JSON header | 1 MiB |
| binary payload | 16 MiB |
| 活动连接 | 4 |
| 每连接在途请求 | 16 |
| 每连接近期 requestId | 1024 |
| 命令队列 | 64 |
| 每连接出站消息 | 128 |
| 每连接推送子队列 | 32 |
| 每连接出站字节 | 32 MiB |
| 服务端发布入口 | 64 条 |
| 指标注册表/指标源 | 1024 项/64 个 |
| 活动报警/已清除历史 | 1024 条/4096 条 |
| 近期日志内存环 | 默认 2048 条 |
| 不完整帧总截止时间 | 5 秒 |

长度在分配对应负载前校验。v1 不包含 CRC；完整性由长度边界、UTF-8 JSON 解析和 DTO 校验保证。
不完整帧截止时间从收到首字节开始，后续零散字节不会延长截止时间。

## 3. 消息

请求：

```json
{
  "protocolVersion": 1,
  "messageType": "request",
  "requestId": "019870f2-6c80-7a31-9b52-6e3b9ca1d88f",
  "command": "system.getStatus",
  "timestamp": "2026-08-01T12:00:00.123Z",
  "payload": {}
}
```

- `requestId` 必须是 36 字符规范 UUID，同一连接的近期 ID 不得重复；
- `timestamp` 必须是 RFC 3339，协议超时使用服务端单调时钟，不信任客户端时间；
- v1 只允许顶层字段 `protocolVersion`、`messageType`、`requestId`、`command`、
  `timestamp`、`payload` 和可选对象 `extensions`；
- 本文列出的 system、alarm、log 和 preview 控制命令均不接受请求二进制负载。

成功响应：

```json
{
  "protocolVersion": 1,
  "messageType": "response",
  "requestId": "019870f2-6c80-7a31-9b52-6e3b9ca1d88f",
  "timestamp": "2026-08-01T12:00:00.124Z",
  "success": true,
  "payload": {},
  "error": null
}
```

失败响应的 `error` 使用 `docs/error-codes.md` 的公共错误对象；不公开 Win32 原始码、绝对路径或
敏感配置。只读 `system.getLocations` 的事件根目录是经服务校验、仅供本机资源管理器打开的窄例外，
不进入错误对象、日志详情或上位机响应。无法提取有效 requestId 的非法前缀、非法 JSON 或通用畸形消息直接断开。

推送：

```json
{
  "protocolVersion": 1,
  "messageType": "push",
  "eventName": "status.changed",
  "timestamp": "2026-08-01T12:00:00.125Z",
  "payload": {"serviceState": "stop-requested"}
}
```

推送没有 requestId。`status.changed` 按状态 key 保留最新值；`alarm.raised`、
`alarm.cleared` 和 `alarm.acknowledged` 在有界队列满时允许丢弃。
响应优先于尚未发送的推送；响应无法进入有界出站队列时关闭该客户端。
推送只是低延迟提示，不是事实来源。客户端重连或发现版本跳跃后必须重新调用
`system.getStatus` 或 `alarm.list`。

## 4. 命令

### `system.getStatus`

权限：已认证本机用户。请求 payload 必须为空。

响应包含 `serviceState`、`acceptingWrites`、`startedAt`、`timestamp`、`machineId`、
`configSchemaVersion`、`storedConfigRevision`、`effectiveConfigRevision`、
`pendingRestartPaths` 和 `recoveredFromHistory`。

### `system.getVersion`

权限：已认证本机用户。请求 payload 必须为空。

响应包含应用版本、Git 提交/dirty 状态、构建时间、编译器以及 Qt、OpenCV、spdlog、
nlohmann/json 和 SQLite 版本。

### `system.getLocations`

权限：已认证本机用户。请求 payload 必须为空。

响应只包含 `eventRoot`：服务以当前有效配置和配置文件目录解析、规范化后的 UTF-8 绝对路径。
该命令只允许 Qt 客户端打开既定事件根目录，不接受客户端路径，也不授予目录写权限。连接断开或
响应失效后客户端必须把路径标记过期并禁用打开动作，不能继续无提示使用旧路径。

### `system.reloadConfig`

权限：提升后的本机管理员。

```json
{"expectedConfigRevision": 42}
```

payload 必须且只能包含非负整数 `expectedConfigRevision`。服务以客户端 SID 作为 actor、
requestId 作为 correlationId、`ConfigChangeSource::local_ipc`（审计序列化为 `local-ipc`）作为来源调用配置仓储。修订冲突返回
`SYS_CONFIG_VERSION_CONFLICT`；校验、应用或持久化失败保持最后有效配置。

### `system.getMetrics`

权限：已认证本机用户。

```json
{"prefixes":["process.","ipc."],"limit":256}
```

- `prefixes` 可省略，最多 16 个非空字符串，每项不超过 64 字节；按指标名称前缀匹配；
- `limit` 可省略，范围 1～256，默认 256；
- 响应包含 `snapshotVersion`、`sampledAt`、`metrics` 和 `truncated`；每项指标包含
  `name`、`value`、`unit`、`available`。

指标快照按来源原子替换。数据库模块在 M5 前以 `database.state=not-initialized` 和
`database.available=false` 明确表示尚未初始化，不等价于数据库故障。

### `alarm.list`

权限：已认证本机用户。

```json
{
  "active": true,
  "minimumSeverity": "Warning",
  "source": "process",
  "beforeAlarmId": 100,
  "limit": 100
}
```

所有字段均可省略。`minimumSeverity` 为 `Info`、`Warning`、`Error` 或 `Critical`；
`source` 为不超过 128 字节的非空字符串；`beforeAlarmId` 是无符号整数；`limit` 范围
1～200，默认 100。结果按 `alarmId` 从新到旧返回，包含 `registryRevision`、`alarms`、
`nextBeforeAlarmId`（无下一页时为 `null`）和 `truncated`。

每条报警包含 `alarmId`、`revision`、`code`、`severity`、`source`、
`firstOccurredAt`、`lastOccurredAt`、`active`、`occurrenceCount`、`message`、
`details` 和 `acknowledged`。活动报警及已保留历史均可查询；活动报警不会因容量压力淘汰。

### `alarm.acknowledge`

权限：提升后的本机管理员。

```json
{"alarmId":123}
```

payload 必须且只能包含正整数 `alarmId`。确认对活动报警和仍保留的已清除历史幂等生效；
记录不存在或已被历史环淘汰时返回 `ALARM_NOT_FOUND`。成功响应返回完整报警记录。

### `log.tail`

权限：已认证本机用户。

```json
{
  "afterSequence": 1200,
  "categories": ["service", "ipc"],
  "minimumLevel": "warning",
  "limit": 100
}
```

所有字段均可省略。`afterSequence` 是无符号整数；`categories` 最多 10 项，可用值为
`service`、`camera`、`algorithm`、`event`、`storage`、`uplink`、`ipc`、`ui`、
`audit`、`performance`；`minimumLevel` 为 `trace`、`debug`、`info`、`warning`、
`error` 或 `critical`；`limit` 范围 1～200，默认 100。

响应包含 `firstAvailableSequence`、`latestSequence`、`records` 和 `truncated`。
每条记录包含 `sequence`、`timestamp`、`threadId`、`category`、`level` 和脱敏后的
`message`。查询读取异步日志线程维护的内存环，不读取滚动日志文件，因此允许短暂最终一致性。
当请求游标早于当前最早可用序号或匹配结果超过 `limit` 时，`truncated=true`。

### `preview.subscribe`

权限：已认证本机用户。请求二进制负载必须为空。

```json
{"cameraIds":["CAM01","CAM02"]}
```

`cameraIds` 必须包含 1 至 4 个不重复的逻辑相机编号，每项最长 32 字节。订阅绑定当前 IPC
连接；同一连接再次订阅会原子替换其相机集合。服务最多接受 4 个并发预览订阅。无订阅者时服务
不进行 JPEG 编码。响应包含 `subscribed=true` 与实际 `cameraIds`。

### `preview.unsubscribe`

权限：已认证本机用户。payload 必须为空。取消当前 IPC 连接的全部预览订阅，响应
`{"subscribed":false}`。连接断开后的订阅不得恢复；客户端重连后必须重新订阅。

报警 ID、报警历史、日志序号和近期日志均是进程内状态，服务重启后重置；M1-06 不提供跨重启游标。

## 5. 推送事件

- `status.changed`：payload 包含 `serviceState` 和 `acceptingWrites`，按固定 key 合并；
- `alarm.raised`：新报警或同一 `(code, source)` 重复发生；
- `alarm.cleared`：报警生命周期结束并进入内存历史；
- `alarm.acknowledged`：报警首次被确认，重复确认不重复推送。
- `preview.frame`：仅发送给订阅该逻辑相机的当前连接。payload 包含 `cameraId`、
  `cameraFrameNumber`、`sequenceNumber`、源图像 `width`/`height`/`stride`、可选 `brightness`、
  `actualFps`、`cameraStatus`、`roi` 和 `detectionResult`；二进制负载为 JPEG。每个连接/相机
  在服务端只保留最新待发送帧，旧帧可丢弃。二进制负载仍受 16 MiB 通用上限约束。

三类报警推送 payload 均包含 `registryRevision` 和完整报警字段。报警推送允许丢弃，
`alarm.list` 始终是恢复事实来源。

## 6. Qt 客户端连接语义

- 客户端状态为 `stopped`、`connecting`、`connected` 或 `retry-wait`；每次连接尝试分配新的单调递增连接代次；
- 首次立即连接，失败后从 250 ms 开始指数退避，最大 10 秒，并应用 ±20% 抖动；收到有效服务消息或稳定连接 5 秒后重置退避；
- 客户端最多保留 128 个在途请求和 32 MiB 待发送数据；达到上限返回 `IPC_BUSY`，断线期间不缓存新请求；
- 默认连接截止时间为 2 秒，请求截止时间为 5 秒。显式取消返回 `IPC_REQUEST_CANCELLED`，超时返回 `IPC_REQUEST_TIMEOUT`；
- 请求句柄同时包含 requestId 和连接代次。断线以 `IPC_CONNECTION_LOST` 完成该代请求，旧 socket 回调、未知 requestId 和迟到响应不能修改新连接状态；
- 请求不跨连接自动重放。Qt 状态模型在每次新连接后重新发起幂等的 `system.getStatus`，同步完成前及断线后将服务状态标为过期；
- 客户端停止只 abort 自身 QLocalSocket、定时器和在途请求，不发送服务停止命令。

## 7. 兼容和错误行为

- 当前只支持 `protocolVersion=1`；其他版本返回 `IPC_PROTOCOL_VERSION_UNSUPPORTED`，写完响应后关闭；
- 未知命令或字段返回 `IPC_REQUEST_INVALID`；重复 ID 返回 `IPC_REQUEST_CONFLICT`；
- 命令队列或在途请求达到上限返回 `IPC_BUSY`；
- 非管理员执行重载或报警确认返回 `IPC_UNAUTHORIZED`；服务停止后拒绝新写请求并返回
  `SYS_SERVICE_STOPPING`；
- 同一版本只通过 `extensions` 增加可忽略扩展；改名、删除、语义变化或新增必需字段必须提升版本。
