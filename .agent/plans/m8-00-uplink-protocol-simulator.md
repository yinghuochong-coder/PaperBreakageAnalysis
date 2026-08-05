# M8-00：上位机协议与参考模拟器 ExecPlan

## 元数据

- 状态：complete
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：M8-00 协议评审门禁
- 关联需求：4.18 上位机通信、阶段 M8

## 目的与可观察结果

交付默认构建的 `PaperBreakUplinkSimulator`，既可通过 Qt GUI 供测试人员长期人工联调，也可无界面运行。模拟器是 Uplink v1 的参考服务端，支持最多 16 台边缘设备、明文 HTTP/WebSocket、事件元数据、持续低帧率 JPEG、分块断点上传、命令、持久化和可脚本化故障。完成后可用真实回环网络和独立工作区证明协议、重启恢复和幂等行为，但不宣称已完成边缘端 M8 或真实上位机联调。

## 范围

### 范围内

- Uplink v1 传输无关 DTO、严格校验、JSON/二进制预览编解码和机器可读协议文档。
- Qt HttpServer/WebSockets 参考服务端、GUI、无界面 CLI、SQLite/文件工作区。
- 最多 16 台设备、远程命令、故障注入、回环集成测试和独立模拟器安装组件。
- 将 ADR-012 固定为用户批准的明文、无鉴权、默认全部网卡方案，并同步需求/架构/路线图。

### 范围外

- `IUplinkTransport`、边缘端网络适配器、上传调度器以及 M8-01～M8-04。
- 完整上位机业务、用户管理、生产报表、云存储和自动根因分析。
- 实体相机、正式上位机、生产网络、PLC、SCM 重启实现和硬件验收。

## 当前基线

- 工作区在任务开始时无已跟踪修改。
- `paperbreak_uplink` 只有 `module_name()` 占位静态库，无线上 DTO 或网络 I/O。
- 顶层依赖已含 Qt Core/Gui/Widgets/Network、nlohmann/json 和 SQLite；本机 Qt 6.10.2 已安装 HttpServer/WebSockets。
- 边缘配置 v2 仍要求启用 uplink 时使用 HTTPS，本任务不改变该尚未接入的配置行为，留给 M8 边缘适配器任务迁移。

## 前置条件与假设

- 用户明确批准 Uplink v1 正式使用明文 HTTP/WS、无应用鉴权、默认监听 `0.0.0.0`，并接受窃听、伪造命令和中间人风险。
- 部署方使用隔离 VLAN、防火墙和物理访问控制；模拟器只能显示警告，不能验证隔离网络真实存在。
- 默认端口 18080，默认设备上限 16，默认工作区上限 20 GiB。

## 设计说明

`paperbreak_uplink` 保存不暴露 Qt 类型的协议模型和编解码。`paperbreak_uplink_simulator_core` 负责参考服务端、持久化和故障场景；`PaperBreakUplinkSimulator` 仅负责 CLI、GUI 和依赖装配。REST 承担会话、事件和文件上传，WebSocket 承担心跳、状态、报警、预览和双向命令。所有外部 ID、字段、大小、版本和路径段先校验再持久化。

HTTP/WS 无认证。错误响应使用稳定业务错误对象；新增 `UPLINK_PROTOCOL_VERSION_UNSUPPORTED` 和 `UPLINK_SERVER_BUSY`。协议消息只允许在 `extensions` 对象扩展。命令按边缘握手能力启用，`service.restart` 未声明时返回不支持。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 存储任务 | 网络线程 | 单存储线程 | 128 | 返回 503/`UPLINK_SERVER_BUSY` | 停止接入后排空，10 秒截止 | 深度、高水位、拒绝 |
| 待 HTTP 响应 | 网络线程 | 网络线程 | 32 | 新请求返回 503 | 关闭时统一失败 | 当前、拒绝 |
| 每设备命令 | GUI/场景 | WebSocket | 64 | 拒绝新命令 | 已发送命令等待结果，未发送持久化 | 深度、重放、结果 |
| GUI 快照 | 运行时 | GUI | 1 | 覆盖旧快照 | 关闭后停止发布 | 覆盖次数 |
| 每设备预览 | WebSocket | GUI | 1 | 覆盖旧帧 | 关闭即释放 | 接收、覆盖、非法 |

### 持久化与恢复

独立工作区使用 SQLite `user_version=1`，保存设备、会话、事件、报警、上传、分块、命令和故障配置。上传数据先进入 `.partial`，每块验证长度/SHA-256，完成时流式验证整文件并同卷原子重命名。重复同内容幂等，不同内容返回冲突。启动时复核数据库 checkpoint 与部分文件；无法证明一致的内容移动至隔离目录，不删除证据。达到工作区上限返回 507，不自动清理已收事件。

### 错误和降级

