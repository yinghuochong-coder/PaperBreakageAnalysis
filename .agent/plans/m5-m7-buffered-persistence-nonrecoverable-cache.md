# M5/M7：事件缓冲持久化与非恢复式 NVMe 缓存 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-09
- 最后更新：2026-08-09
- 路线图条目：M5 正式事件写入、M7 NVMe 滚动缓存（用户明确批准合并）
- 关联需求：4.13 NVMe 滚动缓存、4.14～4.17 事件与存储、4.18 上位机上传

## 目的与可观察结果

把正式事件和滚动缓存从“每次写入都要求断电耐久并同步读回校验”调整为普通缓冲写。新正式事件使用 manifest v3 和 `PBNVME2`，写入时由 Windows CNG 对实际成功写入的字节只计算一次 SHA-256，关闭文件后以非覆盖原子改名发布，目录以同卷非覆盖原子改名提交。提交和启动对账只检查 manifest、路径、文件存在性和长度，不读取原始负载；详情、导出和在线上传才执行完整内容校验。

滚动缓存每次服务进程启动使用唯一 `cacheRoot/sessions/<session-id>`，索引、代次和租约仅在当前会话有效；启动不扫描、修复、迁移或删除任何旧 session/旧版根目录块。目标是在同机 Release、1624×1240 Mono8、911 帧、约 1.709 GiB 模拟场景达到至少 100 MiB/s，冻结窗口到 `Committed` 不超过约 18 秒（不含后置窗口采集时间），并证明索引阶段没有第二次全量读取。

## 范围

### 范围内

- 新增 ADR-017，并更新需求、架构、领域模型、IPC、错误码、NVMe 格式验证资料、路线图和 M7-04 历史证据说明。
- manifest v3、`PBNVME2`、CNG SHA-256 缓冲写结果、普通原子文件/目录发布；v2/`PBNVME1` 只读检查和导出兼容。
- SQLite schema v5→v6 完整性状态迁移；按需检查、导出和上传失败后的数据库/上传任务状态。
- 上传连接前置检查、单遍文件 SHA/分块 SHA、缺块续传和源变化语义。
- 删除滚动缓存恢复接口、配置、指标、构建接线和启动恢复；会话目录、会话索引、从 1 开始代次、当前会话容量回绕和租约。
- 自动化故障注入、Debug/Release 构建与 CTest，以及本机模拟性能测量（环境允许时）。

### 范围外

- 不自动删除、迁移或重写既有正式事件、旧 session、旧根目录块或配置。
- 不增加后台完整性校验线程，不自动检查长期 `Unverified` 事件。
- 不执行或宣称实体相机、物理断电、拔盘、跨重启缓存恢复和生产上位机验收。
- 不开始后续 M9 工作，不做与持久化/上传/滚动缓存无关的重构。

## 当前基线

- 工作区开始时干净。
- `src/storage/src/event_store.cpp` 自研逐字节 SHA-256；新事件为 manifest v2/`PBNVME1`，逐帧 CRC、Data CRC，写后同步读回；manifest 写后也同步读回。
- `src/storage/src/event_file_system_win.cpp` 使用 `FILE_FLAG_WRITE_THROUGH`、`FlushFileBuffers`、`MOVEFILE_WRITE_THROUGH`，原始块分两阶段刷新/提交。
- `src/storage/src/metadata_database.cpp` schema v5，索引和对账通过完整 manifest 校验读取所有负载，没有完整性状态列。
- `src/storage/src/event_inspector.cpp` 检查、详情和导出之间重复读取相同文件。
- `src/uplink/transport/src/qt_transport.cpp` 上传在连接检查前预读整文件，之后再次逐块读取。
- `src/storage/src/nvme_cache.cpp`、`nvme_recovery_win.cpp` 和 `nvme_recovery.hpp` 启动扫描/修复/重建旧缓存和租约；滚动写块为 `PBNVME1` 两阶段耐久提交。
- 旧 ADR-011/M7-04 和系统架构要求断电恢复；本任务由 ADR-017 显式取代该部分。

