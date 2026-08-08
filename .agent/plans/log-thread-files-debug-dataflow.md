# 按功能命名线程并按线程名生成本地时间日志 ExecPlan

## 元数据

- 状态：complete
- 负责人：Codex
- 创建日期：2026-08-07
- 最后更新：2026-08-07
- 路线图条目：横向可观测性改进（不改变任何既有或后续里程碑状态）
- 关联需求：需求 4.20～4.22、5、6、7、10、13、16；用户于 2026-08-07 给出的线程命名与逐线程日志方案

## 目的与可观察结果

服务端和控制台自有业务线程具有稳定、唯一且可由 Windows 调试器读取的功能名。异步日志工作线程按记录携带的逻辑线程名将日志写入独立文件，文件内容和近期日志使用带本地时区偏移的 RFC 3339 时间。`logging.level` 与 `retentionDays` 可通过现有配置事务热更新，IPC 状态和日志查询公开兼容新增字段，控制台跟随服务端有效日志等级。自动化测试证明线程注册、路由、轮转、过滤、降级、时间、热更新和旧响应兼容性。

## 范围

### 范围内

- 日志运行时的线程注册 RAII、Windows 线程描述、逐线程异步文件路由、有界文件状态、保留期清理、动态等级和结构化入口。
- 所有项目自有业务线程入口命名；不可控的 Qt、Windows、spdlog 以外第三方内部线程不纳入强制注册。
- 已有数据流模块的默认空操作诊断回调和 Debug 关键节点；不得在生产者线程执行文件 I/O。
- `/logging/live` 的配置事务应用器、状态/日志 IPC 字段和控制台兼容同步。
- 相关单元/集成测试与架构、配置、IPC、服务/控制台代码指南。

### 范围外

- 不新增 UI 快捷开关，不提升配置 schema 版本。
- 不迁移、不重命名或删除旧聚合日志。
- 不开始或标记后续 M9 里程碑，不引入新的生产依赖。
- 不执行实体相机、生产四路吞吐、真实上位机、SCM、断电或 7×24 小时测试。

## 当前基线

- 工作区开始时 `git status --short` 为空。
- `src/logging` 当前使用单 spdlog 异步工作线程、有界队列和 `overrun_oldest`，但只有按日期/大小轮转的聚合文件，近期时间为 UTC，记录没有线程名。
- `src/config` schema v2 已有 logging level、retentionDays、目录、大小、文件数、队列容量；当前 live 配置应用路径未装配 logging 应用器。
- `src/service/core/src/system_commands.cpp` 已实现 `system.getStatus` 和 `log.tail`；后者支持分类和等级但不支持 threadName。
- `src/service/main.cpp` 与 `src/console/main.cpp` 创建日志运行时；自有工作线程散布于 camera、pipeline、event、storage、uplink、monitoring、IPC 和控制台导出/重启模块。
- 现有 `tests/unit/logging_tests.cpp` 覆盖聚合文件轮转、脱敏、近期缓存和并发关闭。

## 前置条件与假设

- 构建使用已配置的 `local-windows-vs2026-debug` 和对应 Release 预设。
- Windows `SetThreadDescription`/`GetThreadDescription` 在目标 Windows 10/11 可用；非 Windows 编译路径保持无操作兼容。
- 相机采集测试使用 Mock；无法据此声称真实 MVS 回调或硬件吞吐已验证。
- 每条日志在入队前复制当前逻辑线程名，文件选择、轮转、清理和刷新全部只发生在日志工作线程 sink 中。

## 设计说明

`LoggingRuntime::ThreadRegistration` 是 move-only RAII 对象。注册验证 `[a-z0-9-]`、1～63 字符和进程内唯一性，在 Windows 设置原生线程描述；析构时注销。日志 payload 内部携带有界元数据封装，两个 sink 解析出 threadName/category/业务字段：近期 sink 形成结构化记录，路由 sink 维护最多 64 个 `(threadName, localDate)` 状态。未注册、非法、重复或超上限记录进入 `unregistered-thread-<tid>` 应急文件，并生成不会递归扩散的 Error 记录。

