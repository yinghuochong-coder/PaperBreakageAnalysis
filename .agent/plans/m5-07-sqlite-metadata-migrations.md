# M5-07：SQLite 元数据和迁移 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M5-07
- 关联需求：`docs/requirements/edge-system-requirements.md` 4.16、阶段 M5；`docs/architecture/system-architecture.md` 12.3、13.1、13.2

## 目的与可观察结果

在 `paperbreak_storage` 中提供不泄漏 SQLite 类型的 schema v1 元数据数据库。新库和旧的 user_version=0 库可事务式迁移；已有旧库迁移前生成一致备份；损坏或高版本库明确拒绝。M5-06 已原子提交的事件目录可被验证后索引，并支持有界分页及时间、状态、相机筛选。目录与数据库不一致时，对账以已提交目录为事实源补建索引，并把缺失目录的数据库记录标为 `Missing` 而不删除证据或记录。

## 范围

### 范围内

- 建立 `events`、`event_cameras`、`key_frames`、`event_files`、`upload_jobs`、`device_status_history`、`config_history`、`alarm_history`、`audit_logs` 及必要索引和约束；
- 使用 `PRAGMA user_version=1`，从 0 到 1 在单事务内迁移，重复打开幂等，高版本明确拒绝；
- 迁移前 SQLite 在线备份、显式备份和离线恢复入口；
- 启动/按需完整性检查和 SQLite 损坏分类；
- 从 M5-06 已验证 manifest 原子更新事件、相机、关键帧和文件索引；
- 有界事件分页和时间、状态、相机筛选；
- 有界扫描正式事件目录，补建缺失索引、刷新已有索引，并标记数据库孤儿记录；
- 任务代码、单元测试、依赖目标修正和路线图完成证据。

### 范围外

- M5-08 存储水位、删除和保留策略；
- M5-09 服务组合根、配置、IPC、Qt 查询/复核/导出接线；
- M8 上传执行器和重试领取逻辑；
- 自动删除损坏数据库、事件目录、备份或用户数据；
- 实体相机、真实断电、生产磁盘性能和 SCM 服务验证。

## 当前基线

- `src/storage/src/event_store.cpp` 已提供 schema v1 不可变事件目录、manifest 严格验证、恢复和隔离；数据库尚无实现。
- `src/storage/CMakeLists.txt` 已私有链接 SQLite，但使用会产生弃用提示的兼容目标 `SQLite::SQLite3`。
- `tests/unit/storage_event_store_tests.cpp` 已覆盖 M5-06 文件事务；测试入口尚无 M5-07 数据库测试。
- 服务指标仍固定报告 `database.state=not-initialized`；生产接线属于 M5-09，本任务不伪造已接入。
- 任务开始时 `git status --short` 为空；无用户既有修改。

## 前置条件与假设

- v1 是首个正式数据库 schema，因此唯一自动迁移链为 0→1；应用不自动降级。
- 事件目录采用 M5-06 schema v1，正式目录路径为 `YYYY/MM/DD/EventId`；`.transactions` 和 `.quarantine` 永不进入索引。
- 物理恢复要求数据库对象已关闭；恢复入口不会删除备份，目标库由 SQLite backup API 一致覆盖。
- SQLite 3.53.4 已由 vcpkg manifest 记录引入原因和版本，不新增生产依赖。

## 设计说明

公开 `EventMetadataDatabase` 使用 PImpl，头文件只包含领域值对象、路径、时间和 `Result`。实现持有一个 RAII SQLite 连接和互斥量，设置 foreign_keys、extended result codes、固定 busy timeout，并对迁移和关键写事务串行化。schema 仅使用整数、REAL、TEXT 和外键，不存图像 BLOB。事件索引事务先解析并验证 manifest，再以 EventId upsert 主表，替换该事件的相机、关键帧和文件子项；任一步失败全部回滚。

