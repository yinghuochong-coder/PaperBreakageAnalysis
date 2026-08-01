# M3-04：取流和自动恢复 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M3-04
- 关联需求：需求 4.5、4.6、8、11、16；架构 5.2～5.4、6.1、7.1～7.5、9.1、10.1、13～16.3

## 目的与可观察结果

Hikrobot 设备可在已有每相机采集线程中，把 MVS 原始帧直接写入预分配 `FrameBuffer`，安全映射厂商帧号、设备时间戳、丢包导致的不完整标记、尺寸、步长和受支持像素格式。运行会话能识别连续超时、断流、尺寸/格式变化等故障，停止并销毁当前设备后复用 M2 状态机与 1/2/5/10/30/60 秒有界退避重新枚举、连接和启流；停止请求取消取流等待和退避，不产生无界线程、队列或分配。

## 范围

### 范围内

- MVS `MV_CC_GetOneFrameTimeout` 的预分配限时拉流、帧字段/像素格式/错误映射和安全长度校验；
- 采集基线尺寸与格式变化检测、连续超时升级、断流错误上下文和现有队列溢出统计；
- 连接、启流、采集、停流、断开、退避重建的每相机会话编排；
- 伪 SDK、Mock 和自动化测试覆盖元数据、异常、超时/断流、变化、退避、溢出及关闭；
- 稳定业务码、MVS 原始码与白名单上下文文档。

### 范围外

- M3-05 相机测试工具、实体相机、四路目标吞吐/拔线/网卡性能记录；
- 软触发、用户参数集和恢复默认值（非 M3-04 验收项）；
- UI、IPC、预览、JPEG、推理、落盘、事件和后续里程碑；
- 改变 M2 已批准的固定容量与丢最旧队列策略。

## 当前基线

- `paperbreak_camera` 已有固定 `FrameBufferPool`、每相机 `AcquisitionQueue`（容量由调用方固定，满载丢最旧）、可停止 `AcquisitionWorker`、采集统计和 `CameraSessionController`/`ReconnectController`；目前采集线程只把非超时错误作为终止结果，未检测运行中尺寸/格式变化，也没有自动恢复编排。
- Hikrobot M3-01～M3-03 已有 SDK 隔离、句柄/流 RAII、发现/绑定和参数事务；`start_acquisition`、`capture_into`、`stop_acquisition` 仍返回未实现。
- MVS 4.8.0.3 提供 `MV_CC_GetOneFrameTimeout`，允许调用方提供预分配缓冲，并返回 `MV_FRAME_OUT_INFO_EX`；`MV_E_NODATA` 表示取流超时，`MV_E_NETER` 等表示链路/设备故障。
- 工作区包含父任务 M3-01～M3-03 尚未提交的修改；本任务只在其上增加 M3-04 文件和必要的同文件小范围编辑，不覆盖或删除已有改动。

## 前置条件与假设

- 生产路径采用每相机一个限时拉流线程，不注册业务图像回调；已有 C 回调边界保持仅做短小异常隔离，M3-04 不在回调中执行磁盘、网络、JPEG 或推理。
- 支持 `Mono8`、未打包 `Mono10`/`Mono12`、`BayerRG8`；MVS 返回未知或打包格式时以 `CAMERA_FRAME_FORMAT_CHANGED` 停止该路，不猜测步长。
- 设备时间戳由高低 32 位组合；只有能读取并验证 `GevTimestampTickFrequency` 时才标记有效，否则 `camera_timestamp` 为空，绝不虚构频率/同步质量。
- `nLostPacket > 0` 标记帧不完整；有效载荷、宽高、扩展宽高、步长均做溢出与预分配容量校验。
- 当前无实体相机；自动化只能证明 API 映射、所有权、故障与恢复语义，不能证明真实设备时间基准、驱动断线码、目标帧率或带宽。

## 设计说明

