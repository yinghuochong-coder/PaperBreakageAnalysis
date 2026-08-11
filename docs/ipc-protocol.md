# 本机 IPC v1 协议

## 1. 边界

`PaperBreakEdgeService` 通过 `QLocalServer` 监听固定名称
`PaperBreakEdgeService.Ipc`。Windows 下仅已认证的本机用户可执行请求；运行期查询和变更命令均不
要求管理员组身份或 UAC 提升。无法确认身份、匿名或远程连接不会进入业务命令处理。

控制台托盘的后台服务重启不经过 IPC；管理员首次安装或收敛服务配置时，会把该服务对象的查询、
启动和停止权限授予交互式登录用户，但不会授予修改服务配置、删除服务或安装其他服务的权限。

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
| 顺序控制命令队列 | 128 |
| 只读查询命令队列 | 512 |
| 每连接出站消息 | 128 |
| 每连接推送子队列 | 32 |
| 每连接出站字节 | 32 MiB |
| 服务端发布入口 | 64 条 |
| 指标注册表/指标源 | 1024 项/64 个 |
| 活动报警/已清除历史 | 1024 条/4096 条 |
| 近期日志内存环 | 默认 2048 条 |
| 诊断 ZIP 内部上限 | 8 MiB |
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
`pendingRestartPaths`、`recoveredFromHistory` 和当前有效的 `loggingLevel`。旧服务没有
`loggingLevel` 时控制台按 `info` 兼容；首次连接前也使用 `info`，断线后保留最后一次有效等级。

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

权限：已认证本机用户。

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
生产服务每秒采集 Windows 系统/进程/磁盘、IPC 和相机指标；相机采集运行时提供实际帧率、
帧缺口、接收超时、最近帧 Unix 毫秒时间、当前曝光/增益和采集带宽。算法、上位机、事件、
NVMe 写入率以及设备不支持的亮度/温度/重连数据，在对应里程碑或设备能力接入前保留稳定指标名，
并以 `available=false` 明确表示不可用，客户端不得把零值解释为实际测量结果。

### `system.exportDiagnostics`

权限：已认证本机用户。请求 payload 必须为空；请求不接受二进制负载。服务记录脱敏后的
audit 日志，不接受客户端提供的路径，也不在服务端创建诊断临时文件。

成功响应 JSON 包含 `fileName`、`contentType="application/zip"`、`size` 和
`redacted=true`，二进制负载为 ZIP store 格式。包的内部上限为 8 MiB，且仍受 IPC 16 MiB
通用二进制上限保护。固定条目为：

```text
manifest.json
config-redacted.json
system.json
metrics.json
cameras.json
network.json
alarms.json
recent-logs.json
version.json
```

配置中的 credential/certificate reference、password、token、secret 和 private key 类字段
递归替换为 `<redacted>`；日志和报警沿用服务端既有脱敏结果。包不包含滚动日志文件、原始图像、
密钥或客户端任意路径。报警和日志各取最近最多 200 条并在 JSON 中保留 `truncated`；M5-07 前报警
历史仍为当前服务进程内历史。

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

