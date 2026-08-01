# 业务错误模型与错误码登记表

## 1. 目的和适用范围

本文定义服务核心、适配器、IPC、日志、报警、持久化和上位机传输共同使用的稳定业务错误契约。调用方只能依据 `businessCode`、结构化字段和协议状态作判断；面向操作员的 `message`、厂商原始码和日志文本不得成为控制逻辑。

## 2. 统一错误对象

错误实例的逻辑结构至少为：

| 字段 | 必需 | 语义 |
| --- | ---: | --- |
| `businessCode` | 是 | 本文登记的稳定字符串 |
| `severity` | 是 | `Info`、`Warning`、`Error`、`Critical` |
| `message` | 是 | 面向操作员的本地化可读信息，不作为程序判断依据 |
| `module` | 是 | `system`、`config`、`camera`、`pipeline`、`algorithm`、`event`、`storage`、`database`、`ipc`、`uplink`、`upload` 等 |
| `operation` | 是 | 失败阶段的稳定操作名，如 `camera.open` |
| `sourceId` | 否 | `CAM01`、`EventId`、`UploadTaskId` 或逻辑文件 ID |
| `nativeDomain` | 否 | 原始码命名空间，如 `hikrobot-mvs`、`win32`、`sqlite`、`qt-network` |
| `nativeCode` | 否 | 原始码文本；十六进制或十进制表示由 `nativeDomain` 约定 |
| `details` | 是 | 经白名单和脱敏的结构化上下文对象；无内容时为空对象 |
| `retryable` | 是 | 当前错误实例是否允许由所属策略受限重试 |
| `timestamp` | 是 | UTC RFC 3339 墙上时间，固定三位毫秒 |
| `correlationId` | 否 | 关联请求、事件、写入和上传流程 |

示例：

```json
{
  "businessCode": "CAMERA_OPEN_FAILED",
  "severity": "Error",
  "message": "无法打开逻辑相机 CAM01",
  "module": "camera",
  "operation": "camera.open",
  "sourceId": "CAM01",
  "nativeDomain": "hikrobot-mvs",
  "nativeCode": "0x80000203",
  "details": {
    "deviceSerialNumberSuffix": "4821",
    "attempt": 2
  },
  "retryable": true,
  "timestamp": "2026-07-30T05:47:00.123Z",
  "correlationId": "req-019870f2-6c80-7a31-9b52-6e3b9ca1d88f"
}
```

IPC 失败响应必须携带同一个 `businessCode`，但可以只暴露允许公开的字段。完整原始诊断写入受保护日志；UI 和上位机不得默认接收堆栈、绝对路径或完整设备序列号。

## 3. 错误码规则

### 3.1 命名

- 格式为大写 ASCII `^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$`；
- 前缀表示稳定故障域，不表示具体 C++ 类；
- 名称描述失败事实，不嵌入厂商名、数值原始码、严重级别或是否可重试；
- 同一失败在日志、报警、IPC 和上传状态中引用同一业务码。

### 3.2 兼容性

1. 已发布业务码不得改义、复用或静默删除；
2. 需要更细语义时新增错误码，旧码可标为 deprecated，但仍保留解析；
3. 仅修改文字、默认严重级别说明或补充安全上下文不改变业务码；
4. 删除/合并代码前必须有版本化兼容方案；
5. 未识别的未来业务码由旧客户端显示为“未知业务错误”，保留原字符串，不映射成成功；
6. 厂商、Win32、SQLite 或网络原始码变化不得迫使业务码变化。

### 3.3 严重级别和重试

登记表中的严重级别和可重试性是默认值。运行时可以根据影响范围、连续次数和降级状态提升严重级别，或在达到截止时间/重试上限后把 `retryable` 改为 `false`；不得把本质永久失败无依据地改成可重试。

`retryable=true` 不是立即重试命令。所属模块必须同时定义：

- 最大尝试次数或总截止时间；
- 退避和抖动；
- 停止令牌/取消路径；
- 幂等条件；
- 达到上限后的终态和报警。

队列满、帧超时和网络断开等高频状态不用 C++ 异常表达。它们先计入指标，仅在需要对调用方响应、状态转换、限频日志或报警时创建结构化错误实例。

### 3.4 原始码和上下文

