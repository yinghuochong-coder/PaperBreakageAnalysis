# R0-03：PBNVME3 与 manifest v4 格式门禁 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-14
- 最后更新：2026-08-14
- 路线图条目：R0-03 PBNVME3 与 manifest v4 格式门禁
- 关联需求：EDGE-DATA-001～005、EDGE-CFG-001～004

## 目的与可观察结果

在 D2 编写生产读写器前冻结严格时间证据、PBNVME3 二进制布局和 manifest v4 JSON 语义。完成后，仓库包含可机器复验的字段合同、最小/多帧/不完整帧/无校正时间黄金样例，以及能稳定发现损坏、缺尾标、越界和未来版本的独立格式检查器。

## 范围

### 范围内

- 新增时间证据模型 ADR，明确原始值、接收值、校正值、模型身份和历史不可变性。
- 新增 PBNVME3/manifest v4 ADR，冻结小端布局、稳定枚举、固定上限、CRC32C、SHA-256、尾标和发布语义。
- 新增四组 v3/v4 黄金样例、字段合同和独立 PowerShell 检查器。
- 把格式检查器接入 CTest，并记录 PBNVME2/manifest v3 只读兼容与未来版本拒绝规则。

### 范围外

- 不实现 D2-01/D2-02 的生产 PBNVME3 读写器、manifest v4 writer、SQLite v7 或配置快照工作队列。
- 不修改现有 PBNVME1/2 或 manifest v2/v3 的生产解析、校验、导出和上传行为。
- 不实现 T1 时间运行时、相机时间探针、外部 T0 锁定或后续路线图任务。

## 当前基线

- 工作区开始时干净，R0-01/R0-02 已完成。
- `event_store.cpp` 和 `nvme_block_file_win.cpp` 写 `PBNVME2`；`event_inspector.cpp` 只读识别 `PBNVME1/2`。
- manifest 当前 schema 为 v3，声明普通缓冲写、非断电耐久和上传/按需校验。
- ADR-011 冻结 4096 B 页、96 B v1/v2 索引、CRC32C 和容量公式；ADR-017 冻结 PBNVME2/v3 缓冲发布语义。
- R0-02 已冻结 `FrameTimeMetadata`、稳定字符串枚举和 JSON 纳秒十进制字符串规则，但尚无独立时间模型 ADR 或持久格式布局。

## 前置条件与假设

- R0-01 已完成；R0-02 的时间字段合同作为本任务输入，不改变其线上语义。
- PBNVME3 仍是不压缩的每相机原始帧块；整文件 SHA-256 保存于 manifest，不能自引用写入块本身。
- 本任务不需要实体相机、MVS、PTP/Grandmaster 或生产 NVMe；不声明同步精度、硬件吞吐或断电耐久通过。

## 设计说明

PBNVME3 使用独立 `PBNVME3\0` magic、formatVersion=3、4096 B 头尾页和 160 B 定长逐帧索引。每项并列保存原始 ticks/频率、接收单调偏移、接收 UTC、可空校正 UTC/偏移/不确定度、模型修订、稳定时间枚举、帧身份、几何、格式、数据范围及不完整标志；可空字段由显式位控制，零不是不可用哨兵。

有效索引项逐项 CRC32C，索引区和有效负载区分别 CRC32C；manifest 对每个文件保存 `sha256:<64 lowercase hex>`。块只有完整 `PBCOMMIT` 尾页、`COMMIT3\0` 标记和全部结构/范围/校验通过后才可读。临时文件关闭后用同卷、不覆盖原子改名发布；该命名空间提交不表示断电耐久。

manifest v4 保留 v3 既有业务字段语义并增加规范 T0、触发节点、实际相机时间范围、模型身份、整体时间质量、PBNVME 版本/校验和不可变实际配置快照。纳秒 JSON 字段使用规范十进制字符串；不可用值为 `null` 且 availability 标志为 false。

### 线程和队列

不适用。本任务只增加离线文档、样例和同步执行的检查器，不创建运行时线程或跨线程队列。D2 的存储工作队列仍须沿用既有有界队列和确定性关闭合同。

### 持久化与恢复

黄金文件由确定性生成逻辑产生并以 SHA-256 复验。PBNVME3 采用完整尾标和同卷不覆盖发布；PBNVME2/manifest v3 只读、校验、导出和上传，不迁移、不原地重写。未知更高块版本和 manifest schema 必须在读取任何声明负载前拒绝。

### 错误和降级

- PBNVME 未知版本：`NVME_FORMAT_UNSUPPORTED`；保留文件，不猜测低版本布局。
- manifest 未知版本：`EVENT_SCHEMA_UNSUPPORTED`；不部分解析为完整事件。
- 缺失/破损尾页：`NVME_BLOCK_INCOMPLETE`；不作为已提交块使用。
- CRC、保留位、范围或长度不一致：`NVME_BLOCK_CORRUPT`；隔离/拒绝，不返回部分完整结果。
- manifest 文件长度/SHA 不一致：`EVENT_CHECKSUM_FAILED`；拒绝导出和上传，保留证据。

## 实施步骤

- [x] 1. 新增时间证据 ADR，冻结持久化含义、模型身份和枚举映射。
- [x] 2. 新增格式 ADR和机器可读字段合同，冻结 PBNVME3 与 manifest v4。
- [x] 3. 新增确定性黄金生成/独立检查脚本和四组黄金样例。
- [x] 4. 增加负向故障注入、未来版本和 PBNVME2/manifest v3 兼容检查，并接入 CTest。
- [x] 5. 更新架构索引、错误码、路线图状态和本计划验证证据。

