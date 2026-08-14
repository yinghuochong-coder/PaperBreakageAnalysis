# 配置格式

当前配置格式为 `configSchemaVersion = 7`。服务是 `configRevision` 的唯一分配者；`modifiedAt` 使用 UTC RFC 3339 三位毫秒。配置对象严格拒绝未知字段，完整机器可读约束见 `config/schemas/edge-config-v7.schema.json`，可部署起点见 `config/default-config.json`。v1～v6 合同继续归档；程序读取 v2 时为每路相机补入安全的 Line I/O 默认值，读取 v2/v3 时把缺少的自动曝光模式补为 `Off`，读取 v2～v4 时迁移算法抽样字段，读取 v2～v5 时补入 `rearmDurationMs=500`，并把 v2～v6 归一化为内存 v7；下一次保存统一输出完整 v7。v1 和 v8 及未来版本均拒绝。

配置根对象包含 system、cameras、acquisition、preview、algorithm、event、storage、uplink、plantIo、logging 和 health。最多配置六路相机，逻辑编号限定为 CAM01～CAM06，允许稀疏槽位；启用相机必须具有唯一序列号。默认配置仅把版本提升到 v7，仍保留现有四台现场相机，不虚构 CAM05/CAM06 的序列号或安装位置；新增相机继续通过绑定流程加入。相机数值在 M1 只应用安全结构上限，M3 还必须按真实设备能力回读校验。每路相机的 `reverseX`、`reverseY` 分别控制水平、垂直镜像；旧 v2 配置省略时均按 `false` 处理，序列化保存后会显式写出。

## 自动曝光（schema v4）

每个相机对象必须包含 `autoExposure`，取值为 `Off`、`Once` 或 `Continuous`，分别映射
Hikrobot `ExposureAuto` 的关闭、单次和连续模式。v2/v3 迁移固定补为 `Off`，因此升级不会自行
改变既有固定曝光行为。完整参数事务写入 `exposureUs` 时先把自动曝光关闭、写入曝光基准，再写入
目标 `autoExposure` 并回读；设备未声明支持的模式由相机能力校验拒绝。

## 相机线路 I/O（schema v3）

每个相机对象必须包含完整 `lineIo`：`alarmInputEnabled` 默认 `false`，`alarmActiveLevel` 只能为 `High`/`Low`，`strobeOutputEnabled` 默认 `false`，`strobeDurationUs`、`strobePreDelayUs`、`strobePostDelayUs` 默认均为 `0`。任一路启用都要求相机自身 `enabled=true`；启用频闪时持续时间必须大于 0。保存前还要按设备回报的三项范围和步长校验。`strobePreDelayUs` 映射 `StrobeLinePreDelay`，`strobePostDelayUs` 映射 `StrobeLineDelay`，脉冲完全由设备产生，不使用软件定时。

路径支持 UTF-8 中文和空格。相对路径以主配置所在目录解析，不能用 `..` 逃逸；拒绝 Windows 设备路径。普通 JSON 不得出现 password、token、secret 或 privateKey，外部凭据只能使用 `credentialReference`/`certificateReference`。

## 版本和应用

- 更新命令携带 `expectedConfigRevision`；冲突返回 `SYS_CONFIG_VERSION_CONFLICT`。
- 内容未变化时返回当前修订；成功修改严格递增修订。
- system、相机拓扑、acquisition、存储根目录、NVMe 滚动缓存、日志运行时、uplink 传输和 Plant IO 适配器变更等待重启。
- 相机参数、preview、algorithm、event、存储水位、日志等级/保留和 health 可立即应用。
- 返回值同时包含存储修订、有效修订和 `pendingRestartPaths`。

## 服务启动自动采集

`acquisition.autoStart` 控制服务启动时是否自动处理所有 `enabled=true` 的已配置相机槽位。
启用后，每个槽位依次执行连接、应用保存参数和开始采集；一个槽位失败不会阻止其他槽位或服务
继续启动。`acquisition.startupRetryIntervalMs` 是失败后的重试间隔，默认 1000 ms，范围
1～60000 ms；`acquisition.startupRetryCount` 是首次失败后的额外重试次数，默认 3，范围
0～10，因此默认最多尝试 4 次。重试耗尽后相机保持断开并记录稳定业务错误，操作员仍可通过
IPC 诊断和手动重试。这三个字段均属于 `/acquisition` 待重启配置。

旧配置缺少这三个字段时按 `autoStart=false`、1000 ms 和 3 次迁移，序列化后显式写出；本次
向 schema v4 添加向后兼容的可选字段，不提升 `configSchemaVersion`。

