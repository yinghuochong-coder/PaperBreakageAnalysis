# M6-03/M6-04：算法降采样、最新帧调度与时间确认 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-13
- 最后更新：2026-08-13
- 路线图条目：M6-03 候选确认和降级；M6-04 UI 配置和结果叠加
- 关联需求：需求 4.5～4.12、4.20、7、10、13；架构 7～9；配置 schema v5

## 目的与可观察结果

相机继续按原帧率采集并把全部帧写入内存环及事件原始窗口；自动检测按每相机配置的 15/30/60 FPS 只处理节拍截止时最新的一帧，人工触发保留请求后的第一帧并绕过节拍。传统视觉在原图坐标裁剪 ROI 后按 1、1/2 或 1/4 分析，结果坐标仍为原图 ROI。候选确认从连续帧数改为单调时间持续区间，默认 120 ms。新配置默认 `half + 15 FPS + 120 ms`，v2～v4 迁移为 `disabled + 60 FPS` 并按旧帧数换算确认时间。

## 范围

### 范围内

- schema v5、v2～v4 迁移、严格校验、统一 v5 序列化和配置历史回滚说明。
- C++ 配置、算法 IPC、Qt Console 的降采样/FPS/确认时间编辑与展示。
- classical-vision 原图 ROI 裁剪、`INTER_AREA` 降采样、工作区/背景复用和按处理 FPS 换算 EMA。
- 每相机容量 1 自动 latest-wins 槽、容量 1 人工保留槽、节拍、错过周期、正常抽样指标和有界停止/热重配。
- 基于检测结果单调时间的确认区间、两周期新鲜度、外部确认和时间回退。
- 单元/模拟/IPC/Console/集成测试、Debug/Release 构建、格式、静态分析和文档。

### 范围外

- 调整相机采集率、采集分辨率、预览源、事件原始帧、检测阈值或结果队列容量。
- 正式数据集准确率验收、阈值自动重标定、MVS 适配器边界变更或后续里程碑。
- 未实际执行的实体相机、生产 CPU/检测效果或四路硬件吞吐结论。

## 当前基线

- schema v4 的 `algorithm.consecutiveFrames` 同时供候选状态机确认计数；默认部署为 7 帧。
- 每相机算法 Lane 是容量 8 的 `deque`，满时 drop-oldest 并触发积压窗口；人工请求通过保护目标序号避免被丢弃。
- 内存环和 `latest_frame` 在算法队列之前接收全部提交帧；结果队列容量 256 并跨相机按单调时间、相机 ID、序号排序。
- classical-vision 对完整原图 ROI 融合扫描，背景为 ROI 尺寸，正常帧固定 0.02 EMA；热更新重建宿主并清空状态。
- IPC/Console 使用 `consecutiveFrames`，运行指标已有输入/处理 FPS、队列等待、跳帧和积压。
- 任务开始时 `git status --short` 无输出；已有计划和源码修改均不属于未提交用户改动。

## 前置条件与假设

- 自动化测试使用内存帧/Mock 检测器；实体相机、MVS SDK 行为和生产检测效果只能人工验证。
- 旧帧数按 `ceil(consecutiveFrames / 60 s × 1000 / 10) × 10 ms` 迁移，故 7 帧为 120 ms。
- `confirmationDurationMs` 不超过现有候选超时（由 `event.maxEventSeconds` 换算）；使用 64 位算术避免溢出。
- `testCurrentFrame` 使用当前降采样参数但创建隔离检测器，因而不受自动节拍限制且不污染活动背景。
- M6-00 仍 blocked，传统视觉继续标记 prototype。

## 设计说明

### 接口与数据结构

- 增加 `DownsampleMode { disabled, half, quarter }` 和 `ProcessingFps { fps15, fps30, fps60 }` 强类型；JSON/IPC 使用计划指定字符串/整数。
- `AlgorithmConfig` 删除连续帧并增加降采样、配置 FPS和确认毫秒。检测器参数传入降采样因子及按 `1-(1-0.02)^(60/fps)` 计算的 EMA。
- 降采样尺寸为 `max(1, floor(original/factor))`；检测输出 `evaluated_region` 始终保持原图 ROI，不缩放事件或预览坐标。
- 候选相机跟踪器保存确认区间起点和最近合格结果时间；相邻合格间隔大于两个周期时从当前结果重启区间。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `algorithm.autoLatest[i]` | 每相机帧分发 | 同相机算法 worker | 1 | latest-wins；覆盖计入正常抽样跳过，不触发积压 | 停止接收后最多处理一张最新帧 | 深度/高水位、输入/处理/config FPS、sampledSkipped |
| `algorithm.manualReserved[i]` | 人工请求后的首帧 | 同相机算法 worker | 1 | 已有请求时拒绝重复请求；目标帧不被自动槽覆盖 | 停止/热重配最多处理一张人工帧 | pending、处理计数 |
| `algorithm.results` | 最多四个算法 worker | 单事件线程 | 256 | 拒绝并降级来源 Lane；既有策略不变 | 生产端结束后排空并按既有安全水位排序 | 深度、高水位、拒绝 |

