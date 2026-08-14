# ADR-019：PBNVME3 与 event manifest v4 格式

- 状态：Accepted
- 日期：2026-08-14
- 决策范围：R0-03、D2、E3、V5
- 关联需求：EDGE-DATA-001～005、EDGE-CFG-001～004
- 继承：ADR-011 的容量/CRC 基线、ADR-017 的缓冲写与 session 语义

## 背景

PBNVME2 的 96 B 索引只保存接收时间、可选相机 ticks 和基础帧字段，不能无歧义保存 R0-02/ADR-018 冻结的校正 UTC、偏移、不确定度、同步状态和模型修订。manifest v3 也没有统一 T0、触发节点、逐相机实际范围、模型身份或事件时实际配置快照。在修改生产读写器前，必须用独立版本、字节布局和黄金文件冻结互操作语义。

## 决策

### 兼容与版本选择

- PBNVME3 使用 8 字节 `PBNVME3\0` magic 和 `formatVersion=3`；旧读取器不能把它识别为 PBNVME2。
- event manifest v4 使用根字段 `schemaVersion: 4`。v4 不是把未知字段附加到 v3 后继续声称 v3；读取器必须按 schema 选择完整验证器。
- PBNVME2/manifest v3 保持只读校验、导出和上传兼容，不迁移、不原地重写。需要 v4 派生导出时创建新对象并记录来源摘要。
- 版本 0、负数、未知低版本和任何高于当前支持范围的版本明确拒绝。PBNVME 未知版本返回 `NVME_FORMAT_UNSUPPORTED`；manifest 未知版本返回 `EVENT_SCHEMA_UNSUPPORTED`。

### 通用二进制规则与上限

- 所有多字节整数按小端显式编码；禁止直接转储 C++ 结构体、MSVC padding、枚举序数、指针或 `time_point` 对象。
- 头页和尾页各 4096 B，索引项固定 160 B，索引区和数据区分别向 4096 B 对齐；未使用索引、数据 padding 和所有 reserved 字节必须为零。
- 每块只含一个规范相机 ID，UTF-8/ASCII 1～16 B；块时长固定 1000 ms；不压缩、不转换原始负载。
- `indexCapacity` 范围 1～4096，`frameCount` 范围 1～`indexCapacity`，`maxFrameBytes` 范围 1～134,217,728 B，文件硬上限 68,719,476,736 B（64 GiB）。声明布局计算溢出或超过上限时在分配/读取负载前拒绝。
- 稳定像素格式 ID 继承 ADR-011：1 `Mono8`、2 `Mono10LE16`、3 `Mono12LE16`、4 `BayerRG8`；0 和未知值拒绝。时间枚举 ID 见 ADR-018。
- `maximumBlockBytes = 4096 + align4096(indexCapacity*160) + align4096(indexCapacity*maxFrameBytes) + 4096`，必须等于实际文件长度。尾页固定放在最大数据区之后，不允许扫描文件寻找提交标记。

### 头页（4096 B）

| 偏移 | 长度 | 类型 | 字段 | 约束 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | bytes | `magic` | ASCII `PBNVME3\0` |
| 8 | 2 | u16 | `formatVersion` | 3 |
| 10 | 2 | u16 | `headerBytes` | 4096 |
| 12 | 4 | u32 | `endianTag` | `0x01020304` |
| 16 | 16 | bytes | `blockId` | RFC 4122 UUID 网络字节序 |
| 32 | 8 | u64 | `generation` | 当前 session/相机内大于 0 |
| 40 | 16 | bytes | `cameraId` | UTF-8，尾部补零 |
| 56 | 2 | u16 | `cameraIdBytes` | 1～16 |
| 58 | 2 | u16 | `initialPixelFormat` | 稳定 ID |
| 60 | 4 | u32 | `initialWidth` | 大于 0 |
| 64 | 4 | u32 | `initialHeight` | 大于 0 |
| 68 | 4 | u32 | `initialStride` | 大于 0 |
| 72 | 4 | u32 | `flags` | 当前必须为 0；未知位拒绝 |
| 76 | 4 | u32 | `blockDurationMs` | 1000 |
| 80 | 4 | u32 | `indexEntryBytes` | 160 |
| 84 | 4 | u32 | `indexCapacity` | 1～4096 |
| 88 | 4 | u32 | `dataAlignmentBytes` | 4096 |
| 92 | 4 | u32 | `maxFrameBytes` | 1～128 MiB |
| 96 | 8 | i64 | `startReceivedUtcNs` | 首帧接收 UTC |
| 104 | 8 | i64 | `startReceivedMonotonicNs` | 当前进程证据，不跨重启映射 |
| 112 | 8 | u64 | `startSequenceNumber` | 首帧服务序号 |
| 120 | 8 | u64 | `timeRuntimeInstanceHash` | runtime UUID UTF-8 的 SHA-256 前 8 B 按小端解释；仅快速关联，完整 ID 在 manifest |
| 128 | 4 | u32 | `headerCrc32c` | 本字段置零后覆盖整个头页 |
| 132 | 3964 | bytes | `reserved` | 全零 |

