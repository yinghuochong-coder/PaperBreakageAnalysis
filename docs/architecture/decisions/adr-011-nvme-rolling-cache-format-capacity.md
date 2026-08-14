# ADR-011：NVMe 滚动缓存块格式与容量预算

- 状态：Accepted
- 日期：2026-08-05
- 决策者：项目架构基线
- 关联：M7-01、DEC-007、需求 4.7/4.13/4.17、架构 7～9/12.4/13/17/19

## 背景

NVMe 滚动缓存需要持续保存最多六路相机的原始帧，同时不能把磁盘延迟反压到采集线程。需求允许每 1 秒或 5 秒形成一个块，并要求索引、校验、固定总容量、事件保护和断电恢复。实现前必须固定块的互操作语义和最坏资源预算，否则不同写入、扫描和事件引用实现可能对同一文件产生不同解释。

当前默认相机配置是 1624×1240、Mono8、60 fps，但生产 ROI、步幅、像素格式、相机数量和目标 NVMe 尚未最终批准。M3 仅验证过一台实体相机约 58.35 fps，不能作为六路磁盘吞吐证据。因此本 ADR 接受格式和公式，不批准某个生产容量或某块 NVMe 的性能。

## 决策

### 分块与文件生命周期

- v1 每相机独立形成 1000 ms 块。帧按服务 `sequenceNumber` 严格递增，同一相机的块不得混入其他相机帧；
- 时间窗以接收单调时钟划分，文件只保存相对块起点的单调纳秒偏移；UTC 纳秒用于跨重启检索，并保留相机时间戳质量标志；
- 块先写入同一卷上的临时文件。写线程写完头、有效索引、原始数据和校验后，最后写提交尾页、flush 文件，再原子发布；目录 flush 能力和 Windows 具体调用由 M7-02/M7-04 验证；
- 正常回绕先逻辑淘汰并删除最旧的未保护已提交块，再为新块预分配。事件保护块、当前临时块和无法确认状态的块不能被覆盖；没有可淘汰空间时停止普通滚动缓存并记录缺口；
- v1 不压缩、不转换像素、不封装视频，原样保存 `FrameView::bytes()` 的有效字节。不得用 `width × height` 替代可能带 padding 的 `stride × height` 上界；
- 选择 1 秒而不是 5 秒，是为了把断电尾部损失、事件租约粒度、队列驻留和单块恢复扫描量缩小到五分之一。每秒约 8 KiB 页面开销和每帧 96 B 索引相对 2 MP 原始帧负载可忽略。

### 字节序和通用规则

- 所有多字节整数均按小端编码；有符号 UTC 纳秒采用二进制补码；UUID 使用 RFC 4122 的 16 个网络顺序字节，不直接转储 Windows `GUID` 结构；
- 所有偏移以文件起点为基准，单位为字节。页面和数据区起点按 4096 B 对齐；
- 头、索引和尾页必须逐字段编码/解码，禁止 `write(sizeof(CppStruct))`，避免 MSVC padding、枚举宽度和 ABI 变化污染格式；
- 所有保留字段和保留字节写零；读取器遇到非零保留位时按未知扩展处理，v1 默认隔离而非猜测；
- 文件长度必须等于头页声明的最大块字节数。预分配只保证空间上界，不代表块已提交；提交判定只依赖格式、长度和校验。

v1 像素格式 ID 固定如下，0 和其他未知值无效；不得使用 `camera::PixelFormat` 的隐式枚举序数：

| ID | 名称 | 原始负载语义 |
| ---: | --- | --- |
| 1 | `Mono8` | 每像素 1 B，8 位灰度 |
| 2 | `Mono10LE16` | 每像素 2 B，小端 u16 的低 10 位有效 |
| 3 | `Mono12LE16` | 每像素 2 B，小端 u16 的低 12 位有效 |
| 4 | `BayerRG8` | 每像素 1 B，RG Bayer 8 位 |

