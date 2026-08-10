# 存储缓存运维

## NVMe session 目录

ADR-017 起，服务每次启动在配置的 `cacheRoot/sessions/<session-id>` 创建独立滚动缓存。服务不
扫描、不恢复、不修改、也不自动删除旧 session 或旧版直接位于 `cacheRoot` 下的块。

`maximumCacheStorageGiB` 只限制当前 session；旧 session 仍占用卷空间，卷级
`warningFreeSpaceGiB`、`criticalFreeSpaceGiB` 和 `stopFreeSpaceGiB` 按实际剩余空间生效。

## 人工清理规则

只有同时满足以下条件才允许人工清理旧滚动缓存：

1. PaperBreak Edge Service 已停止，并确认进程不再持有缓存文件；
2. 已从当前运行状态或最近日志确认本次启动的 `activeSessionRoot`；
3. 待清理目标是 `cacheRoot/sessions` 下的旧 session，且绝不包含当前 session；
4. 已确认目标不是正式 `eventRoot`、`.transactions`、`.quarantine`、数据库或配置目录；
5. 运维工单记录绝对路径、预计释放空间、操作者和时间。

先用只读目录清单和磁盘占用检查解析每个绝对目标。禁止对 `cacheRoot`、卷根、用户目录或含
通配符/未解析环境变量的宽泛路径执行递归删除。清理后重新启动服务并确认新
`activeSessionRoot` 唯一、索引为空、卷水位恢复正常。

本策略不提供跨重启缓存恢复。正式事件目录不能按本页流程清理，必须遵循事件保留、上传和
人工锁定策略。