动态等级由原子值和 logger level 同步更新；`enabled` 是日志文本构造前的低成本检查。结构化入口限制最多 16 个键值字段并统一脱敏。已有三参数 `log` 保留以降低迁移风险。

配置应用器只声明 `/logging/live`，prepare 保存旧等级/保留期，apply 设置新值并执行受限清理，readback 比较，commit 丢弃快照，rollback 恢复。目录、队列、文件大小和每日文件数仍由既有 restart-required 判定处理。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 异步日志队列 | 所有已注册业务线程 | `logging-worker` | `logging.queueCapacity` | `overrun_oldest` | shutdown flush 后确定性 join | 覆盖量、队列容量 |
| 各现有数据流队列 | 相机/管线/事件/存储/上行生产者 | 既有工作线程 | 保持各模块现有有界容量 | 保持既有 drop/reject/coalesce 策略 | 保持既有 stop_token/join | 深度、高水位、接受/拒绝数 |

本任务不新增无界跨线程队列；诊断回调只同步构造并入队一条有界日志消息，不做 I/O。

### 持久化与恢复

- 新文件名为 `paperbreak-service|console-<thread>-YYYY-MM-DD.log[.N]`，重启追加当天 `.log`；轮转数对每线程独立。
- 保留期只识别上述严格命名规则，不触碰旧聚合日志和其他文件；失败仅告警，不阻止业务运行。
- 不改变配置 schema 或领域数据格式。IPC 仅增加可选/附加字段，旧服务响应继续可解析。

### 错误和降级

- 沿用 `LOG_INITIALIZATION_FAILED`、`LOG_WRITE_FAILED`；新增线程注册/容量错误使用稳定 `LOG_THREAD_REGISTRATION_FAILED` 和 `LOG_THREAD_FILE_LIMIT_REACHED`。
- 重复/非法/未注册线程不静默共享命名文件，降级至应急文件并保留线程 ID。
- 文件状态超过 64 个时不创建更多命名状态，记录 Error 并写应急文件。
- 配置 apply/readback 失败时由事务回滚到旧有效等级和保留期。

## 实施步骤

- [x] 1. 重构 `src/logging`：增加 RAII 注册、Windows 描述、动态等级、结构化入口、线程名近期字段、本地 RFC 3339、逐线程路由/大小日期轮转、64 状态降级和保留清理；扩展日志单元测试立即验证。
- [x] 2. 在服务、IPC、camera、pipeline、event、storage、uplink、monitoring 及控制台实际自有线程入口装配固定功能名；使用依赖注入传递日志/诊断能力，不让业务模块直接依赖 spdlog。
- [x] 3. 增加 logging live 配置应用器并装配既有事务；沿用配置仓储事务的 prepare/apply/readback/commit/rollback 与 restart-required 边界。
- [x] 4. 扩展 `system.getStatus`、`log.tail`、控制台状态/过滤/显示及连接等级同步；覆盖新增字段和旧服务缺字段兼容性。
- [x] 5. 在已实现的数据流关键节点加入先 `enabled(debug)` 的有界逐帧诊断，覆盖相机、转发/检测、预览、事件/存储、IPC/上行/控制台中可从现有 DTO 安全获得的字段。
- [x] 6. 更新架构、配置、IPC 与服务/控制台代码指南；未修改路线图里程碑状态。
- [x] 7. 运行格式、Debug/Release 构建、完整 CTest、静态分析和 `git diff --check`，把实际结果写回本计划。

## 验证计划

### 自动化测试

