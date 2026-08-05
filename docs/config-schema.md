# 配置格式

当前配置格式为 `configSchemaVersion = 2`。服务是 `configRevision` 的唯一分配者；`modifiedAt` 使用 UTC RFC 3339 三位毫秒。配置对象严格拒绝未知字段，完整机器可读约束见 `config/schemas/edge-config-v2.schema.json`，可部署起点见 `config/default-config.json`。v1 合同继续归档，但当前程序不静默迁移 v1；部署升级必须先生成并验证完整 v2 配置。

配置根对象包含 system、cameras、acquisition、preview、algorithm、event、storage、uplink、plantIo、logging 和 health。最多配置四路相机，逻辑编号限定为 CAM01～CAM04；启用相机必须具有唯一序列号。相机数值在 M1 只应用安全结构上限，M3 还必须按真实设备能力回读校验。

路径支持 UTF-8 中文和空格。相对路径以主配置所在目录解析，不能用 `..` 逃逸；拒绝 Windows 设备路径。普通 JSON 不得出现 password、token、secret 或 privateKey，外部凭据只能使用 `credentialReference`/`certificateReference`。

## 版本和应用

- 更新命令携带 `expectedConfigRevision`；冲突返回 `SYS_CONFIG_VERSION_CONFLICT`。
- 内容未变化时返回当前修订；成功修改严格递增修订。
- system、相机拓扑、acquisition、存储根目录、NVMe 滚动缓存、日志运行时、uplink 传输和 Plant IO 适配器变更等待重启。
- 相机参数、preview、algorithm、event、存储水位、日志等级/保留和 health 可立即应用。
- 返回值同时包含存储修订、有效修订和 `pendingRestartPaths`。

## M5 采集容量与事件配置

- `acquisition.framePoolCapacity` 是每路相机固定原始图像缓冲数，不只是转发队列长度。
  服务启动会按启用相机帧率、`event.preEventSeconds`、`event.postEventSeconds`、采集队列、
  事件队列、预览槽和事件租约计算下限；不足时以稳定配置错误拒绝启动，不在运行中扩容。
  默认单路 60 fps、前后各 10 秒的配置使用 2048 槽，最终四路生产容量仍须按实际 ROI 和
  工控机内存实测校准。
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
  `confirmationThreshold`、`consecutiveFrames`、`cooldownMs`、`modelReference`、
  `modelVersion`、`device` 和 `debugOverlay`；未知字段会被拒绝。
- 两级阈值范围均为 0～1，且确认阈值不得低于候选阈值；连续帧为 1～1000，冷却时间为
  0～3,600,000 ms。模型引用、模型版本和设备名分别限制为 512、128 和 64 字节。
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
- `storage.maximumCacheStorageGiB`：已提交块和单个在写临时块的总物理容量上限；
- `storage.rollingCacheWriteLimitMiBps`：普通滚动写限速，既不能低于完整原始输入需求，也不能
  高于目标卷实测持续写带宽的 80%。
- `storage.rollingCacheIoTimeoutMs`：单个块从开始写入到完成提交的总截止时间，范围 100～600000 ms。

NVMe v1 块时长固定为 1000 ms，不作为可任意修改的配置字段。块上限必须从服务已校验并回读
的相机数、最大帧率、stride、height 和像素格式计算；`warningFreeSpaceGiB`、
`criticalFreeSpaceGiB`、`stopFreeSpaceGiB` 与最大缓存容量同时生效，取更严格的准入结果。
完整格式和计算公式见 `docs/architecture/decisions/adr-011-nvme-rolling-cache-format-capacity.md`。
四个字段均需重启应用；水位字段仍可热应用。默认 `rollingCacheEnabled=false`，在生产 ROI、
stride 和目标 NVMe 持续写能力未验收前不自动启用。启用时，每相机当前组装块、两个排队块和
写线程当前块共最多四块的共享帧引用会计入 `acquisition.framePoolCapacity` 启动门禁。

M7-04 启动恢复不新增 schema v2 配置字段，采用固定安全上限：最多扫描 100000 个
`.pbnvme`/`.partial` 候选、恢复摘要最多 64 MiB、总截止时间 5 分钟，并用固定 1 MiB 缓冲流式
校验负载。达到任一上限时普通滚动缓存显式降级，事件内存缓存继续运行；不得通过放宽为无界
扫描来绕过门禁。隔离文件保存在 `<cacheRoot>/.quarantine/`，不计入可回绕正常块，需运维审查。

## M8 Uplink v1 配置

- `uplink.enabled=true` 时，`serverUrl` 必须是明文 `http://` 基址；WebSocket 地址由会话响应给出的 `ws://` URL 决定。Uplink v1 不支持 TLS 或应用鉴权，因此 `credentialReference` 与 `certificateReference` 必须为空；部署网络必须隔离。
- `chunkBytes` 范围为 65536～4194304 字节，默认 1048576；除最后一块外，所有上传块必须精确使用该大小。
- `ioTimeoutMs` 是单次 REST 或 WebSocket 确认等待上限，范围 100～60000 ms，默认 10000 ms；停止或断开还会主动取消当前等待。
- `uploadLimitMiBps` 是单上传工作线程预校验读盘和网络分块阶段的非零速率上限，范围 1～1024 MiB/s，默认 20 MiB/s。上传不在相机采集回调中执行。
- uplink 传输字段变化记录为 `/uplink/transport` 待重启项；配置 schema、解析器、序列化器和默认配置使用相同字段集。

## 原子保存和恢复

服务在目标目录创建唯一临时文件，完整写入并刷新后使用 Windows 原子替换。最近 5 份有效配置保存在 `<配置文件名>.history/`，文件名为 20 位修订号。主配置损坏时只从可完整验证的最新历史恢复；残留临时文件不会被采用。选择历史回滚时会创建新的修订，不会复用历史修订号。
