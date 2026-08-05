# M7-02：存储配置控制端补接 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-05
- 最后更新：2026-08-05
- 路线图条目：M7-02 异步块写入（配置与运行时消费者的控制端补接）
- 关联需求：4.13、4.17、5.2、6.1

## 目的与可观察结果

控制端“存储配置”导航不再显示 M4 占位文案，而是从后台服务读取并展示事件目录、缓存目录、NVMe 滚动缓存参数、磁盘水位和事件容量上限；管理员可通过 IPC 保存完整 storage 对象，并明确看到已应用或需要重启的字段。页面同时展示既有有界指标快照中的存储/NVMe 实际状态。

## 范围

### 范围内

- 新增 `storage.getConfig` / `storage.updateConfig` 本机 IPC 合同。
- 新增控制端 StorageClient、页面和状态绑定。
- 使存储水位与事件容量上限的热更新真正下发到 `StoragePolicyManager`。
- 更新 IPC 文档与相关自动化测试。

### 范围外

- 不实现 M8 上位机上传、PLC、事件人工锁定或新的 NVMe 文件格式。
- 不变更相机采集、事件目录、SQLite schema 或硬件验收结论。
- 不修改用户已有的 `config/default-config.json` 工作区改动。

## 当前基线

- `src/console/src/main_window.cpp` 对 storage 页面仍走 `placeholder_message()`。
- schema v2 已包含完整 storage 字段，配置仓储已区分 `/storage/roots`、`/storage/nvme`（重启）与 `/storage/watermarks`（热应用）。
- 服务已导出存储/NVMe 指标，但没有 storage 配置 IPC 命令或控制端客户端。
- `StoragePolicyManager` 仅支持热更新保留天数，尚不能更新水位和事件容量上限。
- 工作区开始时已有用户修改：`config/default-config.json`。

## 前置条件与假设

- 本任务使用 Mock IPC/临时文件自动化，不需要实体相机、MVS SDK 或目标 NVMe。
- 路径继续由配置 schema 和服务仓储校验；Qt 客户端不直接读写生产配置。
- 所有 storage 更新仍携带乐观并发修订并由服务审计。

## 设计说明

`storage.getConfig` 返回保存/有效 storage DTO、配置修订和 pendingRestartPaths；`storage.updateConfig` 只接受完整 storage 对象和 expectedConfigRevision。服务复用 ConfigRepository 的严格 schema、原子保存、回滚和审计。控制端 StorageClient 只保留一个配置请求和一个写操作请求，不缓存断线写入。根目录和 NVMe 字段保存后保留旧有效值并标记需重启；水位与事件容量上限通过 StoragePolicyManager 原子重配置并立即刷新准入快照。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| StorageClient 配置请求 | Qt UI/刷新定时器 | 既有 IpcClient | 1 | 已有请求时合并刷新 | 断线/停止取消在途请求 | 复用 IPC 指标 |
| StorageClient 更新请求 | Qt UI | 既有 IpcClient | 1 | 忙时返回 `IPC_BUSY` | 不跨连接重放 | 复用 IPC 指标 |

不新增服务工作线程或跨线程队列。

### 持久化与恢复

不新增格式或 schema。更新继续由 ConfigRepository 使用同目录临时文件、flush、原子替换、历史版本和失败回滚。需重启字段只改变 stored 快照；effective 快照在重启前保持旧值。

### 错误和降级

- 未授权写入：`IPC_UNAUTHORIZED`。
- 停止阶段写入：`SYS_SERVICE_STOPPING`。
- 非完整或未知字段：`IPC_REQUEST_INVALID` / `SYS_CONFIG_INVALID`。
- 乐观并发冲突：`SYS_CONFIG_VERSION_CONFLICT`。
- 客户端未同步/断线/忙：稳定客户端业务错误并禁止保存。
- 热应用失败：`SYS_CONFIG_APPLY_FAILED`，保留旧文件和旧有效策略。

## 实施步骤

- [x] 1. 为 StoragePolicyManager 增加水位/容量上限事务式热更新和单元测试。
- [x] 2. 增加 storage IPC 命令、严格合同测试及 IPC 文档。
- [x] 3. 增加 StorageClient、控制端页面、指标展示和客户端/smoke 测试。
- [x] 4. 运行格式、Debug/Release 构建及 CTest，记录限制。

