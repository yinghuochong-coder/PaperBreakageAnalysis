# M4-05：Line I/O 真机写入失败继续排查 ExecPlan

## 元数据

- 状态：completed（Line 0 实际电平翻转与 Line 1 电气/示波器验证未执行）
- 负责人：Codex
- 创建日期：2026-08-11
- 最后更新：2026-08-11
- 路线图条目：M4-05 相机配置与实际值回显（缺陷修复）
- 关联需求：4.4

## 目的与可观察结果

定位相机已连接但 `camera.connect` 返回 `CAMERA_PARAMETER_WRITE_FAILED` 的准确 MVS 节点，解释 Line I/O 保存值与设备回读不一致的原因，并修复可由自动化和真机证据确认的软件缺陷。

## 范围

### 范围内

- 扩展现有 `PaperBreakCameraHardwareTest`，使硬件计划可携带 Line I/O 参数并记录能力、请求值、实际回读和结构化错误。
- 分别执行 Line 0、Line 1 和组合配置的有界真机诊断。
- 根据诊断结果最小修复 Hikrobot 适配器，并更新相关单元测试与硬件工具文档。

### 范围外

- 改变 Line 0/Line 1 的产品定义。
- 未核对电气规格前驱动外部负载或声称完成电气/示波器测试。
- 开始后续里程碑或进行无关重构。

## 当前基线

- 2026-08-11 IPC 控制日志显示两次 `camera.connect` 以 `CAMERA_PARAMETER_WRITE_FAILED` 结束，随后 `camera.start` 成功。
- 截图显示保存/编辑值为 Line 0 启用、Line 1 启用、23/5/0 us；设备回读为 Line 0 未启用、Line 1 启用、100/0/0 us。
- 服务端当前只把 `CAMERA_CONFIG_FAILED` 转换为“连接成功、参数未应用”，其他参数应用失败会作为整个连接命令失败返回。
- 现有硬件工具能保存完整 `Error.details`，但计划解析和输出尚不包含 Line I/O。

## 前置条件与假设

- 本机 MVS 4.8.0.3 Runtime 可用，服务进程已退出，相机可被诊断工具独占打开。
- 真机诊断只通过生产 Hikrobot 适配器执行；不在工具中直接调用 MVS SDK。
- Line 1 输出可能连接外部设备，因此仅执行配置写入和回读，不执行额外软件触发或电气断言。

## 设计说明

工具继续使用既有有界 JSON 计划和原子证据文件。`parameters.lineIo` 必须包含两个启用位和三项无符号微秒值；工具在应用前注册无副作用的 Line 0 观察器，并在能力记录和实际参数中输出 Line I/O。生产适配器之外不增加 MVS SDK 调用。

### 线程和队列

不新增线程或队列。硬件工具沿用既有有界采集队列和确定性清理路径。

### 持久化与恢复

适配器现有参数事务在写入失败时回滚旧快照。诊断证据写入 `out/diagnostics`，不覆盖现有文件。

### 错误和降级

硬件证据保留业务码、模块、操作、MVS 原生码、节点和原因。生产连接现有部分成功语义不变；本次通过修正两个真实 MVS 调用错误，使保存参数能够完整应用和回读，不扩大 IPC 行为。

## 实施步骤

- [x] 1. 扩展硬件工具的 Line I/O 计划解析、能力/参数 JSON 和观察器边界，并补无硬件解析测试。
- [x] 2. 构建工具，分别执行 Line 0、Line 1 和组合诊断，记录准确失败节点和回读。
- [x] 3. 根据真机结果最小修复适配器，更新单元测试与硬件工具文档。
- [x] 4. 运行格式化、定向测试、Debug/Release 构建和非硬件 CTest，记录限制与完成摘要。

## 验证计划

### 自动化测试

- 硬件工具帮助与无效计划门禁继续通过。
- 新增有效 Line I/O 计划的解析检查，不要求实体相机。
- Hikrobot 伪 API 覆盖诊断发现的失败路径和修复路径。
- 既有系统命令和客户端状态测试保持通过。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

### 人工或硬件验证

- 环境：两台 MV-CS020-60GM；本机 MVS 4.8.0.3；服务进程退出。
- 步骤：对 CAM01 分别应用 Line 0-only、Line 1-only、组合计划，检查 JSON 的能力、错误节点及实际回读。
- 预期：能把故障收敛到单一节点/事件操作；修复后组合配置写入和回读一致（允许按设备步进量化）。
- 电气验证：未执行；本任务只验证相机参数 API 和回读。

