# 工控机与上位机集成新增需求规格说明书

## 1. 文档信息

| 项目 | 内容 |
| --- | --- |
| 文档性质 | 工控机端增量需求，不替代 `edge-system-requirements.md` |
| 需求基线 | 2026-08-14 仓库代码、Uplink v1、event manifest v3、PBNVME2 |
| 目标环境 | Windows 10/11 x64、C++20、Qt 6、Hikrobot MVS SDK |
| 项目验收规模 | 每台工控机最多六台 MV-CS020-60GM，相机编号 CAM01～CAM06 |
| 关联文档 | [工控机端功能清单与开发方案](edge-system-requirements.md)、[调整后的上位机需求规格](host-system-requirements-adjusted.md)、[Uplink v1 协议](../uplink-protocol-v1.md) |

本文只定义为了满足调整后上位机需求而需要在工控机端增加或扩展的能力。未在本文中明确列出的既有模块不得因本需求进行无关重构。仓库当前部分通用代码已经支持 CAM01～CAM06，本项目交付与硬件验收仍以每台工控机最多六台目标相机为准。

## 2. 术语与优先级

- **T0**：触发工控机给出的断纸触发统一 UTC 时间，内部类型为有符号 64 位 Unix Epoch 纳秒。
- **原始相机时间**：相机提供的计数值及其频率，禁止被校正时间覆盖。
- **校正时间**：依据当前时钟模型映射到 UTC 的采集时间。
- **时间不确定度**：对校正时间误差上界的保守估计，不等同于实测跨相机误差。
- **严格 T0 模式**：所有参与节点都声明并成功使用 `event.lockByUtc` 能力的协同模式。
- **传统模式**：仅有 Uplink v1 基础能力，不能按外部 T0 锁定其他工控机缓存的模式。
- **P0**：首版集成闭环必须完成；**P1**：生产增强；**P2**：后续演进。

JSON 中的纳秒时间使用十进制字符串传输，避免通用 JSON 数值实现丢失 64 位整数精度；工控机内部和持久层使用 64 位整数。

## 3. 当前实现基线与差距

### 3.1 已有能力，不列为新增开发

| 能力 | 当前基线 |
| --- | --- |
| Uplink 会话 | 明文 HTTP/WebSocket、心跳、状态、自动重连和指数退避已经存在 |
| 远程控制 | 相机发现、绑定、连接、启停、参数更新、抓图、软件触发及事件复核/重传已经通过 Uplink 命令复用本机 dispatcher |
| 事件链 | 候选事件、前后窗口、跨本机相机合并、关键帧、事务式事件目录和 SQLite 索引已经存在 |
| 离线可靠性 | 上位机离线时继续采集与保存，上传任务持久化，恢复后重试 |
| 文件上传 | 分块、服务端 `receivedChunks`、断点续传、逐块及整文件 SHA-256、幂等 complete 已经存在 |
| 存储 | event manifest v3、PBNVME2 原始块、关键帧 JPEG、存储水位和保留策略已经存在 |
| 预览 | 本机低帧率 JPEG 预览、最新帧覆盖和 Uplink 二进制预览编解码已经存在 |
| 运维 | 系统、相机、算法、存储、数据库、Uplink 指标以及报警登记表已经存在 |

### 3.2 必须补齐的差距

1. 当前帧保存相机 ticks、接收单调时间和接收系统时间，但没有稳定的校正 UTC 时间、偏移、不确定度和完整同步状态。
2. 当前事件窗口按本机单调时钟创建，不能接收其他工控机产生的全局 `event_id + T0` 并锁定缓存。
3. 当前周期 Uplink 状态只包含服务状态和待上传汇总，不能满足上位机设备、相机和时间同步监控。
4. 当前事件通知在事件提交/上传阶段产生，不能在首次触发时立即通知上位机进行广播。
5. Uplink 已定义 JPEG 预览二进制格式，但生产服务尚未把预览运行时桥接到远程 WebSocket。
6. manifest v3/PBNVME2 未保存每帧原始时间、校正时间和时间质量的完整组合，也未保存事件发生时的逐相机参数快照。
7. 当前命令结果去重主要是进程内缓存；外部事件锁定要求跨进程重启仍保持事件级幂等。

## 4. 分级时间同步服务

