# T1-02：TimeSyncRuntime 与时钟模型 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-14
- 最后更新：2026-08-14
- 路线图条目：`docs/roadmap/development-roadmap.md` T1-02
- 关联需求：EDGE-TS-001～003、EDGE-TS-010～013、EDGE-NFR-002

## 目的与可观察结果

新增平台无关的 `TimeSyncRuntime`。它在一个独立工作线程中串行采样可注入系统/相机时钟探针，按“硬件 PTP → OS PTP/NTP → OFFSET_MODEL → RECEIVE_CLOCK”形成系统和逐相机不可变模型，使用进程内严格递增修订发布到固定单槽，并提供携带创建时模型快照的 UTC↔单调时间 checked 映射。探针失败、漂移、来源切换或系统时间跳变只影响后续快照；历史帧和已返回映射不被回写。

## 范围

### 范围内

- 在 `paperbreak_time` 定义无 Win32/MVS 类型的系统与相机探针端口、样本 DTO、状态投影和映射结果。
- 新增一个时间同步工作线程、固定容量 16 的刷新控制通道和“系统 1 槽 + 每相机 1 槽”的不可变发布。
- 实现来源优先级、offset/uncertainty/last-error 传播、系统时间跳变 P0 降级锁存和 RECEIVE_CLOCK 兜底。
- 实现 UTC↔单调时间的 checked 整数映射；结果保留所用 `shared_ptr<const ClockModelSnapshot>`。
- 通过脚本化 Mock 探针覆盖偏移、漂移、不可用、来源切换、跳变和截止关闭。

### 范围外

- T1-03 的 Windows PTP/NTP 探针、Hikrobot 相机探针、schema v8、可配置报警阈值和报警登记表。
- P1-01 的自动恢复、跳变后重新收敛、Grandmaster 切换恢复和 ticks 回绕恢复。
- Service 生产组合根接线；生产探针尚未在 T1-03 提供，本任务仅以注入端口和 Mock 验证运行时。
- E3 的外部 T0 锁定、D2 持久格式、O4 状态/Uplink 协议和实体硬件精度验收。

## 当前基线

- `paperbreak_time` 已有冻结枚举、`FrameTimeMetadata`、`ClockModelSnapshot`、模型验证、容量 1 原子槽和相机 ticks→UTC checked 映射。
- 采集 worker 每帧最多读取一次逐相机模型；当前没有模型生产者、探针端口、系统模型槽、状态投影或 UTC↔单调服务。
- `ServiceRuntime` 通过通用生命周期组件管理现有模块，但尚无生产系统/相机时间探针可装配。
- 任务开始时 `git status --short` 无输出，工作区干净。

## 前置条件与假设

- T1-01 和 R0-02 已完成，时间字段、枚举、稳定错误码及模块边界已冻结。
- 探针实现必须遵守传入的 `stop_token` 和 deadline；运行时可报告超时，但 C++ 不能安全强杀拒绝取消的第三方调用。生产实现必须在 T1-03 通过可取消/有截止的 OS 或 SDK 调用满足此合同。
- 本任务按已冻结架构为时间运行时固定最多四个相机探针；不改变现有业务相机容量或配置。
- RECEIVE_CLOCK 是明确的降级校正：逐帧以接收 UTC 作为近似采集 UTC，并使用保守不确定度，不能报告 `SYNCED`。

## 设计说明

`ISystemClockProbe::sample` 返回 OS/外部同步样本；`ICameraClockProbe::sample` 返回一个相机的 ticks、频率、配对单调/UTC 样本及硬件 PTP 证据。两者只暴露稳定业务类型，并接收停止令牌和绝对截止时间。运行时串行采样，所有模型构造和修订分配只发生在时间线程。

每轮先读取进程时钟配对点并探测系统来源，再逐相机探测。相机明确提供硬件 PTP 证据时选择 `PTP_HARDWARE`；否则使用有效 OS `PTP_SOFTWARE`/`NTP`（或系统硬件 PTP）质量；只有相机配对样本时选择 `OFFSET_MODEL`；探针不可用时发布无 ticks 锚点的 `RECEIVE_CLOCK` 模型。系统槽在无有效探针时也发布 RECEIVE_CLOCK 模型，确保可解释降级映射。

