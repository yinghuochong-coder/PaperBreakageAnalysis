# M5/M7：事件实时可见性、状态拆分与块持久化修复 ExecPlan

## 元数据

- 状态：complete
- 负责人：Codex
- 创建日期：2026-08-08
- 最后更新：2026-08-08
- 路线图条目：M5-04～M5-09、复用 M7-01～M7-04 已完成块格式（用户明确批准本次跨条目修复）
- 关联需求：事件立即可见、状态正交、事务式证据、人工复核、IPC 查询/推送、资源有界

## 目的与可观察结果

候选经窗口管理器取得规范 EventId 后不到一秒即可由 `event.list` 查询并收到生命周期推送；同一行在算法判定、证据持久化和人工复核三个正交维度原地更新。持续时间查询不发送固定结束时间。事件原始证据按相机和一秒单调窗口写为最多 256 帧的 `PBNVME1` v1 块，schema v2 manifest 记录 `rawBlocks`、范围、CRC32C 与顺序计算的 SHA-256，manifest 最后写且事件目录最后原子发布。

## 范围

### 范围内

- 候选创建/合并、算法决策、冻结、编码、排队、写入、提交和失败的数据库状态迁移与推送。
- SQLite 新 schema、独立筛选和生命周期汇总；旧 `eventState` 兼容别名。
- schema v2 manifest 与 `PBNVME1` 事件块、检查器、导出和上传清单适配。
- Qt 事件列表三列状态、启动期汇总、持续结束时间开关、查询在途时的合并补查。
- 模拟、故障注入、格式、性能计数和客户端恢复测试。
- IPC、领域模型、错误码和运维文档更新。

### 范围外

- 删除、移动或迁移用户现有事件数据与 `events.db`。
- 修改相机/MVS 适配器边界，新增生产依赖或开始后续里程碑。
- 实体相机、生产 NVMe、四路满速、拔盘或断电验收。
- 用户已修改的 `src/console/main.cpp`。

## 当前基线

- 工作树开始时仅 `src/console/main.cpp` 有用户修改，本任务不触碰。
- M5 现状为冻结后才排队持久化、提交后才写 SQLite；manifest schema v1 用逐帧 `rawFiles`，复核覆盖 `event_state`。
- `event.list` 支持固定 `endTimeUtcMs` 和单一 `eventState`；服务只保留 `event.committed` 推送语义。
- M7 已有 `PBNVME1` v1：4 KiB 页、固定头/索引/尾页、逐帧 CRC32C、两次刷新、提交标记和原子发布；本任务复用其格式实现，不改 NVMe 格式版本。
- 事件写队列是单工作线程、容量 8，须保持有界及确定性停止。

## 前置条件与假设

- 部署前由用户人工归档 schema v1 事件目录和 `events.db`，并提供空事件根目录。
- 代码检测到非空旧库或旧事件目录时以 `EVENT_SCHEMA_UNSUPPORTED` 拒绝启动，不删除或移动数据。
- 开发机模拟吞吐仅用于回归，不代表生产 NVMe 验收。
- 本地 Qt/OpenCV/MVS/vcpkg 路径由忽略的 `CMakeUserPresets.json` 提供。

## 设计说明

- `decisionState`：Candidate/Confirmed/Rejected/Timeout，聚合优先级固定为 Confirmed > Candidate > Timeout > Rejected；确认不可被后续来源降级。
- `persistenceState`：Collecting/Encoding/Queued/Writing/Committed/Incomplete。
- `reviewState`：Unreviewed/Reviewed；`reviewDecision`：Confirmed/Rejected/null。复核仅改这两个字段和复核修订，不改算法判定或 manifest。
- 首个候选在事件管理串行路径调用数据库 `create_collecting_event`，合并触发更新同一行的聚合判定与 `triggerCount`。正式目录在 Committed 前为空。
- 只有 Committed 行可访问 manifest、缩略图、目录、导出、上传清单和人工复核。
- schema v2 是硬切换，不迁移 v1。空数据库创建新结构；任何既有非空旧版本库或旧事件事实源明确失败。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 算法帧队列 | 相机观察者 | 每相机算法线程 | 既有每路 8 | 既有拒绝/降级 | stop token、join | 深度/拒绝/处理耗时 |
| 算法结果队列 | 算法线程 | 事件串行线程 | 既有 256 | 拒绝并标记来源降级 | stop token、排空 | 深度/高水位/拒绝 |
| JPEG 队列 | 事件线程 | 单编码线程 | 既有固定容量 | 整事件拒绝，Incomplete | 排空已接收任务 | 深度/失败 |
| 事件写队列 | 编码回调 | 单持久化线程 | 8 | 立即拒绝，Incomplete/报警 | stop token；取消保留事务目录 | 深度/活动事件/最后字节/耗时/MiB/s |
| IPC 推送队列 | 服务事件观察者 | IPC 连接写端 | 既有有界容量 | 允许丢推送，周期查询恢复 | 连接关闭即取消 | 既有推送指标 |