## 验证计划

### 自动化测试

- StoragePolicyManager：合法水位/容量更新立即改变分类/准入；非法更新不改变旧策略。
- SystemCommandService：读取、管理员更新、未知字段、版本冲突、pending restart 语义。
- StorageClient：连接后读取、完整更新 payload、失败/断线陈旧状态、有界并发。
- Qt smoke：storage 页面存在、可编辑、无占位文案，并能显示运行指标。

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

- 环境：本机 Qt 离屏 smoke；不依赖实体相机或 NVMe。
- 步骤：自动化构造已同步 storage 快照并检查页面控件、状态和指标。
- 预期：页面可编辑，保存按钮启用，重启语义可见。
- 目标 NVMe 持续写、拔盘、断电和四相机生产负载：未执行，本任务不改变既有 M7 硬件限制。

## 回滚与恢复

代码变更可按本计划列出的文件逐项撤销；配置格式没有变化。测试只使用临时目录。不得覆盖或删除用户现有 `config/default-config.json` 改动。

## 验收标准

- [x] 存储配置页不再显示“将在 M5、M7 接入”。
- [x] 页面完整显示和保存 schema v2 storage 字段，并明确 saved/effective/restart 状态。
- [x] 服务端严格校验、授权、审计并原子保存。
- [x] 水位/事件容量热应用与页面运行状态有测试覆盖。
- [x] 相关 Debug/Release 构建与测试实际运行并记录结果。

## 进度记录

- 2026-08-05：核对需求、路线图、架构、配置 schema、服务和控制端基线，创建计划并开始实施。
- 2026-08-05：完成服务 IPC、StorageClient、Qt 页面、运行指标绑定和存储策略热应用；Debug/Release 全量验证通过，状态更新为 completed。

## 决策记录

- DEC-001：使用独立 StorageClient 和 `storage.*` IPC 命令，不把 storage DTO 塞入 event 客户端；保持导航/业务边界清晰。
- DEC-002：更新必须提交完整 storage 对象；沿用 schema v2 的未知字段拒绝和配置修订语义。
- DEC-003：运行指标复用 OperationsClient 的有界快照，不增加高频采样或新线程。

## 意外发现

- ConfigRepository 已把 `/storage/watermarks` 视为热应用，但 StoragePolicyManager 尚无对应重配置入口；在暴露更新 UI 前必须补齐，避免“已应用”与实际运行时不一致。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-05 | 基线源码与文档检查 | 确认 storage 页面为占位且配置链路缺失 | 未执行硬件测试 |
| 2026-08-05 | `PaperBreakTests.exe --gtest_filter=StoragePolicy...:SystemCommand...:StorageClient...` | 3/3 通过 | 覆盖热更新、IPC 合同和客户端读写/重启回显 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | MSVC Debug `/WX` 全量构建 |
| 2026-08-05 | `ctest --preset local-windows-vs2026-debug` | 25/25 通过 | 通用 unit 入口 294 项，Qt 离屏 smoke 通过 |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-release` | 通过 | MSVC Release `/WX` 全量构建 |
| 2026-08-05 | `ctest --preset local-windows-vs2026-release` | 25/25 通过 | 通用 unit 入口 294 项，Qt 离屏 smoke 通过 |
| 2026-08-05 | 12 个任务 C++ 文件 `clang-format --dry-run --Werror` | 通过 | 与全仓目标使用同一 VS clang-format |
| 2026-08-05 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 被既有文件阻断 | 未修改的 `src/pipeline/include/paperbreak/pipeline/preview.hpp:6` 格式问题 |
| 2026-08-05 | `git diff --check` | 通过 | `config/default-config.json` 为任务开始前的用户修改，未触碰 |

## 完成摘要

控制端存储页已从占位页替换为完整 schema v2 配置与实际运行指标页面；新增独立 StorageClient 和两项严格 storage IPC 命令，所有更新继续经过服务端管理员授权、乐观修订、审计、原子保存和失败回滚。根目录/NVMe 字段明确显示需重启，水位和事件容量上限现在能真正热应用。Debug/Release 构建和两套 CTest 均通过。未执行实体四相机、目标 NVMe、拔盘、断电或持续热稳定测试，不改变既有 M7 硬件验收限制。