- 扩展 Hikrobot 私有 `MvsApi` 注入 `get_one_frame_timeout`。`HikrobotCameraDevice` 用已有 `StreamSession` 持有启流状态；`capture_into` 在设备互斥保护下把固定池缓冲传给 SDK，成功后只设置逻辑长度并构造厂商无关元数据。
- 首帧建立当前流的尺寸/格式基线；后续变化返回 `CAMERA_FRAME_FORMAT_CHANGED`，附带 expected/actual 宽高、格式和 payload 上下文，并由恢复会话停止该路，避免新布局未经预算校验进入管线。
- 采集工作线程增加可配置连续超时阈值；阈值内仅计数，达到阈值后以稳定 `CAMERA_FRAME_TIMEOUT` 终止并交给恢复编排。断流与格式变化立即终止。
- 新增每相机恢复会话，独占 Provider、逻辑/序列号、固定池和固定队列引用，在一个 `jthread` 中执行发现/创建/连接/启流/采集/清理/退避循环。所有失败先经过 `CameraSessionController::handle_failure`；恢复成功进入 Streaming 并重置退避。设备和采集 worker 每次尝试均由 RAII 销毁，其他相机无共享锁。
- 会话对外提供只读状态、采集与队列快照；错误保留业务码、可选 `nativeDomain/nativeCode`、操作、逻辑相机及 attempt/reason 等有界上下文。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `acquisition.frames[i]` | 每相机采集线程 | 每相机预处理消费者 | 调用方固定，架构默认 16 帧 | 丢弃最旧未处理帧，采集不阻塞 | 停采后关闭生产端；消费者可排空已有帧 | 深度、高水位、入/出、丢最旧、关闭拒绝、等待超时/取消 |
| 恢复控制 | 每相机会话线程内部 | 同一线程 | 不使用队列 | 不适用；状态机同步串行 | `stop_token` 取消取帧轮询和退避；析构确定性 join | 恢复次数、下次延迟、最后错误、状态转换序号 |

### 持久化与恢复

不修改配置 schema、数据库或用户数据。恢复状态仅驻留当前服务进程；每次成功回到 Streaming 后退避计数归零。进程重启后从已验证配置重新发现和连接。

### 错误和降级

- `CAMERA_FRAME_TIMEOUT`：单次为指标，连续达到配置阈值后可重试并进入 Recovering；无厂商码时保留 timeout/threshold 上下文，MVS 返回码时保留 `hikrobot-mvs` 原始码。
- `CAMERA_DISCONNECTED`：网络/句柄/设备异常，立即停止当前尝试，清理后有界退避重连。
- `CAMERA_FRAME_INCOMPLETE`：帧仍携带 incomplete 标记供采集统计和隔离策略使用，不重试同一帧。
- `CAMERA_FRAME_FORMAT_CHANGED`：尺寸、格式、步长或 payload 不符合当前预算；停止该路并进入恢复/最终 Faulted，绝不重新分配更大帧缓冲。
- `CAMERA_STREAM_START_FAILED`/`CAMERA_OPEN_FAILED`/`CAMERA_NOT_FOUND`：保留厂商原始码和 attempt，上限后进入 Faulted。
- 达到 M2 最大恢复次数后把最后错误置为不可重试并进入 Faulted；停止请求取消等待且不再安排重连。

## 实施步骤

- [x] 1. 扩展 Hikrobot 私有 API 表和设备实现，完成预分配限时取流、时间戳/帧号/不完整/几何/像素映射、长度校验及 MVS 原始错误翻译；以伪 API 测试映射、未知格式、缓冲不足、超时和断流。
- [x] 2. 扩展 `AcquisitionWorker` 连续超时阈值、首帧尺寸/格式基线和结构化变化错误，保持现有有界队列/池及确定性停止；补充超时阈值、变化、溢出和关闭测试。
- [x] 3. 新增每相机恢复会话，把 Provider/设备生命周期、M2 状态机、退避、采集 worker 和固定资源编排起来；补充发现/打开/启流/断流后恢复、次数耗尽及退避/采集中停止测试。
- [x] 4. 扩展 Mock/伪 SDK 测试以覆盖故障上下文和无需实体相机的恢复行为，确认其他相机及队列策略不受影响。
- [x] 5. 运行 OFF/ON Debug/Release 配置、构建、非硬件 CTest、格式、ON/OFF 静态分析、SDK 边界、安装路径、缺 SDK 负向配置和 `git diff --check`。
- [x] 6. 更新错误码说明、路线图 M3-04 状态、本计划进度/决策/发现/证据和完成摘要；不开始 M3-05。