- 日志：名称验证、重复、未注册、Windows 回读、并发隔离、重启追加、逐线程轮转、日期/时区、64 状态、动态等级、结构化字段上限、脱敏、队列饱和和确定性关闭。
- 配置：logging live 两字段事务成功和故障回滚，其他字段仍要求重启。
- IPC/控制台：状态 loggingLevel、tail threadName 请求/响应、显示线程名与 ID、旧响应兼容。
- Mock 数据流：四路逻辑相机以 cameraId + sequenceNumber 关联，并验证 Debug 关闭/开启和非反压语义。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --build --preset local-windows-vs2026-debug --target static-analysis
git diff --check
```

只运行仓库实际存在的目标；不存在时记录为未执行及原因，不虚构成功。

### 人工或硬件验证

- 环境：Windows 10/11、Visual Studio 调试器、四台 MV-CS020-60GM、生产等价 NVMe/网络。
- 步骤：观察原生线程描述，短时打开 Debug，以四路满帧率触发采集/预览/事件/上传并使日志队列饱和。
- 预期：线程名可读且唯一；同一帧跨模块可关联；日志覆盖不反压采集。
- 证据保存位置：未执行；本任务环境不保证实体硬件和生产等价吞吐条件。

## 回滚与恢复

代码回滚可按本计划修改文件逐项反向应用，不删除任何日志或用户配置。新逐线程日志是附加产物，旧版本会忽略；schema 未变，无需数据迁移。热更新失败由配置事务恢复运行时旧值，磁盘清理失败只报警且不删除不匹配文件。

## 验收标准

- [x] 所有实际自有业务线程在入口注册合法唯一名称并设置 Windows 描述；SCM 启停前置脉冲线程只设置原生描述且不写业务日志。
- [x] 日志只由单后台 worker 执行 I/O，并按线程名、本地日期和逐线程大小轮转。
- [x] 未注册、重复、非法和 64 状态上限具有可测试的应急路径与 Error。
- [x] 近期记录和文件均使用本地 RFC 3339，保留 threadId 并新增 threadName。
- [x] level/retentionDays 热更新、状态/tail IPC 和控制台同步兼容通过测试。
- [x] Debug 数据流日志默认关闭，启用即时生效且不记录禁止内容。
- [x] 相关文档和测试已更新，Debug/Release 构建与完整 CTest 已实际运行。
- [x] 最终明确实体相机和生产四路吞吐测试状态，不改变后续里程碑状态。

## 进度记录

- 2026-08-07：阅读需求、架构、路线图和 ExecPlan 规范；检查工作区与日志/线程基线；创建计划并置为 in-progress。
- 2026-08-07：完成日志运行时、线程入口、诊断回调、配置事务、IPC/控制台兼容、文档和测试；Debug/Release CTest 各 28/28 通过，静态分析通过。

## 决策记录

- DEC-001：线程名在生产者入队时复制到内部日志元数据，确保异步 worker 不依赖已退出线程的 TLS。
- DEC-002：保留现有 `log(Category, Level, message)` 作为兼容入口，新增结构化重载；模块只依赖 logging 抽象/诊断回调，不依赖 spdlog。
- DEC-003：严格匹配新服务/控制台文件名后才执行 retention 清理，旧聚合文件永不由新策略处理。

## 意外发现

- 当前近期日志使用 UTC `Z`，而文件日期已按本地日期选择，二者需要统一。
- 当前文件 sink 在创建时立即打开聚合文件，即使没有业务日志；逐线程路由后应延迟到首条记录创建。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-07 | `git status --short` | 通过 | 开始时工作区无修改 |
| 2026-08-07 | `cmake --preset local-windows-vs2026-debug` + build | 通过 | 全量目标成功 |
| 2026-08-07 | `ctest --preset local-windows-vs2026-debug` | 通过 | 28/28；预设排除 hardware-integration |
| 2026-08-07 | `cmake --preset local-windows-vs2026-release` + build | 通过 | 全量目标成功 |
| 2026-08-07 | `ctest --preset local-windows-vs2026-release` | 通过 | 28/28；预设排除 hardware-integration |
| 2026-08-07 | `cmake --preset local-windows-vs2026-static-analysis` + build | 通过 | MSVC 静态分析全量目标成功 |
| 2026-08-07 | `format-check` | 基线失败 | 本次修改文件单独 dry-run 通过；未修改的 `tests/unit/event_runtime_tests.cpp:255` 不符合格式 |
| 2026-08-07 | `git diff --check` | 通过 | 无空白错误 |

## 完成摘要

已完成按功能线程名路由的本地时间日志、线程 RAII/Windows 描述、日志等级和保留期热更新、
IPC/控制台兼容扩展以及各数据流 Debug 诊断。未提升 schema 版本，未修改路线图状态，未增加生产依赖。
实体相机、四路生产吞吐、真实上位机、SCM 和长稳测试未执行。