### 4.1 时钟源与降级顺序

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-TS-001 | P0 | 工控机必须实现独立于采集回调的时间同步服务，并按“外部硬件 PTP → 操作系统 PTP/NTP → 相机 ticks 与工控机 UTC 偏移模型 → 接收系统时间”自动选择当前可用时钟源。 |
| EDGE-TS-002 | P0 | 时钟源切换必须产生状态变更和报警记录；切换不得修改已采集帧的原始时间信息。 |
| EDGE-TS-003 | P0 | 当相机、网卡或现场时钟链路不支持硬件 PTP 时，服务必须降级并提高 `uncertaintyNs`，不得继续报告硬件同步或亚毫秒可信度。 |
| EDGE-TS-004 | P0 | 工控机不得承担固定 PTP Grandmaster 角色；Grandmaster 地址、域、配置方式和实际精度属于现场部署参数。 |
| EDGE-TS-005 | P1 | 支持对 PTP/NTP 服务重启、Grandmaster 切换、系统时间跳变和相机时间戳回绕进行检测与自动恢复。 |

### 4.2 帧时间模型

每个有效帧必须至少携带下列不可变时间字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `cameraId` | string | 本机规范相机 ID |
| `cameraFrameNumber` | uint64 | 厂商帧号 |
| `sequenceNumber` | uint64 | 当前采集会话内序号 |
| `cameraTimestampTicks` | uint64/null | 原始相机计数 |
| `cameraTimestampFrequencyHz` | uint64/null | 计数频率 |
| `receivedMonotonicNs` | int64 | 本机单调时间，仅用于本进程内排序和截止时间 |
| `receivedUtcNs` | int64 | 驱动取帧完成后的工控机 UTC 时间 |
| `correctedCaptureUtcNs` | int64/null | 经时钟模型校正的真实采集 UTC 时间 |
| `clockSource` | enum | `PTP_HARDWARE`、`PTP_SOFTWARE`、`NTP`、`OFFSET_MODEL`、`RECEIVE_CLOCK`、`UNKNOWN` |
| `clockOffsetNs` | int64/null | 原始时间到 UTC 的估计偏移 |
| `uncertaintyNs` | uint64/null | 校正时间的不确定度 |
| `syncState` | enum | `SYNCED`、`SYNCING`、`DEGRADED`、`UNSYNCED`、`UNKNOWN` |

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-TS-010 | P0 | `correctedCaptureUtcNs` 不得覆盖相机 ticks、接收 UTC 或接收单调时间。 |
| EDGE-TS-011 | P0 | 在 `UNSYNCED` 或无法计算校正时间时，仍可保存帧，但必须将校正时间置空或标记为不可用于严格对齐。 |
| EDGE-TS-012 | P0 | 所有事件时间窗内部仍使用单调时钟保证不受系统时间跳变影响；外部 T0 通过同一时钟模型映射到单调时间。 |
| EDGE-TS-013 | P0 | 映射结果必须与创建事件时的时钟模型版本一起快照，后续时钟模型变化不得改写历史事件。 |
| EDGE-TS-014 | P0 | Warning、Alarm 阈值和持续时间必须可配置。初始目标为硬件 PTP 路径 Warning 1 ms、Alarm 5 ms；最终值必须经目标硬件实测冻结。 |

### 4.3 同步状态快照

工控机级和每相机级状态至少包含：`currentUtcNs`、`clockSource`、`offsetNs`、`uncertaintyNs`、`maximumObservedOffsetNs`、`lastSynchronizedAt`、`syncState`、`grandmasterIdentity`（可空）、`modelRevision` 和最近错误码。

## 5. 严格统一 T0 事件协同

### 5.1 触发工控机

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-EVT-001 | P0 | 首个确认断纸的工控机必须生成全局唯一 `eventId`，并从触发帧的 `correctedCaptureUtcNs` 生成 T0；无法取得可信校正时间时可生成事件，但必须将时间质量标为降级。 |
| EDGE-EVT-002 | P0 | 工控机必须在本机候选窗口获得保护后立即向上位机发送事件通知，不能等待后置窗口、JPEG、磁盘提交或文件上传完成。 |
| EDGE-EVT-003 | P0 | 立即通知复用 Uplink `alarm` 控制消息，`code=BREAK_EVENT_TRIGGERED`；可靠事件文件上传仍使用现有 REST/分块端点。 |
| EDGE-EVT-004 | P0 | 通知发送失败不得撤销本地事件。工控机必须记录未通知状态并在恢复连接后补发；上位机按 `eventId` 幂等处理。 |