头页 `flags` 的 v1 位定义为：bit 0 `wallClockDiscontinuityObserved`、bit 1
`cameraTimestampPresent`、bit 2 `cameraTimestampSynchronized`、bit 3 `externalTrigger`。索引项
`flags` 的 v1 位定义为：bit 0 `incomplete`、bit 1 `cameraTimestampPresent`、bit 2
`cameraTimestampSynchronized`、bit 3 `cameraTimestampUnsynchronized`。同步/未同步位不得同时为 1；
未知和保留位必须为 0。

### v1 布局

```text
0
├─ Header page                 4096 B
├─ Frame index reservation    align4096(indexCapacity × 96 B)
├─ Raw frame reservation      align4096(indexCapacity × maxFrameBytes)
└─ Commit footer page         4096 B（固定在最大块末尾）
```

`indexCapacity = ceil(maximumAppliedFrameRateHz × 1 s) + 2`。其中 `maximumAppliedFrameRateHz` 必须来自已校验并回读的相机配置/能力上限，不能使用运行期平均帧率。额外两项只吸收时间窗边界取整，不授权无界外触发突发。若帧数、单帧字节数或偏移超过声明上限，当前普通块失败并形成可观测缺口，不扩容、不拆出未声明格式，也不阻塞采集。

#### 头页（4096 B）

| 偏移 | 大小 | 类型 | 字段 | 语义 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | bytes | `magic` | ASCII `PBNVME1\0` |
| 8 | 2 | u16 | `formatVersion` | 1 |
| 10 | 2 | u16 | `headerBytes` | 4096 |
| 12 | 4 | u32 | `endianTag` | 0x01020304；按小端读取 |
| 16 | 16 | bytes | `blockId` | RFC 4122 UUID 字节 |
| 32 | 8 | u64 | `generation` | 每相机单调递增的块代次 |
| 40 | 16 | bytes | `cameraId` | UTF-8，末尾补零 |
| 56 | 2 | u16 | `cameraIdBytes` | 0～16，不含补零 |
| 58 | 2 | u16 | `pixelFormat` | 上述 v1 稳定 ID，不使用 C++ 枚举序数 |
| 60 | 4 | u32 | `width` | 像素宽度 |
| 64 | 4 | u32 | `height` | 像素高度 |
| 68 | 4 | u32 | `stride` | 每行有效步幅上界 |
| 72 | 4 | u32 | `flags` | 时间质量、触发模式等已定义位 |
| 76 | 4 | u32 | `blockDurationMs` | 1000 |
| 80 | 4 | u32 | `indexEntryBytes` | 96 |
| 84 | 4 | u32 | `indexCapacity` | 本块索引硬上限 |
| 88 | 4 | u32 | `dataAlignmentBytes` | 4096 |
| 92 | 4 | u32 | `maxFrameBytes` | 已验证的 `stride × height` 上界 |
| 96 | 8 | i64 | `startWallUtcNs` | Unix epoch UTC 纳秒 |
| 104 | 8 | u64 | `startCameraTicks` | 无有效相机时钟时为 0 |
| 112 | 8 | u64 | `cameraTickFrequencyHz` | 无效时为 0 |
| 120 | 8 | u64 | `startSequenceNumber` | 预计首帧服务序号 |
| 128 | 4 | u32 | `headerCrc32c` | 本字段置零后覆盖整个头页 |
| 132 | 3964 | bytes | `reserved` | 全零 |

#### 帧索引项（96 B）

