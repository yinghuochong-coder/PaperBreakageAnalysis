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
| `SYS_BUSY` | Warning | 是 | 普通控制通道达到固定容量；拒绝最新请求，停止等保留控制不依赖普通队列空位 |
| `SYS_DIAGNOSTIC_TOO_LARGE` | Error | 否 | 脱敏诊断 ZIP 的条目、条目数或总大小超过 8 MiB 内部上限；拒绝返回部分包 |
| `SYS_DIAGNOSTIC_EXPORT_FAILED` | Error | 视 I/O | Qt 客户端无法创建、写入或原子提交用户选择的诊断/报警导出文件；现有目标不被部分覆盖 |
| `SYS_SERVICE_START_FAILED` | Critical | 视阶段 | 必需启动阶段失败；按逆序回滚已启动组件 |
| `SYS_SERVICE_INSTALL_FAILED` | Error | 否 | SCM 注册或配置收敛失败；保留 Win32 原始码，新建服务的扩展配置失败时尝试删除回滚 |
| `SYS_SERVICE_UNINSTALL_FAILED` | Error | 视服务状态 | 服务停止、等待或删除失败；停止超时保留服务注册，不谎报卸载成功 |
| `SYS_SERVICE_RESTART_FAILED` | Error | 否 | 用户请求的 SCM 重启未能在单次总截止时间内完成，或服务不存在、权限不足、停止/启动失败；保留 Win32 原始码，不自动循环重试 |
| `SYS_SERVICE_RESTART_CANCELLED` | Warning | 否 | Qt 客户端退出时取消尚未完成的重启等待；不再启动新的 SCM 操作，保留服务实际状态 |
| `SYS_SERVICE_CONTROL_FAILED` | Critical | 视操作 | SCM 调度、控制回调注册或状态上报失败；服务进入受控停止并保留 Win32 原始码 |
| `SYS_SERVICE_STOPPING` | Warning | 是 | 服务已拒绝新的写命令；客户端可在服务重新运行后重试 |
| `SYS_NOT_SUPPORTED` | Error | 否 | 已知操作或能力没有由当前组件/会话声明；拒绝且不进入业务 dispatcher，不用厂商“不支持”码替代 |
| `SYS_SHUTDOWN_TIMEOUT` | Critical | 否 | 组件未在共享关闭截止时间内停止；记录未完成阶段 |
| `SYS_TIME_JUMP_DETECTED` | Warning | 否 | 墙上时间与单调时间增量显著不一致；继续单调计时并降低时间质量 |
| `TIME_PROBE_UNAVAILABLE` | Warning | 是 | 当前系统或相机时间探针不支持、超时或暂不可用；按固定优先级降级并保留适配器原始诊断 |
| `TIME_MODEL_INVALID` | Error | 否 | 探针样本、频率、锚点、修订或 checked arithmetic 不能形成有效模型；不发布半成品模型 |
| `TIME_MAPPING_UNAVAILABLE` | Error | 是 | 没有覆盖目标 T0/ticks 的已发布模型或映射溢出；保留原始/接收时间并明确标为未同步 |
| `TIME_SYNC_DEGRADED` | Warning | 是 | 当前来源或不确定度只能满足降级语义；不得报告硬件同步或未经实测的精度 |
| `SYS_ID_COLLISION` | Critical | 是 | 新生成 ID 命中本地唯一约束；重新生成并记录生成器健康状态 |
| `SYS_ID_GENERATION_FAILED` | Critical | 是 | 本地 UUIDv7 时间范围或系统熵源不可用；拒绝创建无可靠身份的事件并报警 |
| `SYS_CONFIG_INVALID` | Error | 否 | 配置 JSON、schema、类型、范围或依赖校验失败；保持最后有效配置 |
| `SYS_CONFIG_VERSION_CONFLICT` | Warning | 否 | `expectedConfigRevision` 与当前修订不一致；返回当前修订供调用方重新读取 |
| `SYS_CONFIG_APPLY_FAILED` | Error | 视原因 | 组件预应用或回读失败；回滚已修改组件，不提交新修订 |
| `SYS_CONFIG_PERSIST_FAILED` | Critical | 视 I/O | 临时写、刷新或原子替换失败；内存和文件均保持最后已提交版本 |
| `SYS_CONFIG_SCHEMA_UNSUPPORTED` | Error | 否 | 配置 schema 高于支持范围或没有迁移路径 |
| `LOG_INITIALIZATION_FAILED` | Error | 视 I/O | 日志目录创建、日志文件打开或后台运行时初始化失败；保留文件系统原始码和逻辑日志目录，不得静默退化 |
| `LOG_WRITE_FAILED` | Error | 视 I/O | 已初始化日志运行时无法接受、写入或刷新日志；记录受限诊断并进入可观测降级，不得把日志失败升级为业务成功 |

