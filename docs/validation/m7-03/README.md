# M7-03 索引、保护和事件引用验证记录

## 实现范围

- 缓存根 `.index/nvme-index-v1.db` 保存 schema v1 SQLite 派生块索引、事件租约和块引用；块文件仍是恢复事实来源；
- 已提交块登记相机、UTC 时间窗、序号范围、块内缺口及头/索引/数据/尾 CRC32C，可按相机/时间有界查询并追踪跨块缺口或重叠；
- 活动事件租约固定最多 64 条，保护已提交和随后提交的单调时间重叠块；索引重建可按持久 UTC 租约重新绑定已验证块摘要；
- 固定容量回绕只删除零租约块；全部块受保护时返回 `NVME_CACHE_PROTECTED`、保留文件并降级内存缓存；
- 事件候选建立租约；事件目录与 SQLite 元数据均提交成功后释放，编码/准入/文件/元数据失败时保留；
- 新增索引、租约、保护字节及失败指标和稳定业务错误码。

## 自动化验证

| 验证 | 结果 |
| --- | --- |
| `PaperBreakTests.exe --gtest_filter=EventRuntimeNvme.*:StorageNvmeIndex.*:StorageNvmeCache.*` | 16/16 通过 |
| `cmake --build --preset local-windows-vs2026-debug` | 通过，MSVC `/W4 /WX` |
| `ctest --test-dir out/build/local-windows-vs2026-debug -C Debug --output-on-failure` | 26/26 通过 |
| `cmake --preset local-windows-vs2026-release`；`cmake --build --preset local-windows-vs2026-release` | 通过 |
| `ctest --preset local-windows-vs2026-release --output-on-failure` | 25/25 通过；Release 预设不包含 hardware-baseline |
| `cmake --preset local-windows-vs2026-static-analysis`；`cmake --build --preset local-windows-vs2026-static-analysis` | 通过，MSVC 静态分析无报告 |
| 本任务涉及 C++ 文件执行 clang-format `--dry-run --Werror` | 通过 |
| `git diff --check` | 通过 |

定向测试覆盖：派生索引关闭重开、异构相机/块时间查询、跨块序号缺口、CRC 摘要、查询上限、活动租约容量、未来重叠块绑定、全部块保护、释放后恢复回收、事务式重建重新绑定、事件成功提交释放，以及事件写失败保留租约。

仓库全量 `format-check` 未通过，唯一报告仍为本任务未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 既有格式问题；本任务涉及的 C++ 文件使用同一 Visual Studio clang-format 可执行文件单独检查并通过。

## 尚未执行的硬件验收

当前环境未提供四台 MV-CS020-60GM、批准的目标 NVMe、最终生产 ROI/stride 或受控断电设备，因此未执行或声称以下验证通过：

- 四路持续采集下的完整缓存回绕、事件保护竞争与采集丢帧预算；
- 目标盘 80% 持续带宽、接近满盘、热稳定和并发正式事件性能；
- 实体拔盘、断电、撕裂尾块和损坏索引恢复；
- 跨进程启动扫描、尾块修复/隔离和自动索引重建。

最后一项属于 M7-04。本任务只提供可重建索引入口，不扫描、接纳、删除或修复启动前既有块；M7-04 完成前，生产块存储发现既有 `.pbnvme`/`.partial` 仍保持文件不动并降级内存缓存。
