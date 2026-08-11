# M6-03/M5-06：候选事件 ID 重复持久化热修 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-11
- 最后更新：2026-08-11
- 路线图条目：M6-03 候选确认、故障隔离与降级；M5-06 事务式事件目录
- 关联需求：候选事件唯一身份、正式事件目录不可覆盖、资源有界与确定性恢复

## 目的与可观察结果

修复候选窗口冻结后仍复用同一来源候选 ID 再次落盘的问题。一个候选 ID 最多创建一个事件窗口；窗口冻结后到候选终结前仍保留有界映射，使确认或超时可以更新原事件而不会重开窗口。持久化发现正式目录已存在时，在写入事务负载前拒绝，同时保留提交前最终复检。

## 范围

### 范围内

- `EventRuntime` 候选创建/终结通知、来源到聚合事件映射的生命周期；
- `EventTransactionWriter` 正式目标目录早期预检和最终竞态保护；
- 自动确认、超时、下一候选唯一 ID、重复正式目录和提交竞态回归测试；
- Debug/Release 构建和全部非硬件 CTest。

### 范围外

- 算法队列容量、`drop-oldest`、检测阈值或采集帧率；
- 现场 `.pending`、正式事件、数据库和配置清理；
- 实体相机性能优化、ETW 归因及后续里程碑。

## 当前基线

- `EventRuntime::freeze()` 无条件删除 `source_to_canonical`；尚未终结的候选随后可用同一 ID 启动新窗口。
- `CandidateEventManager::process()` 已产生有序的 `candidate_created`/`decision_changed` 通知，但返回值不携带这些通知，运行时通过最终快照和当前检测结果推断生命周期。
- 持久化仅在完整写完事务后检查正式目标目录，重复 ID 会产生数 GiB 无效残留。
- 工作区开始时 `git status --short` 无输出；现场 `config/data/event_root` 和日志由 `.gitignore` 排除，本任务不修改或删除。

## 前置条件与假设

- 用户明确批准本次跨 M6-03/M5-06 的单一热修计划。
- 候选管理器每相机最多一个活动候选，来源映射上限由最多四台相机约束；管线停止或重配置时状态整体销毁。
- 无实体相机性能验收条件；只能执行模拟相机和文件系统自动化验证。

## 设计说明

- `CandidateProcessOutcome` 追加本次调用产生的有序通知；现有字段和回调保持兼容。
- 运行时只响应 `candidate_created` 创建窗口，使用通知内不可变的 `candidate_trigger`，不再用任意后续触发帧重新创建。
- `decision_changed` 更新来源决策和聚合事件生命周期。窗口仍活动时保留映射供冻结聚合；窗口已冻结时在更新完成后清理映射。冻结只清理已终结来源，未终结来源保留到终结通知。
- 正式目录预检在创建事务目录前执行：检查错误保留原生文件系统上下文，已存在返回 Critical `EVENT_WRITE_FAILED`。写完 manifest 后继续复检并原子移动，防止预检与提交之间的竞态。

### 线程和队列

不新增线程或队列。候选通知随既有单事件线程的同步返回值传递；外部回调合同不变。算法帧队列仍为每相机容量 8、`drop-oldest`，事件持久化队列仍为容量 8。

### 持久化与恢复

- 不改变 manifest schema、目录布局和恢复格式。
- 早期拒绝不创建 `.pending`；最终复检失败仍保留已写事务，供既有恢复/隔离流程处理。
- 不清理现有现场残留。

### 错误和降级

- 继续使用 `EVENT_WRITE_FAILED`；早期目标存在与路径检查失败使用不同消息/操作阶段。
- 不修改 `ALGORITHM_QUEUE_BACKLOG`、`ALGORITHM_DEGRADED` 或其他算法降级合同。

## 实施步骤

- [x] 1. 扩展候选处理结果并将事件运行时改为通知驱动，修正映射的冻结/终结清理。
- [x] 2. 增加冻结后确认、超时、外部确认、下一候选唯一身份和不重复持久化的运行时测试。
- [x] 3. 增加正式目录早期预检与提交竞态保护测试。
- [x] 4. 运行任务文件格式检查、Debug/Release 构建、全部非硬件 CTest 和差异检查。

## 验证计划

### 自动化测试

- 候选窗口先冻结，后续普通触发不产生第二个同 ID 窗口；
- 冻结后确认/超时更新原数据库事件，映射清理后下一候选生成不同 ID；
- 已存在正式目录在任何事务文件写入前拒绝；
- 预检后目标目录出现时最终复检仍拒绝且保留事务。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行修改 C++ 文件的 `clang-format --dry-run --Werror` 和 `git diff --check`。

