# M1-04：IPC 帧协议和服务端 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-04
- 关联需求：需求 6、11、12；架构 5.2、7、8、11、13、15、16、19

## 目的与可观察结果

交付版本化、有界、仅本机且可确定停止的 QLocalServer IPC 服务端，实现状态、版本和配置重载三条命令，并能隔离畸形输入、慢客户端和未授权请求。

## 范围

### 范围内

- IPC v1 编解码、请求/响应/推送 DTO 和稳定错误映射；
- QLocalServer 服务端、连接/读取/发送/请求队列上限和超时；
- Windows 本机身份检查、查询权限与管理员重载权限；
- 三条 system 命令、服务生命周期接入、测试和协议文档。

### 范围外

- M1-05 Qt IPC 客户端与重连；
- M1-06 指标、报警和日志查询命令；
- 相机、预览、事件及上位机命令。

## 当前基线

- 开始本任务前必须先完成并验证 M1-03；
- Qt 6.10.2 Network 已是批准依赖，但仓库尚无 `paperbreak_ipc` 目标；
- 服务核心已有配置、IPC、日志关闭阶段。

## 前置条件与假设

- 协议版本 1；pipe 名为 `PaperBreakEdgeService.Ipc`；
- 普通已认证本机用户只读，提升管理员可执行 reloadConfig；
- 连接 4、每连接在途 16、近期 requestId 1024、命令队列 64、推送 32、待发送 32 MiB；
- header 1 MiB、binary 16 MiB、不完整帧总截止 5 秒。

## 设计说明

IPC 事件线程只执行身份检查、解帧、基础校验、关联和有界发送；可能访问文件的服务命令进入单工作线程有界执行器。所有服务调用通过标准 C++ DTO/接口，Qt/Win32 类型不进入业务接口。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| IPC 命令 | IPC 事件线程 | 命令工作线程 | 64 | 返回 `IPC_BUSY` | 拒绝新任务，排空已接受任务 | M1-06 接入 |
| 每客户端推送 | 服务 | IPC 事件线程 | 32 | 同类覆盖旧值，否则丢弃 | 客户端断开即丢弃 | M1-06 接入 |
| 每客户端发送字节 | IPC 事件线程 | QLocalSocket | 32 MiB | 响应无法容纳则断开 | 截止时间内关闭 | M1-06 接入 |

### 持久化与恢复

IPC 不持久化请求或推送；配置重载复用 M1-03 的原子仓储。客户端重连不重放断线期间推送。

### 错误和降级

使用既有 IPC 错误码；无 requestId 的畸形输入断开，可关联错误返回失败响应。未知高协议版本返回支持范围后关闭。服务停止时新写请求返回 `SYS_SERVICE_STOPPING`。

## 实施步骤

- [x] 1. 新增 IPC 目标、v1 DTO、帧编解码和请求校验。
- [x] 2. 实现有界命令执行器、QLocalServer 线程和连接状态。
- [x] 3. 实现 Windows peer 鉴权和单实例保护。
- [x] 4. 接入三条 system 命令及 ServiceRuntime 生命周期。
- [x] 5. 增加编解码、故障、权限、真实本机套接字和停止测试。
- [x] 6. 更新协议文档、路线图和验证证据。

## 验证计划

### 自动化测试

覆盖拆包/粘包、长度边界、随机字节、二进制、版本、重复 ID、未知命令、慢客户端、连接/积压上限、权限、断线推送、重复监听和停止。

### 构建与测试命令

```powershell
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
git diff --check
```

### 人工或平台验证

LocalService 跨账户、普通用户只读、提升管理员重载和远程命名管道拒绝需隔离 Windows 环境；无法执行时标记待验证。

## 回滚与恢复

停止 IPC 并移除独立目标即可回到 M1-03；不得删除配置或历史。配置命令失败由 M1-03 仓储回滚。

## 验收标准

- [x] v1 帧协议和三条命令符合文档；
- [x] 所有连接、队列、缓冲和截止时间有界；
- [x] 畸形、慢速、重复和未授权客户端不拖垮服务；
- [x] 工作线程确定停止，Debug/Release/CTest/格式/静态分析通过；
- [x] 文档、路线图和验证证据完整。

## 进度记录