## 回滚与恢复

适配器写入失败由既有事务回滚。代码回滚仅涉及本计划列出的工具、适配器、测试和硬件工具文档；不删除用户配置或诊断证据。

## 验收标准

- [x] 真机证据包含准确失败节点、MVS 原生码和原因。
- [x] Line I/O 组合配置的软件写入与回读一致。
- [x] 当前保存参数完整应用，连接命令不再进入参数应用失败分支。
- [x] 相关自动化测试、Debug/Release 构建和非硬件 CTest 通过。

## 进度记录

- 2026-08-11：创建计划；完成日志、截图、连接命令和现有硬件工具基线检查。
- 2026-08-11：扩展硬件工具并执行分离诊断；Line 1-only 成功，Line 0-only 在 `LineMode` 返回 `0x80000106`。
- 2026-08-11：移除固定输入 Line 0 的只读 `LineMode` 写入后，事件注册在 `EventLine0RisingEdge` 返回 `0x80000004`；按 MVS EventSelector symbolic 规则更正为 `Line0RisingEdge` / `Line0FallingEdge`。
- 2026-08-11：组合真机复测通过，完成 Debug/Release 构建、全量非硬件 CTest 和定向格式检查。

## 决策记录

- DEC-001：扩展现有硬件工具而不是新增直接调用 MVS SDK 的临时程序，保持所有供应商调用位于 Hikrobot 适配器内。
- DEC-002：不扩大 IPC 部分成功语义；真机证明当前错误来自适配器调用，修复后连接参数事务完整成功。

## 意外发现

- `MV-CS020-60GM` 的 Line 0 `LineMode` 枚举可读且包含 Input，但节点不可写；写入相同值仍返回 `MV_E_GC_ACCESS (0x80000106)`。
- `MV_CC_RegisterEventCallBackEx` / `MV_CC_EventNotificationOn` 需要 EventSelector symbolic（`Line0RisingEdge`），而不是 GenICam 节点名（`EventLine0RisingEdge`）。
- 仓库级 `format-check` 被任务外既有 `src/console/main.cpp` 格式问题拦截；本次三个 C++ 变更文件的 clang-format 定向检查通过，未修改该无关文件。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-11 | 检查 `paperbreak-service-ipc-control-2026-08-11.log` | 已确认 | connect 两次写参数失败，start 随后成功；日志缺少节点和 MVS 原生码 |
| 2026-08-11 | CAM01 Line 0-only 诊断 | 预期失败并定位 | `LineMode` / `write-failed` / `0x80000106` |
| 2026-08-11 | CAM01 Line 1-only 诊断 | 通过 | 23/5/0 us 原样回读；67 帧、无跳帧/超时/残帧 |
| 2026-08-11 | 移除 LineMode 写入后的组合诊断 | 预期失败并定位 | `EventLine0RisingEdge` / `register-failed` / `0x80000004` |
| 2026-08-11 | 修复事件 symbolic 后组合诊断 | 通过 | Line 0/1 均启用，23/5/0 us 原样回读；66 帧、无跳帧/超时/残帧；`out/diagnostics/line-io-result-combined-fixed-v2.json` |
| 2026-08-11 | Hikrobot 适配器与硬件工具定向测试 | 通过 | 适配器 40/40；工具 4/4 |
| 2026-08-11 | Debug/Release 全量构建与非硬件 CTest | 通过 | 两种配置均 30/30 |
| 2026-08-11 | 本次 C++ 文件 clang-format dry-run | 通过 | 3/3；仓库级检查被任务外 `src/console/main.cpp` 拦截 |
| 2026-08-11 | Line 0 实际电平翻转、Line 1 外部负载及示波器 | 未执行 | 只验证 MVS 参数 API、事件订阅成功、回读与连续取流 |

## 完成摘要

已扩展既有硬件工具以支持 Line I/O 计划、能力和结构化错误记录，并通过真机分离诊断定位两个缺陷：固定输入 Line 0 的只读 `LineMode` 被误写，以及事件 API 使用了错误的 GenICam 节点名。修复后 CAM01 组合配置中 Line 0/Line 1 均启用，23/5/0 us 原样回读，连续取流无跳帧、超时或残帧。Debug/Release 全量构建与非硬件 CTest 通过；实际电平翻转和电气波形仍需现场安全条件下验证。
