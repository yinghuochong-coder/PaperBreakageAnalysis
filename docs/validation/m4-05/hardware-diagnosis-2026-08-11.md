# CAM01 启动全零帧硬件取证记录（2026-08-11）

## 当前结论

- 软件哨兵判定能力：已实现；定向 Debug 构建和相机单元测试已通过，完整门禁见 ExecPlan；
- 实体 CAM01 已执行 20 次同连接 start/stop 和 5 次 connect/start，共保存 200 条首 8 帧探针
  记录，全部为 `normal`；本轮没有复现启动全零、未写入或首尾哨兵残留；
- 实体 CAM01 根因：**仍未确定（当前条件未复现）**；官方 MVS 对照的触发条件
  `all-zero-overwritten` 未出现，因此未执行该对照；
- 不能据此把根因归于 SDK、驱动、相机或频闪。

## 环境与版本

| 项目 | 记录值 | 证据状态 |
| --- | --- | --- |
| 操作系统/架构 | Windows x64 | 当前构建环境 |
| 编译器 | Visual Studio 2026 MSVC v145 | CMake 本机预设配置输出 |
| MVS Runtime | 4.8.0.3，产品版本 4.8.0.3.1819716 | 已读取 `MvCameraControl.dll` 版本信息 |
| 目标相机 | CAM01 / MV-CS020-60GM / DB1888674 / 192.168.11.117 | 本轮只读枚举，主机接口 192.168.11.102 |
| 实体相机占用检查 | 独占访问可用 | `PaperBreakCameraHardwareTest --probe` 成功 |
| 官方 MVS 客户端版本 | 预期 4.8.0.3 | 未启动；未出现要求对照的完整零覆盖 |

## 参数记录

临时配置只把日志改为 Debug、关闭算法/预览/上联，并把事件窗口和采集池缩为诊断所需的有界
容量；下列相机参数未改。实际值来自首次 `camera.connect` 响应：

| 参数 | 保存值 | 本轮实际回读 |
| --- | ---: | --- |
| 自动曝光 | Off | Off |
| 曝光 | 500.0 µs | 500.0 µs |
| 增益 | 23.01 dB | 23.011899948120117 dB |
| 帧率 | 60.0 fps | 60.0 fps |
| ROI | 1624×1240，偏移 (0, 0) | 1624×1240，偏移 (0, 0) |
| 像素格式 | Mono8 | Mono8 |
| 触发模式 | Continuous | Continuous |
| 触发延迟 | 0 µs | 0 µs |
| 包长 | 8164 字节 | 8164 字节 |
| 包间隔 | 400 ns | 400 ns |
| Line 1 频闪 | 开；持续 7 µs；前置 5 µs；后置 0 µs | 开；持续 7 µs；前置 5 µs；后置 0 µs |

## 执行矩阵

| 场景 | 要求轮次 | 已执行轮次 | 探针记录 | `normal` | `unwritten-sentinel` | `partial-write-suspected` | `all-zero-overwritten` | 状态 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 同连接 start/stop | 20 | 20 | 160 | 160 | 0 | 0 | 0 | 完成，未复现 |
| connect/start/stop/disconnect | 5 | 5 | 40 | 40 | 0 | 0 | 0 | 完成，未复现 |
| 官方 MVS 同参数启停 | 出现完整零覆盖后执行 | 0 | 0 | 0 | 不适用 | 不适用 | 0 | 未执行：触发条件未出现 |

## 每轮首 8 次取帧统计

正式运行 UTC 范围为 2026-08-11 10:12:17.600Z～10:12:35.771Z。每轮 8 条的完整逐帧字段保存在
`raw-logs/cam01-buffer-probe-final-25-rounds-20260811.log`；下表是逐轮摘要。所有记录的
`payloadBytes=2013760`、`cameraFrameNumber=1～8`、首/尾连续哨兵均为 0、最大值均为 255，
`lostPacketFlag=false`。

| 场景/轮次 | probeAttempt | zeroBytes 范围 | sentinelBytes 范围 | 首/尾哨兵最大值 | 分类 | 丢包 |
| --- | --- | ---: | ---: | --- | --- | --- |
| start/stop 1 | 1～8 | 0～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 2 | 1～8 | 0～8 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 3 | 1～8 | 0～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 4 | 1～8 | 0～3 | 0～0 | 0 / 0 | normal×8 | false×8 |
| start/stop 5 | 1～8 | 0～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 6 | 1～8 | 0～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 7 | 1～8 | 1～6 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 8 | 1～8 | 1～5 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 9 | 1～8 | 0～6 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 10 | 1～8 | 1～4 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 11 | 1～8 | 1～5 | 0～3 | 0 / 0 | normal×8 | false×8 |
| start/stop 12 | 1～8 | 0～5 | 0～3 | 0 / 0 | normal×8 | false×8 |
| start/stop 13 | 1～8 | 0～4 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 14 | 1～8 | 1～4 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 15 | 1～8 | 2～5 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 16 | 1～8 | 1～6 | 0～2 | 0 / 0 | normal×8 | false×8 |
| start/stop 17 | 1～8 | 1～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 18 | 1～8 | 0～6 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 19 | 1～8 | 1～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| start/stop 20 | 1～8 | 0～5 | 0～1 | 0 / 0 | normal×8 | false×8 |
| connect/start 1 | 1～8 | 0～7 | 0～1 | 0 / 0 | normal×8 | false×8 |
| connect/start 2 | 1～8 | 0～4 | 0～1 | 0 / 0 | normal×8 | false×8 |
| connect/start 3 | 1～8 | 0～5 | 0～2 | 0 / 0 | normal×8 | false×8 |
| connect/start 4 | 1～8 | 0～5 | 0～2 | 0 / 0 | normal×8 | false×8 |
| connect/start 5 | 1～8 | 1～5 | 0～1 | 0 / 0 | normal×8 | false×8 |

## 官方 MVS 对照

未执行。计划规定只有程序出现 `all-zero-overwritten` 后，才在相同曝光、增益、ROI、帧率、触发
和频闪条件下用官方 MVS 客户端重复启停；正式 200 条记录均为 `normal`，未满足触发条件。

## 原始日志

- `cam01-buffer-probe-final-25-rounds-20260811.log`：正式 25 轮完整采集线程日志，含 200 条探针记录；
- `cam01-ipc-control-final-25-rounds-20260811.log`：6 次连接、25 次启动、25 次停止、6 次断开，均为成功；
- `service-main-final-25-rounds-20260811.log`：正式运行服务主线程日志；
- `pilot-invalid-any-sentinel-classifier-20260811.log`：初版判据试运行，仅用于证明图像内部会自然出现
  孤立 `0xA5`；该文件不用于根因分类。

## 最终根因

**未确定（已完成计划轮次，但当前条件未复现）。** 25 轮共 200 个启动帧均被完整覆盖且内容正常，
没有证据支持“SDK 成功返回但未写缓冲”“SDK/驱动部分复制”或“相机/频闪交付全零”中的任一根因。
本次阴性结果也不能排除事件发生时的偶发启动时序问题；若再次出现
`all-zero-overwritten`，再按矩阵执行官方 MVS 同参数对照。
