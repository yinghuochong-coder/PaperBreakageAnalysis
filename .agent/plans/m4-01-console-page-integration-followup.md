# M4-01：控制台页面既有能力接入修正 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：M4-01（已完成页面骨架的后续缺口修正）
- 关联需求：`docs/requirements/edge-system-requirements.md` 5.2、5.3，以及 M6、M8 已完成能力

## 目的与可观察结果

控制台不再对已经实现的算法、事件和上位机功能显示“待接入”里程碑占位。总览显示真实的算法运行状态、候选事件数、上位机连接状态和待上传任务数；上位机配置页可读取、编辑并保存既有 edge config v2 的 uplink 配置，且明确提示需要重启后台服务的字段。

## 范围

### 范围内

- 修正总览指标订阅遗漏和早期硬编码提示。
- 新增 `uplink.getConfig/updateConfig` 本机 IPC 命令，复用配置仓储校验、乐观修订、审计和原子保存。
- 新增控制台 Uplink 客户端、上位机配置页和状态回显。
- 为服务命令、客户端解析/状态和离屏 UI smoke 增加或更新自动化测试。

### 范围外

- 不修改 Uplink v1 协议、传输、调度器或上传数据格式。
- 不实现 M8-05 PLC/现场 IO。
- 不把需要重启的 Uplink transport 配置伪装成热更新。
- 不执行正式上位机、实体相机或生产网络测试。

## 当前基线

- `src/console/src/main_window.cpp` 的总览仍硬编码 M6/M8“待接入”，上位机配置页仍为骨架页。
- `ClientStateStore` 能解析 `uplink.*`，但 `system.getMetrics` 请求遗漏 `uplink.` 前缀。
- M8-02～M8-04 已完成；服务指标已发布 `uplink.state`、`uplink.pending_upload_tasks` 和 `uplink.pending_upload_bytes`。
- edge config v2 已包含 uplink 配置，`/uplink/transport` 被定义为重启后生效。
- 工作区已有用户数据修改：`config/data/事件 数据/.metadata/events.db` 及其 `backups/`，本任务不修改、不清理这些文件。

## 前置条件与假设

- 使用现有本机认证和管理员写权限规则；读取仅要求认证本机用户，写入要求管理员。
- 配置仓储是 uplink 配置的唯一持久事实源；不直接改写配置文件。
- 服务运行时没有现成的动态 Uplink 重建接口，因此保存后由 `pendingRestartPaths` 明确提示重启。

## 设计说明

`uplink.getConfig` 返回 stored/effective uplink 对象、两类修订和待重启路径；`uplink.updateConfig` 接收完整 uplink 对象和期望修订，合并到完整配置文档后交给 `ConfigRepository::update`。控制台客户端沿用现有 IPC 自动重连、单请求在途、代次拒绝旧响应和断线标记过期模式。总览只显示已有快照，不推断或伪造运行值。

### 线程和队列

不新增队列。控制台 Uplink 客户端复用 `IpcClient` 的既有有界请求通道和确定性 `stop()`；UI 更新继续投递到 Qt 事件线程。

### 持久化与恢复

复用 `ConfigRepository` 的严格 schema 校验、历史版本、临时文件加原子替换和失败回滚。不新增 schema 或数据迁移。

### 错误和降级

- 未认证/非管理员分别返回既有 `IPC_UNAUTHORIZED`。
- 非法字段或类型返回 `IPC_REQUEST_INVALID`/`SYS_CONFIG_INVALID`。
- 乐观修订冲突返回 `SYS_CONFIG_VERSION_CONFLICT`。
- 断线保留最后值但标为过期；保存按钮禁用，不使用旧修订提交。

## 实施步骤

- [x] 1. 增加服务端 Uplink 配置读写 IPC 与系统命令测试。
- [x] 2. 增加控制台 Uplink 客户端及解析/断线/冲突测试。
- [x] 3. 将上位机配置页和总览摘要绑定真实快照，更新离屏 smoke。
- [x] 4. 运行格式、差异、Debug/Release 构建和 CTest，记录限制。

