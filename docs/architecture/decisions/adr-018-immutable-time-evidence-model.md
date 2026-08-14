# ADR-018：不可变帧时间证据与模型身份

- 状态：Accepted
- 日期：2026-08-14
- 决策范围：R0-03、T1、D2、E3、O4
- 关联需求：EDGE-TS-001～005、EDGE-TS-010～014、EDGE-DATA-004～005

## 背景

相机原始计数、工控机接收时间和时钟模型校正时间具有不同来源与可信度。若只保存一个“帧时间”，后续无法判断它是设备测量、接收近似值还是模型估计，也无法解释同步源切换、系统时间跳变或模型更新后的历史事件。R0-02 已冻结领域字段和线上纳秒表示，本 ADR 冻结这些值写入持久格式时的证据关系与身份。

## 决策

### 三层时间证据

每帧时间是一次性形成的不可变值对象，三层证据并列保存，任何派生值不得覆盖来源值：

1. 原始相机证据：`cameraTimestampTicks` 与 `cameraTimestampFrequencyHz` 必须同时可用或同时不可用；频率大于零。它们是厂商计数证据，不声称 UTC。
2. 接收证据：`receivedMonotonicNs` 与 `receivedUtcNs` 在取帧完成附近记录。前者只用于同一进程/当前 session 内排序和截止，后者是始终存在的降级 UTC。
3. 校正证据：`correctedCaptureUtcNs`、`clockOffsetNs`、`uncertaintyNs`、`clockSource`、`syncState` 和模型身份来自同一份已发布模型快照。校正时间不存在时不得用接收 UTC、0 或相机 ticks 冒充。

PBNVME3 保存块起点的接收单调时间和逐帧非负相对偏移；该值只用于块内顺序与当前 session 诊断，旧 session 的单调 epoch 不得在新进程中映射 T0。JSON、SQLite 和跨节点协议不把单调 epoch 当作可比较 UTC。

### 可用性与一致性

- 二进制定长记录使用显式 availability 位；JSON 使用 `null` 与同名 `Available` 布尔值；SQLite 使用 `NULL`。零是合法值，不是缺失哨兵。
- `correctedCaptureUtcNs` 可用时，`uncertaintyNs` 必须可用且非负，`modelRevision > 0`，`syncState` 只能是 `SYNCED`、`SYNCING` 或 `DEGRADED`。
- `correctedCaptureUtcNs` 不可用时，`syncState` 为 `UNSYNCED` 或 `UNKNOWN`；offset/uncertainty 只有探针确有解释时才可独立保存，不因校正时间缺失而虚构。
- 所有纳秒运算使用经检查的有符号 64 位整数；ticks、频率、帧号、序号和修订使用无符号 64 位整数。任何映射溢出使校正值不可用并产生稳定错误，不饱和、不回绕。
- 帧离开采集适配器后所有时间字段不可修改。新模型只影响新帧；事件和块保存创建时的证据，禁止后台回写“改善”历史时间。

### 模型身份

`modelRevision` 只在一个 `TimeSyncRuntime` 实例中严格递增，不独立构成全局身份。持久格式中的模型身份为：

```text
machineId + timeRuntimeInstanceId + cameraId-or-system + modelRevision
```

`timeRuntimeInstanceId` 是每次服务时间运行时启动时生成的 UUID 文本，最长 36 ASCII 字节；它不用于安全认证。`cameraId` 为空表示工控机模型。manifest v4 在事件级列出实际使用的模型身份，PBNVME3 逐帧保存 `modelRevision`，块/manifest 提供 machine、runtime instance 和 camera 上下文。不能解析完整身份时仍保留帧原始证据，但整体时间质量不得为 `SYNCED`。

### 稳定枚举

领域/JSON 使用 R0-02 字符串；PBNVME3 使用以下固定一字节 ID，0 永远表示未知：

| 类型 | ID | 字符串 |
| --- | ---: | --- |
| `ClockSource` | 0 | `UNKNOWN` |
|  | 1 | `PTP_HARDWARE` |
|  | 2 | `PTP_SOFTWARE` |
|  | 3 | `NTP` |
|  | 4 | `OFFSET_MODEL` |
|  | 5 | `RECEIVE_CLOCK` |
| `SyncState` | 0 | `UNKNOWN` |
|  | 1 | `SYNCED` |
|  | 2 | `SYNCING` |
|  | 3 | `DEGRADED` |
|  | 4 | `UNSYNCED` |

新增来源或状态不能复用 ID；旧读取器遇到未知 ID 必须拒绝当前 PBNVME3 块，不能映射成已同步。

### 事件整体时间质量

manifest v4 的 `overallTimeQuality` 由实际保存帧聚合，包含 `syncState`、最大不确定度、模型身份集合及稳定 reason code 列表。任一参与相机缺少请求范围、存在序号缺口、使用未知模型、校正时间不可用或状态不是 `SYNCED` 时，跨节点锁定结果不得为 `COMPLETE`。聚合状态不覆盖逐帧证据。

## 后果

- 历史帧可解释为原始测量、接收近似或某一确定模型的估计，模型更新不会篡改证据。
- 单调时间继续安全地驱动本进程窗口，又不会被误当成跨重启时间轴。
- PBNVME3 索引比 PBNVME2 更大，读写器必须显式处理 availability、枚举和溢出。
- 模型身份不提供真实性保证；Uplink v1 的明文、无鉴权风险仍由 ADR-012 接受。

## 被否决的方案

### 只保存校正 UTC

否决。失步时会丢失原始 ticks 和接收证据，且无法复核模型错误。

### 用 0 表示不可用

否决。Unix epoch、offset、ticks 和 revision 的零值各有合法或明确保留语义，无法区分缺失。

### 新模型回写旧事件

否决。它会改变已提交证据、文件摘要和幂等上传对象，并把后见模型伪装成事件发生时模型。

## 验证要求

- R0-03 黄金样例必须覆盖校正时间可用、不可用、不完整帧和多模型/多帧字段。
- T1 必须测试模型切换后历史帧不变、ticks/frequency 成对校验、溢出、UNSYNCED 和系统时间跳变。
- D2 必须验证 PBNVME3/manifest v4 往返及事件后配置/模型变化不改变历史文件。
- 实体相机时间戳能力、PTP/Grandmaster 和亚毫秒精度必须在 V5-02 实测；本 ADR 不构成硬件通过证据。
