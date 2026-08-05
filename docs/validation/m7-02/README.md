# M7-02 自动化验证记录

日期：2026-08-05

## 验证范围

本记录覆盖异步 NVMe 普通滚动缓存的软件验证：ADR-011 v1 块布局与 CRC32C、固定文件长度和提交标记、每相机 2 块有界队列、单写线程、限速与截止时间、同步 I/O 取消、固定容量回绕、磁盘不可用时的内存降级，以及 critical/stop-save 分级准入。

测试使用模拟帧、受控故障存储和临时目录，不依赖实体相机或专用 NVMe。

## 自动化结果

| 命令或检查 | 结果 |
| --- | --- |
| `cmake --preset local-windows-vs2026-debug` | 通过 |
| `cmake --build --preset local-windows-vs2026-debug` | 通过 |
| `PaperBreakTests.exe --gtest_filter=StorageNvmeCache.*` | 9/9 通过 |
| `ctest --test-dir out/build/local-windows-vs2026-debug -C Debug --output-on-failure` | 26/26 通过 |
| `cmake --build --preset local-windows-vs2026-release` | 通过 |
| `ctest --test-dir out/build/local-windows-vs2026-release -C Release --output-on-failure` | 26/26 通过 |
| `Get-Content config/schemas/edge-config-v2.schema.json -Raw \| ConvertFrom-Json` | 通过 |
| 本任务涉及 C++ 文件执行 clang-format `--dry-run --Werror` | 通过 |
| `git diff --check` | 通过 |

仓库全量 `format-check` 未通过，唯一报告为本任务未修改的既有文件 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6`。本任务涉及的 C++ 文件已使用同一 Visual Studio clang-format 可执行文件单独检查并通过。

## 尚未执行的硬件验收

当前环境未提供四台 MV-CS020-60GM、最终生产 ROI/stride 或批准的目标 NVMe，因此未执行或声称以下验证通过：

- 四路持续采集和目标盘 80% 持续带宽门禁；
- 长时间容量回绕、温度和尾延迟；
- 实体拔盘、断电和损坏尾块恢复；
- 并发正式事件下的端到端吞吐和丢帧率。

启动时接纳既有块和断电恢复属于 M7-04，事件块索引、租约和保护回收属于 M7-03，均未在 M7-02 中实现。生产 ROI 的实际 stride 若大于配置推导值，当前运行时会拒绝该帧并报告错误；最终预算与带宽准入需在硬件环境按相机回读值复核。