## 验证计划

### 自动化测试

- 服务命令：认证、管理员权限、读回、保存、修订冲突、未知字段。
- 客户端：完整响应解析、断线过期、更新请求和错误保留。
- UI smoke：上位机页面不再是 placeholder，可编辑保存；总览存在真实状态标签。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

### 人工或硬件验证

- 环境：本机 Qt 离屏 smoke；不要求实体相机。
- 步骤：运行既有 `service_console_smoke`（若固定 IPC 未被外部服务占用）。
- 预期：页面构造、状态应用和关闭重开稳定。
- 证据保存位置：本计划“验证证据”。正式上位机/生产网络人工联调标记未执行。

## 回滚与恢复

代码回滚只涉及本计划列出的服务命令、控制台模型/UI 和测试文件。配置仍由现有原子仓储管理；测试使用临时配置，不删除用户数据。

## 验收标准

- [x] 上位机配置页无“将在 M8 接入”占位，并可真实读写配置。
- [x] 总览无已完成 M6/M8 的“待接入”提示，显示真实或明确不可用/过期状态。
- [x] 相关自动化测试、Debug/Release 构建和 CTest 已实际运行并记录结果。
- [x] 不修改工作区既有事件数据库数据。

## 进度记录

- 2026-08-05：完成需求、架构、路线图和相关源码检查，创建计划并开始实施。
- 2026-08-05：完成实现、自动化测试、Debug/Release CTest 和 Release 全量构建，状态改为 completed。

## 决策记录

- DEC-001：使用专用 `uplink.getConfig/updateConfig`，而非让控制台直接读写整个配置，缩小权限和冲突面并与 storage/algorithm/event 页面一致。
- DEC-002：Uplink transport 配置保存后以待重启路径回显，不新增未经设计的热重建接口。

## 意外发现

- 总览客户端解析器已支持 `uplink.*`，但查询前缀遗漏，导致实现存在却始终无法显示。
- 工作区存在非本任务的事件 SQLite 数据变化，必须原样保留。
- 配置仓储原先只在待重启时恢复 Uplink URL/凭据，导致启用、心跳、分块和限速被错误标为立即有效；已改为完整 Uplink 配置保持旧有效值直至重启。
- Debug 全量构建一次在 vcpkg applocal 阶段受正在运行的控制台锁定影响而失败；此前 Debug 全量编译成功，之后 Debug 完整 CTest 28/28 通过，Release 最终全量构建和 CTest 均通过。未结束用户进程。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | `cmake --preset local-windows-vs2026-debug` | 通过 | VS 2026、Qt、OpenCV、MVS、vcpkg 配置成功 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-debug` | 编译通过；一次最终 applocal 受文件锁阻断 | `PaperBreakEdgeConsole.exe` 正在运行；未擅自终止。此前同配置全量构建成功 |
| 2026-08-05 | Uplink/ClientStateStore/EventClient 定向测试 | 9/9 通过 | Release，覆盖配置、重启语义、指标前缀和重试上传 |
| 2026-08-05 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 28/28 通过 | 包含离屏 UI smoke、集成、安装树和模拟测试 |
| 2026-08-05 | `cmake --preset local-windows-vs2026-release` + build | 通过 | 最终格式化后再次全量构建通过 |
| 2026-08-05 | `ctest --preset local-windows-vs2026-release --output-on-failure` | 28/28 通过 | 全套非硬件门禁 |
| 2026-08-05 | `format-check` | 本任务文件通过后被既有 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 阻断 | 未修改该无关文件 |
| 2026-08-05 | `git diff --check` | 通过 | 仅有 Git 的 LF→CRLF 提示 |

## 完成摘要

总览和上位机配置页已接入现有 M6/M8 服务能力；事件重试上传也已接到持久队列命令。新增专用 Uplink IPC/控制台客户端并保持完整配置重启语义。两套 CTest 均 28/28，通过 Release 全量构建；未执行实体相机、正式上位机或生产网络联调，未触碰用户既有事件数据库修改。