### 4.2 监测与报警

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `SYS_MONITORING_SAMPLE_FAILED` | Warning | 是 | 一个健康指标源采样失败；保留其他来源快照，按来源合并报警并在恢复后清除 |
| `SYS_CPU_USAGE_HIGH` | Warning | 是 | 进程 CPU 使用率跨越配置阈值；仅在状态跨越时 raise/clear |
| `SYS_MEMORY_USAGE_HIGH` | Warning | 是 | 系统内存使用率跨越配置阈值；仅在状态跨越时 raise/clear |
| `MONITORING_RECORD_INVALID` | Error | 否 | 指标、报警、详情或监测配置字段违反类型、字符或长度边界；拒绝该次变更并保留旧快照 |
| `MONITORING_CAPACITY_EXCEEDED` | Error | 否 | 指标、指标源或活动报警达到固定容量；拒绝新增项，活动报警不得淘汰 |
| `ALARM_NOT_FOUND` | Error | 否 | 确认的 alarmId 不存在或已经从进程内历史淘汰；调用方应重新查询报警事实源 |

### 4.3 相机

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `CAMERA_NOT_FOUND` | Error | 是 | 绑定的实体相机未枚举到；该路进入断开/恢复，其他路继续 |
| `CAMERA_OPEN_FAILED` | Error | 是 | 设备打开失败且不是更明确的访问拒绝；保留 MVS 原始码 |
| `CAMERA_ACCESS_DENIED` | Error | 是 | 相机被占用、不可独占访问或权限不足；绑定不得修改配置，且不得紧循环重试 |
| `CAMERA_CONFIG_FAILED` | Error | 否 | 参数不受设备能力支持、范围/步进/组合非法、发现重复序列号，或绑定槽位/序列号/目标型号冲突；可在触达 SDK 前判断的请求必须提前拒绝 |
| `CAMERA_PARAMETER_READ_FAILED` | Error | 是 | 参数能力或当前值读取失败；保留 MVS 原始码，不把不完整快照报告为成功 |
| `CAMERA_PARAMETER_WRITE_FAILED` | Error | 是 | 参数写入或写后回读失败；尝试恢复旧快照并恢复原采集状态 |
| `CAMERA_PARAMETER_FAULTED` | Critical | 否 | 参数事务无法恢复旧快照或原采集状态；锁定当前连接会话的参数操作，要求断开重连 |
| `CAMERA_STREAM_START_FAILED` | Error | 是 | 开始取流失败；关闭本次句柄后按状态机恢复 |
| `CAMERA_DISCONNECTED` | Warning | 是 | 活跃设备掉线或 MVS 取流返回链路/设备错误；保留 `hikrobot-mvs` 原始码、阶段和恢复次数，当前路清理句柄后进入 Recovering |
| `CAMERA_FRAME_TIMEOUT` | Warning | 是 | 截止时间内未接收帧；单次只计数，连续达到会话阈值后携带超时次数/阈值并进入有界恢复 |
| `CAMERA_FRAME_INCOMPLETE` | Warning | 否 | 当前帧不完整；丢弃/隔离该帧，不对同一帧重试 |
| `CAMERA_FRAME_FORMAT_CHANGED` | Error | 视配置 | 运行中尺寸、步长、有效载荷或像素格式意外改变；停止该路且不在线扩容，重新校验固定缓冲预算后方可恢复 |
| `CAMERA_INVALID_STATE_TRANSITION` | Error | 否 | 相机会话请求了状态表不允许的转换；拒绝转换并记录相机、源状态、目标状态和原因 |

