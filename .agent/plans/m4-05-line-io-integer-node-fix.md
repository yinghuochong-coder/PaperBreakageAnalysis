# M4-05：Line 1 频闪整数节点连接失败修复 ExecPlan

## 元数据

- 状态：completed（真机连接与电气/时序验证待现场执行）
- 负责人：Codex
- 创建日期：2026-08-11
- 最后更新：2026-08-11
- 路线图条目：M4-05 相机配置与实际值回显（缺陷修复）
- 关联需求：4.4

## 目的与可观察结果

启用 Line 1 频闪后，Hikrobot 适配器使用目标 MVS SDK 定义的整数节点 API 探测、写入并回读三项微秒时序参数，不再因以 Float API 访问 Integer 节点而使相机连接阶段的保存参数应用失败。

## 范围

### 范围内

- 修正 `StrobeLineDuration`、`StrobeLinePreDelay`、`StrobeLineDelay` 的 MVS 节点类型。
- 更新伪 MVS 测试，明确拒绝错误类型访问并覆盖整数范围、写入、回读和回滚。
- 执行 Debug/Release 构建和非硬件 CTest。

### 范围外

- 更改 Line 0/Line 1 的产品语义、配置 schema、IPC 或 UI。
- 现场电气接线、光电输入、频闪波形及示波器验收。
- 自动开始后续里程碑。

## 当前基线

- 已阅读需求、架构、路线图、原 M4-05 ExecPlan 及 Hikrobot 适配器。
- 工作区已有且必须保留的用户修改仅为 `config/default-config.json` 中两台相机启用线路 I/O 及其时序值。
- 当前适配器以 `MV_CC_Get/SetFloatValue` 访问三项 Strobe 节点；本机 MVS 4.8 官方 AreaScan IO 示例以 `MV_CC_SetIntValueEx` 访问相同节点。
- 现有伪 MVS 把三项节点错误建模为 Float，因而未能发现真实 SDK 节点类型不匹配。

## 前置条件与假设

- 目标构建仍使用本机 `C:\Program Files (x86)\MVS` 的 4.8.0.3 SDK/Runtime。
- 自动化测试能验证 SDK API 类型和事务语义，但不能替代实体 MV-CS020-60GM 的电气及时序验证。

## 设计说明

领域层继续使用非负 `uint32_t` 微秒值；仅 Hikrobot 适配器内部改用 `MVCC_INTVALUE_EX` 与 `set_int_value`。能力范围保留设备返回的整数步长，不再虚构步长 1。线程、队列、持久化和公开错误码均不改变。

### 线程和队列

不适用；本修复不新增或修改线程与跨线程队列。

### 持久化与恢复

不适用；配置 schema 和保存值不变。参数事务仍按旧快照回滚并回读确认。

### 错误和降级

整数节点访问失败继续映射既有稳定 `CAMERA_PARAMETER_READ_FAILED` / `CAMERA_PARAMETER_WRITE_FAILED`，保留厂商错误码、节点名和失败原因。

## 实施步骤

- [x] 1. 将三项 Strobe 能力探测、读回和写入改为整数节点 API，并保留设备步长。
- [x] 2. 将伪 MVS 三项节点改为 Integer，补断言确保没有 Float 访问，并更新事务回滚断言。
- [x] 3. 运行定向 Hikrobot 测试、Debug/Release 全量构建与非硬件 CTest、SDK 边界和 diff 检查。
- [x] 4. 记录验证证据、真机限制和完成摘要。

## 验证计划

### 自动化测试

- 能力探测通过 Integer API 取得三项范围和真实步长。
- 启用频闪时通过 Integer API 写入三项值并完整回读。
- 任一写入失败时恢复旧频闪状态和整数值。
- 伪 MVS 不提供同名 Float 节点，防止类型错误再次被测试掩盖。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug

cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

### 人工或硬件验证