| 偏移 | 大小 | 类型 | 字段 | 语义 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | u64 | `sequenceNumber` | 服务内每相机序号 |
| 8 | 8 | u64 | `cameraFrameNumber` | 厂商帧号 |
| 16 | 8 | u64 | `receivedDeltaNs` | 相对块起点的单调时钟偏移 |
| 24 | 8 | i64 | `receivedWallUtcNs` | Unix epoch UTC 纳秒 |
| 32 | 8 | u64 | `cameraTicks` | 无效时为 0 |
| 40 | 8 | u64 | `cameraTickFrequencyHz` | 无效时为 0 |
| 48 | 8 | u64 | `dataOffset` | 帧负载在文件内的绝对偏移 |
| 56 | 4 | u32 | `payloadBytes` | 实际有效字节数 |
| 60 | 4 | u32 | `width` | 逐帧几何快照 |
| 64 | 4 | u32 | `height` | 逐帧几何快照 |
| 68 | 4 | u32 | `stride` | 逐帧步幅快照 |
| 72 | 2 | u16 | `pixelFormat` | v1 稳定枚举 |
| 74 | 2 | u16 | `flags` | incomplete、时间戳有效/质量等 |
| 76 | 4 | u32 | `payloadCrc32c` | 仅覆盖该帧有效负载 |
| 80 | 4 | u32 | `entryCrc32c` | 本字段置零后覆盖整个 96 B 项 |
| 84 | 12 | bytes | `reserved` | 全零 |

有效负载在原始区连续紧密排列，不为单帧额外补齐；`dataOffset + payloadBytes` 必须落在声明的原始区内且不与前一帧重叠。未使用的索引/原始预留区不参与有效区 CRC，也不能被读取器返回。

#### 提交尾页（4096 B）

| 偏移 | 大小 | 类型 | 字段 | 语义 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | bytes | `magic` | ASCII `PBCOMMIT` |
| 8 | 2 | u16 | `formatVersion` | 1 |
| 10 | 2 | u16 | `footerBytes` | 4096 |
| 12 | 4 | u32 | `frameCount` | 有效索引项数，且不超过容量 |
| 16 | 8 | u64 | `validIndexBytes` | 必须等于 `frameCount × 96` |
| 24 | 8 | u64 | `validDataBytes` | 所有有效帧负载字节和 |
| 32 | 8 | u64 | `maximumBlockBytes` | 必须等于实际文件长度 |
| 40 | 8 | i64 | `endWallUtcNs` | 最后一帧 UTC；空块为窗口结束值 |
| 48 | 8 | u64 | `endSequenceNumber` | 最后一帧序号；空块为起始序号 |
| 56 | 4 | u32 | `indexCrc32c` | 覆盖有效索引项拼接字节 |
| 60 | 4 | u32 | `dataCrc32c` | 覆盖有效原始负载拼接字节 |
| 64 | 4 | u32 | `headerCrc32c` | 回显已验证头页 CRC |
| 68 | 4016 | bytes | `reserved` | 全零 |
| 4084 | 4 | u32 | `footerCrc32c` | 本字段置零后覆盖整个尾页，包括提交标记 |
| 4088 | 8 | bytes | `commitMarker` | ASCII `COMMIT1\0`，最后写入 |

尾页的 CRC 在内存中按最终提交标记计算；写入时先写偏移 0～4087 并 flush，再单独写偏移 4088～4095 的提交标记并再次 flush。M7-04 只有在提交标记、尾页 CRC、回显头 CRC、区域边界和块级 CRC 全部有效时才直接接纳正常块。缺标记、短写或 CRC 错误的尾块进入有界恢复/隔离流程，绝不作为已提交块静默使用。

### CRC32C 参数与校验语义

统一使用 CRC-32C (Castagnoli)：正常多项式 `0x1EDC6F41`、反射多项式 `0x82F63B78`、初值 `0xFFFFFFFF`、输入/输出反射、结果异或 `0xFFFFFFFF`；ASCII `123456789` 的检查值必须为 `0xE3069283`。实现可使用硬件指令或批准的平台 API，但结果必须一致。

CRC 检测介质/短写损坏，不提供来源认证或抗恶意修改。事件导出或上位机协议需要安全完整性时应在 M8 合同中对最终制品计算 SHA-256 等安全哈希，不得把 CRC32C 描述成密码学保证。

### 容量公式

对相机 `i`：