### 4.4 管线与算法

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `PIPELINE_QUEUE_FULL` | Warning | 否 | 有界通道按既定溢出策略拒绝/丢弃；记录队列名、容量和动作，不阻塞采集 |
| `PIPELINE_BUFFER_POOL_EXHAUSTED` | Error | 否 | 固定帧池无可用缓冲；丢帧、计数并按持续情况报警，禁止无界分配 |
| `PIPELINE_FRAME_ORDER_VIOLATION` | Warning | 否 | 会话序号回退、重复或出现缺口；隔离受影响帧并更新统计 |
| `PIPELINE_PREVIEW_ENCODE_FAILED` | Warning | 是 | 预览 JPEG 编码、缩放或二进制大小校验失败；丢弃该预览帧并继续后续帧，不影响采集 |
| `PIPELINE_PREVIEW_INVALID_STATE` | Warning | 否 | 预览运行时重复启动或处于不允许的生命周期状态 |
| `PIPELINE_PREVIEW_START_FAILED` | Error | 是 | 无法创建预览编码工作线程；服务保持无预览降级，不启动无界替代线程 |
| `ALGORITHM_INIT_FAILED` | Error | 视原因 | 新算法实例初始化/验证失败；保持旧实例或禁用检测并报警 |
| `ALGORITHM_PLUGIN_LOAD_FAILED` | Error | 否 | 编译期插件未注册、工厂失败或插件身份无效；候选配置不提交，旧实例和旧配置继续生效 |
| `ALGORITHM_NOT_READY` | Error | 视状态 | 检测器尚未装载或已停止接收；不调用空实例，不影响采集与人工触发 |
| `ALGORITHM_PROCESS_FAILED` | Error | 否 | 单帧处理异常时跳过该帧；连续失败达到门限后按相机持续更新活动报警，但后续帧仍继续检测，首次成功后清除活动报警 |
| `ALGORITHM_PROCESS_TIMEOUT` | Error | 否 | 同步检测调用返回后确认超过软件预算；丢弃迟到结果并计数，不声称已安全抢占调用 |
| `ALGORITHM_QUEUE_BACKLOG` | Warning | 是 | 每相机容量 8 的待检测队列已满；丢弃最旧待检测帧、接收最新帧并计数，持续积压时降级 |
| `ALGORITHM_DEGRADED` | Error | 否 | 持续队列积压或算法结果队列拒绝达到保护条件；来源相机的自动视觉检测切换到 `manual-trigger-only`，采集、缓存和人工触发继续 |