- 环境：MV-CS020-60GM、已核对电气规格及隔离的频闪灯、示波器。
- 步骤：启用 Line 1，以非零持续/前置/后置参数连接，读取实际值并观察波形。
- 预期：连接参数事务成功，回读值符合设备量化，波形符合目标时序。
- 当前状态：未执行；只有用户现场运行才能形成真机结论。

## 回滚与恢复

只需回退本修复对适配器和伪 MVS 测试的变更；不得覆盖或删除用户的 `config/default-config.json` 修改。

## 验收标准

- [x] 三项 Strobe 节点只使用整数 API，范围保留设备步长。
- [x] 定向及全量非硬件测试通过。
- [x] 未修改任务外文件或用户配置。
- [x] 真机验证状态如实记录。

## 进度记录

- 2026-08-11：创建计划；从截图、当前配置、适配器源码和本机 MVS 官方示例定位到 Strobe Integer/Float 节点类型不匹配。
- 2026-08-11：完成适配器、伪 MVS 和回滚测试修复；完成 Debug/Release 构建、CTest 和静态检查，状态更新为 completed。

## 决策记录

- DEC-001：领域 DTO 不变，只修供应商适配器内部节点类型，避免扩大公开行为和配置迁移范围。

## 意外发现

- 现有伪 MVS 与错误实现使用了相同的 Float 建模，导致自动化测试无法暴露真实设备的 GenICam 节点类型错误。
- Debug 全量 CTest 首轮出现一次既有 `EventRuntimeNvme.FailedEventPersistenceKeepsLeaseProtected` 异步观察时序失败；该用例单独连续复跑 3 次通过。第二轮该用例通过，但任务外 `qt_console_smoke` 一次超过 10 秒门限；最终使用 CTest 有界 `until-pass:3` 门禁 29/29 通过。本次未修改上述模块。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-11 | 检查本机 MVS 4.8 `ParametrizeCamera_AreaScanIOSettings.cpp` | 已定位 | 官方示例以 `MV_CC_SetIntValueEx` 写入三项 Strobe 微秒节点 |
| 2026-08-11 | `cmake --preset local-windows-vs2026-debug`、全量构建 | 通过 | VS 2026 / MSVC v145 x64 Debug |
| 2026-08-11 | Debug `hikrobot_adapter_unit` | 通过 | 39/39；格式化后重新构建并复测通过 |
| 2026-08-11 | Debug `ctest --repeat until-pass:3` | 通过 | 29/29；此前两轮各有一个任务外既有时序波动，均如实记录 |
| 2026-08-11 | `cmake --preset local-windows-vs2026-release`、全量构建 | 通过 | VS 2026 / MSVC v145 x64 Release |
| 2026-08-11 | Release `ctest --repeat until-pass:3` | 通过 | 29/29；格式化后适配器 39/39 再次通过 |
| 2026-08-11 | 变更 C++ 文件 clang-format、SDK 边界、默认 JSON 解析、`git diff --check` | 通过 | MVS 引用仍只位于 Hikrobot 适配器；仅有 Git LF→CRLF 提示 |
| 2026-08-11 | 实体相机连接及 Line 0/Line 1 电气/示波器验证 | 未执行 | 未打开或改写实体相机；需要现场确认接线和安全隔离后执行 |

## 完成摘要

已将三项 Line 1 频闪微秒节点从错误的 Float API 更正为 MVS Integer API，能力探测保留真实设备步长，写入、回读和事务回滚使用同一节点类型。伪 MVS 只提供同名 Integer 节点并显式拒绝任何 Float 访问回归。Debug/Release 构建、适配器测试、非硬件 CTest 和静态检查通过；用户 `config/default-config.json` 的既有线路 I/O 配置未被修改。

实体 MV-CS020-60GM 的连接应用、Line 0 事件名兼容性、实际电平及 Line 1 波形尚未执行硬件验证，不能以自动化结果代替现场结论。