## 验证计划

### 自动化测试

- 伪 MVS：FrameNum、设备 timestamp 高低位/frequency、宽高/扩展字段、stride、四种批准像素格式、lost packet/incomplete、payload 长度与目标缓冲；`MV_E_NODATA`、网络/句柄错误、未知格式和变化。
- 公共采集：连续超时阈值前继续、达到阈值退出；尺寸/格式变化立即退出；队列容量满丢最旧；池耗尽不无界分配；关闭/停止唤醒并按截止时间 join。
- 恢复会话：失败进入 Recovering，等待后 Connecting，重新创建/连接/启流并回到 Streaming；恢复次数耗尽 Faulted；停止取消退避和限时取流；业务码/native/context 保真。
- OFF/ON 全部非硬件回归和 SDK 版本/link smoke，不访问实体相机。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=OFF
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release -DPAPERBREAK_ENABLE_HIKROBOT=OFF
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release

cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=ON
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release -DPAPERBREAK_ENABLE_HIKROBOT=ON
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行 `format-check`、ON/OFF `local-windows-vs2026-static-analysis`、`hikrobot_sdk_boundary`、安装路径泄漏扫描、缺 SDK 预期配置失败及 `git diff --check`。

### 人工或硬件验证

- 环境：MVS 4.8.0.3、目标 MV-CS020-60GM、目标网卡/交换机；当前未提供实体相机。
- 步骤：单路取流核对元数据；连续运行；拔插相机网线和交换机链路；观察状态/退避/重连；逐路扩展到四路。
- 预期：帧字段与 MVS 一致，掉线只影响当前路，按退避恢复，停止不残留线程/句柄。
- 证据保存位置：本任务未执行硬件步骤；M3-05 在目标机保存日志、指标和硬件记录，不能以伪 SDK 结果替代。

## 回滚与恢复

本任务只修改相机公共采集/会话、Hikrobot 私有适配器、Mock/伪 SDK 测试和文档。失败时撤销 M3-04 增量即可保留 M3-03 可构建基线；不删除配置、SDK 或用户数据。运行时任何失败均先停止采集、停流和断开，释放池引用后再重试，不复用可能失效的厂商句柄。

## 验收标准

- [x] MVS 帧号、有效设备时间戳、不完整标记、尺寸/stride 和批准像素格式被安全映射；
- [x] 图像写入预分配池缓冲，每帧路径不申请大块内存，回调边界无磁盘/网络/JPEG/推理；
- [x] 连续超时、断流、尺寸/格式/长度变化产生稳定业务错误和受限上下文；
- [x] 自动恢复复用 M2 状态机与退避，成功复位、上限 Faulted、停止可取消且线程确定性退出；
- [x] 采集队列保持固定容量、丢最旧溢出策略、关闭语义和指标；
- [x] 所有 MVS 调用仍只在 Hikrobot 适配器，厂商码不作为唯一业务码；
- [x] 自动化覆盖映射、故障、恢复、溢出和关闭，OFF/ON Debug/Release 与静态检查通过；
- [x] 路线图和本计划记录真实证据及硬件限制，未开始 M3-05。

## 进度记录

- 2026-08-01：完整阅读项目约束、需求、架构、路线图、规划规范、M3-01～M3-03 ExecPlan、相机公共层/Mock/Hikrobot/CMake/测试及 MVS 4.8.0.3 取流字段；创建计划，状态 in-progress。
- 2026-08-01：完成 MVS 预分配限时取流、帧元数据/错误映射、连续超时和运行中布局变化检测，以及厂商无关恢复会话编排。
- 2026-08-01：完成伪 SDK/Mock 的映射、断流恢复、次数耗尽、队列溢出和停止测试；OFF/ON Debug/Release、静态分析与全部门禁通过，路线图回写，状态 completed。

