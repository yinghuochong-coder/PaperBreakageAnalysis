# T1-03：生产探针、配置和报警 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-15
- 最后更新：2026-08-15
- 路线图条目：`docs/roadmap/development-roadmap.md` T1-03
- 关联需求：EDGE-TS-001～004、EDGE-TS-014、EDGE-NFR-002

## 目的与可观察结果

在 Windows 生产组合根中启动 T1-02 的 `TimeSyncRuntime`，以只读 Win32 探针采样系统时间服务、
以 Hikrobot 适配器采样相机 ticks/PTP 能力，并把来源、质量和稳定错误发布为不可变状态。配置升级
到 schema v8；v7 启动加载时通过原子替换迁移。来源变化形成报警历史，偏移/不确定度持续超过
可配置 Warning/Alarm 阈值后进入活动报警，恢复后清除。自动化测试证明不支持/失败路径不会伪报
硬件同步，持续时间判定和关闭是确定性的。

## 范围

### 范围内

- `paperbreak_platform_windows` 的只读 Windows 时钟探针；
- Hikrobot 适配器内的相机时间戳频率、latch、IEEE 1588 能力/状态和偏移采样；
- 不含 Win32/MVS 类型的相机时间采样边界以及到 `ICameraClockProbe` 的服务适配；
- schema v8、v7 原子迁移、默认配置和配置文档；
- 时间来源变化、Warning/Alarm 持续超限的有界内存报警登记；
- 生产 `ServiceRuntime` 组合、重启生效配置和确定性关闭；
- 单元、适配器、配置迁移和非硬件回归测试。

### 范围外

- P1-01 的服务重启/Grandmaster 切换/时间跳变自动恢复；
- O4-01 的完整 Uplink 状态快照；
- P1-02 的报警推送生命周期和 Critical 持久补发；
- 修改现场 PTP/NTP 配置或让工控机充当固定 Grandmaster；
- 实体相机、PTP Grandmaster、目标交换机的精度结论（留待 V5-02）。

## 当前基线

- 已阅读 `docs/requirements/edge-system-requirements.md`、
  `docs/requirements/edge-host-integration-additional-requirements.md`、
  `docs/architecture/system-architecture.md`、`docs/roadmap/development-roadmap.md` 和
  `.agent/PLANS.md`。
- T1-02 已在工作区提供 `paperbreak_time` 的探针端口、独立线程、容量 16 控制通道、系统/四相机
  latest-wins 模型槽和关闭合同；相关文件当前未提交，属于任务开始前基线，必须保留。
- 当前配置公开版本为 v7；`parse_config` 可迁移 v2～v6，但仓储加载不会把旧版本原子写回。
- `AlarmRegistry` 已提供 1024 条活动/4096 条历史的有界 raise/merge/clear；不持久化。
- Hikrobot 适配器已有 `GevTimestampTickFrequency` 与帧 ticks 读取，尚无独立 PTP 能力采样。
- 生产服务尚未装配 `TimeSyncRuntime`。

## 前置条件与假设

- 以当前已完成的 T1-02 工作区为前置，不提交、不清理用户或前序任务修改。
- Windows W32Time 运行只能证明 OS 时间服务可用，不能单独证明硬件 PTP；系统探针因此最多报告
  OS NTP/同步中，并携带保守不确定度，不宣称 `PTP_HARDWARE`。
- Hikrobot 节点存在性由 SDK 返回码探测；`GevIEEE1588Status=Slave` 才允许报告相机硬件 PTP
  已同步。节点缺失必须显式降级。
- MVS 同步节点名和枚举值按 GigE Vision/目标 SDK 接口实现，但实体 MV-CS020-60GM 行为未验证。

## 设计说明

Windows 探针只查询 W32Time 服务状态、精确 UTC 和系统时间增量，不修改服务或时钟配置。相机公共
边界增加无厂商类型的 `CameraClockSample`；Hikrobot `DeviceHandle` 在既有互斥量下执行固定次数
的节点读取。`CameraControlRuntime` 暴露按逻辑 ID 的有截止采样，服务侧轻量适配为
`ICameraClockProbe`。

