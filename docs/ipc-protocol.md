# 本机 IPC v1 协议

## 1. 边界

`PaperBreakEdgeService` 通过 `QLocalServer` 监听固定名称
`PaperBreakEdgeService.Ipc`。Windows 下仅已认证的本机用户可执行请求：普通用户只能查询，
提升后的管理员可执行 `system.reloadConfig`。无法确认身份、匿名或远程连接不会进入业务命令处理。

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
- 当前三条 system 命令均不接受二进制负载。

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
敏感配置。无法提取有效 requestId 的非法前缀、非法 JSON 或通用畸形消息直接断开。

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

推送没有 requestId。状态推送按 coalescing key 保留最新值；其他推送在有界队列满时丢弃。
响应优先于尚未发送的推送；响应无法进入有界出站队列时关闭该客户端。

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

### `system.reloadConfig`

权限：提升后的本机管理员。

```json
{"expectedConfigRevision": 42}
```

payload 必须且只能包含非负整数 `expectedConfigRevision`。服务以客户端 SID 作为 actor、
requestId 作为 correlationId、`ConfigChangeSource::local_ipc`（审计序列化为 `local-ipc`）作为来源调用配置仓储。修订冲突返回
`SYS_CONFIG_VERSION_CONFLICT`；校验、应用或持久化失败保持最后有效配置。

## 5. Qt 客户端连接语义

- 客户端状态为 `stopped`、`connecting`、`connected` 或 `retry-wait`；每次连接尝试分配新的单调递增连接代次；
- 首次立即连接，失败后从 250 ms 开始指数退避，最大 10 秒，并应用 ±20% 抖动；收到有效服务消息或稳定连接 5 秒后重置退避；
- 客户端最多保留 128 个在途请求和 32 MiB 待发送数据；达到上限返回 `IPC_BUSY`，断线期间不缓存新请求；
- 默认连接截止时间为 2 秒，请求截止时间为 5 秒。显式取消返回 `IPC_REQUEST_CANCELLED`，超时返回 `IPC_REQUEST_TIMEOUT`；
- 请求句柄同时包含 requestId 和连接代次。断线以 `IPC_CONNECTION_LOST` 完成该代请求，旧 socket 回调、未知 requestId 和迟到响应不能修改新连接状态；
- 请求不跨连接自动重放。Qt 状态模型在每次新连接后重新发起幂等的 `system.getStatus`，同步完成前及断线后将服务状态标为过期；
- 客户端停止只 abort 自身 QLocalSocket、定时器和在途请求，不发送服务停止命令。

## 6. 兼容和错误行为

- 当前只支持 `protocolVersion=1`；其他版本返回 `IPC_PROTOCOL_VERSION_UNSUPPORTED`，写完响应后关闭；
- 未知命令或字段返回 `IPC_REQUEST_INVALID`；重复 ID 返回 `IPC_REQUEST_CONFLICT`；
- 命令队列或在途请求达到上限返回 `IPC_BUSY`；
- 非管理员执行重载返回 `IPC_UNAUTHORIZED`；服务停止后拒绝新请求并返回
  `SYS_SERVICE_STOPPING`；
- 同一版本只通过 `extensions` 增加可忽略扩展；改名、删除、语义变化或新增必需字段必须提升版本。
