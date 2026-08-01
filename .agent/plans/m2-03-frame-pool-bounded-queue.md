# M2-03：固定容量帧池和有界队列 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M2-03
- 关联需求：需求 4.5、4.6、7；架构 6.1、7.1～7.3、8.1～8.2、9.1

## 目的与可观察结果

为每路相机提供启动时预分配且运行期不扩容的帧缓冲池、采用 `drop-oldest` 的有界采集队列，以及只执行限时取帧、元数据填充和非阻塞入队的可停止工作线程。通过单元测试证明池耗尽、队列满、等待取消、并发关闭和重复租用不会导致无界增长或线程无法退出。

## 范围

### 范围内

- `paperbreak_camera` 内的 `FrameBufferPool`、`AcquisitionQueue` 和 `AcquisitionWorker`；
- 池/队列容量、深度、高水位、正常操作和背压结果快照；
- 工作线程的启动、停止、截止时间 join 和最后错误快照；
- 无 SDK、无硬件的单元测试与构建验证。

### 范围外

- M2-04 模拟相机、故障注入和四路稳定运行；
- M2-05 帧率、带宽、帧号跳变、健康监控注册和预处理骨架；
- Hikrobot MVS 适配、真实硬件验证、磁盘、网络、JPEG 和推理。

## 当前基线

- M2-01 已提供固定容量 `FrameBuffer`、`FramePacket`、`FrameView` 和同步 `ICameraDevice::capture_into`；
- M2-02 已提供相机会话状态与重连控制器，但尚无常驻采集线程；
- 配置已有 `framePoolCapacity`、`queueCapacity`、`receiveTimeoutMs`，并校验帧池容量不小于队列容量；
- 架构规定 `acquisition.frames[i]` 默认采用 `drop-oldest`，采集侧不得阻塞下游；
- 工作区在任务开始时无已报告的未提交修改。

## 前置条件与假设

- `AcquisitionWorker` 引用的设备、帧池和队列生命周期均长于工作线程；
- 设备已由外层 `CameraSession` 启流，工作线程不连接、配置、启停设备或驱动状态机；
- `capture_into` 遵守有限超时契约，真实 SDK 调用取消能力留到 M3 验证；
- 不访问 MVS SDK 或实体相机，不把自动化测试描述为硬件测试。

## 设计说明

- 帧池构造时创建固定数量、统一字节容量的缓冲。租约删除器捕获共享内部状态，最后引用释放时清空并归还；即使外层池对象已销毁也不悬空。
- 池获取明确返回 acquired、exhausted、timeout、stopped、closed，不以异常表示正常资源竞争。
- 采集队列用预分配环形槽位保存 `FramePacket`；满载时同步释放最旧槽并接纳最新帧。消费者在关闭后先排空，再观察 closed。
- 工作线程通过 `std::jthread` 和停止令牌关闭；池等待可取消，设备取流受 `receiveTimeout` 上界约束。帧接收成功后生成服务序号和两种接收时间，随后立即非阻塞入队。
- `CAMERA_FRAME_TIMEOUT` 继续采集；其他设备错误结束线程并保留错误；join 超时返回 `SYS_SHUTDOWN_TIMEOUT`。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `acquisition.frames[i]` | 单路 `AcquisitionWorker` | 后续每相机预处理线程 | 配置 `queueCapacity` | 丢弃最旧、接纳最新、生产者不等待 | 停采后关闭；消费者排空后返回 closed；stop token 可取消等待 | 容量、深度、高水位、入/出、丢弃、关闭拒绝、超时、取消 |

帧池不是队列，但其等待同样有明确超时、停止和关闭语义，并暴露容量、空闲、在用、高水位、获取、耗尽、超时和取消计数。

### 持久化与恢复

不适用。本任务不修改配置 schema、数据库、文件格式或用户数据。容量变更通过停止并重建对应运行时对象完成。

### 错误和降级

- 池耗尽、队列覆盖和等待超时使用状态值及计数，不创建高频业务错误；
- `CAMERA_FRAME_TIMEOUT` 是可恢复取流结果，工作线程继续；
- 其他设备业务错误保留并终止当前工作线程，交由后续会话编排恢复；
- join 超过共享截止时间返回 `SYS_SHUTDOWN_TIMEOUT`；
- 关闭后拒绝生产，不扩容、不回退为临时大块堆分配。

## 实施步骤

- [x] 1. 新增固定容量帧池公共接口和实现，覆盖租约、安全析构、等待、关闭和快照。
- [x] 2. 新增预分配环形采集队列，覆盖 `drop-oldest`、可取消限时消费、排空关闭和快照。
- [x] 3. 新增采集工作线程，限定职责并实现确定性停止和最后错误保留。
- [x] 4. 将源码与单元测试接入 CMake，覆盖池、队列、并发关闭及采集路径。
- [x] 5. 运行格式、Debug/Release、非硬件 CTest 和 MSVC 静态分析。
- [x] 6. 更新路线图状态、验证证据和完成摘要，复核无无关修改。