## 验证计划

### 自动化测试

- 默认检查四组 v3/v4 黄金文件的 magic、版本、字节序、页/索引大小、稳定枚举、保留位、范围、连续数据、CRC32C、尾标、manifest 字段、长度和 SHA-256。
- 内存故障注入覆盖头/索引/负载损坏、尾页截断、数据偏移越界、块未来版本、manifest 未来版本和 SHA 不匹配。
- 旧格式检查确认 PBNVME2/manifest v3 设计合同仍存在且关键 magic/version/write policy 未变，检查过程不写旧文件。

### 构建与测试命令

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate-r0-03-formats.ps1
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
```

### 人工或硬件验证

- 环境：实体相机、PTP/Grandmaster、生产 NVMe 和断电注入均不属于本格式契约任务。
- 状态：未执行；硬件时间精度、吞吐、断电和正式上位机兼容仍待 T1/D2/V5。

## 回滚与恢复

本任务不修改生产数据或 schema。审查失败时仅回退新增 ADR、合同、黄金样例、检查器和 CTest 注册；不得删除或改写既有事件、PBNVME1/2 或 manifest v2/v3 文件。

## 验收标准

- [x] 时间证据与模型身份有唯一、不可变解释。
- [x] PBNVME3 小端布局、数据区、CRC32C、SHA-256、完整尾标、固定上限和发布规则已冻结。
- [x] manifest v4 的 T0、触发来源/节点、实际范围、模型、质量、块和校验字段已冻结。
- [x] 四类黄金样例和字段说明已提交且独立检查通过。
- [x] 损坏、缺尾标、越界、错误版本和 SHA 不匹配均被稳定发现。
- [x] PBNVME2/manifest v3 生产行为未改变，仍明确只读兼容且未来版本拒绝。
- [x] Debug/Release 构建与 CTest、静态分析完成；未执行硬件测试的限制已记录。

## 进度记录

- 2026-08-14：阅读需求、架构、路线图、PLANS、PBNVME1/2 ADR、生产读写器和现有测试，创建计划并进入 `in-progress`。
- 2026-08-14：完成 ADR、机器合同、四组黄金文件、独立检查器、故障注入和 CTest 接入；Debug/Release/静态分析门禁通过，状态更新为 `completed`。

## 决策记录

- DEC-001：R0-03 只冻结格式和验证向量，不提前修改 D2 生产读写路径。
- DEC-002：PBNVME3 使用 160 B 新索引和独立 magic，不尝试在 96 B PBNVME2 索引中复用保留字节。
- DEC-003：CRC32C 覆盖结构及有效负载以定位损坏；整文件 SHA-256 由 manifest 保存，避免块内自引用摘要。
- DEC-004：runtime 模型身份以完整 UUID 文本保存在 manifest，块头只保存该 UUID SHA-256 前 8 B 的小端快速关联值；该值不承担认证或唯一性事实源职责。

## 意外发现

- 当前 v3 writer 主动拒绝不完整帧，而新增需求要求格式能表达不完整标志；黄金样例会验证格式表达能力，但是否保存这类帧属于 D2 策略。
- 当前系统时间合同规定单调 epoch 不跨进程复用；PBNVME3 只保存相对块起点的单调偏移，跨节点和跨重启检索以 UTC/模型证据为准。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-14 | 需求、架构、旧格式和源码只读检查 | 完成 | 工作区开始时干净；确认生产路径保持 v2/v3 |
| 2026-08-14 | `powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate-r0-03-formats.ps1` | 通过 | 四组黄金、CRC/SHA/范围/尾标通过；八类负向变体稳定拒绝；旧合同只读复验 |
| 2026-08-14 | 黄金文件 `-Regenerate` 前后 SHA-256 对比 | 通过 | 八个文件逐一相同，生成确定性通过 |
| 2026-08-14 | `cmake --preset local-windows-vs2026-debug` 及 build | 通过 | PATH 无 `cmake`，改用 VS 18 Community 随附 CMake 绝对路径；仅有可选 Vulkan headers 缺失警告 |
| 2026-08-14 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | 最终全量 33/33；含格式和 PBNVME1/2 设计回归 |
| 2026-08-14 | Release configure/build/CTest | 通过 | 最终全量 33/33；含格式门禁 |
| 2026-08-14 | static-analysis configure/build | 通过 | MSVC 静态分析目标完成，无失败 |
| 2026-08-14 | `git diff --check` | 通过 | 无空白错误 |
| 2026-08-14 | 实体相机、PTP/Grandmaster、生产 NVMe、断电和正式上位机 | 未执行 | 本任务为格式契约；留待 T1/D2/V5 |

## 完成摘要

R0-03 已完成。ADR-018/019 冻结不可变时间证据、模型身份、PBNVME3 二进制布局和 manifest v4；机器合同、四组确定性黄金文件及独立检查器已提交，损坏、截断、越界、未来版本和 SHA 错误均能稳定发现，PBNVME2/manifest v3 保持只读兼容且生产源码未修改。Debug/Release 全量 CTest 均为 33/33，静态分析构建通过。未执行实体硬件、PTP、生产 NVMe、断电或正式上位机测试，也未开始 D2-01。