权限：已认证本机用户。

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
  "threadName": "ipc-command",
  "limit": 100
}
```

所有字段均可省略。`afterSequence` 是无符号整数；`categories` 最多 10 项，可用值为
`service`、`camera`、`algorithm`、`event`、`storage`、`uplink`、`ipc`、`ui`、
`audit`、`performance`；`minimumLevel` 为 `trace`、`debug`、`info`、`warning`、
`error` 或 `critical`；`threadName` 可选，必须符合小写字母/数字/连字符且最长 63 字符；
`limit` 范围 1～200，默认 100。

响应包含 `firstAvailableSequence`、`latestSequence`、`records` 和 `truncated`。
每条记录包含 `sequence`、本地 RFC 3339 `timestamp`、`threadName`、`threadId`、
`category`、`level` 和脱敏后的 `message`。查询读取异步日志线程维护的内存环，不读取滚动日志文件，
因此允许短暂最终一致性。
当请求游标早于当前最早可用序号或匹配结果超过 `limit` 时，`truncated=true`。

### 相机查询与控制

`camera.list` 和 `camera.discover` 允许已认证本机用户调用，payload 必须为空。`camera.list`
返回顶层 `storedConfigRevision`、`topologyRestartRequired` 和最多四个配置槽位 `cameras`；
每个槽位包含 `cameraId`、`location`、`enabled`、`serialNumber`、`state`、
`savedConfigRevision`、完整 `saved` 参数，以及设备已连接时的 `device`、服务回读 `actual`
参数和 `capabilities`。当前 `capabilities.roi` 包含传感器宽高，以及 `width`、`height`、
`offsetX`、`offsetY` 各自的 `minimum`、`maximum`、`increment`；控制台必须按设备返回的范围和
步进约束输入，服务仍会对宽高与偏移量组合做最终校验。`topologyRestartRequired=true` 表示保存的
相机拓扑与当前运行拓扑不同，连接、采集和参数
下发均应等待服务重启。

`saved.autoExposure` 和连接后的 `actual.autoExposure` 使用 `Off`、`Once`、`Continuous`；
`capabilities.autoExposureModes` 列出设备实际支持的子集。`Once` 完成后设备可能自行回到 `Off`；
列表和状态轮询仍返回最近一次事务回读缓存，不在采集期间周期性访问 MVS 节点。

每个槽位还公开 `saved.lineIo`；连接后公开 `actual.lineIo`、`lineInput` 和
`capabilities.lineIo`。能力对象包含 Line 0 输入、上升沿/下降沿支持状态，Line 1 频闪支持状态、
不支持原因，以及 `strobeDurationUs`、`strobePreDelayUs`、`strobePostDelayUs` 三项设备实际
`minimum/maximum/increment`。`lineInput` 包含 `enabled`、原始 `rawLevel`、按保存有效电平换算的
`alarmActive`、`revision`、`timestampUtcMs` 和 `stale`。查询快照是断线重连和漏推送后的事实源。

设备已连接后，`device`、`capabilities` 和 `actual` 表示最近一次成功连接或参数更新时的已验证
回读值。采集期间 `camera.list` 和 `camera.getConfig` 读取该缓存及线程安全采集统计，不周期性访问
MVS 参数节点；因此状态查询不会与取帧争用设备互斥。配置 `frameRate` 时适配器先启用
`AcquisitionFrameRateEnable`（节点明确不支持时兼容直写），再写入并回读帧率；enable 状态属于同一
参数事务并在失败时回滚。

`camera.discover` 返回结构如下；字段名是 IPC v1 的固定名称：

```json
{
  "devices":[{
    "model":"MV-CS020-60GM",
    "serialNumber":"DB1888674",
    "ip":"192.168.11.115",
    "networkInterface":"192.168.11.102",
    "exclusiveAccessAvailable":false
  }]
}
```

生产服务固定装配 Hikrobot MVS 设备提供者；若部署不完整导致提供者缺失，则返回
`SYS_NOT_SUPPORTED`，不得伪造设备。客户端在连接成功后
自动发现一次，也可以显式重新发现；发现结果不依赖是否已有配置槽位。

全部相机命令只允许已认证本机用户调用，不要求进程提升为管理员。服务仍根据命名管道客户端令牌
校验本机来源和身份，并以客户端 SID 记录相机绑定和参数配置变更审计。
除 `camera.updateConfig` 外，单相机命令 payload 必须且只能包含：

```json
{"cameraId":"CAM01"}
```

支持 `camera.connect`、`camera.disconnect`、`camera.start`、`camera.stop`、
`camera.getConfig`、`camera.captureSnapshot` 和 `camera.softwareTrigger`。连接时服务把当前保存参数
下发给设备并回读实际值；如果设备已打开但保存参数不符合当前设备能力，连接仍返回成功并保持设备
连接，响应包含 `state="connected"`、设备 `actual`、`applied=false` 和结构化 `applyError`，客户端应
提示用户先用只读 `camera.getConfig` 读取当前参数再确认保存，不得把该情况显示成连接失败。断开会先
停止仍在进行的采集。`camera.captureSnapshot` 只返回帧号、宽和高，
不在 IPC 工作线程进行磁盘写入或 JPEG 编码。软件触发仅在设备实际处于软件触发采集模式时成功。

`camera.bind` 要求已认证本机用户身份，payload 为：

```json
{
  "cameraId":"CAM01",
  "serialNumber":"DB1888674",
  "location":"用户填写的安装位置",
  "expectedConfigRevision":1
}
```

服务每次绑定都重新枚举设备，仅接受尚未使用的 `CAM01`～`CAM04`、尚未绑定的序列号和批准型号
`MV-CS020-60GM`。设备必须可独占访问；服务连接设备、完整回读当前参数并成功断开后，才通过配置
仓储乐观修订与原子替换保存。占用、型号不符、参数回读失败、修订冲突、断开失败或持久化失败均不
修改配置。成功响应包含 `saved=true`、新的 `storedConfigRevision` 与
`restartRequired=true`；客户端只提示用户通过既有入口重启服务，不自动重启。

`camera.updateConfig` payload：

```json
{
  "cameraId":"CAM01",
  "expectedConfigRevision":42,
  "parameters":{
    "exposureUs":1000.0,
    "autoExposure":"Off",
    "gainDb":2.0,
    "frameRate":30.0,
    "roi":{"width":1920,"height":1080,"offsetX":0,"offsetY":0},
    "reverseX":false,
    "reverseY":false,
    "pixelFormat":"Mono8",
    "triggerMode":"Continuous",
    "triggerSource":"",
    "triggerDelayUs":0,
    "packetSizeBytes":1500,
    "interPacketDelayNs":0,
    "lineIo":{
      "alarmInputEnabled":false,
      "alarmActiveLevel":"High",
      "strobeOutputEnabled":false,
      "strobeDurationUs":0,
      "strobePreDelayUs":0,
      "strobePostDelayUs":0
    }
  }
}
```

`autoExposure` 映射 Hikrobot `ExposureAuto`。完整参数事务先写 `Off`、再写 `exposureUs`，最后写
目标 `Off`/`Once`/`Continuous` 并回读；开始和恢复取流只确认并保留该模式，不再强制关闭。
旧 v2/v3 配置迁移为 `Off`。

`interPacketDelayNs` 始终表示真实纳秒，不是 Hikrobot `GevSCPD` 寄存器的原生 tick。
Hikrobot 适配器根据设备 `GevTimestampTickFrequency` 转换能力范围、写入值和回读值；例如
125 MHz 设备的 1 tick 为 8 ns，因此请求 `400` ns 时写入 `GevSCPD=50`，原生
`GevSCPD=400` 则回读为 `3200` ns。无法取得有效频率或无法精确换算时，参数能力/操作失败，
不得把原生 tick 原样返回为纳秒。

`parameters` 只允许上述字段，并由配置 schema 与设备能力共同校验。Line 0 固定为输入；Line 1
固定为 Strobe 且源为 `ExposureStartActive`，客户端不能覆盖线路模式或源。服务先解析完整候选配置；设备
已连接时，还必须在保存前按该设备能力校验候选完整参数，校验失败不得提高配置修订号。通过后再由
配置仓储执行乐观修订、审计和原子替换，并向已连接设备下发完整参数及回读。成功响应以 `saved`、`dispatched`、
`applied`、`restartRequired` 和 `storedConfigRevision` 区分阶段；保存成功但设备未连接或拒绝参数时，
响应仍保留 `saved=true`，同时返回 `applied=false` 和结构化 `applyError`，客户端不得把保存值显示为
实际值。

### 存储配置与实际生效值

`storage.getConfig` 和 `storage.updateConfig` 均只要求已认证本机用户，服务停止写入阶段拒绝更新。
两项命令均不接受二进制负载。

- `storage.getConfig`：payload 为空。返回完整保存配置 `storage`、实际生效配置
  `effectiveStorage`、`storedConfigRevision`、`effectiveConfigRevision` 和
  `pendingRestartPaths`。
- `storage.updateConfig`：payload 必须且只能包含无符号 `expectedConfigRevision` 和完整
  `storage` 对象。对象包含 `eventRoot`、`cacheRoot`、`rollingCacheEnabled`、
  `maximumCacheStorageGiB`、`rollingCacheWriteLimitMiBps`、
  `rollingCacheIoTimeoutMs`、`warningFreeSpaceGiB`、`criticalFreeSpaceGiB`、
  `stopFreeSpaceGiB` 和 `maximumEventStorageGiB`，未知或缺失字段由 schema 拒绝。
  服务复用配置仓储的乐观修订、审计、原子替换与失败回滚。根目录和四项 NVMe 参数进入
  `pendingRestartPaths`，重启前 `effectiveStorage` 保留旧值；三项磁盘水位和事件容量上限
  事务式热应用到运行时。响应同时返回 `applied`，客户端必须以 `effectiveStorage` 和
  `pendingRestartPaths` 展示实际生效状态，不能把保存成功等同于已经应用。

控制端存储页的实际水位、容量、NVMe 状态、队列、恢复、索引和租约数据继续来自有界
`system.getMetrics` 快照；配置读取响应不重复或伪造运行指标。

### 算法配置、实际状态与当前图像测试

`algorithm.getConfig`、`algorithm.updateConfig` 和 `algorithm.testCurrentFrame` 均只要求已认证
本机用户。三项命令均只接受 CAM01～CAM04，
未知字段被拒绝；服务停止写入阶段拒绝后两项命令。

- `algorithm.getConfig`：payload 为 `{"cameraId":"CAM01"}`。返回完整保存配置
  `algorithm`、实际有效配置 `effectiveAlgorithm`、两项配置修订和 `runtime`。运行时包含实际
  检测器信息、`active` / `partially-degraded` / `disabled` / `manual-trigger-only` 状态、当前帧可用性及序号；
  `metrics` 包含有界算法运行时的队列深度/容量/高水位、提交/处理/跳过/失败帧、处理调用与
  最近/平均/最大耗时、候选/确认/拒绝计数，并追加 `consecutiveBacklogEvents` 和
  `resultQueueRejected`。状态和指标均来自请求 `cameraId` 对应的独立 Lane，不再返回共享汇总；
  禁用时 `detector` 为 `null`。
- `algorithm.updateConfig`：payload 必须且只能包含 `cameraId`、无符号
  `expectedConfigRevision` 和完整 `algorithm` 对象。服务复用严格 schema、乐观修订、原子
  保存、审计和事务式热应用；冲突返回 `SYS_CONFIG_VERSION_CONFLICT`，失败保留旧检测器和
  旧有效配置。成功响应与 `algorithm.getConfig` 相同，以便客户端立即显示实际应用修订与状态。
- `algorithm.testCurrentFrame`：payload 为 `{"cameraId":"CAM01"}`。服务复制该相机最近一帧
  的 RAII 视图，在正式算法队列和候选状态机之外创建隔离检测器，执行一次检测并同步生成最多
  8 MiB 的 JPEG；没有当前帧返回 `ALGORITHM_NOT_READY`。JSON 返回 `detector`、完整
  `DetectionResult`、`isolated=true`、`candidateCreated=false`、JPEG 格式/字节数及源图尺寸，
  二进制负载为该 JPEG。该操作不改变正式检测器状态/指标，不创建候选且不写盘；Qt 客户端按
  `evaluatedRegion` 绘制 ROI、候选类型和置信度叠加。

M6-00 仍为阻塞门禁；检测器响应中的 `prototypeOnly=true` 必须在 UI 持续可见，不能解释为
正式断纸算法验收通过。

### 事件配置、查询、复核与导出

事件读取和写入命令均只要求已认证本机用户，包括 `event.updateConfig`、`event.manualTrigger`、
`event.confirm`、`event.reject`、`event.export` 和 `event.retryUpload`。
事件目录只有在 manifest 写完并完成同卷原子提交后才对这些命令可见。

- `event.getConfig`：payload 为空。返回完整 `event` 配置、存储/有效配置修订，及
  `previewVideoGenerationAvailable`、`uploadRuntimeAvailable` 能力标志。M5 两项均为
  `false`，客户端不得把配置开关显示成运行成功。
- `event.updateConfig`：payload 必须且只能包含 `expectedConfigRevision` 和完整 `event`
  对象。沿用配置修订、原子保存、审计、热应用和失败回滚语义。
- `event.list`：payload 可包含 `startTimeUtcMs`、`endTimeUtcMs`、`decisionState`、
  `persistenceState`、`reviewState`、`reviewDecision`、`cameraId`、`offset` 和 `limit`；
  `limit` 为 1～200。持续“截至当前”模式不发送 `endTimeUtcMs`，启动后事件因此不会被旧的
  固定结束时间过滤。响应返回稳定排序的 `events`、`total`、`offset`、`limit` 及不受当前
  筛选影响的全局生命周期 `summary`。兼容筛选 `eventState` 暂时等同于
  `decisionState`。
- 每条事件记录向后兼容新增 `integrityState`、可空 `integrityCheckedAtUtcMs` 和
  `integrityErrorCode`。列表不读取或散列事件原始负载。
- `event.get`：payload 为 `{"eventId":"..."}`。所有生命周期均返回数据库状态；仅当
  `persistenceState=Committed` 且制品未标损时，以每文件一次顺序读取复验格式、长度和
  SHA-256，并返回正式绝对目录、
  manifest 字节数、原始/关键帧数、序列缺口和追溯状态及首张关键帧 JPEG。列表和详情都
  包含 `decisionState`、`persistenceState`、`reviewState`、可空 `reviewDecision`、
  `artifactsAvailable`、`triggerCount`，以及兼容别名 `eventState`。
- `event.getSummary`：payload 为 `{"eventId":"..."}`，供交互式详情选择使用。返回与
  `event.get` 相同的目录、计数、序列缺口、结构追溯状态和首张关键帧 JPEG，但只解析并
  结构检查 manifest，且只读取、校验实际返回的关键帧；不读取原始块或 `event.json`，也不把
  `integrityState=Unverified` 提升为 `Verified`。结构或缩略图校验失败仍将事件标损。
- `event.getManifest`：payload 为 `{"eventId":"..."}`。只解析 manifest 并检查路径、普通文件
  存在性和声明长度，不读取负载或计算 SHA；以不超过 8 MiB 的二进制 UTF-8 JSON 返回完整
  不可变 manifest。`verified` 反映数据库既有完整性状态，响应同时返回 `integrityState`。
- `event.manualTrigger`：payload 为 `{"cameraId":"CAM01"}`。只在下一张新有效帧触发；
  返回 `accepted` 和 `alreadyPending`，不会停止相机采集或在相机回调中编码/写盘。
- `event.confirm` / `event.reject`：只允许 `Committed` 事件。payload 必须包含 `eventId` 和正整数
  `expectedReviewRevision`。SQLite 使用乐观版本并发；相同终态重复请求幂等，过期或相反
  终态返回 `EVENT_VERSION_CONFLICT`；只更新复核字段，不覆盖算法判定，不可变 manifest 不修改。
- `event.export`：payload 为 `{"eventId":"..."}`。服务再次完整校验，只将正式事件按
  manifest 顺序以每源文件一次读取完成 SHA、块结构检查和 ZIP 流写入；响应包含
  `verified=true`、大小、
  文件数和 `exportSourcePath`，不通过 IPC 传完整原始序列。Qt 客户端只从该服务返回路径
  分块读取，并通过 `QSaveFile` 原子保存到用户选择的目标；服务不接受任意目标路径。导出器
  支持 ZIP64，并在显式 64 GiB 总上限或单文件/文件数上限之外返回
  `EVENT_EXPORT_TOO_LARGE`。
- 详情或导出完整性失败会将事件设为 `integrityState=Failed`、`storageState=Damaged`、
  `artifactsAvailable=false`，登记 Critical 报警并拒绝发布部分导出；不改变判定、复核和
  `persistenceState=Committed`。
- `event.retryUpload`：payload 必须且只能包含 `eventId`。M8-03 起，已装配事件数据库时将该事件处于 `RetryWait`、`PermanentFailed` 或 `ManualIntervention` 的持久任务重置为 `Pending` 并返回 `requeuedJobs`；重复请求不创建任务或事件，没有匹配失败任务时返回 0。未装配持久仓库时返回 `SYS_NOT_SUPPORTED`。

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
- `camera.lineInputChanged`：payload 包含 `cameraId`、`rawLevel`、`alarmActive`、递增
  `revision` 和 `timestampUtcMs`；无二进制负载。每相机使用独立
  `camera.lineInputChanged:<cameraId>` 合并键和 `coalesce_latest` 策略，快速边沿允许合并但最终
  电平与修订必须准确。本推送不创建系统报警或断纸事件。
- `event.lifecycleChanged` 在候选建立及每个生命周期变化时发布，`event.committed` 在正式
  提交后发布；两者都走现有有界 IPC 推送队列并按 Event ID 合并。客户端在列表查询在途时
  只记一次补查，查询完成后立即刷新；即使推送丢失或重连，5 秒 `event.list` 周期补查仍是
  最终事实来源。

三类报警推送 payload 均包含 `registryRevision` 和完整报警字段。报警推送允许丢弃，
`alarm.list` 始终是恢复事实来源。

## 6. Qt 客户端连接语义

- 客户端状态为 `stopped`、`connecting`、`connected` 或 `retry-wait`；每次连接尝试分配新的单调递增连接代次；
- 首次立即连接，失败后从 250 ms 开始指数退避，最大 10 秒，并应用 ±20% 抖动；收到有效服务消息或稳定连接 5 秒后重置退避；
- 客户端最多保留 128 个在途请求和 32 MiB 待发送数据；达到上限返回 `IPC_BUSY`，断线期间不缓存新请求；
- 默认连接截止时间为 2 秒，普通查询请求截止时间为 5 秒；相机控制操作默认使用 30 秒且可由客户端构造参数调整。显式取消返回 `IPC_REQUEST_CANCELLED`，超时返回 `IPC_REQUEST_TIMEOUT`；
- 相机开始/停止等控制请求超时只表示响应未在客户端截止时间内到达，不能据此宣称设备操作失败。控制台显示“结果未知，正在同步”，并使用后续 `camera.list` 快照确认目标状态；快照确认结果必须与普通成功响应区分标记；
- 请求句柄同时包含 requestId 和连接代次。断线以 `IPC_CONNECTION_LOST` 完成该代请求，旧 socket 回调、未知 requestId 和迟到响应不能修改新连接状态；
- 请求不跨连接自动重放。Qt 状态模型在每次新连接后重新发起幂等的 `system.getStatus`，同步完成前及断线后将服务状态标为过期；
- 相机客户端每次连接或重连后调用 `camera.list` 恢复 Line 0 状态；运行中消费
  `camera.lineInputChanged`。状态过期时保留最后有效值并设置 `stale=true`，重连初始电平负责纠正；
- 客户端停止只 abort 自身 QLocalSocket、定时器和在途请求，不发送服务停止命令。

## 7. 兼容和错误行为

- 当前只支持 `protocolVersion=1`；其他版本返回 `IPC_PROTOCOL_VERSION_UNSUPPORTED`，写完响应后关闭；
- 未知命令或字段返回 `IPC_REQUEST_INVALID`；重复 ID 返回 `IPC_REQUEST_CONFLICT`；
- 控制队列、查询队列或在途请求达到各自上限时返回 `IPC_BUSY`；只读查询由两个工作线程执行，顺序控制由一个工作线程执行，未知命令按控制命令处理；
- 未认证或非本机请求返回 `IPC_UNAUTHORIZED`；服务停止后拒绝新写请求并返回
  `SYS_SERVICE_STOPPING`；
- 同一版本只通过 `extensions` 增加可忽略扩展；改名、删除、语义变化或新增必需字段必须提升版本。