## 验证计划

### 自动化测试

- 帧池预分配、立即耗尽、限时等待、停止、关闭、跨外层对象生命周期归还和地址集合稳定；
- 队列容量边界、丢弃最旧顺序、指标、超时、停止、关闭排空和并发 close；
- 工作线程元数据、序号、池等待取消、队列满不阻塞、取流超时继续、永久错误退出和截止时间 join。

### 构建与测试命令

```powershell
cmake --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug --target format-check
ctest --preset windows-vs2026-debug
cmake --preset windows-vs2026-release
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-release
cmake --preset windows-vs2026-static-analysis
cmake --build --preset windows-vs2026-static-analysis
```

### 人工或硬件验证

- 环境：未提供 MV-CS020-60GM 或 MVS SDK。
- 步骤：本任务不执行硬件验证。
- 预期：M3 使用真实适配器验证 SDK 取流超时、停止和缓冲复制行为。
- 证据保存位置：本计划验证证据表记录“未执行”。

## 回滚与恢复

本任务只增加相机源码、测试和文档记录，不修改用户数据。若实现失败，逐项撤销 M2-03 新增文件及 CMake/路线图登记即可恢复 M2-02 基线；不使用破坏性 Git 命令。

## 验收标准

- [x] 固定数量缓冲只在池构造时分配，池耗尽不扩容；
- [x] 采集队列容量固定且满载采用 `drop-oldest`，生产者不等待；
- [x] 池和队列等待均可超时、取消和关闭，所有线程有确定性退出路径；
- [x] 采集工作线程只执行获取帧、填元数据和入队；
- [x] 池、队列和工作线程行为测试通过；
- [x] Debug/Release、格式、非硬件 CTest 和静态分析验证完成；
- [x] 未修改无关文件，未开始 M2-04/M2-05。

## 进度记录

- 2026-08-01：创建计划，状态 in-progress；已复核需求、架构、路线图、M2-01/M2-02 和现有配置模型。
- 2026-08-01：完成帧池、环形采集队列、采集工作线程及首轮单元测试实现，待构建验证。
- 2026-08-01：收紧停止优先级和关闭后拒绝语义，完成 Debug/Release、格式、非硬件 CTest 和静态分析验证，状态 completed。

## 决策记录

- DEC-001：采集队列固定采用架构基线的 `drop-oldest`，不新增运行期策略配置。
- DEC-002：池租约删除器捕获共享内部状态，避免租约晚于池外层对象时访问悬空指针。
- DEC-003：工作线程不拥有设备启停和重连，保持 M2-03 与后续会话编排边界。

## 意外发现

- 仓库公共 `windows-vs2026-*` 预设依赖调用进程提供外部 SDK 环境变量；当前进程未提供 `OpenCV_DIR`，因此直接配置失败。使用未提交且已存在的 `local-windows-vs2026-*` 用户预设注入相同本机依赖路径后完成全部验证。
- `clang-format.exe` 已随 Visual Studio LLVM 工具安装，但不在当前 PATH；将其目录临时加入当前验证命令 PATH 后格式检查通过，未修改机器环境。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | `cmake --preset windows-vs2026-debug` | 未完成 | 当前进程缺少 `OpenCV_DIR`，CMake 无法找到 OpenCV 4.12.0 |
| 2026-08-01 | `cmake --preset local-windows-vs2026-debug`、`cmake --build --preset local-windows-vs2026-debug` | 通过 | MSVC Debug x64，MVS 适配关闭 |
| 2026-08-01 | 临时加入 VS LLVM 路径后执行 Debug `format-check` | 通过 | 新增和既有 C++ 文件均符合 `.clang-format` |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 非硬件 17/17；unit 120 项，其中本任务新增 11 项 |
| 2026-08-01 | M2-03 新增 11 项测试连续重复 20 轮 | 通过 | 池/队列并发关闭、取消和工作线程截止时间场景无偶发失败 |
| 2026-08-01 | `cmake --preset local-windows-vs2026-release`、Release build、Release CTest | 通过 | 非硬件 17/17 |
| 2026-08-01 | `cmake --preset local-windows-vs2026-static-analysis`、static-analysis build | 通过 | MSVC `/analyze`，无构建失败 |
| 2026-08-01 | 实体相机/MVS SDK | 未执行 | 当前任务不访问硬件或供应商 SDK |

## 完成摘要

新增固定容量帧池、有界 `drop-oldest` 采集队列和可停止采集工作线程。池租约通过共享内部状态安全归还，池/队列等待支持超时、停止与关闭，队列关闭后排空已接纳帧，工作线程保留永久设备错误并使用截止时间 join。新增 11 项单元测试；Debug/Release、格式、非硬件 CTest 和 MSVC 静态分析均通过。未访问 MVS SDK 或实体相机，未实现 M2-04/M2-05。