## 前置条件与假设

- 目标为 Windows 10/11 x64、MSVC、C++20；CNG `bcrypt` 是 Windows 系统库，不增加第三方生产依赖。
- `Committed` 表示文件已关闭且事件目录已发布到操作系统命名空间，不表示突然断电后可恢复。
- 普通缓冲写接受突然断电后最近事件/缓存丢失或损坏的风险；运行期显式短写、取消、CNG 或文件 API 失败仍保留事务/partial 并返回稳定业务错误。
- `maximumCacheStorageGiB` 只限制当前 session；旧数据继续占用卷空间，由现有卷级 warning/critical/stop 水位保护。
- 本机性能结果只代表模拟相机和当前 WD Green SN350；若构造 1.709 GiB 场景受内存或执行时间限制，记录为未执行/受限而不伪造通过。

## 设计说明

`IEventFileSystem` 的新文件写接口返回 `{bytesWritten, sha256}`；Windows 实现用 `CreateFileW(CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN)`，每次 `WriteFile` 成功后仅把实际写入片段送入 `BCryptHashData`，关闭句柄后用 `BCryptFinishHash` 取得小写十六进制摘要。原始块先完整写入 `.partial`（尾页已含发布标记），关闭后普通 `MoveFileExW(..., 0)` 非覆盖发布；事件目录同卷普通非覆盖移动。文件写接口支持停止令牌，短写和 CNG 故障不发布目标。

manifest v3 显式记录 `writeMode=buffered`、`powerLossDurable=false`、`verification=upload-or-on-demand`。`PBNVME2` 沿用 4 KiB header/index/footer 和字段偏移；索引项 76～79 的逐帧 CRC、footer 60～63 的 Data CRC 为零，索引项结构 CRC、header/index/footer CRC 保留。历史 manifest v2/`PBNVME1` 仍按旧 CRC 和 SHA 规则只读检查。

结构检查只解析 manifest、约束目标/相对路径、检查条目类型和声明长度；完整检查每文件只顺序读取一次，同时计算 SHA 并检查块结构。检查成功将数据库完整性改为 `Verified`；失败保留判定、复核和 `Committed`，改为 `storage_state=Damaged`、`artifacts_available=false`、`integrity_state=Failed`，未完成上传任务改为 `ManualIntervention`。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| `event.persistence` | 事件运行时 | 单事件写线程 | 既有默认 8、硬上限 64 | 拒绝新事件并报警 | stop 后不接收，排空/取消有确定截止时间 | 深度、高水位、字节、耗时、MiB/s、失败 |
| `nvme.blocks` | 预处理/缓存分支 | 单 NVMe 写线程 | 每相机 2 | 拒绝新块，不反压采集 | 停止新块，当前 session 内完成已入队块或截止取消 | 深度、回绕、租约、字节、速率、失败 |
| `upload_jobs` | SQLite 调度 | 单上传工作线程 | 数据库默认 10000 条及字节上限 | 不领取更多，不丢任务 | checkpoint 当前任务，停止后恢复为 RetryWait | 待处理数/字节、重试、人工处理 |

不增加线程或无界队列。

### 持久化与恢复

- manifest v3 为新写格式；v2 不迁移、不重写。
- SQLite schema v6 迁移前保留现有备份流程，新增三列并把既有/新建正常事件初始化为 `Unverified`。
- 启动事务恢复和目录对账仅做结构/存在/长度检查；完整事务可最佳努力发布，其他事务隔离，不读原始内容。
- 滚动缓存不再有跨重启恢复。启动只唯一创建当前 session 根和其中 `.index`；旧目录完全忽略。
- 回滚不删除任何数据；旧程序不认识 v3/PBNVME2，因此代码回滚前应停止新写并保留数据供新版重新启用。

### 错误和降级

