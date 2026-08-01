# M2-01：相机接口和领域模型 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M2-01
- 关联需求：需求 4.3～4.6、7、11、13、16；架构 5～8、14、19

## 目的与可观察结果

在不安装 MVS SDK、不连接实体相机的条件下，提供可由 Mock 和 Hikrobot 适配器共同实现的厂商无关相机接口、能力/参数模型和零拷贝只读帧模型。单元测试证明能力校验、序列号绑定、固定容量缓冲和公共接口契约。

## 范围

### 范围内

- `ICameraProvider`、`ICameraDevice` 及设备发现/绑定模型；
- 能力描述、参数快照和参数校验；
- `PixelFormat`、`FrameBuffer`、`FramePacket`、`FrameView`；
- 稳定相机业务错误映射和无硬件单元测试。

### 范围外

- M2-02 状态机与重连；
- M2-03 帧池、有界队列和采集工作线程；
- M2-04 模拟相机与故障注入；
- Hikrobot MVS SDK、真实相机和硬件吞吐验证。

## 当前基线

- 工作区开始时干净；
- `paperbreak_camera` 仅包含 `module_name()` 占位 API；
- 配置模块已有独立的 `config::PixelFormat` 和相机配置 DTO；
- 2026-08-01 使用 `local-windows-vs2026-debug` 完成基线配置、构建和非硬件 CTest，17/17 通过，unit 入口 87 项测试；
- 仓库文档示例 `windows-msvc-debug` 不存在，实际使用 `local-windows-vs2026-*` 预设。

## 前置条件与假设

- 采用同步、有限超时、调用方缓冲的拉取式采集端口；
- 适配器提供设备帧元数据，后续 `CameraSession` 负责逻辑相机 ID、服务序号和接收时钟；
- 配置枚举保持独立，映射留给后续服务编排任务；
- 无 MVS SDK 或实体相机验证条件，不将任何自动化测试描述为硬件测试。

## 设计说明

- 公共头文件只依赖 C++20 标准库和 `paperbreak_common`；
- `FrameBuffer` 构造时固定分配容量，之后仅改变逻辑长度；
- `FramePacket` 和 `FrameView` 通过 `shared_ptr<const FrameBuffer>` 共享只读所有权；
- 能力以可选步进范围、受支持枚举集合和数字 IO 描述，参数快照中不存在的字段表示设备不支持；
- 绑定辅助函数拒绝空序列号、设备缺失和重复序列号；
- 参数验证在写设备前完成，覆盖支持性、范围、步进、ROI 和触发组合。

### 线程和队列

不适用。本任务不创建线程或跨线程队列；只定义后续采集线程使用的同步接口和不可变帧所有权模型。

### 持久化与恢复

不适用。本任务不修改配置 schema、数据库、文件格式或用户数据。

### 错误和降级

- `CAMERA_NOT_FOUND`：绑定序列号未发现，可重试；
- `CAMERA_CONFIG_FAILED`：重复序列号、参数不支持/越界/步进或组合非法，不自动重试；
- `CAMERA_OPEN_FAILED`、`CAMERA_ACCESS_DENIED`、`CAMERA_STREAM_START_FAILED`、`CAMERA_FRAME_TIMEOUT`、`CAMERA_DISCONNECTED`：供设备实现使用；
- 原生诊断仅作为 `nativeDomain`/`nativeCode` 附加字段，禁止成为唯一业务错误。

## 实施步骤

- [x] 1. 增加帧和相机公共头文件及实现，保留 `module_name()` 兼容 API。
- [x] 2. 实现序列号绑定、参数能力校验、固定容量缓冲和只读帧视图校验。
- [x] 3. 增加相机单元测试并接入 CMake/CTest unit 标签。
- [x] 4. 运行格式、Debug/Release、静态分析和全部非硬件 CTest。
- [x] 5. 更新路线图、计划证据和完成摘要，复核无无关修改。

## 验证计划

### 自动化测试

- 不同能力集合、支持/不支持/越界/非步进参数；
- 缺失、唯一和重复设备序列号；
- 固定容量缓冲、超容量拒绝、帧元数据、零拷贝只读视图和非法布局；
- 测试内假 Provider/Device 完整实现公共接口，无 SDK 依赖。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug --target format-check
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
```

### 人工或硬件验证

- 环境：未提供实体 MV-CS020-60GM 或 MVS SDK。
- 步骤：未执行。
- 预期：后续 M3 任务验证真实发现、参数回读和取流。
- 证据保存位置：本计划验证证据表中明确记录“未执行”。

## 回滚与恢复

本任务只增加源码、测试和文档，不修改用户数据。失败时逐项撤销 M2-01 新增文件及 CMake/文档登记，即可恢复原占位模块；不删除配置或构建环境。

## 验收标准

- [x] 公共接口不泄漏 MVS、Win32 或 Qt 类型；
- [x] 能力、参数和绑定失败返回稳定业务错误；
- [x] 帧对象固定容量、只读共享且布局可校验；
- [x] Debug/Release、格式检查、静态分析和非硬件 CTest 通过；
- [x] 无无关文件修改，硬件验证限制明确。

## 进度记录

- 2026-08-01：创建计划并开始 M2-01，状态 in-progress。
- 2026-08-01：完成领域模型、接口、验证逻辑、单元测试和构建矩阵，状态 completed。

## 决策记录

- DEC-001：采用拉取式 `capture_into` 和调用方提供的固定容量缓冲，隔离厂商回调缓冲生命周期。
- DEC-002：`FrameView` 持有只读共享缓冲所有权，避免裸指针跨线程且保持零拷贝。
- DEC-003：保留 `config::PixelFormat`，避免配置模块反向依赖相机模块。
- DEC-004：重复序列号和参数校验失败复用 `CAMERA_CONFIG_FAILED`，通过稳定 `reason` 详情区分；设备缺失使用 `CAMERA_NOT_FOUND`。

## 意外发现

- 仓库说明中的 `windows-msvc-debug` 示例预设与当前实际预设名称不一致。
- `clang-format` 已随 Visual Studio 安装，但其目录未进入当前 shell 的 `PATH`；首次格式目标因此失败，使用仅对验证命令生效的临时 `PATH` 后通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | M2-01 开始前 Debug 构建与非硬件 CTest | 通过 | 17/17 CTest，unit 入口 87 项；未使用相机硬件 |
| 2026-08-01 | `PaperBreakTests --gtest_filter=Camera*` | 通过 | 9/9 相机测试 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | MSVC `/W4 /WX` Debug 全量构建 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 通过 | 临时将 VS LLVM `clang-format` 目录加入本次命令 `PATH` |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 17/17 非硬件 CTest；unit 入口 96 项，其中新增相机测试 9 项 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-release` | 通过 | Release 全量构建 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-static-analysis` | 通过 | MSVC 静态分析构建，无错误 |
| 2026-08-01 | 公共头文件禁用类型扫描、`git diff --check` | 通过 | 未发现 MVS、Win32、Qt 类型或空白错误 |
| 2026-08-01 | 实体相机/MVS SDK 验证 | 未执行 | 本任务无硬件和 SDK 条件，留待 M3 |

## 完成摘要

新增厂商无关相机接口、设备能力与参数快照、稳定错误映射、序列号绑定校验、固定容量 `FrameBuffer`、完整 `FramePacket` 和零拷贝只读 `FrameView`。新增 9 项单元测试并纳入默认 unit 入口，Debug/Release、格式、静态分析和全部非硬件 CTest 均通过。未访问 MVS SDK 或实体相机；未实现 M2-02～M2-05。