`timeRuntimeInstanceHash` 不是安全身份或完整摘要；值 0 表示 runtime ID 不可用，并导致整体时间质量降级。

### 逐帧索引项（160 B）

| 偏移 | 长度 | 类型 | 字段 | 约束 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | u64 | `sequenceNumber` | 项间严格递增 |
| 8 | 8 | u64 | `cameraFrameNumber` | 厂商帧号 |
| 16 | 8 | u64 | `cameraTimestampTicks` | availability 位为 0 时必须为 0 |
| 24 | 8 | u64 | `cameraTimestampFrequencyHz` | 与 ticks 同时可用且大于 0 |
| 32 | 8 | u64 | `receivedMonotonicDeltaNs` | 相对头页起点，首项为 0，非递减 |
| 40 | 8 | i64 | `receivedUtcNs` | Unix epoch ns |
| 48 | 8 | i64 | `correctedCaptureUtcNs` | availability 位为 0 时必须为 0 |
| 56 | 8 | i64 | `clockOffsetNs` | availability 位为 0 时必须为 0 |
| 64 | 8 | i64 | `uncertaintyNs` | 可用时非负，否则为 0 |
| 72 | 8 | u64 | `clockModelRevision` | 有校正时间时大于 0 |
| 80 | 8 | u64 | `dataOffset` | 文件绝对偏移，位于数据区且有效负载连续 |
| 88 | 4 | u32 | `payloadBytes` | 1～`maxFrameBytes` |
| 92 | 4 | u32 | `width` | 大于 0 |
| 96 | 4 | u32 | `height` | 大于 0 |
| 100 | 4 | u32 | `stride` | 大于 0 且 `payloadBytes <= stride*height` |
| 104 | 2 | u16 | `pixelFormat` | 稳定 ID |
| 106 | 1 | u8 | `clockSource` | ADR-018 稳定 ID |
| 107 | 1 | u8 | `syncState` | ADR-018 稳定 ID |
| 108 | 4 | u32 | `flags` | 见下表；未知位拒绝 |
| 112 | 4 | u32 | `payloadCrc32c` | 覆盖该项有效负载 |
| 116 | 4 | u32 | `entryCrc32c` | 本字段置零后覆盖整个 160 B 项 |
| 120 | 40 | bytes | `reserved` | 全零 |

索引 flags：bit 0 `incomplete`、bit 1 `cameraTimestampAvailable`、bit 2 `correctedCaptureUtcAvailable`、bit 3 `clockOffsetAvailable`、bit 4 `uncertaintyAvailable`。其他位为 0。ticks/frequency 必须同由 bit 1 控制；校正时间可用时 bit 4 同时置位、revision 大于 0 且 syncState 不能为 UNKNOWN/UNSYNCED。不完整帧仍可保存其实际收到的非空负载，但 manifest 的范围/完整性不得宣称 `COMPLETE`。

### 数据区

数据区起点为 `4096 + align4096(indexCapacity*160)`。有效帧负载按索引顺序紧密排列，不在帧间插 padding；首项 `dataOffset` 等于数据区起点，后续项等于前项末尾。有效负载之后直至预留数据区末尾全部为零。读取器必须先验证所有加法、范围和总和，再访问负载。

### 提交尾页（4096 B）

| 偏移 | 长度 | 类型 | 字段 | 约束 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | bytes | `magic` | ASCII `PBCOMMIT` |
| 8 | 2 | u16 | `formatVersion` | 3 |
| 10 | 2 | u16 | `footerBytes` | 4096 |
| 12 | 4 | u32 | `frameCount` | 1～capacity |
| 16 | 8 | u64 | `validIndexBytes` | `frameCount*160` |
| 24 | 8 | u64 | `validDataBytes` | payloadBytes 总和 |
| 32 | 8 | u64 | `maximumBlockBytes` | 实际文件长度 |
| 40 | 8 | i64 | `endReceivedUtcNs` | 最后一帧接收 UTC |
| 48 | 8 | u64 | `endSequenceNumber` | 最后一帧序号 |
| 56 | 4 | u32 | `indexCrc32c` | 有效索引项拼接字节 |
| 60 | 4 | u32 | `dataCrc32c` | 有效负载拼接字节 |
| 64 | 4 | u32 | `headerCrc32c` | 回显已验证头 CRC |
| 68 | 4 | u32 | `flags` | bit 0 表示任一不完整帧；其余为 0 |
| 72 | 4012 | bytes | `reserved` | 全零 |
| 4084 | 4 | u32 | `footerCrc32c` | 本字段置零后覆盖整个尾页，包括提交标记 |
| 4088 | 8 | bytes | `commitMarker` | ASCII `COMMIT3\0` |

CRC 统一为 ADR-011 的 CRC-32C/Castagnoli：反射多项式 `0x82F63B78`、初值/结果异或 `0xFFFFFFFF`，`123456789` 检查值 `0xE3069283`。检查顺序为：固定头和版本、声明上限/溢出、文件长度、头 CRC、固定尾位置/标记/尾 CRC、frameCount、索引项/索引区 CRC、范围、负载 CRC。缺尾标返回 `NVME_BLOCK_INCOMPLETE`；其余结构/CRC/范围不一致返回 `NVME_BLOCK_CORRUPT`。

