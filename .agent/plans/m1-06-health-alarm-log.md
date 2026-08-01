# M1-06：健康、报警和近期日志基础 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-06
- 关联需求：需求 4.20～4.22、6、7、13；架构 5、7、8、11、13、17

## 目的与可观察结果

交付线程安全且有界的指标、报警和近期日志基础设施，并通过 IPC v1 提供查询、报警确认和状态变更通知。服务采样 Windows/进程、磁盘、数据库占位及 IPC 基础指标，监测线程能够按共享截止时间停止。

## 范围

### 范围内

- `paperbreak_monitoring` 指标、报警、健康线程和扩展接口；
- Windows 系统指标、IPC 指标和日志内存环；
- `system.getMetrics`、`alarm.list`、`alarm.acknowledge`、`log.tail`；
- 状态/报警推送、测试和相关文档。

### 范围外

- 相机/MVS SDK、M4 完整 UI、M5 SQLite 报警历史、诊断包和日志全文检索；
- 实体相机、SCM 重启和目标机性能验收。

## 当前基线

- 工作区开始时干净；
- M1-01～M1-05 已完成，IPC 已具备每连接有界推送队列；
- 2026-08-01 Debug 构建及非硬件 CTest 17/17 通过，unit 入口 72 项测试。

## 前置条件与假设

- 报警确认仅允许提升后的本机管理员；
- 近期日志来自异步日志线程维护的内存环；
- 报警和日志游标只在当前服务进程内有效；
- 不访问实体相机或 MVS SDK。

## 设计说明

新增 `paperbreak_monitoring` 作为指标和报警单一状态源。指标源按命名空间原子替换快照；报警按 code/source 合并，活动记录不淘汰，清除记录进入有限历史。`HealthMonitor` 使用一个 `jthread` 周期读取非阻塞快照源并评估 CPU、内存和磁盘阈值。

日志运行时增加由同一 spdlog 后台线程写入的结构化内存 sink。IPC 查询只复制有限结果，不触碰日志文件。状态和报警推送复用 `IpcServer::try_publish`，推送丢失由查询恢复。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止行为 |
| --- | --- | --- | ---: | --- | --- |
| 指标注册表 | 健康线程/指标源 | IPC 查询 | 1024 项 | 拒绝超限来源快照 | 只读快照保留至销毁 |
| 报警活动/历史 | 各模块/健康线程 | IPC 查询 | 1024/4096 条 | 活动拒绝、历史淘汰最旧 | 不持久化 |
| 日志近期环 | 日志后台线程 | IPC 查询 | 2048 条 | 覆盖最旧 | IPC 停止后随日志关闭 |
| IPC 发布入口 | 服务/监测线程 | IPC 事件线程 | 64 条 | 非阻塞拒绝；状态按 key 合并 | IPC 关闭时丢弃 |

### 持久化与恢复

本任务不持久化指标、报警或近期日志游标；M5-07 接入 SQLite `alarm_history`。滚动日志文件行为保持不变。

### 错误和降级

- 监测输入/容量错误使用新增稳定业务码；
- 指标源采样失败标记 unavailable 并产生/清除采样失败报警，不阻止服务；
- 慢 IPC 客户端只导致有界推送丢弃，查询事实源不受影响；
- 数据库在 M5 前明确为 `not-initialized`，不产生虚假数据库报警。

## 实施步骤

- [x] 1. 新增 monitoring 模块、指标/报警模型及并发单测。
- [x] 2. 实现健康线程、Windows 与 IPC 指标及生命周期装配。
- [x] 3. 扩展日志内存环和有界 tail 查询。
- [x] 4. 实现四条 IPC 命令、权限、分页和推送。
- [x] 5. 更新文档并完成全量验证。

## 验证计划

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

## 回滚与恢复

本任务不修改生产数据或 schema。失败时移除新增目标/API并恢复服务装配、IPC 和日志扩展；不得删除配置、历史或日志文件。

## 验收标准

- [x] 指标、报警和日志结果均有固定容量与查询上限；
- [x] 报警生命周期、管理员确认、并发和历史淘汰有测试；
- [x] Windows/进程、磁盘、数据库占位和 IPC 指标可查询；
- [x] 健康线程能够确定停止，慢客户端不阻塞生产者；
- [x] Debug/Release、非硬件 CTest、格式和静态分析通过；
- [x] 协议、错误码、路线图和验证证据更新。

## 进度记录

- 2026-08-01：创建计划，状态 `in-progress`；确认 Debug 基线 17/17 CTest 通过。
- 2026-08-01：完成 monitoring、Windows 采样、IPC 指标、近期日志环、四条命令和服务生命周期装配；Debug 首轮全量 17/17 CTest 通过，unit 入口增至 81 项，后续边界补测尚待最终复核。
- 2026-08-01：补齐来源容量、字符串脱敏、热更新、历史幂等确认、分页/游标、并发 tail 和关闭阶段测试；完成 Debug/Release、格式和静态分析验收，unit 入口最终为 87 项。

## 决策记录

- DEC-001：报警确认仅限提升管理员；查询对已认证本机用户开放。
- DEC-002：`log.tail` 使用异步日志线程写入的内存环，不在 IPC 请求中读取磁盘。
- DEC-003：M1 报警历史只保留内存，持久化留给 M5-07。
- DEC-004：Windows 采样复用 Win32、PSAPI 和 ToolHelp；不引入新的第三方生产依赖。

## 意外发现

- CTest 的 `-R` 过滤的是 CTest 用例名而不是 GoogleTest 子用例名；一次局部复核因此报告“未找到测试”，后续使用 `PaperBreakTests.exe --gtest_filter=...` 或全量 CTest。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | Debug build/CTest 基线 | 通过 | 17/17，unit 入口 72 项；排除 hardware-integration。 |
| 2026-08-01 | Debug 首轮实现 build/CTest | 通过 | 17/17，unit 入口 81 项；服务 Console smoke 包含监测线程启动和关闭。 |
| 2026-08-01 | Debug configure/build/CTest | 通过 | `/WX` 构建；17/17，unit 入口 87 项。 |
| 2026-08-01 | Release configure/build/CTest | 通过 | `/WX` 构建；17/17，unit 入口 87 项。 |
| 2026-08-01 | `format-check` / `git diff --check` | 通过 | clang-format 由 VS LLVM x64 目录加入本次命令 PATH；无空白错误。 |
| 2026-08-01 | static-analysis configure/build | 通过 | `PAPERBREAK_ENABLE_STATIC_ANALYSIS=ON`，`BUILD_TESTING=OFF`。 |
| 2026-08-01 | 硬件/SCM 场景 | 未执行 | 未访问实体相机、MVS SDK；未执行 SCM 重启和目标机性能测试。 |

## 完成摘要

已交付有界指标/报警/近期日志事实源、Windows 与 IPC 指标采样、热更新健康线程、管理员报警确认、IPC v1 查询与推送，并完成非硬件自动化验收。报警和日志游标保持进程内语义，SQLite 报警持久化留给 M5-07。