墙上增量与单调增量之差超过固定运行时选项时锁存 `SYS_TIME_JUMP_DETECTED`，后续模型保持 `DEGRADED` 且扩大不确定度；本任务不自动清除该锁存，恢复留给 P1-01。所有加减和不确定度合并检查溢出，半成品模型不发布。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `time.control.refresh` | 主控制/测试 | 时间同步线程 | 16 条 | 拒绝最新请求，返回 `SYS_BUSY` | stop 独立于队列，关闭入口后丢弃未开始刷新并唤醒线程 | 接受、处理、拒绝、高水位 |
| `time.model.latest` | 时间同步线程 | 采集/事件/状态 | 系统 1 槽 + 最多 4 个相机槽 | 原子 latest-wins，旧快照由读者只读保留 | join 前发布最终停止降级快照；槽随 runtime 生命周期释放 | 发布轮次、最后修订 |

工作线程通过 `std::jthread`、`stop_token` 和条件变量等待采样周期。`request_stop()` 非阻塞并唤醒探针/等待；`join(deadline)` 在 deadline 内等待完成，超时返回 `SYS_SHUTDOWN_TIMEOUT`。

### 持久化与恢复

不适用。本任务不改变配置、SQLite、manifest 或 PBNVME。模型修订和单调 epoch 只在当前 `TimeSyncRuntime` 生命周期内有效。

### 错误和降级

- `TIME_PROBE_UNAVAILABLE`：探针失败、超时、不支持或样本无效；记录为快照 `lastErrorCode` 并选择下一来源。
- `TIME_MODEL_INVALID`：样本/模型字段或 checked arithmetic 无法形成合同；不发布半成品，改发可验证的 RECEIVE_CLOCK 降级模型。
- `TIME_MAPPING_UNAVAILABLE`：无模型、目标早于模型有效点或映射溢出；返回失败且不伪造值。
- `SYS_TIME_JUMP_DETECTED`：系统时间跳变被锁存；继续单调调度，后续时间模型降级，本任务不自动恢复。
- `SYS_BUSY`：刷新控制容量已满；拒绝最新普通请求，stop 不受影响。
- `SYS_SHUTDOWN_TIMEOUT`：工作线程未在共享截止时间完成；调用者可继续等待，析构仍作为最终 join 防线。

## 实施步骤

- [x] 1. 扩展冻结时间值对象：补齐 `ClockSyncSnapshot`、系统 UTC↔单调 checked 映射和 RECEIVE_CLOCK 帧降级路径，并添加值对象测试。
- [x] 2. 新增探针端口、运行时选项、固定发布槽与生命周期接口，实现来源选择、修订、跳变锁存、控制容量和确定性停止。
- [x] 3. 新增脚本化测试探针，覆盖硬件/OS/offset/receive 优先级、偏移与漂移、来源切换、错误、历史快照、映射、跳变和关闭超时。
- [x] 4. 运行格式、Debug 配置/构建、定向测试和全量非硬件 CTest；修复本任务回归。
- [x] 5. 更新路线图、ExecPlan 进度和验证证据，不开始 T1-03。

## 验证计划

### 自动化测试