### 4.5 事件与存储

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `EVENT_NOT_FOUND` | Error | 否 | 请求的 `EventId` 不存在或不可见 |
| `EVENT_VERSION_CONFLICT` | Warning | 否 | 事件命令的期望版本过期；返回当前版本，不覆盖新状态 |
| `EVENT_INVALID_TRANSITION` | Error | 否 | 命令不符合事件状态机且不是可识别的幂等重复 |
| `EVENT_LOCK_CONFLICT` | Critical | 否 | 相同全局 eventId 携带不同 T0、触发节点/相机/来源或窗口；保留首次持久结果并拒绝覆盖 |
| `EVENT_LOCK_OUT_OF_RANGE` | Error | 否 | T0 过度超前或早于全部内存/NVMe 可用范围；返回 FAILED 和实际可用范围，不创建虚假完整事件 |
| `EVENT_LOCK_CAPACITY_FULL` | Critical | 是 | 活动事件/租约容量不足以接受新的外部 T0；拒绝最新请求，禁止 drop-oldest 或阻塞采集 |
| `EVENT_BUFFER_INCOMPLETE` | Critical | 否 | 前后窗口缺帧或租约不足；保留可用证据并明确标为不完整 |
| `EVENT_KEYFRAME_SELECTION_FAILED` | Error | 否 | 冻结窗口、逐帧证据、确认帧引用或分数非法；拒绝本次选择并保留完整原始事件序列 |
| `EVENT_KEYFRAME_QUEUE_FULL` | Error | 否 | 固定关键帧任务队列容量不足、内存预算不足或已经停止接收；事件内任务整批拒绝并标记关键帧不完整，原始事件仍优先 |
| `EVENT_KEYFRAME_ENCODE_FAILED` | Error | 是 | 关键帧像素布局、输入/输出上限或 OpenCV JPEG 编码失败；当前关键帧标损，工作线程继续处理后续已接受任务 |
| `EVENT_WRITE_FAILED` | Critical | 是 | 事件文件、清单或最终提交写入失败；保留临时目录供恢复 |
| `EVENT_WRITE_CANCELLED` | Warning | 是 | 持久化收到停止令牌；保留事务目录且不发布正式事件目录 |
| `EVENT_CHECKSUM_FAILED` | Critical | 否 | 写入器返回的实际长度或 CNG SHA-256 结果无效；保留事务目录，不提交事件 |
| `EVENT_INTEGRITY_FAILED` | Critical | 否 | 按需详情、导出或上传完整校验失败；文件保留，事件标损并禁止制品继续流出，上传转人工处理 |
| `EVENT_RECOVERY_FAILED` | Critical | 视原因 | 启动事务的 manifest、路径、存在性或长度结构检查失败；隔离事务且不读取原始负载 |
| `EVENT_SCHEMA_UNSUPPORTED` | Critical | 否 | 检测到非空旧事件库、schema v1/未知 manifest 或超出读取范围；拒绝启动且不移动、删除或部分解析旧数据 |
| `EVENT_NOT_COMMITTED` | Warning | 是 | 事件证据尚未提交；manifest、缩略图、目录、导出、上传或人工复核暂不可用 |
| `EVENT_QUEUE_FULL` | Critical | 是 | 待编码/持久化事件达到固定上限；拒绝新事件，不扩容或反压采集 |
| `EVENT_EXPORT_TOO_LARGE` | Error | 否 | 已校验事件 ZIP64 超过显式 64 GiB、单文件或文件数上限；不返回截断包或成功标志 |
| `EVENT_EXPORT_FAILED` | Error | 视 I/O | Qt 客户端无法原子写入事件 ZIP；`QSaveFile` 放弃临时文件且不覆盖既有目标 |
| `STORAGE_LOW_SPACE` | Warning | 是 | 达到预警水位；清理已上传且允许删除的旧事件 |
| `STORAGE_CRITICAL_SPACE` | Critical | 是 | 达到严重水位；停止普通滚动缓存，优先正式事件 |
| `STORAGE_STOP_SAVE` | Critical | 否 | 达到停止保存水位；禁止新大文件并明确事件未保存 |
| `STORAGE_IO_FAILED` | Error | 视 I/O | 非事件特定的文件系统读写/刷新/重命名失败 |
| `STORAGE_CHECKSUM_MISMATCH` | Error | 视来源 | 通用存储对象读回校验失败；隔离对象并触发恢复 |
| `NVME_QUEUE_FULL` | Warning | 是 | 每相机两个待写块已满；拒绝当前完整普通块、记录缺口，不阻塞采集 |
| `NVME_BLOCK_INVALID` | Error | 否 | 帧序号、相机、布局、负载或块边界违反 NVMe v2 声明；当前普通块不提交 |
| `NVME_FORMAT_UNSUPPORTED` | Error | 否 | PBNVME magic 或版本不在显式支持范围；读取任何声明负载前拒绝，不猜测或降级解析 |
| `NVME_BLOCK_INCOMPLETE` | Error | 否 | 文件短于声明固定布局、尾页或完整提交标记缺失；不作为已提交块使用并保留证据 |
| `NVME_BLOCK_CORRUPT` | Critical | 否 | PBNVME3 结构、保留位、范围、CRC32C 或尾页回显不一致；隔离/拒绝且不返回部分完整结果 |
| `NVME_CACHE_UNAVAILABLE` | Error | 是 | 当前 session 缓存根或容量准入不可安全使用；降级为内存缓存 |
| `NVME_CACHE_PROTECTED` | Error | 是 | 固定容量内没有零租约已提交块可回收；不得覆盖事件证据，普通滚动缓存降级并记录缺口 |
| `NVME_WRITE_TIMEOUT` | Error | 是 | 单块写入未在配置总截止时间内完成；保留临时尾块并降级内存 |
| `NVME_WRITE_FAILED` | Error | 视 I/O | 创建、普通缓冲写、原子发布或当前 session 回绕删除失败；保留原生诊断并降级内存 |
| `NVME_INDEX_FAILED` | Error | 视 SQLite/I/O | 可重建派生块/租约索引无法打开、校验、登记、查询或事务提交；保留块文件事实并降级，不把未登记块视为可回收 |
| `NVME_INDEX_QUERY_LIMIT` | Warning | 是 | 时间窗命中块数超过调用方或 4096 块固定上限；拒绝返回截断结果，不伪称序列完整 |
| `NVME_LEASE_CAPACITY` | Error | 是 | 活动事件租约达到 64 条固定上限；拒绝新 NVMe 租约但继续内存事件链并报警 |