立即通知 payload 最小契约：

```json
{
  "code": "BREAK_EVENT_TRIGGERED",
  "eventId": "EDGE-001-...",
  "triggerTimestampUtcNs": "1786671135123456789",
  "triggerMachineId": "EDGE-001",
  "triggerCameraId": "CAM01",
  "triggerSource": "ALGORITHM",
  "eventType": "PAPER_BREAK",
  "eventLevel": "ALARM",
  "syncState": "SYNCED",
  "uncertaintyNs": "500000",
  "preEventMs": 10000,
  "postEventMs": 10000,
  "configRevision": 93
}
```

### 5.2 接收广播与锁定

上位机通过能力协商后的 `event.lockByUtc` 命令广播同一事件。该命令是机器间实时协同命令，不要求人工二次确认，但必须写审计日志；其他远程变更命令继续遵循 Uplink v1 的 `operatorConfirmed` 规则。

命令 body 最小契约：

```json
{
  "eventId": "EDGE-001-...",
  "triggerTimestampUtcNs": "1786671135123456789",
  "triggerMachineId": "EDGE-001",
  "triggerCameraId": "CAM01",
  "triggerSource": "ALGORITHM",
  "preEventMs": 10000,
  "postEventMs": 10000
}
```

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-EVT-010 | P0 | 工控机只有在会话能力中声明 `event.lockByUtc` 后才允许执行该命令；未声明时返回稳定的 `SYS_NOT_SUPPORTED`。 |
| EDGE-EVT-011 | P0 | 接收命令后，使用命令 T0 和创建时的时钟模型映射到本机单调时间，并为所有已启用相机申请 `[T0-pre, T0+post]` 窗口。 |
| EDGE-EVT-012 | P0 | 外部事件必须使用命令中的全局 `eventId` 持久化，不得为同一次协同事件生成新的对外事件 ID。内部局部 ID 可以保留，但必须建立稳定映射。 |
| EDGE-EVT-013 | P0 | `eventId` 去重记录必须持久化到 SQLite，至少保留至对应事件及上传任务全部删除；进程重启后重复命令返回同一业务结果，不创建第二个事件目录。 |
| EDGE-EVT-014 | P0 | T0 已经过期但所需帧仍在内存或 NVMe 可用范围内时，必须锁定现有证据并报告实际范围；不得仅以“命令迟到”拒绝。 |
| EDGE-EVT-015 | P0 | T0 超出全部可用缓存、T0 过度超前、时间映射不可用或事件容量已满时，必须返回明确失败或部分成功结果，并登记报警。 |
| EDGE-EVT-016 | P0 | 外部锁定不得阻塞相机回调、采集线程或控制心跳；跨线程通道必须有容量和拒绝策略。 |
| EDGE-EVT-017 | P0 | 来源工控机收到自己的广播时必须按 `eventId` 识别为重复，并返回现有锁定结果。 |

### 5.3 EventLockAck

`event.lockByUtc` 的 `command.result` 必须包含下列 `EventLockAck`：

```json
{
  "eventId": "EDGE-001-...",
  "machineId": "EDGE-002",
  "duplicate": false,
  "lockStatus": "PARTIAL",
  "requestedStartUtcNs": "1786671125123456789",
  "requestedEndUtcNs": "1786671145123456789",
  "actualStartUtcNs": "1786671126123456789",
  "actualEndUtcNs": "1786671145123456789",
  "syncState": "DEGRADED",
  "uncertaintyNs": "3000000",
  "cameras": [
    {
      "cameraId": "CAM01",
      "status": "LOCKED",
      "frameCount": 1180,
      "firstCaptureUtcNs": "1786671126123456789",
      "lastCaptureUtcNs": "1786671145100000000",
      "sequenceGaps": 2,
      "errorCode": ""
    }
  ]
}
```

`lockStatus` 只能为 `COMPLETE`、`PARTIAL` 或 `FAILED`。只要任一启用相机缺少请求范围、时间质量降级、存在帧缺口或锁定失败，就不得返回 `COMPLETE`。

## 6. 状态、报警与远程预览扩展