分页 limit 固定为 1～200，offset 和对账扫描均有硬上限。时间按 UTC Unix epoch 毫秒存储和筛选。查询按 `candidate_time_utc_ms DESC, event_id DESC` 稳定排序。SQLite 原始扩展码只放入 `nativeDomain=sqlite/nativeCode`，业务层使用 `DATABASE_BUSY`、`DATABASE_CORRUPT`、`DATABASE_MIGRATION_FAILED`、`DATABASE_SCHEMA_UNSUPPORTED` 或 `DATABASE_RECONCILE_FAILED`。

### 线程和队列

不新增线程或跨线程队列。调用方在 M5-09 组合根中从既有有界存储工作路径调用；本类互斥串行化同一连接的写入、迁移、备份和查询。SQLite busy timeout 有固定截止时间，绝不无限等待。

### 持久化与恢复

- 新库在事务中创建 schema 并最后设置 `user_version=1`；失败自动回滚。
- 既有 version 0 库迁移前使用 SQLite online backup API 写入唯一备份文件，备份成功后才迁移。
- `quick_check` 非 `ok` 或 SQLite 损坏码返回 `DATABASE_CORRUPT`，不会静默重建。
- 显式恢复使用 SQLite backup API 从只读源复制到目标，并在恢复后执行完整性检查；备份保留。
- 对账只读取通过 M5-06 manifest/文件校验的正式目录。目录存在而库缺失则补建；库存在而目录缺失则标 `Missing`；不删除双方数据。

### 错误和降级

- busy/locked：`DATABASE_BUSY`，可重试；固定等待后返回。
- corrupt/not-a-database：`DATABASE_CORRUPT`，Critical，不自动覆盖。
- 迁移 SQL/提交失败：`DATABASE_MIGRATION_FAILED`，Critical，事务回滚。
- user_version 高于 1：`DATABASE_SCHEMA_UNSUPPORTED`，Critical。
- manifest、目录扫描或索引失败：`DATABASE_RECONCILE_FAILED`，保留文件事实并报告具体 EventId/路径。
- 其他 SQLite 操作：`DATABASE_ERROR`，保留扩展错误码和操作名。

## 实施步骤

- [x] 1. 阅读任务、需求、架构、领域版本语义、M5-06 文件事实源、错误码和构建/测试基线，固定模块边界。
- [x] 2. 新增数据库公开契约和 PImpl，实现连接配置、完整性检查、schema v1、0→1 事务迁移及迁移前备份。
- [x] 3. 实现显式一致备份/恢复、manifest 原子索引、分页筛选和有界目录对账。
- [x] 4. 新增数据库定向测试，覆盖新建、迁移、重复迁移、高版本、损坏、备份恢复、分页筛选和双向不一致对账。
- [x] 5. 运行任务文件格式检查、`git diff --check`、Debug/Release 构建和两套 CTest，记录既有或环境阻断。
- [x] 6. 回写路线图完成证据和本计划的决策、发现、验证及完成摘要，不开始 M5-08/M5-09。

## 验证计划

### 自动化测试

- 临时中文/空格路径上的新库创建、九表/索引/外键和 user_version=1；
- 版本 0 旧库迁移、迁移前备份可打开、重复打开不重复迁移；
- 高版本拒绝、随机损坏文件检测、有效备份恢复损坏目标；
- 两个以上已提交事件按时间、状态、相机筛选，验证稳定分页边界和 limit 上限；
- 目录独有事件补索引、数据库独有事件标 `Missing`、再次对账幂等；
- 非法 manifest 或校验不一致不写入半条索引。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行任务文件 clang-format dry-run、`cmake --build ... --target format-check` 和 `git diff --check`；全仓既有格式问题若仍存在，单独记录，不越界修改。

### 人工或硬件验证

- 环境：不适用；数据库行为使用本地临时文件和 M5-06 构造事件目录自动验证。
- 步骤：未执行真实进程强杀、断电、目标 NVMe 或实体相机测试。
- 预期：M5-09 生产接线及 M9 前再验证服务启动对账、并发采集和生产磁盘恢复。
- 证据保存位置：本计划验证证据表。

## 回滚与恢复