- `EVENT_WRITE_FAILED`、`EVENT_WRITE_CANCELLED`、`EVENT_CHECKSUM_FAILED`：保留事务目录并将事件置 `Incomplete`。
- `EVENT_INTEGRITY_FAILED`：按需检查失败，事件标记损坏并阻止详情内容、导出和上传。
- `UPLINK_DISCONNECTED`：连接不可用时零源文件读取，可重试。
- `UPLOAD_SOURCE_CHANGED`：内容/长度与 manifest 不一致，不调用完成接口，转人工处理并保留 checkpoint。
- `UPLOAD_CHECKSUM_MISMATCH`、网络/服务端临时错误：沿既有有界退避；重试耗尽后人工处理。
- Win32/CNG/SQLite/HTTP 原生错误只作诊断，不替代业务错误码。

## 实施步骤

- [x] 1. 新增 ADR-017 和 v3/PBNVME2 格式基线；添加 CNG SHA/CRC32C 公共内部实现和单元测试，链接 `bcrypt`，删除自研 SHA。
- [x] 2. 改造事件文件系统和事务写入：缓冲写单次 SHA、完整尾页一次写、无 flush 原子发布、manifest 最后写且零读回；以文件系统 spy/fault 覆盖短写、取消、CNG 故障、非覆盖和零读取。
- [x] 3. 实现 v2/v3 结构解析与单遍完整检查/导出；详情/导出成功或失败回写完整性状态，导出失败不发布 partial。
- [x] 4. 升级 SQLite v6 和 IPC DTO，完成迁移备份、三状态、损坏副作用、查询/manifest 结构检查以及未完成上传任务人工处理测试。
- [x] 5. 调整上传：连接检查先于源文件访问；在线时单遍源文件读取同时计算整文件/分块 SHA，只上传缺块，源变化不 complete；覆盖离线零读、续传、校验错误和重试耗尽。
- [x] 6. 删除 NVMe 恢复模块与启动接线，创建唯一 session 根和空会话索引；PBNVME2 缓冲写取消逐帧/Data CRC和双 flush，保留结构 CRC、容量回绕、租约、限速、截止和有界关闭测试。
- [x] 7. 更新需求、架构、领域模型、IPC、错误码、格式验证、运维清理说明、路线图和 M7-04 取代标记；运行 Debug/Release 构建与 CTest，执行可行的 Release 模拟性能验证并回填真实证据。

## 验证计划

### 自动化测试

