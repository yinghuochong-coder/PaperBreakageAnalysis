# T1-01：不可变帧时间模型 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-14
- 最后更新：2026-08-14
- 路线图条目：`docs/roadmap/development-roadmap.md` T1-01
- 关联需求：EDGE-TS-002、EDGE-TS-010～013、EDGE-NFR-002

## 目的与可观察结果

每个离开采集线程的帧都携带一次性形成的完整 `FrameTimeMetadata`：保留相机原始 ticks/频率、接收单调时间和接收 UTC，并从同一份已发布的不可变模型快照生成可空校正 UTC、来源、偏移、不确定度、同步状态和模型修订。模型切换不改变历史帧；无模型、缺少 ticks 或 checked arithmetic 失败时帧仍可发布，但明确降级为 `UNSYNCED` 且没有校正 UTC。

## 范围

### 范围内

- 新增架构已冻结的 `paperbreak_time` 目标及稳定时间枚举、值对象、验证与 checked ticks 映射。
- 新增系统/逐相机均可复用的单槽不可变模型发布对象；发布和读取使用 `shared_ptr<const ClockModelSnapshot>` 的原子 release/acquire 语义。
- `FramePacket`/`FrameView` 携带并只读暴露完整帧时间元数据，同时保留既有接收时间访问器兼容当前消费者。
- 采集线程在每个成功帧上读取至多一次已发布模型，执行无等待、无 I/O、无浮点的有界算术。
- Mock/Hikrobot 原始 ticks/频率适配边界回归测试。

### 范围外

- T1-02 的 `TimeSyncRuntime`、探针调度、来源选择、UTC↔单调映射服务和新工作线程。
- T1-03 的 Windows/MVS 时间探针、配置 schema v8、阈值和报警。
- PBNVME3、manifest v4、事件 T0、状态/Uplink 字段及任何持久格式写入。
- 实体相机、PTP/Grandmaster 或亚毫秒精度验收。

## 当前基线

- `src/camera/include/paperbreak/camera/frame.hpp` 当前分别保存 `received_monotonic_time`、`received_wall_clock_time` 和可空 `CameraTimestamp`，没有冻结的帧时间值对象。
- `src/camera/src/acquisition.cpp` 在 `capture_into` 返回后读取两个接收时钟并直接组装 `FramePacket`；当前没有模型读取端口。
- Mock 适配器提供递增 ticks/1 Hz；Hikrobot 适配器只在 SDK 时间戳非零且已回读频率时提供成对时间戳。
- 架构第 5.2、7.5 节已经冻结独立 `paperbreak_time` 边界和原子单槽发布语义，但目标尚不存在。
- 任务开始时工作区已有 R0-03 相关的文档、测试清单、验证样例和脚本修改；这些不是 T1-01 的成果，实施中不撤销或重写。

## 前置条件与假设

- R0-02 已完成，领域字段、枚举和稳定错误码已冻结。
- T1-01 允许建立 `paperbreak_time` 的值对象/映射基础，但不提前实现 T1-02 运行时。
- Hikrobot SDK 的 `nDevTimeStampHigh/Low` 和启动时回读的时间戳频率是当前唯一可用原始时间证据；没有实体相机时只做 fake SDK 边界测试。
- `std::chrono::system_clock::now()` 与 `steady_clock::now()` 的当前值可表示为有符号 64 位纳秒；转换仍显式检查，失败时不得发布畸形帧。

## 设计说明

`paperbreak_time` 定义 `ClockSource`、`SyncState`、`FrameTimeMetadata` 和 `ClockModelSnapshot`。模型验证与帧时间构造返回稳定的 `TIME_MODEL_INVALID`/`TIME_MAPPING_UNAVAILABLE` 诊断；采集路径对映射不可用采用可解释降级而非丢帧。ticks 映射用商/余数和整数 `mul/div` 实现，不使用 `double`、`__int128`、饱和或回绕。

