# M8-01：传输抽象和 Mock ExecPlan

## 元数据

- 状态：complete
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：M8-01 传输抽象和 Mock
- 关联需求：4.18 上位机通信、阶段 M8

## 目的与可观察结果

交付不暴露 Qt、HTTP 或 WebSocket 类型的 `IUplinkTransport`，以及仅供测试和后续服务装配验证使用的 `MockUplinkTransport`。调用方可以建立/断开会话、发送心跳与通用控制消息、事件元数据、文件上传请求并注册命令处理器；测试可通过有界脚本稳定复现离线、慢响应、可重试失败、永久失败、重复确认和校验错误。

## 范围

### 范围内

- `paperbreak_uplink` 中的强类型传输请求、确认、连接状态和 `IUplinkTransport`。
- `paperbreak_uplink_mock` 测试目标、可脚本化故障、命令注入、有界调用历史和统计快照。
- 接口与 Mock 的单元测试、架构/路线图任务状态更新。

### 范围外

- M8-02 心跳循环、指数退避、命令幂等、服务命令校验和审计。
- M8-03 SQLite 上传调度、优先级、重启恢复和 `retryUpload`。
- M8-04 Qt HTTP/WebSocket 适配器、文件读取、分块、断点续传、限速和真实服务器联调。
- edge config v2 的 HTTPS→HTTP/WS 迁移；真实适配器尚未在本任务接入。

## 当前基线

- 任务开始时 `git status --short` 无输出，工作区无已有修改。
- `paperbreak_uplink` 已有 M8-00 的 Uplink v1 DTO、严格 JSON/预览编解码，但没有传输端口。
- `PaperBreakUplinkSimulator` 已提供参考服务端和 `FaultProfile`，不属于边缘传输实现。
- `tests/CMakeLists.txt` 的 unit 过滤器已包含 `Uplink*`；尚无 Mock transport 测试和目标。

## 前置条件与假设

- M8-01 的接口保持传输无关；正式 Qt Network 适配器在 M8-04 实现。
- 接口采用同步调用语义，调用者不得从相机采集回调直接调用；M8-02/M8-03 将负责工作线程和持久队列。
- Mock 的慢响应有显式上限；本任务不通过真实网络、硬盘或实体相机验证。

## 设计说明

`IUplinkTransport` 位于 `paperbreak_uplink`，使用 `Result<T>` 返回稳定业务错误。会话握手复用 `SessionHello`；控制消息复用 `MessageEnvelope`；事件元数据和文件上传使用只含标准库字段的 DTO。`TransportAcknowledgement::delivery_count` 取值 1～2，Mock 的重复确认脚本返回 2，避免引入无界确认集合。

Mock 通过 `create` 校验容量与最大延迟，内部共享状态由 RAII 管理。故障脚本为 FIFO，可限定下一次操作；队列满时返回稳定错误且不扩容。调用历史覆盖最旧记录。所有状态访问受互斥量保护，命令处理器复制后在锁外调用，避免回调重入死锁。

### 线程和队列

本任务不创建工作线程或跨线程队列。Mock API 可被多个测试线程调用并由互斥量串行化；慢响应在调用线程休眠且受 `maximum_delay` 限制。故障脚本是容量 `fault_capacity` 的有界控制队列，满载拒绝新脚本；调用历史容量为 `history_capacity`，满载覆盖最旧记录；`disconnect()` 无等待并确定性完成。

### 持久化与恢复

不适用。Mock 状态、故障脚本和历史只存在于进程内；M8-03 才引入持久上传任务。

### 错误和降级

- 离线或脚本断线：`UPLINK_DISCONNECTED`，可重试，本地业务是否排队由后续调度器负责。
- 可重试失败：`UPLINK_SERVER_BUSY`，可重试。
- 永久失败：`UPLOAD_REJECTED`，不可重试。
- 校验错误：`UPLOAD_CHECKSUM_MISMATCH`，可重试。
- 非法 Mock 配置/脚本/请求：`UPLINK_PROTOCOL_ERROR`，不可重试。
- 脚本容量满：`UPLINK_SERVER_BUSY`，可重试；绝不无界扩容。

## 实施步骤

- [x] 1. 在 `src/uplink/include/paperbreak/uplink/transport.hpp` 定义强类型 DTO、连接状态和 `IUplinkTransport`，用编译测试证明接口不依赖 Qt。
- [x] 2. 新增 `paperbreak_uplink_mock`，实现有界故障脚本、调用历史、统计、命令注入和确定性断开。
- [x] 3. 新增 `tests/unit/uplink_mock_transport_tests.cpp`，覆盖正常调用、离线/恢复、慢速、失败、重复确认、校验错误、容量、历史覆盖和命令回调。
- [x] 4. 更新架构和路线图 M8-01 证据，执行 Debug/Release 构建、CTest、静态分析、格式和差异检查。