## 决策记录

- DEC-001：生产取流采用 `MV_CC_GetOneFrameTimeout` 写入固定池缓冲，与现有每相机采集线程一致；不新增 SDK 图像回调中的业务路径。
- DEC-002：运行中尺寸/格式变化不在线扩容或接受新预算，返回 `CAMERA_FRAME_FORMAT_CHANGED` 并走会话恢复/故障路径。
- DEC-003：自动恢复在厂商无关会话编排层复用 `ICameraProvider`、`ICameraDevice` 和 M2 状态机；MVS 连接/取流调用仍全部留在适配器。
- DEC-004：每个启用相机最多一个恢复控制 `jthread` 和一个活跃采集 `jthread`；四路下数量固定有界，控制线程不处理帧数据，停止令牌同时取消采集和退避。

## 意外发现

- MVS 文档明确 `MV_CC_GetOneFrameTimeout` 由调用方提供缓冲，恰好匹配既有固定帧池；效率高的 `GetImageBuffer/FreeImageBuffer` 使用 SDK 所有缓冲，不适合当前不允许裸 SDK 指针跨边界的所有权规则。
- M2 已分别实现采集 worker 和恢复状态机，但尚缺少拥有设备重建生命周期的会话编排对象；只修改 Hikrobot `capture_into` 无法满足自动恢复验收。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | 文档、源码、测试与 MVS 4.8.0.3 头文件检查 | 通过 | 确认预分配拉流 API、字段与既有固定资源/状态机边界；未访问实体相机 |
| 2026-08-01 | 实体相机取流、拔线、四路吞吐 | 未执行 | 当前无实体相机和目标网络，保留为 M3-05 硬件门禁 |
| 2026-08-01 | OFF Debug/Release configure、build、CTest | 通过 | 两套均 18/18；通用 unit 可执行文件 148 项，不查找或链接 MVS |
| 2026-08-01 | ON Debug/Release configure、build、CTest | 通过 | 两套均 19/19；通用 unit 148 项、Hikrobot 24 项；仅伪 SDK、Mock 和版本/link smoke |
| 2026-08-01 | 伪 SDK 帧号、timestamp、丢包、几何、像素、超时/断流/非法布局 | 通过 | 直接写固定 `FrameBuffer`；MVS 原始码与业务码并存 |
| 2026-08-01 | Mock 断流恢复、重试耗尽、退避停止、队列满/关闭 | 通过 | 恢复回到 Streaming；上限 Faulted；stop 在 500 ms 测试截止内完成 |
| 2026-08-01 | ON/OFF MSVC 静态分析构建 | 通过 | `/analyze` 生产目标无构建失败 |
| 2026-08-01 | `format-check`、SDK 边界、安装路径泄漏、`git diff --check` | 通过 | SDK 引用仅在 `src/camera/hikrobot`；ON/OFF 安装树无注入路径文本 |
| 2026-08-01 | 缺失 MVS Development 根负向配置 | 预期失败 | 退出非零并明确报告 `PAPERBREAK_MVS_ROOT` 不存在 |

## 完成摘要

Hikrobot 设备现以 MVS 限时拉流把帧直接写入预分配缓冲，安全映射帧号、经频率验证的设备时间戳、不完整标记、几何和批准像素格式；超时/断流保留稳定业务码、厂商码和有界上下文。公共采集线程连续超时达到阈值后退出恢复，尺寸/格式变化在进入队列前停止；每相机恢复会话重建 Provider 设备并复用 M2 状态机/退避，成功复位、上限 Faulted、停止关闭固定池/队列并确定性 join。新增 3 项伪 MVS 取流测试、2 项采集异常测试和 3 项恢复测试；OFF/ON Debug/Release、CTest、格式、静态分析、SDK 边界、安装泄漏、缺 SDK 负向配置及 diff 检查通过。未访问实体相机，真实时间基准、驱动错误映射、拔线恢复、目标吞吐与四路资源趋势未验证，明确留给 M3-05；未开始 M3-05。
