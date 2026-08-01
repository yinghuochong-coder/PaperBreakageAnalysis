# M2-04：模拟相机和故障注入 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M2-04
- 关联需求：需求 4.5、阶段 M2、16；架构 4.3、5.2、9.1、19

## 目的与可观察结果

提供不依赖 MVS SDK 或实体相机的独立模拟相机适配器。自动化测试可创建一至四路相机，按配置生成确定性图像，通过连续、软触发或模拟硬触发采集，并可重现掉线、超时、丢帧、不完整帧、帧号跳变及格式变化。

## 范围

### 范围内

- 独立 `paperbreak_camera_mock` 目标、模拟 Provider/Device 和线程安全测试控制 API；
- 固定容量故障与触发控制、确定性图案及脚本化故障；
- 单元测试、四路模拟集成测试、构建和静态分析验证。

### 范围外

- M2-05 帧统计、预处理与处理线程池；
- 服务组合根、IPC、CLI 和生产配置 schema；
- Hikrobot MVS SDK、实体相机与真实硬触发测试。

## 当前基线

- M2-01 已定义 `ICameraProvider`、`ICameraDevice`、能力/参数、帧元数据和业务错误；
- M2-02 已提供相机会话状态与重连控制器；
- M2-03 已提供固定容量帧池、`drop-oldest` 队列和可停止采集工作线程；
- 架构要求 mock 为独立目标且默认测试不依赖 MVS SDK；工作区开始时无未提交修改。

## 前置条件与假设

- 测试控制入口采用 API，不新增 `camera-simulator` CLI；
- Mono10/Mono12 使用每像素 16 位的小端非打包表示，Mono8/Bayer RG8 每像素 8 位；
- 模拟器本身不创建后台线程，取帧调用方拥有线程和关闭期限；
- 不访问或声称验证任何实体硬件和供应商 SDK。

## 设计说明

- `MockCameraProvider` 在构造前通过工厂校验 1～4 路配置、唯一序列号、有限正帧率、有效几何、像素格式负载和控制容量。
- Provider 与 Device/Control 通过每路共享状态协作；Provider 保持状态所有权，设备和控制句柄可安全唤醒等待中的取帧调用。
- 连续模式按 `steady_clock` 截止时间出帧；软/硬触发使用固定上限计数器。停止、断开和模式切换清空触发并通知等待者。
- 初始脚本按取帧序号排序；运行期故障队列固定容量并采用 reject-newest。故障只在一次取帧机会消费，掉线除外，需重新连接恢复。
- 图案直接写入调用方预分配的 `FrameBuffer`，不在热路径创建图像缓冲；随机图案由种子、帧号和像素位置纯函数生成，保证重现。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 运行期故障队列 | 测试控制线程 | 取帧调用线程 | 每路配置，1～1024 | reject-newest | `clear_faults` 清空；设备销毁随共享状态释放 | 深度、已接纳、已拒绝、已执行 |
| 软/硬触发计数 | 控制/设备调用线程 | 取帧调用线程 | 每路配置，1～1024 | reject-newest | 停采、断开、模式切换时清零并唤醒 | 当前待处理和拒绝数 |

模拟器不新增常驻线程。现有 `acquisition.frames[i]` 队列语义不变。

### 持久化与恢复

不适用。本任务不写文件、不修改 schema 或用户数据；掉线通过显式重新连接和启流恢复。

### 错误和降级

- 非法配置、控制队列满、缓冲不足和非法状态返回稳定相机业务错误；
- 超时返回 `CAMERA_FRAME_TIMEOUT`，掉线返回 `CAMERA_DISCONNECTED`；
- 不完整帧以成功元数据的 `incomplete` 标志表达；丢帧推进相机号并在下一成功帧形成缺口；
- 尺寸/格式变化不得超过已声明最大负载，绝不临时扩容。

## 实施步骤

- [x] 1. 新增 mock 公共配置、故障、控制、Provider 工厂接口及独立 CMake 目标。
- [x] 2. 实现生命周期、参数能力、三种触发模式、确定性图案和固定容量控制通道。
- [x] 3. 实现脚本/运行期故障及掉线重连语义。
- [x] 4. 增加 mock 单元测试和四路采集模拟集成测试并接入 CTest。
- [x] 5. 执行格式、Debug/Release、非硬件 CTest、重复模拟测试和 MSVC 静态分析。
- [x] 6. 更新路线图、验证证据和完成摘要，复核未开始 M2-05。

