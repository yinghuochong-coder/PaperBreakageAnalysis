# M1-04：IPC 帧协议和服务端 ExecPlan

## 元数据

- 状态：in-progress
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

- [ ] 1. 新增 IPC 目标、v1 DTO、帧编解码和请求校验。
- [ ] 2. 实现有界命令执行器、QLocalServer 线程和连接状态。
- [ ] 3. 实现 Windows peer 鉴权和单实例保护。
- [ ] 4. 接入三条 system 命令及 ServiceRuntime 生命周期。
- [ ] 5. 增加编解码、故障、权限、真实本机套接字和停止测试。
- [ ] 6. 更新协议文档、路线图和验证证据。

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

- [ ] v1 帧协议和三条命令符合文档；
- [ ] 所有连接、队列、缓冲和截止时间有界；
- [ ] 畸形、慢速、重复和未授权客户端不拖垮服务；
- [ ] 工作线程确定停止，Debug/Release/CTest/格式/静态分析通过；
- [ ] 文档、路线图和验证证据完整。

## 进度记录

- 2026-08-01：创建计划，状态 `not-started`；等待 M1-03 完成。
- 2026-08-01：M1-03 Debug 构建和 17/17 CTest 通过并完成记录；本计划更新为 `in-progress`。

## 决策记录

- DEC-001：协议字段采用领域模型的 `ipcProtocolVersion`。
- DEC-002：普通本机用户只读，提升管理员可重载配置。
- DEC-003：断线推送不缓存，客户端后续通过查询重新同步。

## 意外发现

- 尚无。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| ... | ... | ... | ... |

## 完成摘要

完成时填写。