代码回滚只移除 M5-07 新增 API/实现/测试和 CMake 项，不删除数据库、事件目录或备份。schema v1 程序不自动降级；软件回滚需使用兼容版本或在数据库关闭后从迁移前备份恢复。对账标记 `Missing` 可在目录恢复后再次对账变回 `Present`。

## 验收标准

- [x] 九张元数据表和必要索引/约束创建成功，SQLite 不存图像 BLOB。
- [x] schema 0→1 事务迁移、迁移前备份、重复迁移和高版本拒绝均有测试。
- [x] 损坏检测和备份恢复有自动化证据。
- [x] 已提交事件可按时间、状态、相机有界分页查询。
- [x] 目录/数据库双向不一致按文件事实源可恢复对账且幂等。
- [x] 数据库错误返回稳定业务码并保留 SQLite 扩展码。
- [x] Debug/Release 构建和相关 CTest 已实际运行并记录结果。
- [x] 未实现 M5-08 或 M5-09，未修改无关文件。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、领域模型、错误码、M5-06 实现与测试；创建计划，状态 in-progress。
- 2026-08-04：完成 schema v1、事务迁移与备份恢复、事件索引、分页筛选和目录对账；7 项定向测试及仓库级验证通过，状态 completed。

## 决策记录

- DEC-001：文件系统已提交目录继续作为原始事实源；SQLite 只存可重建元数据，对账不反向修改 manifest。
- DEC-002：使用单连接加互斥的同步协调器，不新增后台线程；生产调用位置留给 M5-09 既有有界工作路径。
- DEC-003：首版数据库只支持 0→1 自动升级，不提供逆迁移；升级前备份并以 `PRAGMA user_version` 作为唯一 schema 版本。
- DEC-004：数据库孤儿记录标 `Missing` 而非删除，避免暂时挂载/权限故障导致元数据丢失。

## 意外发现

- `paperbreak_storage` 已链接 SQLite，但当前 CMake 目标 `SQLite::SQLite3` 会产生弃用提示；M5-07 将改用 CMake FindSQLite3 的正式目标 `SQLite3::SQLite3`。
- 服务的数据库指标和报警持久化尚未装配；按路线图 M5-09 组合根范围，本任务只交付可测试的 storage 数据库边界。
- 全仓格式检查当前首先报告未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp`；任务新增 C++ 文件单独检查通过，未越界修复该既有问题。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | `git status --short` | 通过 | 任务开始时工作区为空。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter=StorageMetadataDatabase.*` | 通过 | 7/7；覆盖 schema、迁移、备份/恢复、损坏、分页、筛选、对账和半索引拒绝。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug`；`ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | Debug `/W4 /WX` 全量构建成功，非硬件 CTest 23/23。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-release`；`cmake --build --preset local-windows-vs2026-release`；`ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | Release `/W4 /WX` 全量构建成功，非硬件 CTest 23/23。 |
| 2026-08-04 | `cmake --preset local-windows-vs2026-static-analysis`；`cmake --build --preset local-windows-vs2026-static-analysis --target paperbreak_storage` | 通过 | storage 及依赖的默认 MSVC 静态分析通过。 |
| 2026-08-04 | 任务文件 `clang-format --dry-run --Werror`；`git diff --check` | 通过 | 新增/修改的 M5-07 C++ 文件格式和差异空白检查通过。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 阻断（既有） | 首个报告为未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp`。 |
| 2026-08-04 | 实体相机、服务组合根、真实强杀/断电、生产 NVMe | 未执行 | 本任务使用本地临时 SQLite 和 M5-06 构造事件目录；生产接线属于 M5-09。 |

## 完成摘要

M5-07 已交付 schema v1 九表元数据数据库、0→1 原子迁移和迁移前不可覆盖备份、完整性/高版本拒绝、显式原子恢复、M5-06 正式目录原子索引、稳定有界分页筛选及以文件为事实源的双向幂等对账。7 项定向测试、Debug/Release 全量构建和两套 23/23 CTest、storage 静态分析均通过；服务/IPC/UI/报警写入和上传执行保持在 M5-09/M8，未执行硬件、强杀、断电或生产性能验证。