### SHA-256 与发布

写入过程中对实际成功写入的完整文件字节流计算一次 SHA-256；规范文本为 `sha256:` 加 64 个小写十六进制字符，存入 manifest v4 的 raw block 和 `fileChecksums`。SHA-256 不写入块内，避免摘要自引用。块写入唯一同卷临时名，完整写尾页并关闭后，用不覆盖目标的同卷原子 rename 发布。manifest 最后写入，事件事务目录再以同样规则发布。继续沿用 ADR-017：不请求 write-through/`FlushFileBuffers`，`Committed` 不等于突然断电后耐久。

### manifest v4

机器可读字段合同位于 `docs/validation/r0-03/manifest-v4-fields.json`。v4：

- 保留 v3 的事件身份、决策、候选/确认/窗口、算法、工单、上传和缓冲写语义；`writeMode="buffered"`、`powerLossDurable=false`、`verification="upload-or-on-demand"` 不变。
- `eventT0` 保存 `available`、规范十进制字符串 UTC ns、触发来源、触发 machine/camera/node；不可用时 timestamp 为 null，事件仍可降级保存。
- `cameraActualRanges` 对每个 machine/camera 保存接收 UTC 范围、可空校正范围、帧数、序号缺口、完整性、同步状态、不确定度和模型身份。
- `clockModels` 以 `machineId + timeRuntimeInstanceId + cameraId-or-null + modelRevision` 唯一标识事件实际使用模型；不能虚构未观测字段。
- `overallTimeQuality` 保存聚合 syncState、可空最大不确定度和非重复稳定 reason codes；它不替代逐帧证据。
- 每个 `rawBlocks` 项声明 `format="PBNVME3"`、`formatVersion=3`、路径、相机、帧数、实际范围、长度、SHA-256 和四类 CRC32C。v2 块只能出现在显式旧 manifest v3；v4 新写事件不得混写 v2/v3 块。
- `cameraConfigSnapshots` 保存事件进入 Collecting 时实际回读值。每个可选设备参数使用 `{available, value, errorCode}`，不可用时 value 为 null 且 errorCode 为稳定业务错误或 null；禁止虚构 Gamma、白平衡、PTP 或缓冲大小。事件后的配置变化不得回写清单。
- `fileChecksums` 和 `fileSizes` 的键是安全规范相对路径；至少覆盖所有 raw block、关键帧和事件辅助文件，不包含 `manifest.json` 自身。
- manifest UTF-8 无 BOM，硬上限 8 MiB；相机最多 6、raw block 最多 262144、配置快照最多 6、模型最多 64、文件项最多 262208。未知根字段默认拒绝；仅命名为 `extensions` 的对象允许接收方忽略未知扩展。

### 黄金文件

`docs/validation/r0-03/golden/` 包含最小一帧、多帧、不完整帧和无校正时间四组 PBNVME3/manifest v4。它们是字段与字节合同，不代表生产 writer 已实现。生成器必须确定性复现相同字节；独立检查器复算所有 CRC/SHA 并注入损坏、截断、越界和未来版本。

## 后果

- 新旧格式不会因保留字节复用而产生歧义，严格时间证据可逐帧复核。
- 160 B 索引及逐帧/数据 CRC 增加元数据和 CPU 开销；D2/V5 必须测量并保持采集线程不执行磁盘或 CRC。
- 固定最大数据区可能形成稀疏/预留文件，writer 必须按已应用上界做容量准入，不能按平均帧大小扩容。
- v4 配置快照较大但有 8 MiB、数量和字符串上限，不允许无界厂商节点转储。

## 被否决的方案

### 在 PBNVME2 的 12 B reserved 中塞入时间字段

否决。空间不足且会改变 v2 字段语义，使旧读取器静默忽略关键证据。

### 只在 manifest 保存每帧时间

否决。会制造与原始块分离的巨大并行索引，难以做逐帧范围和损坏定位。

### 把整文件 SHA-256 写进块尾

否决。摘要字段会参与自身摘要，需要排除规则或二次格式，容易产生互操作歧义。manifest 是最终文件摘要事实源。

### 自动原地升级旧事件

否决。会改变已提交证据、摘要、上传幂等对象和人工复核事实。

## 验证要求

- 独立检查器和黄金文件必须在 Debug/Release CTest 中运行，并能发现损坏、尾标缺失、越界、错误块版本、错误 manifest schema 和 SHA 不匹配。
- D2-01 必须用同一黄金文件驱动 C++ reader/writer 往返，覆盖 PBNVME2 独立回归、部分写入和取消。
- D2-02 必须覆盖 v3/v4 查询/上传、配置回读失败、事件后配置变化及 SQLite v6→v7 迁移。
- 生产 NVMe 带宽、六台实体相机、物理断电和正式上位机导入仍待 V5；本 ADR 和模拟黄金文件不构成硬件验收。
