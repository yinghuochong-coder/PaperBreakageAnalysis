# M7-04 崩溃与断电恢复验证记录

> 历史证据（2026-08-09 起已被 ADR-017 取代）：本页记录的恢复实现已经从生产构建和启动
> 路径移除。当前滚动缓存为 session 级非恢复式缓存；启动不扫描、读取、修复、隔离、删除或
> 恢复本页所述旧块、索引和租约。本页仅用于保留 M7-04 当时确实完成过的实现与测试事实。

本目录记录 M7-04 的自动化恢复证据和硬件限制。块文件仍是恢复事实来源；`.index` 是可重建
SQLite 派生索引，`.quarantine` 保存未通过 v1 证明的原文件，不参与正常回绕且本任务不删除。

## 实现合同

- 启动扫描最多处理 100000 个候选文件、使用最多 64 MiB 恢复摘要、总截止时间 5 分钟；负载
  使用固定 1 MiB 缓冲流式 CRC32C，不读取整个块到内存。
- 完整块验证头/尾字段与保留位、实际/声明长度、索引项、逐帧负载、索引/数据区域、尾页和
  提交标记；任何不一致都不进入正常索引。
- `.partial` 只恢复从首帧开始连续通过项 CRC、边界和负载 CRC 的前缀；尾页主体和标记分别
  `FlushFileBuffers` 后同卷原子改名。无有效前缀、未知版本和损坏已发布块非覆盖隔离。
- 全部扫描完成后才事务重建块表；可解析持久租约和重启后新租约均按 UTC 绑定恢复块，当前
  进程未来块仍使用单调时间保护。
- 恢复的物理字节与每相机最大代次进入运行时；容量回绕只删除零租约块。恢复 I/O/上限失败
  降级普通缓存，事件内存链继续运行。

## 自动化覆盖

`StorageNvmeRecovery.*` 覆盖：

- 正常完整块重开、索引目录丢失重建及相机代次连续；
- 预分配、头页、索引、负载、尾页主体、提交标记、标记完成但未发布等写入阶段残留；
- 头保留区、未知版本、索引项、负载、尾页和提交标记损坏；
- 文件数、摘要内存和总截止时间上限；
- 启动扫描期间模拟磁盘移除后的 `memory-degraded`，且正式事件内存能力保持允许；
- 恢复容量回绕、重启后新租约绑定、持久租约重新绑定和全部块受保护竞争。

M7-02/M7-03 的 `StorageNvmeCache.*`、`StorageNvmeIndex.*` 和 `EventRuntimeNvme.*` 同时作为回归
集合，验证队列、提交、回绕、租约和事件成功/失败释放语义未回退。

## 执行结果

| 命令/检查 | 结果 |
| --- | --- |
| `PaperBreakTests.exe --gtest_filter=StorageNvmeRecovery.*:StorageNvmeCache.*:StorageNvmeIndex.*:EventRuntimeNvme.*` | 23/23 通过，其中 M7-04 恢复测试 7/7 |
| `cmake --build --preset local-windows-vs2026-debug` | 通过，MSVC `/W4 /WX` |
| `ctest --preset local-windows-vs2026-debug --output-on-failure` | 25/25 通过；通用 unit 入口 291 项 |
| `cmake --build --preset local-windows-vs2026-release` | 通过，MSVC `/W4 /WX` |
| `ctest --preset local-windows-vs2026-release --output-on-failure` | 25/25 通过；通用 unit 入口 291 项 |
| `cmake --build --preset local-windows-vs2026-static-analysis` | 通过，生产目标 MSVC `/analyze` |
| 本任务 8 个 C++ 文件执行 `clang-format --dry-run --Werror` | 通过 |
| `git diff --check` | 通过 |
| 全仓 `format-check` | 未通过；仍仅命中未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 既有格式问题 |

## 未执行的硬件验证

以下项目未执行，不能报告通过：

- 四台 MV-CS020-60GM、最终 ROI/stride 与批准目标 NVMe 的持续写、热稳定和实际保留时长；
- 在真实写入各阶段进行物理断电或拔盘，并验证 NTFS/NVMe flush、改名耐久与重启扫描耗时；
- 目标工控机上恢复扫描与事件写入并发时的采集帧率、丢帧、CPU、内存和温度指标。

原因是当前开发环境没有上述实体设备、可控断电台架和批准的生产相机参数。M7 功能任务可由
模拟故障完成，但退出门禁中的目标硬件性能与物理断电证据继续保持未满足。
