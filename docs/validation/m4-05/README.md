# M4-05 相机启动全零帧根因取证

本目录保存启动缓冲哨兵的软件结论、实体 CAM01 取证矩阵、每轮首 8 次取帧统计和官方 MVS
客户端对照证据。本任务只诊断根因，不改变候选事件门槛，也不把完整零覆盖帧作为正式规则丢弃。

## 探针日志

仅当服务的相机类别 Debug 日志在采集工作线程启动时已启用，`AcquisitionWorker` 才执行探针。
每次启动最多探测前 8 次 `capture_into()` 调用；每次调用前以 `0xA5` 填充目标缓冲容量，SDK
成功返回后仅扫描 `FrameBuffer::bytes()` 表示的有效载荷。

日志筛选键：

```text
operation=frame.startupBufferProbe
```

固定字段：

```text
cameraId probeAttempt classification zeroBytes sentinelBytes
leadingSentinelBytes trailingSentinelBytes minimumByte maximumByte
payloadBytes cameraFrameNumber lostPacketFlag
```

固定分类和处理：

| 分类 | 判据 | 当前处理 |
| --- | --- | --- |
| `unwritten-sentinel` | 有效载荷全部为 `0xA5` | 计入不完整帧，不进入下游 |
| `partial-write-suspected` | 有效载荷首部或尾部连续残留部分 `0xA5` | 记录诊断，继续下发 |
| `all-zero-overwritten` | 无哨兵且有效载荷全部为零 | 记录诊断，继续下发 |
| `normal` | 其余情况 | 继续下发 |

`0xA5` 可能是合法像素值。载荷内部孤立出现的哨兵字节只计数，不触发部分写入分类；即便首尾
存在连续残留，`partial-write-suspected` 仍只是取证信号，需结合连续多轮和官方客户端对照判断。

## 临时运行要求

- 从生产配置创建仓库外或 `out/` 下的临时副本；只把 `logging.level` 改为 `debug`、
  `algorithm.enabled` 改为 `false`；不得覆盖默认或生产配置。
- CAM01 曝光、增益、帧率、ROI、像素格式、触发、包长、包间隔和 Line 1 频闪参数保持当前值。
- 启动前回读并记录实际参数；配置保存值不能代替设备实际回读值。
- 至少执行 20 次同连接 `start → 首 8 次取帧 → stop`，另执行 5 次
  `connect → start → 首 8 次取帧 → stop → disconnect`。
- 每轮保存完整相机 Debug 日志，不能只摘录异常行。复制到 `raw-logs/` 时使用包含 UTC、场景和轮次
  的不可覆盖文件名。

## 根因判定矩阵

| 程序探针结果 | 官方 MVS 对照 | 根因结论 |
| --- | --- | --- |
| 首帧 `unwritten-sentinel` | 不要求 | SDK 成功返回但未填充用户缓冲区 |
| 首帧 `partial-write-suspected` | 可选复核 | SDK/驱动负载复制不完整；若存在自然 `0xA5` 像素需先排除误判 |
| 首帧 `all-zero-overwritten` | 相同参数下也全零 | 相机/频闪启动时序 |
| 首帧 `all-zero-overwritten` | 相同参数下正常 | 当前 `MV_CC_GetOneFrameTimeout` 使用或启动调用顺序 |
| 25 轮均 `normal` | 未复现 | 根因未确定；只能记录当前条件未复现 |

## 记录文件

- `hardware-diagnosis-2026-08-11.md`：环境、保存参数、实际回读、复现计数、硬件矩阵和最终结论；
- `raw-logs/`：25 轮正式 CAM01 取证日志、IPC 控制日志、服务主日志，以及一份仅用于说明判据修正原因的无效试运行日志。