- `nativeCode` 不得成为唯一对外错误；
- 同一个错误可以没有原始码，也可以在因果链中保留多个原始诊断；公共错误对象至少暴露最接近失败边界的一个；
- MVS 原始码只能在 Hikrobot 适配器中获取，转换后以 `nativeDomain=hikrobot-mvs` 进入公共错误；
- `details` 使用字段白名单和大小上限，不得放入密码、Token、私钥、完整配置、图像数据、任意服务端响应或未清理的文件内容；
- 绝对路径默认只进入受保护本地日志；IPC/上位机使用逻辑路径或文件 ID；
- 记录序列号时默认仅保留批准的尾号或哈希。

## 4. 错误码登记表

### 4.1 系统与配置

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `SYS_INTERNAL_ERROR` | Error | 否 | 未分类的内部不变量或顶层异常；必须保留模块/操作并限频，不能掩盖为成功 |
| `SYS_SERVICE_START_FAILED` | Critical | 视阶段 | 必需启动阶段失败；按逆序回滚已启动组件 |
| `SYS_SERVICE_STOPPING` | Warning | 是 | 服务已拒绝新的写命令；客户端可在服务重新运行后重试 |
| `SYS_SHUTDOWN_TIMEOUT` | Critical | 否 | 组件未在共享关闭截止时间内停止；记录未完成阶段 |
| `SYS_TIME_JUMP_DETECTED` | Warning | 否 | 墙上时间与单调时间增量显著不一致；继续单调计时并降低时间质量 |
| `SYS_ID_COLLISION` | Critical | 是 | 新生成 ID 命中本地唯一约束；重新生成并记录生成器健康状态 |
| `SYS_CONFIG_INVALID` | Error | 否 | 配置 JSON、schema、类型、范围或依赖校验失败；保持最后有效配置 |
| `SYS_CONFIG_VERSION_CONFLICT` | Warning | 否 | `expectedConfigRevision` 与当前修订不一致；返回当前修订供调用方重新读取 |
| `SYS_CONFIG_APPLY_FAILED` | Error | 视原因 | 组件预应用或回读失败；回滚已修改组件，不提交新修订 |
| `SYS_CONFIG_PERSIST_FAILED` | Critical | 视 I/O | 临时写、刷新或原子替换失败；内存和文件均保持最后已提交版本 |
| `SYS_CONFIG_SCHEMA_UNSUPPORTED` | Error | 否 | 配置 schema 高于支持范围或没有迁移路径 |
| `LOG_INITIALIZATION_FAILED` | Error | 视 I/O | 日志目录创建、日志文件打开或后台运行时初始化失败；保留文件系统原始码和逻辑日志目录，不得静默退化 |
| `LOG_WRITE_FAILED` | Error | 视 I/O | 已初始化日志运行时无法接受、写入或刷新日志；记录受限诊断并进入可观测降级，不得把日志失败升级为业务成功 |

### 4.2 相机

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `CAMERA_NOT_FOUND` | Error | 是 | 绑定的实体相机未枚举到；该路进入断开/恢复，其他路继续 |
| `CAMERA_OPEN_FAILED` | Error | 是 | 设备打开失败且不是更明确的访问拒绝；保留 MVS 原始码 |
| `CAMERA_ACCESS_DENIED` | Error | 是 | 相机被占用或权限不足；不得紧循环重试 |
| `CAMERA_CONFIG_FAILED` | Error | 视参数 | 参数写入或回读不一致；恢复旧快照或进入 Faulted |
| `CAMERA_STREAM_START_FAILED` | Error | 是 | 开始取流失败；关闭本次句柄后按状态机恢复 |
| `CAMERA_DISCONNECTED` | Warning | 是 | 活跃设备掉线；当前路进入 Recovering |
| `CAMERA_FRAME_TIMEOUT` | Warning | 是 | 截止时间内未接收帧；计数并按连续阈值升级报警 |
| `CAMERA_FRAME_INCOMPLETE` | Warning | 否 | 当前帧不完整；丢弃/隔离该帧，不对同一帧重试 |
| `CAMERA_FRAME_FORMAT_CHANGED` | Error | 视配置 | 运行中尺寸或像素格式意外改变；暂停该路并重新校验缓冲预算 |

