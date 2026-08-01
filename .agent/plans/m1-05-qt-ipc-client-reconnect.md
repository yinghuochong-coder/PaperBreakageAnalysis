# M1-05：Qt IPC 客户端与重连 ExecPlan

## 元数据

- 状态：in-progress
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-05
- 关联需求：需求 3、5、6、7、13；架构 4.2、5、7.4、8、11、13、14、15、19

## 目的与可观察结果

交付运行于 Qt GUI 事件循环的有界 IPC 客户端。客户端可在服务先启动或后启动时连接，在服务重启后自动恢复，通过连接代次隔离旧响应，并在托盘中明确显示连接和服务状态；退出客户端只关闭 IPC，不停止后台服务。

## 范围

### 范围内

- IPC v1 请求编码、响应/推送解码；
- 有界在途请求、超时、取消、连接代次和抖动退避重连；
- Console 客户端状态模型、`system.getStatus` 同步和最小托盘反馈；
- 自动化测试、协议/错误/架构文档和路线图证据。

### 范围外

- M1-06 指标、报警和日志命令；
- M4 主窗口、完整四色托盘、预览和配置页面；
- M1-04 跨账户/管理员/远程管道平台验证；
- 相机、MVS SDK、SCM 安装和硬件测试。

## 当前基线

- 工作区开始时干净，HEAD 为 `87fe6d8`；
- M1-04 已提供 IPC v1 帧、服务端和三条 system 命令，但路线图因既有全仓格式/静态分析问题保持 `in-progress`；
- `PaperBreakEdgeConsole` 只有 M0 托盘骨架，尚未链接 `paperbreak_ipc`；
- 开始前 Debug 构建及排除硬件标签的 CTest 为 17/17 通过。

## 前置条件与假设

- 协议版本保持 1，服务名继续使用 `PaperBreakEdgeService.Ipc`；
- 客户端只在已连接时接受请求，不跨连接自动重放；
- 默认连接超时 2 秒、请求超时 5 秒、稳定连接重置 5 秒；
- 默认重连为 250 ms 指数增长、10 秒上限和 ±20% 抖动；
- 最多 128 个在途请求，待发送字节上限 32 MiB；
- 所有客户端和状态模型方法在创建它们的 Qt 事件循环线程调用。

## 设计说明

`paperbreak_ipc` 增加不暴露 Qt 类型的 `IpcClient` PImpl。每次连接尝试分配单调递增代次并创建新 QLocalSocket；socket 信号、请求句柄和回调均校验代次。单个截止时间定时器管理全部请求。断线使当前代请求失败，未知或旧 requestId 不触发回调。

Console 内部状态模型拥有客户端，连接成功后查询 `system.getStatus`。状态结果只在代次匹配时生效；断线保留最后值但设置 `stale=true`。托盘只显示最小状态文本，不提前实现 M4 功能。

### 线程和队列

本任务不新增线程或跨线程队列。QLocalSocket、重连/连接/请求定时器、状态模型和 UI 回调都运行在 Qt GUI 事件循环。内存边界如下：

| 通道/表 | 容量 | 满载策略 | 停止行为 |
| --- | ---: | --- | --- |
| 客户端在途请求 | 128 条 | 返回 `IPC_BUSY` | 以 `IPC_REQUEST_CANCELLED` 完成并清空 |
| 客户端待发送字节 | 32 MiB | 返回 `IPC_BUSY` | abort socket，丢弃未发送数据 |
| 帧解码缓冲 | 1 MiB header + 16 MiB binary + 前缀 | 超限断开并重连 | reset |

### 持久化与恢复

不适用。请求、响应、推送和客户端状态均不持久化；重连后重新查询状态，不重放断线期间消息。

### 错误和降级

- `IPC_NOT_CONNECTED`：当前不可发送，请求可由调用方在新连接后重试；
- `IPC_CONNECTION_LOST`：已接受请求因断线失败，不自动重放；
- `IPC_REQUEST_TIMEOUT`：请求超过单调时钟截止时间；
- `IPC_REQUEST_CANCELLED`：调用方取消或客户端停止；
- `IPC_BUSY`：在途请求或待发送字节达到上限；
- 畸形/不兼容服务消息使用既有协议错误，关闭当前连接并进入有界重连。

## 实施步骤