### 6.1 状态快照

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-STAT-001 | P0 | 周期 `status` 必须包含 machineId、软件版本、服务状态、运行时间、CPU、内存、各数据卷空间、数据库状态、Uplink 状态和上传队列汇总。 |
| EDGE-STAT-002 | P0 | 状态必须包含动态相机数组：逻辑 ID、SN、型号、IP、安装位置、启用状态、运行状态、实际 FPS、丢帧、最后帧时间、曝光、增益、帧率、分辨率、像素格式和时间同步快照。 |
| EDGE-STAT-003 | P0 | 不支持或暂不可用的字段必须使用 `null` 和可用性标志，不得填入零值伪装正常数据。 |
| EDGE-STAT-004 | P0 | 状态 JSON 必须保持在 Uplink v1 的 1 MiB 上限内；超限时拒绝发送并报警，不得截断成语义不完整的 JSON。 |
| EDGE-STAT-005 | P1 | 一般报警通过既有 `alarm` 消息推送 raise/update/clear；Critical 报警元数据进入持久上传队列，避免短时断网丢失。 |

### 6.2 远程预览

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-PRV-001 | P0 | 将现有预览编码结果桥接到 Uplink v1 的 `preview.frame` 二进制格式，不新增第二套 JPEG 编码管线。 |
| EDGE-PRV-002 | P0 | 每相机使用容量 1 的最新帧覆盖槽；全工控机远程预览总帧率不得超过协议协商值，默认不超过 5 fps，并按启用相机公平轮转。 |
| EDGE-PRV-003 | P0 | JPEG 不超过 2 MiB，头部不超过 64 KiB；预览丢弃、上位机断线或慢消费不得阻塞采集、算法、事件和可靠上传。 |
| EDGE-PRV-004 | P0 | 预览必须携带 cameraId、sequence、校正采集时间、syncState 和 uncertaintyNs；传统节点缺少校正时间时明确标为不可用。 |

## 7. 事件格式与配置快照

### 7.1 版本兼容

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-DATA-001 | P0 | 旧 event manifest v3 和 PBNVME2 保持只读、校验、导出和上传兼容，不得就地改写。 |
| EDGE-DATA-002 | P0 | 新事件使用 event manifest v4；需要保存逐帧严格时间信息的原始块使用 PBNVME3。新版本必须有独立版本标记，旧解析器不得把新文件误判为 PBNVME2。 |
| EDGE-DATA-003 | P0 | PBNVME3 延续固定上限、结构校验、CRC32C、整文件 SHA-256、完整尾标和同卷不覆盖原子发布语义。 |
| EDGE-DATA-004 | P0 | PBNVME3 每帧索引必须保存原始相机 ticks/频率、接收 UTC、校正 UTC、偏移、不确定度、同步状态、帧号、序号、几何、像素格式、数据范围和不完整标志。 |
| EDGE-DATA-005 | P0 | manifest v4 必须记录事件 T0、触发来源、触发节点、各节点/相机实际时间范围、时间模型版本、整体时间质量、原始块版本和文件校验。 |

二进制字节布局必须在实现前通过 ADR 和格式样例冻结；该设计不得改变 PBNVME2 的既有字段语义。

### 7.2 相机配置快照

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-CFG-001 | P0 | 事件进入 Collecting 时必须快照所有参与相机的实际回读配置，而不是仅保存配置文件修订号。 |
| EDGE-CFG-002 | P0 | 快照至少包含相机 ID、SN、型号、曝光、增益、实际 FPS、ROI、宽高、像素格式、触发模式/源、包大小、传输延迟及同步相关能力/状态。 |
| EDGE-CFG-003 | P0 | 厂商不支持的参数记录为不可用；不得为满足上位机字段而虚构 Gamma、白平衡、PTP 或 Buffer Size 值。 |
| EDGE-CFG-004 | P0 | 配置快照写入不可变 manifest，同时作为事件元数据上传；事件发生后的配置修改不得改变历史快照。 |

## 8. 兼容性、错误与安全约束