### 4.6 数据库

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `DATABASE_ERROR` | Error | 视原始码 | 未归入更具体类别的 SQLite 操作失败；保留 SQLite 原始码和操作 |
| `DATABASE_BUSY` | Warning | 是 | 在批准的事务/锁等待截止时间内仍忙；受限退避，不无限等待 |
| `DATABASE_CORRUPT` | Critical | 否 | 完整性检查或 SQLite 报告损坏；停止写入并进入备份/恢复/对账 |
| `DATABASE_MIGRATION_FAILED` | Critical | 否 | schema 迁移失败；回滚事务并阻止以错误 schema 正常运行 |
| `DATABASE_SCHEMA_UNSUPPORTED` | Critical | 否 | 数据库版本高于应用支持范围或迁移链缺失 |
| `DATABASE_RECONCILE_FAILED` | Error | 是 | 事件目录与索引对账未完成；保留文件事实并继续受限恢复 |

### 4.7 IPC

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `IPC_PROTOCOL_ERROR` | Error | 否 | 帧长度、消息类型或通用消息结构非法；拒绝消息并按策略断开 |
| `IPC_PROTOCOL_VERSION_UNSUPPORTED` | Error | 否 | 协议版本不受支持；返回支持范围后限制/关闭会话 |
| `IPC_MESSAGE_TOO_LARGE` | Error | 否 | 长度前缀或负载超过固定上限；在分配大缓冲前拒绝 |
| `IPC_REQUEST_INVALID` | Error | 否 | 命令、字段类型、范围或依赖关系非法 |
| `IPC_REQUEST_CONFLICT` | Warning | 否 | 通用资源版本冲突；事件和配置优先使用各自专用码 |
| `IPC_BUSY` | Warning | 是 | 有界控制队列暂时无法接受请求；调用方按建议延迟重试 |
| `IPC_UNAUTHORIZED` | Error | 否 | 客户端身份或权限不允许该命令 |
| `IPC_NOT_CONNECTED` | Warning | 是 | 客户端当前未连接；不缓存请求，调用方可在新连接后重新发起 |
| `IPC_CONNECTION_LOST` | Warning | 是 | 已接受请求因本机连接中断而失败；不得自动重放可能有副作用的请求 |
| `IPC_REQUEST_TIMEOUT` | Warning | 是 | 客户端请求超过单调时钟截止时间；移出在途表并忽略迟到响应 |
| `IPC_REQUEST_CANCELLED` | Info | 否 | 调用方取消请求或客户端停止；请求只完成一次 |

