# 配置格式

当前配置格式为 `configSchemaVersion = 1`。服务是 `configRevision` 的唯一分配者；`modifiedAt` 使用 UTC RFC 3339 三位毫秒。配置对象严格拒绝未知字段，完整机器可读约束见 `config/schemas/edge-config-v1.schema.json`，可部署起点见 `config/default-config.json`。

配置根对象包含 system、cameras、acquisition、preview、algorithm、event、storage、uplink、plantIo、logging 和 health。最多配置四路相机，逻辑编号限定为 CAM01～CAM04；启用相机必须具有唯一序列号。相机数值在 M1 只应用安全结构上限，M3 还必须按真实设备能力回读校验。

路径支持 UTF-8 中文和空格。相对路径以主配置所在目录解析，不能用 `..` 逃逸；拒绝 Windows 设备路径。普通 JSON 不得出现 password、token、secret 或 privateKey，外部凭据只能使用 `credentialReference`/`certificateReference`。

## 版本和应用

- 更新命令携带 `expectedConfigRevision`；冲突返回 `SYS_CONFIG_VERSION_CONFLICT`。
- 内容未变化时返回当前修订；成功修改严格递增修订。
- system、相机拓扑、acquisition、存储根目录、日志运行时、uplink 传输和 Plant IO 适配器变更等待重启。
- 相机参数、preview、algorithm、event、存储水位、日志等级/保留和 health 可立即应用。
- 返回值同时包含存储修订、有效修订和 `pendingRestartPaths`。

## 原子保存和恢复

服务在目标目录创建唯一临时文件，完整写入并刷新后使用 Windows 原子替换。最近 5 份有效配置保存在 `<配置文件名>.history/`，文件名为 20 位修订号。主配置损坏时只从可完整验证的最新历史恢复；残留临时文件不会被采用。选择历史回滚时会创建新的修订，不会复用历史修订号。