- 2026-08-01：创建计划，状态 `not-started`；等待 M1-03 完成。
- 2026-08-01：M1-03 Debug 构建和 17/17 CTest 通过并完成记录；本计划更新为 `in-progress`。
- 2026-08-01：确认工作区干净并再次完成 Debug 构建和 17/17 CTest；开始实现协议、Windows 对端身份检查和有界服务端。
- 2026-08-01：完成 IPC v1、Windows 鉴权、三条 system 命令、ServiceRuntime 接入和测试；Debug/Release 均为 17/17 CTest，单元入口 61 项。
- 2026-08-01：M1-04 新增 C++ 文件定向格式检查及关闭 `/WX` 的全量静态分析通过；全仓质量门禁被 M1-03 既有文件阻断，因此状态保持 `in-progress`。
- 2026-08-01：统一修复 7 个既有文件的格式，按顶层配置分区拆分解析函数并消除 C6262；全仓格式、默认静态分析、Debug/Release 和两套 17/17 CTest 均通过，状态更新为 `completed`。

## 决策记录

- DEC-001：线协议版本字段固定为 `protocolVersion`，当前支持值为整数 1。
- DEC-002：普通本机用户只读，提升管理员可重载配置。
- DEC-003：断线推送不缓存，客户端后续通过查询重新同步。
- DEC-004：帧长度字段使用网络字节序；v1 不增加 CRC，依靠固定长度边界和严格 JSON/DTO 校验隔离畸形输入。
- DEC-005：每客户端总出站上限采用 128 条/32 MiB，推送子队列最多 32 条；固定安全上限不修改配置 schema v1。
- DEC-006：Windows 原生命名管道身份检查封装在平台层；IPC 公开接口只传递标准 C++ `PeerIdentity`。

## 意外发现

- Windows 本机命名管道调用 `GetNamedPipeClientComputerNameW` 可能返回 `ERROR_PIPE_LOCAL`；该结果明确表示本机客户端，应作为本机身份继续执行令牌模拟。
- 服务新增 Qt Network 运行时后，安装树扫描需要同时将 Qt Core 目录加入运行时依赖解析目录。
- 全仓 `format-check` 曾在 M1-03 的 7 个既有文件失败，首个报告为 `basic_config.hpp:207`；默认静态分析曾在 `basic_config.cpp:327` 报 C6262（栈使用 42464 字节）。2026-08-01 已统一格式化并拆分配置解析，两个阻断均已清除。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | MSVC Debug 全量构建成功。 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 17/17 通过 | unit 入口 61 项；包含真实当前账户命名管道令牌冒烟。 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-release` | 通过 | MSVC Release 全量构建成功。 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-release --output-on-failure` | 17/17 通过 | unit 入口 61 项。 |
| 2026-08-01 | M1-04 新增 C++ 文件 `clang-format --dry-run --Werror` | 通过 | 新增协议、服务端、平台、命令和测试文件无格式差异。 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 未通过 | 被未修改的 `basic_config.hpp:207` 阻断。 |
| 2026-08-01 | 静态分析预设，`PAPERBREAK_WARNINGS_AS_ERRORS=OFF` | 通过 | 全量目标完成；唯一报告为既有 `basic_config.cpp:327` C6262。 |
| 2026-08-01 | 静态分析预设，默认 `/WX` | 未通过 | 同一既有 C6262 被提升为错误；`paperbreak_ipc` 在失败前已通过分析。 |
| 2026-08-01 | `git diff --check` | 通过 | 仅 Git 的 LF/CRLF 工作区提示，无空白错误。 |
| 2026-08-01 | 隔离 Windows 权限/远程场景 | 待验证 | 未执行跨账户、提升管理员和远程管道实机验证；未访问相机或 MVS SDK。 |
| 2026-08-01 | 配置定向测试 `BasicConfig*:ConfigRepository*` | 11/11 通过 | 覆盖严格解析、依赖/路径校验、敏感字段和仓储事务/回滚。 |
| 2026-08-01 | Debug/Release build + CTest | 通过 | 两套 CTest 均为 17/17；unit 入口各执行 72 项。 |
| 2026-08-01 | 全仓 `format-check` | 通过 | 使用 VS 2026 clang-format 20.1.8；全部 C++ 文件无格式差异。 |
| 2026-08-01 | 默认 `/WX` 静态分析 clean build | 通过 | 强制重新分析 `basic_config.cpp`，C6262 已消除且无其他分析告警。 |

## 完成摘要

M1-04 的功能实现、自动化测试和公开文档已完成。既有全仓格式违规与配置解析 C6262 已修复，Debug/Release、两套 17/17 非硬件 CTest、全仓格式和默认静态分析门禁均通过，路线图和本计划更新为 `completed`。跨账户、提升管理员和远程管道场景仍待隔离 Windows 环境验证；未访问实体相机或 MVS SDK。
