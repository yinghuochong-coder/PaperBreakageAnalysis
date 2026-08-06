# PaperBreakEdgeConsole.exe 代码导读 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-06
- 最后更新：2026-08-06
- 路线图条目：不适用（本任务仅整理现有实现文档）
- 关联需求：`docs/requirements/edge-system-requirements.md` 第 5、6 节

## 目的与可观察结果

新增一份面向维护者的 Markdown 代码导读，使读者能够从构建目标和 `main()` 入口出发，理解 `PaperBreakEdgeConsole.exe` 的主要类、关键函数、IPC 边界、调用关系、线程模型，以及启动、状态同步、预览、配置写入、事件操作和退出等核心时序。

## 范围

### 范围内

- 检查 `src/console` 的构建目标、入口、UI 组合、客户端模型和托盘实现。
- 检查 `src/ipc` 中 Console 实际依赖的客户端协议和连接行为。
- 必要时追踪 `src/service/core` 的对应命令入口，以说明跨进程调用终点。
- 新增 `docs/development/paperbreak-edge-console-code-guide.md`。
- 通过符号、路径、Markdown/Mermaid 静态检查验证文档。

### 范围外

- 修改产品代码、协议、模块边界或公开行为。
- 补充或执行硬件测试。
- 自动开始任何路线图任务。

## 当前基线

- `PaperBreakEdgeConsole` 由 `src/console/CMakeLists.txt` 定义，链接 `paperbreak_console_model`、IPC、日志、Windows 服务控制和 Qt Widgets。
- `src/console/main.cpp` 是组合根，`MainWindow` 负责页面装配与状态呈现，各 `*Client` 封装领域命令。
- 工作区已有与本任务无关的修改：`src/service/main.cpp` 已修改，`docs/development/paperbreak-edge-service-code-guide.md` 为未跟踪文件；本任务不触碰它们。

## 前置条件与假设

- 文档描述当前工作区代码，而不是仅描述需求中的理想设计。
- 不依赖实体相机或 Hikrobot MVS SDK；本任务不声称进行硬件验证。
- 文档中的服务端调用终点只追踪到解释 Console 行为所需的层级。

## 设计说明

文档采用“进程/模块总览 → 入口与对象所有权 → 主要类与函数 → 调用关系 → 核心时序 → 线程与背压 → 阅读索引”的顺序。调用图与时序图使用 Mermaid，文本明确区分进程内直接调用、Qt 信号/槽和 IPC 跨进程消息。

### 线程和队列

本任务不新增线程或队列。文档将记录现有 Qt GUI 线程、IPC 事件循环、预览解码线程池及其 latest-wins/mailbox 行为。

### 持久化与恢复

不适用；本任务不改变持久化。文档会强调 Console 不直接写生产配置、SQLite 或事件目录。

### 错误和降级

不改变错误语义。文档将说明连接失败、请求失败、状态失效和预览丢弃等现有降级路径。

## 实施步骤

- [x] 1. 提取 Console、IPC 及相关服务命令的类、函数和所有权关系。
- [x] 2. 梳理启动、周期刷新、预览、写配置、事件操作、托盘与退出时序。
- [x] 3. 编写代码导读并加入可定位的源文件/符号索引。
- [x] 4. 静态检查文档中的路径、符号、Mermaid 结构和 Git 变更范围，并运行仓库验证命令。

## 验证计划

### 自动化测试

- 纯文档变更不新增产品行为，因此不新增单元测试。
- 使用 `rg` 检查文档引用的关键类、函数和源文件是否存在。
- 使用脚本检查 Markdown 代码围栏是否成对闭合。

### 构建与测试命令

文档不影响编译产物；仍按仓库完成标准运行：

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

### 人工或硬件验证

- 不适用。本任务不更改运行行为，也不需要实体相机。

## 回滚与恢复

仅新增文档和本计划；需要回滚时移除这两个新增文件即可，不涉及用户数据或配置。

## 验收标准

- [x] 文档至少覆盖主要类、主要函数、调用关系和核心时序。
- [x] 明确区分 Console 与后台服务职责以及 IPC 边界。
- [x] 关键描述可追溯到当前源文件和符号。
- [x] 未修改无关文件，验证命令及结果已记录。

## 进度记录

- 2026-08-06：创建计划，完成构建目标、架构约束和需求章节的初步检查。
- 2026-08-06：完成 Console、IPC 和服务命令调用链分析，新增代码导读；链接、代码围栏、关键符号及变更范围静态检查通过，开始执行构建测试。
- 2026-08-06：本机 Debug 配置、构建和全部 CTest 通过，完成最终变更范围复核，任务完成。

## 决策记录

- DEC-001：新增独立 Console 代码导读，不并入当前未跟踪的 Service 代码导读，避免修改用户已有文件并保持两个进程的阅读边界清晰。

## 意外发现

- 工作区已有用户修改和未跟踪文档，验证时需区分本任务新增文件与既有变更。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-06 | 初始 `git status --short` | 已记录基线 | `src/service/main.cpp` 已修改；Service 导读未跟踪 |
| 2026-08-06 | Markdown 链接、围栏、关键符号检查 | 通过 | 0 个缺失链接、0 行尾空白；抽查 13 个关键符号均存在 |
| 2026-08-06 | `cmake --preset local-windows-vs2026-debug` | 通过 | 配置和生成完成；仅报告非必需 Vulkan headers 未找到 |
| 2026-08-06 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | 成功生成 `PaperBreakEdgeConsole.exe` 及全部构建目标 |
| 2026-08-06 | `ctest --preset local-windows-vs2026-debug` | 通过 | 28/28 测试通过，0 失败，26.17 秒 |

## 完成摘要

新增 `docs/development/paperbreak-edge-console-code-guide.md`，覆盖构建边界、组合根、对象所有权、主要类/函数、IPC 命令映射、调用链、6 组核心时序、线程与容量、错误/过期语义、当前实现差异和推荐阅读路径。未修改产品代码；未执行实体相机采集验证。