| ID | 优先级 | 需求 |
| --- | --- | --- |
| EDGE-COMP-001 | P0 | 未声明严格 T0 能力的 Uplink v1 上位机仍可使用现有会话、状态、命令和上传；工控机在传统模式下不得假装已执行跨节点锁定。 |
| EDGE-COMP-002 | P0 | 新上位机必须依据握手 capability 决定是否发送 `event.lockByUtc`；工控机不得通过猜测服务端版本启用新行为。 |
| EDGE-COMP-003 | P0 | 现有 REST 文件上传路径、分块大小、`receivedChunks`、SHA-256 和 complete 语义保持不变。 |
| EDGE-COMP-004 | P0 | 新业务失败继续使用稳定业务错误码，厂商错误码只能作为附加诊断信息。 |
| EDGE-SEC-001 | P0 | 现有 Uplink v1 明文链路只能部署于隔离 VLAN，并由防火墙、交换机 ACL 和物理访问控制限制来源。SHA-256 只证明内容完整性，不提供身份认证。 |
| EDGE-SEC-002 | P0 | 自动 `event.lockByUtc` 免人工确认的例外必须限制到该命令类型、已协商会话和当前 machineId，并完整写入审计日志。 |
| EDGE-SEC-003 | P1 | 后续安全协议版本应提供双向身份认证、传输加密和命令授权，同时保持旧事件文件的离线读取能力。 |

## 9. 非功能要求

- `EDGE-NFR-001`（P0）：正常局域网内，从上位机收到 `event.lockByUtc` 到完成内存/NVMe 引用保护并形成 ACK 的工控机内部 P95 目标不超过 100 ms；窗口选择仍以 T0 为准。
- `EDGE-NFR-002`（P0）：任何时间同步、通知、预览、上传或 ACK 操作不得在相机采集回调中执行网络、磁盘、JPEG、推理或阻塞等待。
- `EDGE-NFR-003`（P0）：所有新增队列必须定义条数/字节容量、溢出策略、指标和报警；事件锁定不得采用 drop-oldest 静默丢弃。
- `EDGE-NFR-004`（P0）：所有新增工作线程必须支持停止令牌、截止时间 join 和确定性关闭顺序。
- `EDGE-NFR-005`（P0）：严格 T0 的“跨设备误差不超过 1 ms”只有在目标相机、网卡、交换机和 Grandmaster 完成实机测试后才能成为已通过指标；软件测试不得替代硬件精度测试。

## 10. 验收矩阵

| 场景 | 对应需求 | 自动化验收 | 硬件门禁 |
| --- | --- | --- | --- |
| PTP 正常、NTP 降级、接收时钟兜底 | EDGE-TS-001～014 | 注入时钟源、偏移、跳变和回绕，验证状态与历史字段不被覆盖 | 目标网卡/相机/PTP 实测 |
| 单节点触发并广播四台工控机 | EDGE-EVT-001～017 | 模拟四节点、同一 eventId/T0、乱序 ACK 和重复广播 | 生产等价交换机时延 |
| 工控机离线或缓存不足 | EDGE-EVT-014～016 | 断网、迟到触发、部分相机缺帧，必须返回 PARTIAL/FAILED | 实体断链可选 |
| 进程重启后的幂等 | EDGE-EVT-012～013 | 在锁定后重启，重放相同命令，事件目录和数据库只存在一份 | Windows 服务强杀测试 |
| 状态与报警上报 | EDGE-STAT-001～005 | 校验 1 MiB 上限、null/available 语义、raise/clear 和断网补发 | 无 |
| 四相机远程预览与慢消费者 | EDGE-PRV-001～004 | 最新帧覆盖、公平轮转、5 fps/2 MiB 上限、断连不反压采集 | 四路真实图像负载 |
| manifest v3/v4 与 PBNVME2/3 | EDGE-DATA-001～005 | 黄金文件、版本拒绝、CRC/SHA、损坏隔离和旧格式回归 | 生产磁盘性能 |
| 配置快照 | EDGE-CFG-001～004 | 事件后修改相机参数，旧事件快照保持不变 | SDK 回读值实测 |
| 上传兼容 | EDGE-COMP-001～003 | 现有 Uplink v1 回环、断点续传、重复块和 complete 回归 | 正式上位机联调 |

## 11. 完成条件与限制

只有需求对应的代码、模拟测试、协议测试、构建和非硬件 CTest 均通过后，才能声明软件实现完成。PTP 精度、四台实体 MV-CS020-60GM、生产网络、目标 NVMe、真实上位机和长稳测试必须分别记录；未执行时必须明确标记“待硬件/系统联调”，不得推断为通过。