首张自动帧立即处理；其后 worker 以检测启动时间为基准保证相邻自动调用至少一个配置周期。没有新修订不重复处理。人工帧优先且绕过节拍。检测完成晚于下一节拍时累计跨过的周期为 `missedProcessingSlots`，沿用五窗口积压/降级策略；配置导致的 latest-wins 覆盖只计 `sampledSkippedFrames`。

### 持久化与恢复

- 读取 v2～v4 时在内存迁移到 v5：`disabled + 60 FPS + 换算确认毫秒`；保存/原子历史均写 v5。
- 新安装默认文件写 `half + 15 + 120`。旧程序不认识 v5；回滚旧二进制前必须通过配置历史恢复 v4 文件，不直接编辑或降写 v5。
- 运行热更新先构建新检测器/线程组，停止旧生产、唤醒并有界排空后交换；失败保留活动配置。

### 错误和降级

- 非法枚举/FPS/时长、时长超过候选超时、降采样后无有效尺寸均返回稳定配置/处理错误。
- 正常抽样不产生 `ALGORITHM_QUEUE_BACKLOG`；检测慢导致的 missed slots 进入既有报警与五坏窗口降级。
- 单帧检测失败继续按相机报警但不停止后续检测；结果队列拒绝仍只降级来源 Lane。
- 时间戳回退拒绝结果；外部信号到达时最近合格结果超过两周期不得确认。

## 实施步骤

- [x] 1. 升级配置模型/解析/迁移/序列化和默认文件，补齐 v2～v5 严格测试。
- [x] 2. 扩展检测器参数并实现 ROI 后降采样、工作区复用、原坐标结果和 FPS 等效 EMA 测试。
- [x] 3. 把候选确认改为持续时间与两周期新鲜度，覆盖边界、跌落、间隔、外部信号和时间回退。
- [x] 4. 把 Lane 改为自动/人工双单槽，实施节拍、latest-wins、missed slots、四路隔离和有界停止/重配。
- [x] 5. 贯通 IPC、监控指标和 Console 两个枚举下拉框/确认时间字段，保持当前帧测试隔离。
- [x] 6. 更新需求、架构、配置、IPC、路线图和计划证据。
- [x] 7. 执行 Debug/Release 全量验证、格式、MSVC 静态分析和 `git diff --check`；记录硬件未验证项。

## 验证计划

### 自动化测试

- 配置：三种降采样×三种 FPS、非法枚举/数值、时长范围/依赖、v2～v4 迁移、严格字段、v5 回环。
- 算法：全帧/显式 ROI、奇数/极小尺寸、三模式、原坐标、背景初始化/保护、15/30/60 EMA、热更新清空。
- 状态机：120 ms 在三种 FPS 的准确/量化边界、阈值跌落、两周期边界、超界重置、外部信号、超时和回退。
- 调度/集成：60 FPS 输入限制、截止时最新序号、无新帧不重复、正常抽样无报警、慢检测 missed/降级、四路隔离、人工保留、停止/重配、内存环原始帧完整、IPC/Console 指标。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行仓库格式检查、MSVC 静态分析和 `git diff --check`，只记录实际执行结果。

### 人工或硬件验证

- 环境：目标 Windows 工控机、最多四台 MV-CS020-60GM、三种降采样与 15/30/60 FPS 组合。
- 步骤：记录输入/处理/config FPS、CPU、处理耗时、sampled skipped、missed slots、报警、检测效果和事件原始窗口序列。
- 预期：采集/事件原始帧不因算法抽样缺失；正常抽样无积压报警；慢处理可观察并按五窗口降级。
- 证据：后续 `docs/validation/m6-03-04/` 硬件记录；本任务没有实际设备时标记“未执行”。

## 回滚与恢复