`ImmutableClockModelStore` 是一槽原子 `shared_ptr`，不拥有线程、队列或探针。`AcquisitionWorkerOptions` 只持有可空只读 store 指针；调用者必须保证其生命周期覆盖 worker。每帧成功取流后读取一次快照，并把值复制进 `FrameTimeMetadata`。`FrameView` 保存值副本，不保留模型指针，因此后续发布新修订不能修改历史帧。

为避免一次性重构现有事件/算法/存储消费者，既有 `FrameView::received_monotonic_time()`、`received_wall_clock_time()` 和 `camera_timestamp()` 继续作为从新值对象得到的兼容投影；新事实源为 `FrameView::time_metadata()`。

### 线程和队列

本任务不增加线程或队列。现有采集队列容量、drop-oldest 策略、指标和停止语义不变。新增模型通道是容量 1 的原子快照槽：写者覆盖旧快照，读者无等待 acquire 读取；`clear` 后新帧降级，已形成帧不变。

### 持久化与恢复

不适用。本任务不改变 schema、manifest 或 PBNVME。模型修订只在当前运行时实例内有意义；跨重启身份和持久化由后续 D2 任务使用已冻结合同实现。

### 错误和降级

- `TIME_MODEL_INVALID`：模型修订、ticks/频率配对、状态或非负量不符合合同；不接受该模型用于校正。
- `TIME_MAPPING_UNAVAILABLE`：缺少帧 ticks、频率不匹配或 ticks→UTC 算术溢出；保留原始/接收证据，以 `UNSYNCED`、空校正 UTC 和修订 0 发布帧。
- 接收时钟无法安全转换为 int64 纳秒：以 `TIME_MAPPING_UNAVAILABLE` 停止当前采集 worker，避免发布无效必填字段；不重试、不忙循环。
- 厂商错误码只保留在既有相机错误的附加诊断中，不成为时间业务错误码。

## 实施步骤

- [x] 1. 新增 `paperbreak_time` 值对象、验证、单槽发布与 checked ticks 映射，并添加独立单元测试覆盖枚举、字段配对、非负量、缺失 ticks、频率不匹配和正/负方向溢出。
- [x] 2. 扩展 `FramePacket`/`FrameView`，使完整时间元数据成为只读帧投影的事实源；更新相机模型测试验证零拷贝和时间字段不丢失。
- [x] 3. 将模型槽接入采集 worker，每帧只读取一次；测试 UNSYNCED 降级、模型切换后历史帧不变和接收时间必填。
- [x] 4. 更新 Mock/Hikrobot fake SDK 边界测试，确认 ticks/频率成对保留以及缺失厂商 ticks 不伪造校正时间。
- [x] 5. 运行格式、Debug 配置/构建、定向测试和全量非硬件 CTest；更新路线图状态、计划证据及必要的公开文档说明。

## 验证计划

### 自动化测试

- `TimeModel*`：值对象验证、atomic publication、整数映射、溢出/缺失 ticks、UNSYNCED 和历史快照不变。
- `CameraFrameView*`、`CameraAcquisitionWorker*`：新时间事实源、兼容访问器、每帧模型快照和切换不回写。
- `MockCamera*`、`MvsLifecycleTest*`：适配器只提供原始时间证据，MVS 类型不越界。
- 既有完整 `unit`、`simulation` 和集成 CTest 回归。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug

cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

T1-01 按路线图的常规代码任务门禁执行 Debug；上列 Release 命令保留为后续发布/阶段门禁，
本任务未执行也不据此声称 Release 通过。

另运行：

```powershell
ctest --preset local-windows-vs2026-debug -R "unit|hikrobot_sdk_boundary" --output-on-failure
cmake --build --preset local-windows-vs2026-debug --target format-check
git diff --check
```

### 人工或硬件验证

- 环境：目标 MV-CS020-60GM、MVS Runtime、PTP/Grandmaster。
- 步骤：实体相机取流并比较 SDK ticks/频率与已发布模型；后续 V5-02 执行。
- 预期：原始 ticks/频率稳定且时间精度达到现场冻结阈值。
- 证据保存位置：待 V5-02 指定。
- 状态：未执行；T1-01 只使用 Mock 和 fake MVS API，不能证明硬件时间戳能力或同步精度。