## 验证计划

### 自动化测试

- 接口经仅链接 `paperbreak_uplink` 的测试编译，不包含 Qt 类型。
- Mock 正常连接/断开及心跳、事件、上传调用历史。
- 在线开关与脚本离线、可重试/永久失败、校验错误及 retryable 分类。
- 有上限慢响应、重复确认计数、FIFO 和操作限定。
- 故障队列拒绝、调用历史覆盖、命令处理器锁外调用。

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

- 环境：不适用；本任务是纯软件接口和 Mock。
- 步骤：未执行真实上位机、网络中断、实体相机或硬件测试。
- 预期：后续 M8-02/M8-03 可用同一接口注入 Mock。
- 证据保存位置：CTest 输出和本 ExecPlan 验证证据表。

## 回滚与恢复

删除新增 transport/mock 源文件并移除对应 CMake/test 条目即可回到 M8-00 基线；本任务不迁移或删除任何用户数据，不需要数据回滚。

## 验收标准

- [x] 业务可通过 `IUplinkTransport` 使用上行能力且公开接口不含 Qt/HTTP/WebSocket 类型。
- [x] Mock 可脚本化复现路线图列出的六类故障，所有控制集合均有容量和溢出策略。
- [x] 自动化测试覆盖错误语义、重复确认、命令回调和关闭路径。
- [x] Debug/Release 构建与 CTest 已执行并记录真实结果。
- [x] 未实现或声称完成 M8-02～M8-04。

## 进度记录

- 2026-08-05：完成需求、架构、路线图、M8-00 协议和现有源码检查；创建计划，状态 in-progress。
- 2026-08-05：完成接口、测试专用 Mock、6 项单元测试、文档与全量验证；状态 complete。

## 决策记录

- DEC-001：传输端口使用同步强类型调用；线程、重试和持久队列留给 M8-02/M8-03，避免 M8-01 隐式引入无界异步任务。
- DEC-002：重复确认用有界 `delivery_count` 表达，不返回动态确认集合；当前 v1 Mock 上限固定为 2。
- DEC-003：Mock 单独作为 `BUILD_TESTING` 目标，不安装进生产包。

## 意外发现

- edge config v2 仍要求 HTTPS/credential；按 M8-00 已记录迁移点，本任务没有真实适配器，因此不提前改变配置公开行为。
- 标准静态分析预设按设计关闭 `BUILD_TESTING`；为覆盖测试专用 Mock，验证时临时启用 `tests` manifest feature 并只构建 `paperbreak_uplink_mock`，随后恢复标准预设。
- 全仓 `format-check` 仍被未修改的既有 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 阻塞；M8-01 的 4 个新增 C++ 文件已定向执行相同 clang-format `--dry-run --Werror` 并通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线检查 | 通过 | 工作区无已有修改；M8-00 已完成 |
| 2026-08-05 | `PaperBreakTests --gtest_filter=UplinkMockTransport.*` | 通过 | 6/6 |
| 2026-08-05 | Debug 配置、全量构建与 `ctest --preset local-windows-vs2026-debug` | 通过 | 28/28；unit 310 项 |
| 2026-08-05 | Release 配置、全量构建与 `ctest --preset local-windows-vs2026-release` | 通过 | 28/28；unit 310 项 |
| 2026-08-05 | MSVC `/analyze` 标准构建及临时启用 tests 的 `paperbreak_uplink_mock` 目标 | 通过 | Mock 静态分析无警告，随后恢复标准预设 |
| 2026-08-05 | 新增 C++ 文件定向 clang-format、`git diff --check` | 通过 | 无格式或空白错误 |
| 2026-08-05 | 全仓 `format-check` | 既有文件阻塞 | 未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp` |
| 2026-08-05 | 真实上位机、网络与硬件验证 | 未执行 | M8-01 不含真实传输适配器；未宣称通过 |

## 完成摘要

已交付传输无关的 `IUplinkTransport`、标准库 DTO 和测试专用 `MockUplinkTransport`。Mock 提供容量受限的操作限定 FIFO 故障脚本、覆盖式历史、统计、重复确认模型和锁外命令注入，Debug/Release 全量 CTest 与 Mock 静态分析通过。真实网络、心跳/命令编排和持久上传仍属于 M8-02～M8-04。