- 代码回滚不得删除配置历史或事件数据。回滚旧程序前从配置历史恢复最后一个 v4 文件；旧程序不能直接读取 v5。
- 热重配失败继续使用旧线程组/检测器/配置；新线程准备失败不停止活动管线。
- 结果队列、内存环和事件文件格式不变，不需要数据迁移。

## 验收标准

- [x] v5 接口、迁移、严格校验、默认/旧行为和 v5 序列化符合计划。
- [x] 降采样、原坐标、EMA、背景重置与当前帧测试符合计划。
- [x] 自动/人工双单槽、节拍、latest-wins、missed slots、有界关闭和四路隔离符合计划。
- [x] 时间确认及外部信号新鲜度在 15/30/60 FPS 测试通过。
- [x] 内存环、事件原始窗口、IPC、Console、指标和文档同步。
- [x] Debug/Release、格式、静态分析和差异检查完成或如实记录阻断。
- [x] 实体相机、目标工控机四路性能与正式数据集均明确记录为未执行，未作硬件结论。

## 进度记录

- 2026-08-13：读取需求、架构、路线图、ExecPlan 规范、既有 M6 计划和当前代码；确认工作树干净，创建计划并开始实施。
- 2026-08-13：完成 schema v5、降采样、双单槽调度、时间确认、IPC/Console/指标与文档；完成 Debug/Release 自动化验证并记录静态分析既有阻断和硬件未验证项。

## 决策记录

- DEC-001：正常算法节拍丢弃与处理能力积压分开计数；只有 missed slots 进入既有积压报警/五窗口降级。
- DEC-002：人工请求后的第一帧使用独立容量 1 槽，不与 latest-wins 自动槽竞争。
- DEC-003：降采样只在 classical-vision 的 ROI 内部发生，公共检测结果继续使用原图坐标。
- DEC-004：v2～v4 迁移保持 disabled/60 FPS，避免升级改变既有检测；新安装默认使用 half/15 FPS。

## 意外发现

- 既有 M6-03/M6-04 性能跟进计划曾明确禁止抽样、降采样和容量变更，本任务因此使用新的独立 ExecPlan。
- `monotonic_now` 测试时钟不能直接作为 `condition_variable::wait_until` 的真实时钟截止点；worker 改为按剩余周期 `wait_for`，生产语义不变且确定性测试不再忙等。
- 全仓 MSVC 静态分析在未修改的 `src/storage/src/nvme_cache.cpp:73` 报既有 C28020；随后直接运行 `paperbreak_service_core.vcxproj /t:ClCompile`，确认任务涉及的 service-core 源以 `/analyze` 编译为 0 警告。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-13 | `git status --short` | 通过 | 任务开始时无输出 |
| 2026-08-13 | `PaperBreakTests.exe` | 通过 | 417 项：415 通过，2 项按设计跳过（性能/硬件） |
| 2026-08-13 | Debug 配置与全量构建 | 通过 | VS 2026、Qt、OpenCV、MVS 和 vcpkg 本机预设 |
| 2026-08-13 | `ctest --preset local-windows-vs2026-debug` | 通过 | 30/30 非硬件测试 |
| 2026-08-13 | Release 配置与全量构建 | 通过 | `/W4 /WX` |
| 2026-08-13 | `ctest --preset local-windows-vs2026-release` | 通过 | 30/30 非硬件测试 |
| 2026-08-13 | `format-check` | 通过 | 全仓 clang-format 检查 |
| 2026-08-13 | 全仓 MSVC 静态分析 | 阻断 | 未修改的 `src/storage/src/nvme_cache.cpp:73` 既有 C28020 |
| 2026-08-13 | service-core `ClCompile` `/analyze` | 通过 | 0 警告、0 错误；覆盖 event runtime、system commands、algorithm metrics |
| 2026-08-13 | `git diff --check` | 通过 | 无空白错误；换行转换提示不影响结果 |
| 2026-08-13 | 实体相机/目标工控机/正式数据集 | 未执行 | 不以 Mock 或模拟测试替代硬件及准确率结论 |

## 完成摘要

已完成计划内软件实现与自动化验证。schema v5、传统视觉降采样、最新帧节拍、人工保留槽、时间确认、IPC/Console 和指标已经贯通；采集原始帧与事件窗口由集成测试保持连续。全仓静态分析仅剩既有存储告警阻断，任务源的 `/analyze` 定向编译通过。实体相机、目标工控机四路性能和正式数据集准确率未执行，仍需后续硬件证据，且 M6-00 原型门禁继续 blocked。