- CNG：空输入、标准向量、多分块、短写、取消、API 故障，与 Qt/已知 SHA-256 对比。
- 事件：v3/PBNVME2 字段/结构 CRC/保留零；持久化和索引原始负载零读；每文件一次 hash；manifest 最后、非覆盖、目录发布可见性；v2/PBNVME1 检查/导出。
- 数据库/IPC：v5→v6 备份迁移；完整性三状态；失败不改变算法/复核/Committed；损坏事件阻止内容、导出和上传。
- 上传：离线零读；在线、断点续传、源变化、块/整文件错误和重试耗尽。
- NVMe：不调用恢复、不扫描或修改旧目录；session 唯一、索引为空、代次从 1；无 `FlushFileBuffers`；当前 session 回绕和租约不变。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug --output-on-failure
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release --output-on-failure
```

### 人工或硬件验证

- 环境：本机 WD Green SN350、Release、模拟相机；1624×1240 Mono8、911 帧、约 1.709 GiB。
- 步骤：冻结后提交，记录实际写入字节、耗时、MiB/s、CPU、事件队列深度和模拟采集丢帧；检查 SQLite 索引阶段读取计数。
- 预期：至少 100 MiB/s，冻结到 `Committed` 约 18 秒内，索引无第二次负载读取。
- 证据保存位置：本 ExecPlan 验证证据和 `docs/validation/` 对应说明。
- 实体相机、生产上位机、物理断电/拔盘、跨重启缓存恢复：未执行且不再作为新缓存策略能力。

## 回滚与恢复

保留所有 `.transactions`、正式事件、旧/new session、旧版根目录块和数据库迁移备份。代码失败时只回退本任务源码/文档，不清理数据。schema v6 回滚必须使用迁移前备份且先停止服务；不得让旧二进制直接打开 v6 数据库。v3/PBNVME2 文件不降级重写，需由支持它们的版本继续读取。

## 验收标准

- [x] 新事件为 manifest v3/PBNVME2 缓冲写，CNG 对实际写入字节单次 SHA，持久化和索引零负载读回。
- [x] v2/PBNVME1 只读检查和导出兼容；启动/列表/getManifest 仅结构检查。
- [x] schema v6 完整性语义、失败副作用、上传人工处理和 Critical 报警可观察。
- [x] 上传离线零读取，在线单遍校验/分块并只补缺块，源变化不 complete。
- [x] NVMe 启动不恢复/扫描旧缓存，唯一 session 内索引、回绕和租约正确，普通缓冲写不 flush。
- [x] 相关测试、Debug/Release 构建和 CTest 实际运行；性能结果有真实记录。
- [x] 文档同步且未修改无关文件；未宣称实体相机、断电或跨重启恢复测试。

## 进度记录

- 2026-08-09：阅读需求、架构、路线图、ExecPlan 规范、ADR-011、M5/M7/M8 既有计划及关键源码；确认工作区干净，创建合并计划，状态 in-progress。
- 2026-08-09：完成事件 CNG 缓冲写、manifest v3/PBNVME2、单遍按需检查/导出、SQLite v6 完整性状态、上传前置连接检查及单遍校验、非恢复式会话缓存和相关文档/测试。
- 2026-08-09：Debug/Release 配置、构建和 CTest 通过；Release 911 帧性能门禁通过，状态改为 completed。

## 决策记录

- DEC-001：按用户明确授权合并 M5 正式事件写入和 M7 非恢复式缓存；不延伸到其他里程碑。
- DEC-002：SHA 只由实际文件写入/读取流计算，manifest 从写入结果组装，禁止提交路径同步读回。
- DEC-003：v3/PBNVME2 复用物理字段偏移但把负载 CRC 保留字段清零；v2/PBNVME1 保持严格旧校验。
- DEC-004：滚动缓存 session 是唯一运行期事实源，旧数据永久忽略且只能停服后人工清理。

## 意外发现

- 当前事件块先在内存中计算自研 SHA，文件系统耐久写后又同步读回并再次自研 SHA；manifest 还另有写后读回，性能损耗比单纯 `FlushFileBuffers` 更大。
- 当前检查器的详情会先完整验证再读取块，导出又会再次读取，同一原始文件可被读取三次。
- 当前上传在检查连接之前预读整文件，离线任务仍会产生全部读盘 I/O。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-09 | 基线检查与 `git status --short` | 完成 | 工作区开始时干净；尚未运行构建、测试或硬件验证。 |
| 2026-08-09 | Debug 配置、构建、CTest | 通过 | `ctest --preset local-windows-vs2026-debug --output-on-failure`：28/28；单元测试 360 项，性能用例按 Debug 规则跳过。 |
| 2026-08-09 | Release 配置、构建、CTest | 通过 | `ctest --preset local-windows-vs2026-release --output-on-failure`：28/28；单元测试 360 项。首次全量运行的 unit 项瞬时失败，直接复现 360/360 通过，随后完整 CTest 复测 28/28 通过。 |
| 2026-08-09 | Release 911 帧性能门禁 | 通过 | 1,834,847,924 字节，2,575 ms，679.432 MiB/s，CPU 66.7353%，队列高水位 1，拒绝 0，生成/写入 911 帧、模拟丢帧 0，索引负载读取 0。 |
| 2026-08-09 | M7-01 格式验证 | 通过 | v1 只读兼容与 ADR-017 v2 缓冲格式验证脚本通过。 |

## 完成摘要

已完成计划范围内实现。正式事件提交路径不再同步读回，完整内容校验移至详情、导出和在线上传；新滚动缓存只管理当前进程唯一 session，不恢复或清理历史缓存。自动化验证及本机 Release 模拟性能门禁均通过。未执行实体相机、物理断电、拔盘、生产上位机或跨重启恢复测试，且不对这些能力作完成声明。