### 人工或硬件验证

- Release 双实体相机 30 分钟性能和 ETW：未执行，缺少本次受控硬件验收条件。
- 现场残留清理：未执行，未获得删除授权。

## 回滚与恢复

代码回滚仅逆序撤销通知返回、运行时映射和持久化预检改动，不触碰任何事件数据。新增字段不改变磁盘或 IPC 格式。测试临时目录由测试 RAII 清理，现场目录保持原状。

## 验收标准

- [x] 同一候选 ID 不会在窗口冻结后再次创建持久化窗口。
- [x] 冻结后确认和超时仍更新原事件，下一候选 ID 唯一。
- [x] 重复正式目录在事务负载写入前拒绝，最终复检继续覆盖竞态。
- [x] Debug/Release 构建及非硬件 CTest 通过，限制如实记录。

## 进度记录

- 2026-08-11：完成现场日志、正式目录和事务残留只读根因分析；创建热修计划，状态 in-progress。
- 2026-08-11：完成候选通知返回、运行时映射生命周期修复、正式目录早期预检和关联回归测试。
- 2026-08-11：完成 Debug/Release 配置、全量构建和非硬件 CTest；两种配置最终均为 30/30 通过，计划状态 completed。

## 决策记录

- DEC-001：候选窗口创建改为通知驱动，避免继续从最终快照和任意当前帧推断“新候选”。
- DEC-002：未终结候选的映射跨窗口冻结保留；终结且窗口已冻结后立即清理，保持最多四路的有界状态。
- DEC-003：早期预检只减少确定性重复写入，最终复检和原子移动仍是提交正确性的权威保护。
- DEC-004：不以扩容、抽样或降分辨率处理算法积压；硬件性能留给 Release 受控验收。

## 意外发现

- 运行时回归首次暴露了一个关联竞态：候选已经转为 `Confirmed` 或 `Timeout` 后，冻结窗口的 Candidate manifest 完成索引时会通过 UPSERT 把数据库终态降回 `Candidate`。修复为仅在数据库当前仍是 Candidate 时接受 manifest 的决策状态，并保留既有终态及确认时间；测试同时覆盖 `Confirmed`、`Timeout`、`Rejected`。
- 第一次 Debug 全量 CTest 中聚合 `unit` 用例失败，但相同二进制随即单独复跑 387 项全部通过；再次完整执行 Debug CTest 为 30/30 通过。未观察到可复现的本次变更失败。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-11 | `git status --short` | 通过 | 实施前工作区干净。 |
| 2026-08-11 | 新增及相关事件/存储测试 | 通过 | 相关测试集 68/68 通过；冻结后外部确认专项 1/1 通过。 |
| 2026-08-11 | `cmake --preset local-windows-vs2026-debug`、`cmake --build --preset local-windows-vs2026-debug` | 通过 | VS 2026 / MSVC Debug 全量目标构建成功。 |
| 2026-08-11 | `ctest --preset local-windows-vs2026-debug` | 通过 | 最终复跑 30/30 通过；首次聚合 unit 波动已单独及全量复验。 |
| 2026-08-11 | `cmake --preset local-windows-vs2026-release`、`cmake --build --preset local-windows-vs2026-release` | 通过 | VS 2026 / MSVC Release 全量目标构建成功。 |
| 2026-08-11 | `ctest --preset local-windows-vs2026-release` | 通过 | 30/30 通过，包括 Release-only 存储吞吐验收。 |
| 2026-08-11 | `clang-format --dry-run --Werror`、`git diff --check` | 通过 | 全部本次修改的 C++ 文件格式与差异检查通过；仅 Git 提示未来 LF→CRLF 转换。 |
| 2026-08-11 | Release 双实体相机 30 分钟 | 未执行 | 缺少本次受控硬件验收；不得声明算法积压性能通过。 |
| 2026-08-11 | 现场 `.pending` 清理 | 未执行 | 未获得删除授权，正式事件与事务残留保持原状。 |

## 完成摘要

候选窗口现在只响应 `candidate_created` 通知创建，窗口冻结时保留未终结候选映射，确认、拒绝或超时完成生命周期更新后才确定性释放；同一候选不会被后续普通检测帧重新解释为新窗口。正式事件目录在事务创建前预检，重复 ID 不再先写入大体量负载，同时最终提交复检继续防御 TOCTOU。数据库 manifest 重建索引不会覆盖已写入的候选终态。算法队列容量、`drop-oldest` 策略和现场数据均未修改。
