# ADR-017：缓冲事件持久化与非恢复式 NVMe 滚动缓存

- 状态：Accepted
- 日期：2026-08-09
- 决策范围：M5 正式事件持久化、M7 NVMe 滚动缓存、M8 上传前完整性校验
- 取代：ADR-007 的强制持久化部分、ADR-011 的断电/跨重启恢复部分

## 背景

原实现对事件文件逐个执行强制落盘、关闭后全量读回 SHA-256，并在 SQLite 建索引前再次完整校验；滚动缓存还执行启动扫描、尾块修复和租约恢复。1.709 GiB、911 帧的模拟事件在同机 Release 环境约为 9.1 MiB/s，无法满足冻结窗口及时提交要求。滚动缓存本质上是可丢失的派生数据，跨重启恢复的复杂度和写放大也不符合当前产品取舍。

## 决策

### 正式事件

新事件使用 manifest v3 和 `PBNVME2`。文件采用 Windows 普通顺序缓冲写；不使用 `FILE_FLAG_WRITE_THROUGH`，不调用 `FlushFileBuffers`。每次 `WriteFile` 实际成功的字节立即送入 Windows CNG `BCryptHashData`，关闭文件后取得唯一的整文件 SHA-256。原始块完整写入尾页和 `COMMIT2` 后关闭，再以不覆盖目标的同卷原子改名发布。`manifest.json` 仍最后写，事件目录仍由唯一事务目录原子发布。

`PBNVME2` 保留 4 KiB 头页、96 B 索引项、尾页、头/索引项/索引区/尾页结构 CRC32C；逐帧数据 CRC 和整块 Data CRC 字段保留但写零。CRC32C 使用 SSE4.2 运行时分派和表驱动回退。SHA-256 是唯一内容摘要，仓库不再保留自研 SHA-256。

manifest v3 必须声明：

```json
{
  "schemaVersion": 3,
  "writeMode": "buffered",
  "powerLossDurable": false,
  "verification": "upload-or-on-demand"
}
```

`Committed` 表示文件已关闭且事件目录已原子发布到操作系统命名空间，不表示突然断电后可恢复。写入失败、短写、CNG 失败或发布失败保留事务目录并产生 `Incomplete`；最近一次缓冲写在突然断电时丢失或损坏是已接受风险。

manifest v2 / `PBNVME1` 仅只读兼容检查与导出，不迁移、不重写。

### 索引与完整性

正常提交直接用内存中的 writer manifest 建立 SQLite 索引。启动事务恢复和目录对账只解析 manifest、验证路径约束、普通文件存在性与声明长度，不读取原始负载或计算 SHA。

SQLite schema v6 增加 `integrity_state`（`Unverified | Verified | Failed`）、`integrity_checked_at_utc_ms` 和 `integrity_error_code`。新提交和 v5 迁移事件为 `Unverified`。列表与 `event.getManifest` 只做结构检查；交互式 `event.getSummary` 额外只校验实际返回的首张关键帧，不读取原始块且不提升完整性状态；`event.get`、导出和在线上传进行单次顺序读取的完整校验。

完整校验失败不改变算法判定、人工复核或 `persistence_state=Committed`，但设置 `storage_state=Damaged`、`artifacts_available=false`、`integrity_state=Failed`，拒绝详情内容、导出和上传，将未完成上传任务转为 `ManualIntervention` 并登记 Critical 报警。文件不删除。

上传必须先检查连接状态；离线返回 `UPLINK_DISCONNECTED`，不访问源文件。在线使用 manifest SHA 创建上传描述，随后单遍顺序读取，同时计算整文件和分块 SHA，只发送服务端缺失块；整文件摘要不一致时返回 `UPLOAD_SOURCE_CHANGED`，保留 checkpoint，不调用完成接口。

### 滚动缓存

滚动缓存不再提供跨重启恢复。每次服务启动以 `CREATE_NEW` 语义创建 `cacheRoot/sessions/<session-id>`，使用独立派生 SQLite 索引，代次从 1 开始，租约仅在当前进程有效。启动不得枚举、读取、删除旧 session 或旧版根目录块。

滚动块使用 `PBNVME2` 普通缓冲写、完整尾页和一次原子改名，不使用两阶段强制耐久提交。`maximumCacheStorageGiB` 是当前 session 的容量上限；旧数据不计入，但继续占用卷空间，由卷级 warning/critical/stop 水位保护。系统不自动清理旧 session，运维只能在服务停止后人工清理。

## 后果

- 事件写入避免强制落盘和提交阶段读回，目标 Release 顺序提交吞吐不低于 100 MiB/s。
- 从冻结窗口结束到 `Committed` 的目标不超过约 18 秒，不含后置窗口采集时间。
- 启动时间不再与旧缓存体量相关，旧缓存不会因进程启动被修改。
- 断电后最近事件和当前 session 可能丢失或损坏；这是明确接受的可靠性边界。
- 不新增后台校验线程；从未上传、查看或导出的事件可长期保持 `Unverified`。

## 验证边界

自动化使用模拟帧和本机文件系统。实体相机、物理断电和跨重启恢复不属于本决策的新能力，也不得据此声称已验证。