```text
maxFrameBytes_i       = validatedStride_i × validatedHeight_i
indexCapacity_i       = ceil(maxAppliedFps_i × blockSeconds) + 2
indexRegionBytes_i    = align4096(indexCapacity_i × 96)
dataRegionBytes_i     = align4096(indexCapacity_i × maxFrameBytes_i)
maximumBlockBytes_i   = 4096 + indexRegionBytes_i + dataRegionBytes_i + 4096
```

对于同一几何/帧率的 `N` 路相机和 `maximumCacheBytes`：

```text
physicalSlotCount     = floor(maximumCacheBytes / maximumBlockBytes)
usableCommittedSlots  = physicalSlotCount - 1
                         # 至少为单写线程当前临时块预留一个最大块
balancedSeconds       = floor(usableCommittedSlots / N) × blockSeconds
```

异构相机不能用平均块大小估计，应按每相机最大块大小为一个轮次求和：

```text
roundBytes            = sum(maximumBlockBytes_i)
balancedSeconds       = floor((maximumCacheBytes - max(maximumBlockBytes_i))
                              / roundBytes) × blockSeconds
```

若事件保护导致可淘汰块不足，即使字节公式仍有名义空间，也不得覆盖受保护块。M7-03 必须同时按字节和租约状态准入。容量单位统一使用 GiB=`1024^3`，速率报告同时给出 B/s 和 MiB/s，禁止混用厂商十进制 GB 与 GiB。

### 带宽预算与准入

块 CRC 在顺序写入时增量计算，不要求读回整块。对相机 `i` 的普通滚动持续写需求为：

```text
payloadBps_i  = maxFrameBytes_i × maxAppliedFps_i
metadataBps_i = ceil(maxAppliedFps_i) × 96
                + (4096 + 4096) / blockSeconds
rollingWriteBps = sum(payloadBps_i + metadataBps_i)
```

目标卷必须在与生产相同的 Windows、电源/温度、文件系统、空闲空间和队列深度下测量持续顺序写带宽。普通滚动缓存的准入规则为：

```text
rollingWriteBps <= measuredSustainedWriteBps × 0.80
minimumMeasuredSustainedWriteBps = ceil(rollingWriteBps / 0.80)
```

至少 20% 不分配给普通滚动写，留给正式事件、元数据、flush 和设备短时抖动。限速上限不得高于上述 80% 预算；若业务配置的限速低于原始输入需求，系统必须拒绝启用或显式降级并记录缺口，不能通过无声丢帧声称缓存完整。厂商峰值顺序写标称值不能替代持续、热稳定和并发事件场景实测。

### 当前默认配置参考算例（非硬件验收）

以 1624×1240、stride=1624、Mono8、60 fps、六路同配置为例：

| 项目 | 结果 |
| --- | ---: |
| 单帧最大字节 | 2,013,760 B |
| 单路原始负载 | 120,825,600 B/s = 115.228 MiB/s |
| 六路原始负载 | 724,953,600 B/s = 691.370 MiB/s |
| 索引容量 | 62 帧/块 |
| 单块索引预留 | 8,192 B |
| 单块原始区预留 | 124,854,272 B |
| 单块最大物理字节 | 124,870,656 B |
| 六路含有效元数据的滚动写需求 | 725,037,312 B/s = 691.449 MiB/s |
| 按 80% 准入所需最低实测持续写 | 906,296,640 B/s = 864.312 MiB/s |
| 1000 GiB 可容纳最大块 | 8,598 个 |
| 扣除 1 个临时块、六路均衡保留 | 1,432 s，约 23 分 52 秒 |
| 六路仅原始负载 24 小时数据量 | 62,635,991,040,000 B，约 56.967 TiB |

该算例只证明公式和“无压缩原始缓存容量很大”的事实。它没有测量目标 NVMe，也没有证明六台相机、事件并发、温度降速或实际 stride；部署时必须重新生成并批准容量报告。默认 600 MiB/s 写入限制不足以承载该六路负载，六路部署必须依据实测盘能力显式配置。