### 持久化与恢复

- 每相机按单调时钟一秒边界和最多 256 帧分块。
- 每块沿用 `PBNVME1` v1 物理布局，顺序写时同时计算 SHA-256；写主体与尾页、刷新、写提交标记、再刷新、原子发布。
- 全部块、关键帧和 `event.json` 完成后最后写 manifest；manifest 校验通过后事件目录同卷原子提交，再把数据库行转为 Committed。
- 事务目录在短写、刷新、提交标记、重命名、取消等失败时保留；不可作为正式目录查询。失败行转 Incomplete 并保留稳定业务错误。
- 恢复只接受完整 schema v2 事务；校验损坏/缺标记不得产生 Committed 行。

### 错误和降级

- `EVENT_SCHEMA_UNSUPPORTED`：检测到旧数据库/事件 schema，启动拒绝且不改数据。
- `EVENT_WRITE_FAILED`：编码、队列、块写、刷新、发布或清单失败；行转 Incomplete 并报警。
- 继续保留底层 I/O/平台错误作为 cause/context，不以其替代业务错误。

## 实施步骤

- [x] 1. 升级领域/SQLite 模型为三状态轴，加入候选创建、合并更新、生命周期迁移、独立筛选/汇总与旧数据拒绝；补数据库测试。
- [x] 2. 抽取并复用 `PBNVME1` 写入边界，将事件逐帧 raw 改为一秒/256 帧块、顺序 SHA-256、schema v2 `rawBlocks`；适配验证、恢复、检查器、上传文件清单并补故障/格式测试。
- [x] 3. 在 EventRuntime 串行链路接入 Collecting→Encoding→Queued→Writing→Committed/Incomplete，落实聚合优先级、triggerCount、候选/提交观察者和写入指标；补四触发合两事件与延迟测试。
- [x] 4. 扩展 IPC `event.list/get`、独立筛选和生命周期汇总，发布 `event.lifecycleChanged`/`event.committed`，限制非 Committed 工件操作；补协议与服务测试。
- [x] 5. 更新 EventClient 和 Qt 页面（不修改 `src/console/main.cpp`），实现三列、启动汇总、持续结束时间和在途合并刷新；补丢推送/重连/周期兜底测试。
- [x] 6. 更新架构、领域模型、IPC、错误码与部署说明，记录 schema v1 人工归档门禁和硬件未验证边界。
- [x] 7. 运行 Debug/Release 配置、构建、CTest、相关 MSVC 静态分析、任务文件格式检查和 `git diff --check`，把结果回填本计划。

## 验证计划

### 自动化测试