- [x] 1. 扩展协议 DTO 和严格编解码，增加客户端方向单元测试。
- [x] 2. 实现 `IpcClient` 状态机、代次、超时/取消、有界请求和重连测试。
- [x] 3. 实现 Console 状态模型、状态同步、过期语义和托盘接入测试。
- [x] 4. 更新 CMake、协议、错误码、架构和路线图。
- [x] 5. 执行 Debug/Release、CTest、格式、静态分析和空白检查，记录证据。

## 验证计划

### 自动化测试

- 协议请求/响应/推送往返与畸形输入；
- 服务先/后启动、重启、客户端反复开关和退出不停止服务；
- 退避上限、请求成功/超时/取消/断线、容量限制和旧代响应；
- 状态同步、断线过期和重连代次；
- 无服务时 Console smoke test。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
git diff --check
```

### 人工或硬件验证

- 未执行：本任务无需实体相机、MVS SDK、SCM 安装或管理员权限。

## 回滚与恢复

本任务不写用户数据或变更 schema。失败时移除新增客户端/状态模型目标与公开 API，并恢复 Console、测试和文档到任务前状态；不得删除用户配置或历史。

## 验收标准

- [x] 服务先/后启动及重启后客户端均能连接；
- [x] 请求超时、取消、断线和旧代响应语义确定且有界；
- [x] 断线后 Console 明确显示过期状态；
- [x] 退出 Console 不停止服务；
- [x] Debug/Release、非硬件 CTest 和本任务新增文件质量检查通过；
- [x] 文档和验证证据完整，既有范围外门禁问题如实记录。

## 进度记录

- 2026-08-01：创建计划，状态 `in-progress`；确认工作区干净且 Debug 基线 17/17 CTest 通过。
- 2026-08-01：完成客户端、状态模型、托盘接入和文档；新增协议/客户端/状态模型定向测试 16 项通过。
- 2026-08-01：Debug/Release 构建与完整非硬件 CTest 均通过；本任务文件格式和静态分析通过。全仓格式及默认静态分析仍由既有 M1-03 文件阻断，按任务约定保留 `in-progress`。

## 决策记录

- DEC-001：客户端接口采用标准 C++ DTO/回调和 PImpl，Qt 类型限制在实现内部。
- DEC-002：请求不跨连接自动重试，避免写命令重复执行；状态查询由状态模型在每代连接后重新发起。
- DEC-003：M1-05 只提供托盘文本反馈，完整 UI 和四色状态留给 M4。

## 意外发现

- QLocalSocket 在自身 `errorOccurred` 信号处理中不能被同步销毁；客户端改为断开信号后 `deleteLater`，并以 Impl QObject 作为父对象保证事件循环停止时仍可确定回收。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | 任务前 Debug build/CTest | 通过 | 17/17，排除 `hardware-integration`。 |
| 2026-08-01 | `IpcProtocol.*:IpcClient.*:ClientStateStore.*` | 通过 | 16 项定向测试通过。 |
| 2026-08-01 | Debug configure/build/CTest | 通过 | 17/17 CTest；单元测试入口共执行 72 项。 |
| 2026-08-01 | Release configure/build/CTest | 通过 | 17/17 CTest；单元测试入口共执行 72 项。 |
| 2026-08-01 | 定向测试重复运行 | 通过 | 16 项测试连续运行 3 轮，共 48 次通过。 |
| 2026-08-01 | 本任务 C++ 文件 `clang-format --dry-run --Werror` | 通过 | 所有修改和新增 C++ 文件通过。 |
| 2026-08-01 | `format-check` | 未通过（既有阻断） | 未修改的 `basic_config.hpp:207` 不符合格式；本任务文件无报告。 |
| 2026-08-01 | 默认静态分析构建 | 未通过（既有阻断） | 未修改的 `basic_config.cpp:327` 触发 C6262（栈使用 42464 字节）并因 `/WX` 失败；本任务目标在此之前构建成功。 |
| 2026-08-01 | 静态分析（`PAPERBREAK_WARNINGS_AS_ERRORS=OFF`） | 通过（有既有警告） | 全部目标构建成功；仅保留上述既有 C6262，本任务目标无新增报告。 |
| 2026-08-01 | `git diff --check` | 通过 | 无空白错误。 |

## 完成摘要

M1-05 的功能、测试和文档已实现：Qt 事件循环内的有界客户端具备连接代次、请求截止时间、取消和受控重连；状态模型在每次新连接后查询并缓存 `system.getStatus`，断线保留诊断值但标记过期；Console 退出只停止本地 IPC。Debug/Release 和非硬件测试全部通过。本计划与路线图按约定继续标记 `in-progress`，直至范围外的既有全仓格式和默认静态分析阻断被其所属任务修复。