### 后续运行时配置合同

M7-02 已通过配置 schema v2 一次性加入：

- `storage.rollingCacheEnabled`：是否启用普通 NVMe 滚动缓存；
- `storage.maximumCacheStorageGiB`：滚动块及单个在写临时块的总物理上限；
- `storage.rollingCacheWriteLimitMiBps`：普通滚动写令牌桶上限，必须同时满足输入需求和目标盘 80% 预算；
- `storage.rollingCacheIoTimeoutMs`：单块写入、双 flush 和原子发布共享的总截止时间；
- 块时长 v1 固定为 1000 ms，不暴露可任意修改字段；格式/索引容量来自已应用相机配置快照。

schema v1 保持历史合同，当前程序严格接受 v2；升级需显式生成完整 v2 配置，不猜测旧字段。水位仍沿用 warning/critical/stop 免费空间阈值；容量上限和免费空间门禁取更严格者。

## 结果

正面结果：

- 写入器、恢复器、索引和事件引用对 v1 字节语义有唯一解释；
- 1 秒块把故障、保护和队列粒度限制在较小范围，且不引入复杂编码；
- 最大物理块、缓存上限和带宽准入都基于上界，运行时无需依赖平均压缩率；
- CRC32C 可增量、高速计算，并能定位逐帧与块级损坏。

代价与风险：

- 原始 2 MP×60 fps 六路约 691 MiB/s、每天约 57 TiB，容量保留时间短，对 NVMe 耐久和持续带宽要求高；
- 固定索引/数据预留会产生少量内部浪费；一秒一块每相机每天最多 86,400 个块，需要 M7-03 有界索引与目录分片；
- CRC32C 不防恶意篡改；安全传输需另加密码学完整性；
- Windows 预分配、flush、原子发布和断电耐久语义仍需 M7-02/M7-04 以故障注入和目标卷验证。

## 被否决或推迟的方案

### 5 秒块

否决为 v1 默认。它只减少少量页面/目录元数据，却把断电尾块、队列驻留、事件保护过量和恢复扫描单元放大五倍。

### 自定义视频编码或默认压缩

否决。编码会增加 CPU、状态和恢复复杂度，违反“不自定义复杂视频编码”的路线图约束，也会让容量依赖不可保证的压缩率。可选 zstd 若以后启用，必须新增格式版本/标志、独立性能预算和无损恢复测试。

### 直接转储 C++ 结构体

否决。MSVC padding、枚举宽度、ABI 和未来字段变化会使格式不可移植、不可审查。v1 必须显式逐字段小端编码。

### 只校验整个文件或只使用 SHA-256

否决。只有整块校验难以在尾块中定位可恢复帧；对全部高速原始数据计算密码学哈希也不是介质损坏检测所必需。v1 使用逐帧与区域 CRC32C；最终事件/上传安全哈希由相应合同处理。

### 依据 NVMe 厂商峰值标称值批准

否决。峰值不能覆盖盘接近满载、SLC 缓存耗尽、温度降速、flush 和事件并发。必须使用目标环境持续实测值，并限制普通滚动写最多占 80%。

## 验证要求

- CTest 必须机器复算格式页、索引/数据区、最大块、参考六路写需求、80% 最低盘带宽和 1000 GiB 保留时长；
- M7-02 必须测试边界帧数、超大帧、序号倒退、队列满、写限速、短写、flush/发布失败和磁盘水位，证明不反压采集；
- M7-03 必须测试容量回绕、异构块、事件租约竞争、所有块受保护和派生索引重建；
- M7-04 必须在每个写入阶段注入中断，覆盖缺尾页、撕裂提交标记、头/索引/负载/尾页 CRC 错误和未知格式版本；
- M7/M9 硬件门禁退出前必须在目标 NVMe 与六路生产等价负载下记录持续写、热稳定、事件并发、采集统计和实际保留时长。当前没有执行这些硬件测试。