- 非法/超长/未知字段：`UPLINK_PROTOCOL_ERROR`，不写库。
- 不支持版本：`UPLINK_PROTOCOL_VERSION_UNSUPPORTED`，关闭或拒绝会话。
- 设备、请求或队列容量耗尽：`UPLINK_SERVER_BUSY`，允许有界重试。
- 分块或整文件哈希不一致：`UPLOAD_CHECKSUM_MISMATCH`，保留 checkpoint。
- 永久语义冲突：`UPLOAD_REJECTED` 或 409，不覆盖既有数据。
- 明文无认证不产生 `UPLINK_AUTH_FAILED`；该稳定码保留给其他协议版本。

## 实施步骤

- [x] 1. 固定 v1 DTO、校验、JSON/预览二进制编解码及协议测试。
- [x] 2. 实现版本化工作区、幂等事件/上传/分块/完成和恢复测试。
- [x] 3. 实现 HttpServer/WebSocket、会话/设备上限、命令和故障注入。
- [x] 4. 实现 GUI、无界面 CLI、独立安装组件和 smoke test。
- [x] 5. 更新 ADR-012、需求、架构、路线图、依赖和错误码。
- [x] 6. 执行 Debug/Release、CTest、静态分析、格式与差异验证，记录证据。

## 验证计划

### 自动化测试

- DTO 字段、版本、大小、扩展位置、路径 ID、二进制预览边界。
- 最多 16 个设备、重复会话、队列满载、命令能力和重放。
- 事件、乱序/重复分块、摘要冲突、完成、断线续传和重启恢复。
- 回环 HTTP/WebSocket、GUI 离屏和 headless 进程 smoke。
- 生产安装树排除模拟器，独立 `UplinkSimulator` 组件可运行。

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

- 环境：隔离测试 LAN，两台 Windows 主机；若不可用则只记录回环验证。
- 步骤：监听 `0.0.0.0:18080`，连接测试客户端，检查预览、命令、断网续传和重启恢复。
- 预期：所有接收数据和失败均可观察、可校验、可恢复，不产生重复事件。
- 证据保存位置：`out/test-results/m8-00/`；无法执行时标记未执行。

## 回滚与恢复

本任务不迁移边缘生产数据库或事件格式。模拟器使用独立工作区；代码回滚不删除工作区。新版本无法读取工作区时应只读拒绝并保留文件，不执行降级覆盖。

## 验收标准

- [x] 默认构建生成 GUI/headless 模拟器，生产包不包含它。
- [x] v1 端点、WebSocket、预览、命令和故障注入满足合同。
- [x] 持久化、幂等、断点续传、校验、容量和关闭路径有自动化证明。
- [x] 文档完整记录明文无鉴权决策和风险。
- [x] Debug/Release 非硬件构建与测试已执行并记录真实结果。

## 进度记录

- 2026-08-05：创建计划，状态 in-progress；确认工作区干净和 Qt HttpServer/WebSockets 可用。
- 2026-08-05：完成 Uplink v1、参考模拟器、GUI/headless、SQLite 恢复、独立安装、文档和自动化验收；未开始 M8-01～M8-04。

## 决策记录

- DEC-001：正式 v1 使用明文 HTTP/WS、无鉴权、默认全部网卡；这是用户明确批准的风险接受，不是安全推荐。
- DEC-002：模拟器默认构建但不进入生产包，以独立安装组件供测试人员部署。
- DEC-003：最多 16 台设备；持续预览为最新帧覆盖，不进入可靠上传队列。
- DEC-004：扩展命令按能力协商；`service.restart` 允许不支持。

## 意外发现

- 当前 edge config v2 的实现仍拒绝启用明文 uplink；因为本任务不实现边缘传输，该迁移留给 M8 边缘适配任务并在协议文档中明确。
- Windows 存储工作线程若把 1 MiB SHA-256 缓冲放在栈上会触发 `0xC00000FD`；已改为固定上限的堆缓冲，并以重启后完成上传测试覆盖。
- 全仓 `format-check` 被未修改的既有 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 阻塞；本任务全部 C++ 文件使用相同 clang-format 执行定向 `--dry-run --Werror` 并通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线检查 | 通过 | 工作区无已跟踪修改；本机 Qt 模块存在 |
| 2026-08-05 | Debug 全量构建与非硬件 CTest | 通过 | 28/28；含真实回环、GUI/headless smoke、生产排除和独立安装运行 |
| 2026-08-05 | Release 全量构建与非硬件 CTest | 通过 | 28/28 |
| 2026-08-05 | MSVC `/analyze` 静态分析构建 | 通过 | 默认目标含 `PaperBreakUplinkSimulator` |
| 2026-08-05 | `git diff --check` | 通过 | 无空白错误 |
| 2026-08-05 | 全仓 `format-check` | 既有文件阻塞 | `src/pipeline/include/paperbreak/pipeline/preview.hpp` 未修改；任务文件定向检查通过 |

## 完成摘要

已交付 Uplink v1 传输无关 DTO、参考协议文档、ADR-012、Qt GUI/headless 参考服务端、SQLite schema v1 工作区、幂等事件/命令/断点上传、启动复核与损坏隔离、故障场景、独立安装组件及自动化验证。当前只完成 M8-00；未实现边缘传输、上传调度或真实上位机联调。