`TimeSyncAlarmMonitor` 在时间线程发布完整模型后同步执行短小、无 I/O 的内存判定。每个 system/
camera source 使用固定状态项，不创建队列。来源改变时 raise 后立即 clear，留下历史记录；质量值取
`max(abs(offsetNs), uncertaintyNs)`。Alarm 级别优先于 Warning；只有连续超限达到各自持续时间才
raise，恢复或升级时清除互斥的旧报警。

schema v8 新增 `timeSync`：采样周期、探针超时、接收时钟不确定度、Warning/Alarm 阈值和持续
时间。阈值满足 `warning < alarm`，探针超时小于采样周期。v2～v7 读取时补默认值并序列化为 v8；
仓储首次加载 v7 时先写历史副本，再以现有原子文件接口替换主配置，任一步失败不启用半迁移配置。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `time.control` | 配置/主控制 | 时间线程 | 16 | 拒绝最新，`SYS_BUSY` | stop 独立唤醒，丢弃未开始刷新 | 复用 T1-02 指标 |
| `time.model.latest` | 时间线程 | 采集/状态/报警 | system 1 + camera 4 | latest-wins | 停止时发布最终降级快照后 join | 复用 T1-02 修订指标 |
| 时间报警状态 | 时间线程 | `AlarmRegistry` | system + camera 4 固定项 | 原地状态更新，无排队 | 停止时清除活动时间阈值报警 | 当前等级/起始单调时间 |

### 持久化与恢复

配置 schema 从 v7 升至 v8。迁移使用 `IAtomicFileSystem::replace_atomically`，并在配置历史目录保留
原始 v7 修订；原子替换失败时主配置保持 v7、运行时创建失败。报警仍按既有设计只在进程内有界保存。

### 错误和降级

- `TIME_PROBE_NOT_SUPPORTED`：平台/相机节点不支持；可重试性 false，发布降级模型；
- `TIME_PROBE_UNAVAILABLE`：服务停止、相机未连接、SDK/Win32 查询失败或超时；可重试；
- `TIME_SYNC_SOURCE_CHANGED`：来源改变的历史报警；
- `TIME_SYNC_WARNING_THRESHOLD_EXCEEDED` / `TIME_SYNC_ALARM_THRESHOLD_EXCEEDED`：持续超限；
- 原生 Win32/MVS 错误仅作诊断上下文，不能替代稳定业务码；
- 不支持 PTP 时绝不设置 `hardware_ptp_synchronized=true`，接收时钟不确定度使用配置的保守值。

## 实施步骤

- [x] 1. 增加 schema v8 强类型字段、严格校验、v7 默认迁移、原子写回和配置测试。
- [x] 2. 实现可注入/可测试的 Windows 系统时钟探针及稳定错误翻译。
- [x] 3. 扩展无厂商相机时钟采样边界并在 Hikrobot 适配器实现 PTP/ticks 采样与 fake SDK 测试。
- [x] 4. 实现时间报警监视器，覆盖来源记录、持续 Warning/Alarm、恢复和重配置。
- [x] 5. 在服务组合根装配生产探针、`TimeSyncRuntime`、帧模型提供器和生命周期关闭。
- [x] 6. 更新配置/架构/错误码/路线图与本 ExecPlan，并完成全部门禁验证。

## 验证计划

### 自动化测试

- `BasicConfig*` / `ConfigRepository*`：v7→v8、边界、原子失败、回滚、序列化；
- `WindowsClockProbe*`：W32Time 可用、不支持、失败、取消/截止和不伪报硬件 PTP；
- `MvsLifecycleTest*Clock*`：Slave、disabled/unsupported、节点失败、负偏移、取消/截止；
- `TimeSyncAlarmMonitor*`：来源历史、连续时间、Warning→Alarm、恢复、重配置和 stop clear；
- `TimeSyncRuntime*` 与服务 smoke：生产组合降级启动和确定性停止。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug

cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release

cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
```

### 人工或硬件验证

- 环境：目标 Windows 工控机、MV-CS020-60GM、MVS 4.8.0.3、PTP Grandmaster 和生产等价网络。
- 步骤：核对相机 IEEE 1588 节点、来源/offset/uncertainty、失去 Grandmaster 后降级和实测误差。
- 预期：Slave 才报告硬件 PTP；Warning 1 ms/Alarm 5 ms 初始阈值按实测结果另行冻结。
- 证据保存位置：V5-02 验证记录。
- 状态：本任务不执行；不得以 fake SDK 或本机 W32Time 结果替代硬件精度结论。

## 回滚与恢复

代码可按本计划涉及文件逐项反向应用，不删除现有配置或历史。v8 配置回滚旧程序时，从自动历史恢复
最后一份 v7 配置；不直接降写。`/timeSync` 变更列为重启生效，避免运行中替换探针及线程所有权；
探针失败只发布降级快照和报警，不阻止采集/事件本地功能。

## 验收标准

- [x] Windows 与 Hikrobot 生产探针遵守模块边界，SDK/Win32 类型未越界；
- [x] schema v8 和 v7 原子迁移/失败恢复有测试；
- [x] 不支持或失败路径不伪报硬件同步或亚毫秒可信度；
- [x] 来源变化及持续 Warning/Alarm 可在 `AlarmRegistry` 查询，恢复会 clear；
- [x] 时间线程和服务在共享截止时间内确定性关闭；
- [x] Debug/Release、非硬件 CTest、静态分析和格式检查通过；
- [x] 硬件精度明确标记未验证，不开始 D2-01。

## 进度记录

- 2026-08-15：阅读任务基线并创建计划，状态 in-progress；确认 T1-02 工作区改动为前置基线。
- 2026-08-15：完成生产探针、schema v8 原子迁移、报警监视器和生产组合；全量软件门禁通过，状态 completed。

## 决策记录

- DEC-001：W32Time 运行仅报告 OS NTP/同步中，不推断硬件 PTP，避免虚假能力声明。
- DEC-002：相机 PTP 采样复用已连接设备句柄，经无厂商领域结构穿过 camera 边界，避免第二个独占句柄。
- DEC-003：报警判定使用发布时单调时间和固定实体状态，不增加跨线程队列或持久化范围。
- DEC-004：`/timeSync` 配置作为重启生效项；T1-03 不增加在线替换生产探针/线程的第二套所有权协议。

## 意外发现

- 路线图和 T1-02 代码把时间相机槽固定为 4；仓库其他增量需求已有 6 相机表述。本任务不修改已冻结
  T1-02 容量，也不声称 CAM05/CAM06 时间同步已覆盖。
- T1-02 文件当前仍是未提交工作区修改；T1-03 必须保留它们并在最终文件清单中区分前置基线。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-15 | `cmake --preset local-windows-vs2026-debug` + build | 通过 | MSVC/Qt/OpenCV/MVS 本机预设 |
| 2026-08-15 | `ctest --preset local-windows-vs2026-debug` | 33/33 通过 | 单元 458 通过、1 个 Release-only 性能测试跳过 |
| 2026-08-15 | `cmake --preset local-windows-vs2026-release` + build | 通过 | Release 配置 |
| 2026-08-15 | `ctest --preset local-windows-vs2026-release` | 33/33 通过 | 单元 459/459 |
| 2026-08-15 | static-analysis preset build | 通过 | MSVC `/analyze` 门禁 |
| 2026-08-15 | format check、`git diff --check` | 通过 | 无格式或空白错误 |
| 2026-08-15 | 实体相机/PTP/Grandmaster | 未执行 | 按范围留待 V5-02 |

## 完成摘要

已完成 T1-03：生产 Windows/MVS 探针经无厂商端口装配到单线程时间运行时；schema v8 和 v7 原子
迁移可回滚；来源变化、连续 Warning/Alarm 和恢复进入有界报警登记。所有软件门禁通过。实体相机、
Grandmaster 和精度结论仍未验证，未开始后续里程碑。