## 回滚与恢复

本任务没有数据迁移。失败时可撤销新增 `paperbreak_time` 目标及帧/采集接线，并恢复 T1-01 修改的测试；不得触碰任务开始前已有的 R0-03 工作区改动。旧持久文件不受影响。

## 验收标准

- [x] 冻结的帧时间字段和一致性规则已由 C++ 类型/验证覆盖。
- [x] 无模型、缺失 ticks、UNSYNCED 和映射溢出均保留帧原始/接收证据且不伪造校正 UTC。
- [x] 模型发布后切换不会改变已形成帧的时间值。
- [x] 采集热路径无 I/O、等待、浮点映射、探针调用或无界分配。
- [x] Mock/Hikrobot 适配器边界回归通过，MVS 类型没有泄漏。
- [x] Debug 构建和全量非硬件 CTest 通过；硬件限制明确记录。

## 进度记录

- 2026-08-14：读取需求、架构、路线图、ADR-018、相关源文件和测试；创建计划并标记 in-progress。
- 2026-08-14：新增 `paperbreak_time` 值对象、验证、checked 映射和容量 1 原子发布槽，接入帧与采集 worker。
- 2026-08-14：补齐字段、溢出、缺 ticks、UNSYNCED、历史不变及 Mock/fake MVS 边界测试；Debug 构建和非硬件 CTest 通过，状态改为 completed。

## 决策记录

- DEC-001：按已冻结架构新增独立 `paperbreak_time` 目标；T1-01 只实现值对象、发布槽和帧映射，运行时/探针留给 T1-02/T1-03。
- DEC-002：`FrameView` 保存时间元数据值副本而非模型指针，结构性保证历史帧不被新模型回写。
- DEC-003：保留既有接收时间和相机时间访问器作为兼容投影，避免把 T1-01 扩大为事件、算法和存储重构。

## 意外发现

- 任务开始时架构已经声明 `paperbreak_time` 目标，但构建树中尚无该目录/目标。
- 工作区包含未提交的 R0-03 交付物；T1-01 必须在其上增量实施并在最终清单中区分既有修改。
- 当前 PowerShell 会话未把 CMake 加入 `PATH`；验证使用 Visual Studio 2026 自带的 CMake/Ctest 绝对路径执行相同本机预设。
- MSVC 的 `system_clock::duration` 粒度不是纳秒；兼容访问器显式转换 duration，完整纳秒证据仍保存在 `FrameTimeMetadata` 的 int64 字段中。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-14 | 基线代码与文档检查 | 通过 | 未执行硬件测试 |
| 2026-08-14 | `cmake --preset local-windows-vs2026-debug` | 通过 | 使用 VS 自带 CMake 绝对路径；生成到本机预设目录 |
| 2026-08-14 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | 全部 Debug 目标，包括生产 Hikrobot 适配器和 fake SDK 测试 |
| 2026-08-14 | `ctest --preset local-windows-vs2026-debug` | 通过，33/33 | 预设排除硬件集成测试；fake MVS 51/51 通过 |
| 2026-08-14 | `ctest --preset local-windows-vs2026-debug -R '^unit$' --output-on-failure` | 通过 | 441 项：440 passed，1 个仅 Release 执行的既有性能测试 skipped |
| 2026-08-14 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 通过 | 全仓 C++ 格式检查 |
| 2026-08-14 | `git diff --check` | 通过 | 无空白错误；保留任务开始前 R0-03 工作区改动 |

## 完成摘要

新增 `paperbreak_time` 目标和完整帧时间值对象；采集线程从容量 1 原子槽一次读取不可变模型，使用 checked 整数算术形成校正 UTC，并在任何不可用场景保留原始/接收证据后明确降级。`FrameView` 保存值副本且兼容既有时间访问器。新增时间模型、采集、Mock 和 fake MVS 测试，Debug 构建、格式检查和非硬件 CTest 全部通过。实体相机、PTP/Grandmaster 和同步精度未测试，待 V5-02；T1-02 未开始。