- `TimeModel*`：状态投影、RECEIVE_CLOCK、双向 checked 映射、有效范围和溢出。
- `TimeSyncRuntime*`：首轮探测、四级来源、偏移/漂移、失败、来源切换、严格递增修订、历史不可变、跳变锁存、控制满载和停止/超时。
- 现有采集、Mock/Hikrobot 边界与全量非硬件 CTest 回归。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug --target format-check
git diff --check
```

路线图常规门禁要求 Debug；Release 保留给阶段/发布门禁，本任务不以未执行的 Release 结果作为完成证据。

### 人工或硬件验证

- 环境：目标 Windows 工控机、MV-CS020-60GM、MVS SDK、PTP/Grandmaster。
- 状态：未执行；生产探针属于 T1-03，精度和硬件来源真实性待 V5-02。
- 预期：生产探针满足取消/截止合同，硬件 PTP 路径达到现场实测冻结阈值。

## 回滚与恢复

本任务无持久迁移。失败时可撤销新增 runtime/测试及 `paperbreak_time` 的局部扩展，恢复到 T1-01 已验证基线；不删除配置、事件或用户数据。

## 验收标准

- [x] 独立线程按冻结顺序选择来源并发布系统/逐相机不可变快照。
- [x] 模型含锚点、严格递增修订、不确定度和稳定最后错误；旧快照不被修改。
- [x] UTC↔单调映射使用同一创建时模型并对有效范围/溢出显式失败。
- [x] 偏移、漂移、探针不可用、来源切换和系统时间跳变有自动化覆盖；P0 降级不伪报同步。
- [x] 控制容量、满载策略、指标、停止唤醒和 deadline join 有实现与测试。
- [x] Debug 构建、格式检查和全量非硬件 CTest 通过；硬件限制明确记录。

## 进度记录

- 2026-08-14：读取需求、系统架构、路线图、ADR-018、T1-01 计划及相关源码/测试；创建计划并标记 in-progress。
- 2026-08-14：实现时间值对象投影/映射、探针端口、独立运行时、固定控制容量、来源选择、跳变锁存和停止语义；补齐脚本化 Mock 测试。
- 2026-08-14：定向时间测试、完整 Debug 构建、格式检查和非硬件 CTest 通过；更新公开文档和路线图并标记 completed。

## 决策记录

- DEC-001：生产探针和 Service 组合根接线留给 T1-03；T1-02 仅交付平台无关运行时和可注入端口，避免用伪生产探针提前满足硬件语义。
- DEC-002：跳变在 P0 中锁存降级且不自动恢复；P1-01 再实现重新收敛和恢复状态机。
- DEC-003：映射结果持有不可变模型 `shared_ptr`，结构性保证事件可保存创建时修订并避免后续模型回写。

## 意外发现

- 当前 PowerShell 会话没有把 CMake/Ctest 加入 `PATH`；验证使用 Visual Studio 2026 自带工具的绝对路径执行同一本机预设。
- C++ 无法安全强杀拒绝停止令牌和 deadline 的探针调用；运行时会按截止报告 `SYS_SHUTDOWN_TIMEOUT`，生产探针必须在 T1-03 满足可取消/有截止合同。
- T1-02 没有可装配的生产 Windows/MVS 探针，因此没有把 runtime 接入 Service 组合根；以伪生产探针接线会错误暗示硬件语义已实现。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-14 | 基线文档、源码和 `git status --short` | 通过 | 工作区干净；硬件未测试 |
| 2026-08-14 | `cmake --preset local-windows-vs2026-debug` | 通过 | 使用 VS 自带 CMake 绝对路径；配置生成成功 |
| 2026-08-14 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | 完整 Debug 构建，MSVC `/W4 /WX` |
| 2026-08-14 | `PaperBreakTests --gtest_filter=TimeModel*:TimeSyncRuntime*` | 通过，18/18 | 含来源、offset/漂移、切换、跳变、容量和关闭超时 |
| 2026-08-14 | `ctest --preset local-windows-vs2026-debug` | 通过，33/33 | 预设排除实体硬件集成测试 |
| 2026-08-14 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 通过 | 全仓 C++ 格式检查 |
| 2026-08-14 | `git diff --check` | 通过 | 无空白错误 |

## 完成摘要

新增平台无关的 `TimeSyncRuntime`、系统/相机探针端口、系统及四相机不可变发布槽、状态投影和 UTC↔单调映射。运行时在单线程中执行四级来源选择，传播 offset/uncertainty/稳定错误，锁存系统时间跳变降级，并以固定 16 条控制容量、独立停止令牌和 deadline join 确保有界关闭。RECEIVE_CLOCK 对缺失 ticks 的帧提供显式 DEGRADED 近似值。18 项定向测试、完整 Debug 构建、格式检查和非硬件 CTest 33/33 通过。Windows/MVS 生产探针、报警、schema v8、实体相机和同步精度未测试，留待 T1-03/V5-02；未开始 T1-03。