## M5 采集容量与事件配置

- `acquisition.framePoolCapacity` 是每路相机固定原始图像缓冲数，不只是转发队列长度。
  服务启动会按启用相机帧率、`event.preEventSeconds`、`event.postEventSeconds`、采集队列、
  事件队列、预览槽和事件租约计算下限；不足时以稳定配置错误拒绝启动，不在运行中扩容。
  当前可部署配置为单路 60 fps、前后各 10 秒且普通 NVMe 滚动缓存关闭；其启动下限为
  1933 槽（环缓存 601、采集/算法/预览管线 131、单事件租约 1201），默认留有 6 槽余量。
  六路 1624×1240 Mono8 参考下，每路 1939 个槽合计约 21.82 GiB，尚未计对象和运行时开销；
  最终生产容量仍须按实际 ROI 和工控机内存实测校准。
- 默认 `rollingCacheWriteLimitMiBps=600` 低于六路参考滚动写需求 725,037,312 B/s；因为默认缓存关闭且默认仍为四路，不静默提高默认值。六路部署必须依据目标盘实测持续写能力显式配置。
- `event` 完整对象包含 `preEventSeconds`、`postEventSeconds`、`maxEventSeconds`、
  `mergeGapSeconds`、`keyFrameCount`、`saveRaw`、`generatePreviewVideo`、`uploadPolicy` 和
  `retentionDays`。事件页更新必须提交完整对象并携带 `expectedConfigRevision`。
- M5 会热应用窗口、合并、关键帧、原始保存和保留天数配置；
  `generatePreviewVideo` 仅保存配置，生成器尚未实现；`uploadPolicy` 仅保存将来策略，上传
  执行器和重试在 M8 前明确不可用。
- `storage.eventRoot` 下的 `.metadata/events.db` 和 `.metadata/backups/` 是服务内部派生路径，
  不新增公开配置字段。事件根、SQLite、事务目录和维护清理都以主配置目录解析后的同一
  绝对根为准。

## M6 算法配置

- `algorithm` 是严格完整对象，包含 `enabled`、`type`、`roi`、`candidateThreshold`、
  `confirmationThreshold`、`downsampleMode`、`processingFps`、`confirmationDurationMs`、
  `cooldownMs`、`rearmDurationMs`、`modelReference`、`modelVersion`、`device` 和 `debugOverlay`；未知字段会被拒绝。
- `downsampleMode` 只能为 `disabled`、`half` 或 `quarter`；`processingFps` 只能为 15、30 或
  60；`confirmationDurationMs` 范围为 10～60000 ms，且不得大于 `event.maxEventSeconds`
  对应的候选超时时间。两级阈值范围均为 0～1，且确认阈值不得低于候选阈值；冷却时间为
  0～3,600,000 ms；`rearmDurationMs` 同样为 0～3,600,000 ms，默认 500，0 表示第一条严格
  正常结果即可满足稳定时长但仍须满足冷却。模型引用、模型版本和设备名分别限制为 512、128 和 64 字节。
- 新安装默认 `half + 15 FPS + 120 ms`。读取 v2～v4 时固定迁移为
  `disabled + 60 FPS`，并把旧 `consecutiveFrames` 按 60 FPS 换算后向上取整到 10 ms；例如
  7 帧迁移为 120 ms。这样升级不会直接改变旧配置的分析尺寸或处理节拍。
- 算法配置可立即应用。控制台同时显示保存配置、有效配置、运行时实际修订、检测器实现/模型
  版本和降级状态，不得以“已保存”替代“已应用”。更新失败沿用配置仓储事务回滚，保留旧配置
  与旧检测器实例。
- `modelReference`、`modelVersion` 和 `device` 在 M6-00 通过前仅是配置与可观测信息；当前
  `mock` 和 `classical-vision` 检测器均固定标记为原型，不代表模型制品已校验或正式算法已验收。
- `debugOverlay` 控制 Qt 单帧测试图上的 ROI/检测结果叠加。单帧测试结果和 JPEG 只存在于
  运行时，不写入配置或事件目录。

## M7 NVMe 滚动缓存

M7-02 将配置升级为 schema v2，并加入实际运行时消费者：

- `storage.rollingCacheEnabled`：普通 NVMe 滚动缓存开关；
- `storage.maximumCacheStorageGiB`：当前服务 session 内已提交块和单个在写临时块的总物理容量
  上限；旧 session 不计入该值但仍占用卷空间；