## 验证计划

### 自动化测试

- Provider 数量、唯一标识、非法配置和未知设备；
- 四种像素格式、三种图案、相同种子的逐字节重现；
- 生命周期、参数回读、连续/软/硬触发、超时和停止唤醒；
- 七类故障、脚本顺序、队列满载、掉线重连；
- 四路连接现有帧池、采集工作线程和有界队列并隔离单路故障。

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

公共预设缺少本机依赖环境时，使用现有 `local-windows-vs2026-*` 用户预设并如实记录。

### 人工或硬件验证

- 环境：未提供 MV-CS020-60GM 或 MVS SDK。
- 步骤：本任务不执行硬件验证。
- 预期：模拟测试通过不代表真实取流、带宽或硬触发通过。
- 证据保存位置：验证证据表记录未执行原因。

## 回滚与恢复

本任务只新增 mock 源码/测试并更新 CMake 和任务文档，不修改用户数据。失败时逐项撤销本任务文件及登记即可恢复 M2-03 基线，不使用破坏性 Git 命令。

## 验收标准

- [x] 可配置并枚举一至四路相机，所有自动化测试不依赖 MVS SDK；
- [x] 分辨率、帧率、像素格式、图案和随机种子行为可重现；
- [x] 连续、软触发和模拟硬触发均有有限等待与确定性停止；
- [x] 七类故障可脚本化并由运行期 API 控制，掉线可重新连接恢复；
- [x] 所有控制通道容量、满载、清空和唤醒语义有测试；
- [x] Debug/Release、格式、非硬件 CTest 和静态分析完成；
- [x] 未修改无关文件，未开始 M2-05。

## 进度记录

- 2026-08-01：创建计划，状态 in-progress；已复核需求、架构、路线图及 M2-01～M2-03 基线。
- 2026-08-01：完成独立 mock 目标、生命周期、确定性图案、触发控制和七类故障实现，新增单元与四路模拟测试。
- 2026-08-01：完成 Debug/Release、格式、非硬件 CTest、20 轮重复测试和静态分析，状态 completed。

## 决策记录

- DEC-001：使用测试 API 而非 CLI，避免提前扩展服务组合根和进程边界。
- DEC-002：mock 单独建目标，保持生产相机接口与实现目标的依赖方向。
- DEC-003：故障和触发通道采用固定容量 reject-newest，不引入无界控制队列。

## 意外发现

- 默认进程 PATH 不包含 Visual Studio LLVM 的 `clang-format.exe`；首次格式目标因此失败。将已安装的 VS LLVM 目录临时加入该验证命令 PATH 后通过，未修改机器环境。
- 100 fps 下用 1 ms 验证“下一帧必超时”会受测试自身开销影响；测试改用 10 fps 明确拉开周期后稳定验证限时节拍，未放宽实现语义。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | 实体相机/MVS SDK | 未执行 | 当前任务仅实现模拟器 |
| 2026-08-01 | `cmake --preset local-windows-vs2026-debug`、Debug build | 通过 | 使用现有用户预设注入本机 Qt/OpenCV/vcpkg 路径，MVS 关闭 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 非硬件 17/17；unit 入口 129 项，simulation 入口含四路模拟集成测试 |
| 2026-08-01 | Debug `format-check` | 首次失败后通过 | 初次 PATH 缺少 clang-format；临时加入 VS LLVM 路径后通过 |
| 2026-08-01 | `CameraMock*:SimulationMockCamera.*` 连续 20 轮 | 通过 | 每轮 9 项 mock 单元测试和 1 项四路模拟测试 |
| 2026-08-01 | Release configure/build、`ctest --preset local-windows-vs2026-release` | 通过 | 非硬件 17/17 |
| 2026-08-01 | static-analysis configure/build | 通过 | MSVC `/analyze`，无构建失败 |

## 完成摘要

新增独立 `paperbreak_camera_mock` 库及公共配置、故障、控制和 Provider API。模拟设备支持一至四路、四种像素格式、三种确定性图案、连续/软/模拟硬触发和七类一次性故障；故障与触发通道均固定容量并采用 reject-newest，停采和断开会唤醒等待。新增 9 项单元测试和 1 项四路模拟集成测试；Debug/Release、格式、非硬件 CTest、20 轮重复测试及静态分析均通过。未访问 MVS SDK 或实体相机，未实现 M2-05。