### 4.3 管线与算法

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `PIPELINE_QUEUE_FULL` | Warning | 否 | 有界通道按既定溢出策略拒绝/丢弃；记录队列名、容量和动作，不阻塞采集 |
| `PIPELINE_BUFFER_POOL_EXHAUSTED` | Error | 否 | 固定帧池无可用缓冲；丢帧、计数并按持续情况报警，禁止无界分配 |
| `PIPELINE_FRAME_ORDER_VIOLATION` | Warning | 否 | 会话序号回退、重复或出现缺口；隔离受影响帧并更新统计 |
| `ALGORITHM_INIT_FAILED` | Error | 视原因 | 新算法实例初始化/验证失败；保持旧实例或禁用检测并报警 |
| `ALGORITHM_PROCESS_FAILED` | Error | 否 | 单帧处理异常；跳过该帧，持续失败时降级算法 |
| `ALGORITHM_DEADLINE_EXCEEDED` | Warning | 否 | 单帧或队列处理超过预算；按策略跳帧/降级，不反压采集 |

### 4.4 事件与存储

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `EVENT_NOT_FOUND` | Error | 否 | 请求的 `EventId` 不存在或不可见 |
| `EVENT_VERSION_CONFLICT` | Warning | 否 | 事件命令的期望版本过期；返回当前版本，不覆盖新状态 |
| `EVENT_INVALID_TRANSITION` | Error | 否 | 命令不符合事件状态机且不是可识别的幂等重复 |
| `EVENT_BUFFER_INCOMPLETE` | Critical | 否 | 前后窗口缺帧或租约不足；保留可用证据并明确标为不完整 |
| `EVENT_WRITE_FAILED` | Critical | 是 | 事件文件、清单或最终提交写入失败；保留临时目录供恢复 |
| `EVENT_CHECKSUM_FAILED` | Critical | 视来源 | 本地写后校验不一致；隔离文件，不提交完整事件 |
| `EVENT_RECOVERY_FAILED` | Critical | 视原因 | 启动时无法恢复/隔离未完成事件；不得伪报已提交 |
| `EVENT_SCHEMA_UNSUPPORTED` | Error | 否 | 事件清单版本超出读取范围；不得部分解析后宣称完整 |
| `STORAGE_LOW_SPACE` | Warning | 是 | 达到预警水位；清理已上传且允许删除的旧事件 |
| `STORAGE_CRITICAL_SPACE` | Critical | 是 | 达到严重水位；停止普通滚动缓存，优先正式事件 |
| `STORAGE_STOP_SAVE` | Critical | 否 | 达到停止保存水位；禁止新大文件并明确事件未保存 |
| `STORAGE_IO_FAILED` | Error | 视 I/O | 非事件特定的文件系统读写/刷新/重命名失败 |
| `STORAGE_CHECKSUM_MISMATCH` | Error | 视来源 | 通用存储对象读回校验失败；隔离对象并触发恢复 |

### 4.5 数据库

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `DATABASE_ERROR` | Error | 视原始码 | 未归入更具体类别的 SQLite 操作失败；保留 SQLite 原始码和操作 |
| `DATABASE_BUSY` | Warning | 是 | 在批准的事务/锁等待截止时间内仍忙；受限退避，不无限等待 |
| `DATABASE_CORRUPT` | Critical | 否 | 完整性检查或 SQLite 报告损坏；停止写入并进入备份/恢复/对账 |
| `DATABASE_MIGRATION_FAILED` | Critical | 否 | schema 迁移失败；回滚事务并阻止以错误 schema 正常运行 |
| `DATABASE_SCHEMA_UNSUPPORTED` | Critical | 否 | 数据库版本高于应用支持范围或迁移链缺失 |
| `DATABASE_RECONCILE_FAILED` | Error | 是 | 事件目录与索引对账未完成；保留文件事实并继续受限恢复 |

### 4.6 IPC

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `IPC_PROTOCOL_ERROR` | Error | 否 | 帧长度、消息类型或通用消息结构非法；拒绝消息并按策略断开 |
| `IPC_PROTOCOL_VERSION_UNSUPPORTED` | Error | 否 | 协议版本不受支持；返回支持范围后限制/关闭会话 |
| `IPC_MESSAGE_TOO_LARGE` | Error | 否 | 长度前缀或负载超过固定上限；在分配大缓冲前拒绝 |
| `IPC_REQUEST_INVALID` | Error | 否 | 命令、字段类型、范围或依赖关系非法 |
| `IPC_REQUEST_CONFLICT` | Warning | 否 | 通用资源版本冲突；事件和配置优先使用各自专用码 |
| `IPC_BUSY` | Warning | 是 | 有界控制队列暂时无法接受请求；调用方按建议延迟重试 |
| `IPC_UNAUTHORIZED` | Error | 否 | 客户端身份或权限不允许该命令 |