- 四次候选合并成两个规范事件，判定计数 4，候选可见延迟 <1 秒，聚合终态符合优先级。
- 自动确认、拒绝、超时与人工复核正交，复核不改 manifest。
- 持续结束时间不发送 `endTimeUtcMs`；固定模式精确筛选；推送丢失/查询在途/重连后补查。
- `PBNVME1` header/index/footer/commit、逐帧 CRC、文件 SHA、序号缺口、关键帧追溯、导出和上传清单。
- 短写、刷新失败、缺提交标记、rename 失败、校验损坏、取消、队列满均不产生伪提交目录。
- 模拟 20 秒约 907 帧时每相机约 20 块，并记录实际字节、耗时和 MiB/s。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug --output-on-failure
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release --output-on-failure
cmake --build --preset local-windows-vs2026-debug --target format-check
git diff --check
```

相关 MSVC 静态分析按本仓库现有预设/目标执行；若全仓既有问题阻断，记录首个无关阻断和定向结果。

### 人工或硬件验证

- 实体相机：未执行，需目标工控机四路 MV-CS020-60GM。
- 生产 NVMe 四路满速及热稳定吞吐：未执行，需目标工控机记录盘型号、温度、队列和吞吐。
- 断电/拔盘：未执行，需受控硬件台架验证恢复和事务残留。

## 回滚与恢复

代码回滚到本变更前版本；不得用旧程序打开 schema v2 数据。部署回滚时停止服务，将新 schema v2 数据整体人工归档，再恢复先前人工归档的 v1 数据及对应二进制。代码不会删除、移动或自动转换用户事件数据。

## 验收标准

- [x] 用户摘要中的所有接口、状态、生命周期、块格式、控制台和失败语义均有实现及自动化测试。
- [x] 候选立即可查，持续查询不固定结束时间，推送遗漏可由查询恢复。
- [x] 约 907 帧场景不产生约 907 个 raw 文件，块数量符合一秒/256 帧规则。
- [x] 非 Committed 行不能访问工件或复核，失败不产生可查询伪提交目录。
- [x] Debug/Release 构建与非硬件 CTest 通过；仓库级格式目标的既有无关阻断已精确记录，任务改动文件定向格式检查通过。
- [x] 不修改 `src/console/main.cpp`、不动旧数据、不新增生产依赖和无界队列。

## 进度记录

- 2026-08-08：读取需求、架构、路线图、ExecPlan 规范并检查工作树；创建计划，状态 in-progress。
- 2026-08-08：完成 SQLite schema 5、manifest schema 2、三状态轴、候选即时发布、生命周期推送、控制台持续查询及 `PBNVME1` 事件块写入。
- 2026-08-08：完成块格式/校验/故障注入/取消/队列满、候选合并、IPC 门禁和客户端补查测试，并通过 Debug、Release 与 MSVC 静态分析验证。

## 决策记录

- DEC-001：本计划按用户明确方案跨 M5/M7，复用而不改变 `PBNVME1` v1 物理格式。
- DEC-002：schema v2 不提供 v1 迁移，启动门禁优先保护旧数据。
- DEC-003：`eventState` 仅作为 `decisionState` 输出/筛选兼容别名，新控制台只使用三状态轴。

## 意外发现

- 工作树开始时 `src/console/main.cpp` 已被用户修改，必须完全避开。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-08 | `git status --short` | 发现用户修改 | 仅 `M src/console/main.cpp`，本任务不触碰 |
| 2026-08-08 | `cmake --preset local-windows-vs2026-debug`、构建、`ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | 28/28；单元测试 358/358 |
| 2026-08-08 | `cmake --preset local-windows-vs2026-release`、构建、`ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | 最终复跑 28/28；此前一次单元测试偶发失败，单独复跑 358/358 后全量通过 |
| 2026-08-08 | `cmake --preset local-windows-vs2026-static-analysis`、构建 | 通过 | MSVC 静态分析全构建完成 |
| 2026-08-08 | 907 帧/20 秒模拟块测试 | 通过 | 20 块；71 ms、428745 字节、5.7138 MiB/s，仅开发机模拟回归数据 |
| 2026-08-08 | 修改文件 `clang-format --dry-run --Werror` | 通过 | 排除用户拥有的 `src/console/main.cpp` |
| 2026-08-08 | 仓库级 `format-check` | 既有无关阻断 | 未修改的 `tests/unit/pipeline_tests.cpp` 不符合格式；未越界修改该文件 |
| 2026-08-08 | `git diff --check` | 通过 | 无空白错误，仅 Git 的 LF→CRLF 提示 |

## 完成摘要

事件候选现在会以规范 EventId 立即建立 Collecting 行，并沿独立算法、证据和人工复核状态轴原地更新；IPC 和控制台支持持续查询、生命周期推送及查询在途合并补查。原始证据改为 schema v2 `rawBlocks`，采用 `PBNVME1` 两阶段持久刷新、提交标记和原子发布，并新增吞吐指标与完整故障注入覆盖。Debug/Release CTest 和 MSVC 静态分析通过；仓库级格式目标仍被未修改的 `tests/unit/pipeline_tests.cpp` 阻断，但本任务改动文件定向检查通过。实体相机、生产 NVMe、四路满速、拔盘和断电测试均未执行。