- `storage.rollingCacheWriteLimitMiBps`：普通滚动写限速，既不能低于完整原始输入需求，也不能
  高于目标卷实测持续写带宽的 80%。
- `storage.rollingCacheIoTimeoutMs`：单个块从开始写入到完成提交的总截止时间，范围 100～600000 ms。

NVMe v2 块时长固定为 1000 ms，不作为可任意修改的配置字段。块上限必须从服务已校验并回读
的相机数、最大帧率、stride、height 和像素格式计算；`warningFreeSpaceGiB`、
`criticalFreeSpaceGiB`、`stopFreeSpaceGiB` 与最大缓存容量同时生效，取更严格的准入结果。
完整格式和计算公式见 `docs/architecture/decisions/adr-011-nvme-rolling-cache-format-capacity.md`。
四个字段均需重启应用；水位字段仍可热应用。可部署配置当前启用 `rollingCacheEnabled`；在生产
ROI、stride 和目标 NVMe 持续写能力未验收前，不应直接作为生产参数。启用时，每相机当前组装
块、两个排队块和写线程当前块共最多四块的共享帧引用会计入
`acquisition.framePoolCapacity` 启动门禁。

ADR-017 取消滚动缓存启动恢复配置与固定扫描上限。每次启动创建
`<cacheRoot>/sessions/<session-id>`，不枚举、不读取、不删除旧 session 或旧版根目录块。系统不
自动清理这些数据；运维只能在服务停止后人工清理。卷级 warning/critical/stop 水位包含所有
真实占用，继续防止旧 session 耗尽磁盘。

## M8 Uplink v1 配置

- `uplink.enabled=true` 时，`serverUrl` 必须是明文 `http://` 基址；WebSocket 地址由会话响应给出的 `ws://` URL 决定。Uplink v1 不支持 TLS 或应用鉴权，因此 `credentialReference` 与 `certificateReference` 必须为空；部署网络必须隔离。
- `chunkBytes` 范围为 65536～4194304 字节，默认 1048576；除最后一块外，所有上传块必须精确使用该大小。
- `ioTimeoutMs` 是单次 REST 或 WebSocket 确认等待上限，范围 100～60000 ms，默认 10000 ms；停止或断开还会主动取消当前等待。
- `uploadLimitMiBps` 是单上传工作线程预校验读盘和网络分块阶段的非零速率上限，范围 1～1024 MiB/s，默认 20 MiB/s。上传不在相机采集回调中执行。
- uplink 传输字段变化记录为 `/uplink/transport` 待重启项；配置 schema、解析器、序列化器和默认配置使用相同字段集。

## 日志配置

服务进程把通过等级过滤和脱敏后的同一条异步日志同时写到标准输出及按线程滚动文件；
控制台写入由有界日志工作线程执行，不阻塞相机采集线程。

本变更不提升 `configSchemaVersion`。`logging.level` 与 `logging.retentionDays` 属于
`/logging/live`，配置事务按 prepare、apply/readback、commit 和 rollback 热应用；应用失败时恢复
上一个有效等级和保留天数。`logging.directory`、`queueCapacity`、`maximumFileSizeMiB` 和
`maximumFilesPerDay` 仍进入 `/logging/runtime` 待重启路径。

默认等级为 `info`；切换为 `debug` 后逐帧诊断立即生效，恢复较高等级后不再构造逐帧文本。
`retentionDays` 对服务和控制台各自的新命名日志生效，只清理能严格匹配
`paperbreak-service-<thread>-YYYY-MM-DD.log[.N]` 或
`paperbreak-console-<thread>-YYYY-MM-DD.log[.N]` 且超过保留期的文件；旧聚合日志和无关文件不迁移、
不改名且不被该清理规则删除。`maximumFilesPerDay` 对每个线程独立计数。

## 原子保存和恢复

服务在目标目录创建唯一临时文件，完整写入并刷新后使用 Windows 原子替换。最近 5 份有效配置保存在 `<配置文件名>.history/`，文件名为 20 位修订号。主配置损坏时只从可完整验证的最新历史恢复；残留临时文件不会被采用。选择历史回滚时会创建新的修订，不会复用历史修订号。

schema v7 不向旧程序提供降写兼容。若必须回滚到只支持 v6 的程序，须先停止服务，从自动配置历史恢复最后一份 v6 文件，并移除 CAM05/CAM06；不得只修改版本号或把 v7 文件交给旧程序读取。事件、数据库和 NVMe 数据的持久格式未改变，不应在回滚时删除。