### 4.7 上位机连接与上传

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `UPLINK_DISCONNECTED` | Warning | 是 | 上位机连接不可用；上传任务持久排队，本地业务继续 |
| `UPLINK_PROTOCOL_ERROR` | Error | 视协商 | 上位机响应或消息不符合已批准协议；保留受限响应摘要 |
| `UPLINK_AUTH_FAILED` | Error | 否 | 认证/证书拒绝；不得记录凭据，不以紧循环重试 |
| `UPLOAD_ENQUEUE_FAILED` | Critical | 是 | 已提交事件无法建立持久上传任务；事件保持本地并报警 |
| `UPLOAD_TRANSFER_FAILED` | Error | 是 | 受限传输尝试失败；保持 checkpoint 并按策略退避 |
| `UPLOAD_CHECKSUM_MISMATCH` | Error | 是 | 服务端或本地分块校验不一致；重传受影响内容且有上限 |
| `UPLOAD_REJECTED` | Error | 否 | 服务端永久拒绝有效请求；转 `PermanentFailed` 并等待人工处理 |
| `UPLOAD_RETRY_EXHAUSTED` | Error | 否 | 已达到任务次数/时间上限；不再自动重试，保留任务和本地文件 |

## 5. 典型失败映射

| 场景 | 业务码 | 原始诊断 | 对外结果 |
| --- | --- | --- | --- |
| 配置字段类型或依赖非法 | `SYS_CONFIG_INVALID` | JSON 指针和校验规则（脱敏） | 拒绝新配置，旧修订继续生效 |
| MVS 打开相机失败 | `CAMERA_OPEN_FAILED` 或 `CAMERA_ACCESS_DENIED` | `hikrobot-mvs` 原始码 | 该路恢复/故障，其他相机继续 |
| 采集帧队列满 | `PIPELINE_QUEUE_FULL` | 队列名、容量、深度、溢出动作 | 按策略丢弃并计数，不阻塞回调 |
| 事件文件写入失败 | `EVENT_WRITE_FAILED` | `win32` 错误、逻辑文件 ID | 临时目录保留，事件标不完整并报警 |
| SQLite 写事务失败 | `DATABASE_ERROR`/更具体数据库码 | `sqlite` 扩展错误码 | 回滚事务，按类别降级或恢复 |
| IPC 消息版本/结构错误 | `IPC_PROTOCOL_VERSION_UNSUPPORTED` 或 `IPC_PROTOCOL_ERROR` | 收到版本、消息类型和长度 | 返回失败并限制/关闭会话 |
| 上传中断或校验失败 | `UPLOAD_TRANSFER_FAILED` 或 `UPLOAD_CHECKSUM_MISMATCH` | `qt-network`/服务端受限状态 | 保持任务和 checkpoint，受限重试 |

## 6. IPC 失败响应示例

```json
{
  "protocolVersion": 1,
  "messageType": "response",
  "requestId": "019870f2-6c80-7a31-9b52-6e3b9ca1d88f",
  "success": false,
  "error": {
    "businessCode": "SYS_CONFIG_VERSION_CONFLICT",
    "severity": "Warning",
    "message": "配置已被其他客户端更新",
    "module": "config",
    "operation": "config.update",
    "details": {
      "expectedConfigRevision": 41,
      "currentConfigRevision": 42
    },
    "retryable": false,
    "timestamp": "2026-07-30T05:47:00.123Z"
  },
  "payload": {
    "currentConfigRevision": 42
  }
}
```

成功响应不得携带非空错误对象。失败响应不得只返回自然语言或原始码。

## 7. 实现和评审门禁

后续新增错误时必须：

1. 先在本文登记唯一业务码、触发边界和默认处理；
2. 明确是否会成为报警、是否可重试及达到上限后的行为；
3. 为原始码转换、IPC 序列化和敏感字段脱敏新增测试；
4. 验证未知业务码在旧客户端不会被当成成功；
5. 若新增线程/队列故障，另行记录容量、溢出、指标和关闭行为；
6. 若改变公开错误结构，按对应 IPC/上位机协议规则处理版本。