### 4.8 上位机连接与上传

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `UPLINK_DISCONNECTED` | Warning | 是 | 上位机连接不可用；上传任务持久排队，本地业务继续 |
| `UPLINK_TIMEOUT` | Warning | 是 | 单次 HTTP/WebSocket I/O 超过配置截止时间；取消当前等待并退避 |
| `UPLINK_PROTOCOL_ERROR` | Error | 视协商 | 上位机响应或消息不符合已批准协议；保留受限响应摘要 |
| `UPLINK_PROTOCOL_VERSION_UNSUPPORTED` | Error | 否 | 对端没有共同的 Uplink 协议版本；拒绝建立会话并报告双方版本 |
| `UPLINK_SERVER_BUSY` | Warning | 是 | 设备、活动上传、存储任务或设备命令队列达到有界容量；按服务端提示退避 |
| `UPLINK_AUTH_FAILED` | Error | 否 | 认证/证书拒绝；不得记录凭据，不以紧循环重试 |
| `UPLINK_COMMAND_NOT_CONFIRMED` | Error | 否 | 除状态查询和自动 `event.lockByUtc` 外的远程命令缺少 `operatorConfirmed=true`；不进入服务命令 dispatcher |
| `UPLINK_COMMAND_EXPIRED` | Warning | 否 | 远程命令的 RFC 3339 截止时间已过；缓存并返回拒绝结果，不执行副作用 |
| `UPLINK_COMMAND_CONFLICT` | Error | 否 | 相同 `commandId` 携带不同类型、正文、截止时间或确认状态；保留原结果并拒绝冲突内容 |
| `STATUS_PAYLOAD_TOO_LARGE` | Error | 否 | 完整状态序列化超过 1 MiB；拒绝整条消息并报警，不发送截断 JSON |
| `PREVIEW_FRAME_TOO_LARGE` | Warning | 否 | 远程预览头超过 64 KiB 或 JPEG 超过 2 MiB；只丢当前预览并计数，不反压采集或可靠上传 |
| `UPLOAD_ENQUEUE_FAILED` | Critical | 是 | 已提交事件无法建立持久上传任务；事件保持本地并报警 |
| `UPLOAD_JOB_CONFLICT` | Error | 否 | 相同上传幂等键携带不同事件、资源、路径、摘要或负载；保留原任务并拒绝覆盖 |
| `UPLOAD_JOB_INVALID` | Error | 否 | 持久任务缺少事件、逻辑文件、受限相对路径或 manifest 字段；拒绝执行并保留诊断 |
| `UPLOAD_SOURCE_MISSING` | Error | 否 | 声明的本地普通文件或 manifest 不存在；不创建空上传，等待人工处理 |
| `UPLOAD_SOURCE_CHANGED` | Critical | 否 | 在线单遍读取发现本地文件长度或整文件 SHA-256 与 manifest 不一致；不调用完成接口，保留文件/checkpoint，事件标损并转人工处理 |
| `UPLOAD_TRANSFER_INTERRUPTED` | Warning | 是 | 服务或执行器停止时任务仍在途；保留 checkpoint 并在重启后恢复领取 |
| `UPLOAD_TRANSFER_FAILED` | Error | 是 | 受限传输尝试失败；保持 checkpoint 并按策略退避 |
| `UPLOAD_CHECKSUM_MISMATCH` | Error | 是 | 服务端或本地分块校验不一致；重传受影响内容且有上限 |
| `UPLOAD_REJECTED` | Error | 否 | 服务端永久拒绝有效请求；转 `PermanentFailed` 并等待人工处理 |
| `UPLOAD_RETRY_EXHAUSTED` | Error | 否 | 已达到任务次数/时间上限；不再自动重试，保留任务和本地文件 |

### 4.9 硬件验收工具

这些码仅用于目标机硬件验收工具和审计记录，不进入生产服务业务流。

| 业务码 | 默认级别 | 默认可重试 | 触发条件和处理语义 |
| --- | --- | ---: | --- |
| `HW_PLAN_INVALID` | Error | 否 | 测试计划缺失、schema/字段非法或时长、采样、队列、池、缓冲预算超过硬上限；在枚举、打开或写相机前拒绝 |
| `HW_RECORD_WRITE_FAILED` | Error | 否 | 无法创建或原子提交审计记录，或目标记录已存在；拒绝覆盖既有证据，人工修正输出位置后以新记录 ID 重试 |

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
