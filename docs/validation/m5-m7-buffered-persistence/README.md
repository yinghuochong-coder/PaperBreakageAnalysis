# M5/M7 缓冲持久化性能与回归验证

验证日期：2026-08-09

## 自动化回归

- Debug：配置、构建成功；`ctest --preset local-windows-vs2026-debug --output-on-failure` 通过 28/28。
- Release：配置、构建成功；`ctest --preset local-windows-vs2026-release --output-on-failure` 最终复测通过 28/28。
- M7-01 格式验证确认 `PBNVME1` 只读兼容及 ADR-017 `PBNVME2` 缓冲格式约束一致。

Release 首次全量 CTest 的 `unit` 项出现一次瞬时失败；使用完全相同过滤器直接复现时 360/360 通过，随后重新执行完整 Release CTest 为 28/28。未隐去该次非最终结果。

## 911 帧 Release 性能门禁

测试：`StorageEventStorePerformance.ReleaseCommits911Mono8FramesAbove100MiBpsWithoutIndexReadback`

场景：1624×1240 Mono8、911 帧、模拟帧源；通过正式 `EventPersistenceRuntime` 写入事件并使用写入器返回的 manifest 建立 SQLite 索引。

结果：

| 指标 | 实测值 |
| --- | ---: |
| 事件写入字节 | 1,834,847,924 |
| 提交耗时 | 2,575 ms |
| 吞吐 | 679.432 MiB/s |
| 进程 CPU | 66.7353% |
| 事件队列高水位 | 1 |
| 持久化拒绝 | 0 |
| 生成/写入帧 | 911 |
| 模拟采集丢帧 | 0 |
| SQLite 索引阶段原始负载读取 | 0 |

实测超过 100 MiB/s 门槛，且低于 18 秒提交窗口。该结果仅代表当前工作站、本机文件系统缓存和模拟帧场景；未执行实体相机、物理断电、拔盘、生产上位机或跨重启恢复测试。
