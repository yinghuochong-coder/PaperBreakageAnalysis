# M4-05：自动曝光参数读写跟进 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-11
- 最后更新：2026-08-11
- 路线图条目：`docs/roadmap/development-roadmap.md` M4-05
- 关联需求：需求 4.4；架构 5.2～5.4；IPC 相机配置协议

## 目的与可观察结果

相机配置页“采集参数”区域新增“自动曝光”下拉框，可读取、保存并下发 Hikrobot
`ExposureAuto` 的关闭、单次和连续三种模式。设备实际模式经参数回读返回；失败沿用参数事务的
回滚与连接会话故障锁语义。旧配置安全迁移为关闭模式，保持现有生产行为。

## 范围

### 范围内

- 配置 schema、公共相机参数、Mock、Hikrobot 私有适配器的自动曝光三态读写与回读；
- `camera.list/getConfig/updateConfig/bind/connect` IPC 和 Console 状态/编辑控件；
- 相关配置、命令、客户端、Mock、伪 MVS 和离屏 UI 自动化；
- 需求、架构、配置 schema、IPC 与路线图跟进说明。

### 范围外

- 自动曝光上下限、目标亮度、自动增益或曝光 ROI 等其他 GenICam 节点；
- 相机算法、采集队列、触发/同步编辑及后续里程碑；
- 实体相机参数修改和人工图像质量调优。

## 当前基线

- `CameraParameterSnapshot`、配置和 IPC 只有 `ExposureTime`；Console 采集区显示曝光、增益和帧率；
- Hikrobot 参数事务在写曝光及每次启动/恢复取流前强制 `ExposureAuto=Off`，无法保留自动模式；
- 配置 schema v3，v2 可安全迁移；默认配置当前两路相机均使用固定曝光；
- 工作区开始时干净，没有需避让的用户未提交修改。

## 前置条件与假设

- 使用 SFNC/MVS `ExposureAuto` 枚举：`Off`、`Once`、`Continuous`；配置和 IPC 使用同名稳定字符串；
- MV-CS020-60GM 预计支持三态，但本轮不操作实体相机；伪 MVS 只证明节点和事务映射；
- 自动模式下 `ExposureTime` 是设备控制的实际值；事务写完整配置时先关闭自动曝光、写入基准曝光，
  再启用目标自动模式，以保证顺序确定且可回滚。

## 设计说明

- 配置升级为 schema v4；v2/v3 缺少 `autoExposure` 时迁移为 `Off`，序列化始终输出 v4 字段。
- 公共层增加 `ExposureAutoMode` 和设备能力模式列表；参数校验拒绝设备不支持的模式。
- Hikrobot 能力读取 `ExposureAuto` 支持值，参数快照读取当前值；写入曝光时间时先置 `Off`，
  最后写目标模式。启动和事务恢复不再无条件覆盖为 `Off`，而是读取并确认当前模式可识别。
- Console 使用三项下拉框。保存值和实际值均通过 IPC 原样展示；“读取当前参数”仍只刷新实际值，
  不隐式覆盖已保存候选值。

### 线程和队列

不新增线程或队列。参数仍在每设备互斥事务中同步串行；IPC 与 Console 复用既有有界通道。

### 持久化与恢复

schema v2/v3 迁移默认 `Off`；首次成功保存按现有仓储原子替换为 v4。任务直接更新仓库默认配置
为 v4/`Off`，不修改其他生产参数。参数事务失败恢复旧快照（含旧自动曝光模式）；无法确认恢复时
进入既有 `CAMERA_PARAMETER_FAULTED`。

### 错误和降级

- 非法/不支持模式：`SYS_CONFIG_INVALID` 或 `CAMERA_CONFIG_FAILED`，不触达写节点；
- 节点读写失败：既有 `CAMERA_PARAMETER_READ_FAILED`/`CAMERA_PARAMETER_WRITE_FAILED`，保留 MVS 码；
- 回滚或恢复取流失败：既有 `CAMERA_PARAMETER_FAULTED`；不以厂商码作为唯一业务错误。

## 实施步骤

- [x] 1. 扩展 schema v4、三态公共模型和校验，补配置迁移/拒绝与公共相机测试。
- [x] 2. 扩展 Mock 与 Hikrobot 能力、读取、确定写入顺序、回读和回滚，调整启动取流语义并补测试。
- [x] 3. 扩展服务 JSON/白名单/绑定/配置映射、Console DTO 与采集参数下拉框，补命令、客户端和 UI 测试。
- [x] 4. 更新需求、架构、配置 schema、IPC、路线图及本计划证据。
- [x] 5. 运行本机 Debug/Release 构建、CTest、格式、静态分析、SDK 边界和 diff 检查。

## 验证计划

### 自动化测试

- 配置：v2/v3 迁移 `Off`、v4 三态往返、未知枚举拒绝；
- 公共/Mock：能力校验、三态应用和回读；
- 伪 MVS：能力映射、当前值读取、Off/Once/Continuous 写序、实际回读、失败回滚、取流不覆盖模式；
- 服务/客户端/UI：JSON 往返、字段白名单、绑定默认、保存/实际状态解析、下拉框存在与就绪。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行适用的 `format-check`、Hikrobot SDK 边界、静态分析及 `git diff --check`。

### 人工或硬件验证

- 环境：目标 Windows 工控机、MVS Runtime、MV-CS020-60GM；本轮不操作实体相机。
- 步骤：连接后依次写入关闭/单次/连续，读取当前值并观察曝光变化和采集稳定性；断开重连复核保存值。
- 预期：模式与 MVS 一致，实际值回读正确，模式切换期间采集可确定恢复。
- 证据保存位置：未执行；自动化结果不能代替真机图像和模式收敛验证。

## 回滚与恢复

代码回滚只撤销本跟进增量；v4 配置可删除 `autoExposure` 并把版本降为 v3 后由旧版本读取，
不删除任何相机或事件数据。运行时失败由既有参数事务恢复旧快照。

## 验收标准

- [x] UI 可选择并提交三种自动曝光模式，保存值和设备实际值可区分；
- [x] 配置、IPC、Mock 与 Hikrobot 均完整读写回读，旧配置保持固定曝光；
- [x] 参数失败可回滚，启动/恢复取流不再覆盖用户已确认的模式；
- [x] 相关自动化、Debug/Release 构建和非硬件 CTest 已实际运行；
- [x] 文档已更新，未修改无关模块，真机限制明确。

## 进度记录

- 2026-08-11：检查需求、架构、路线图、旧 M3-03 事务计划及相关源码；创建计划，状态 in-progress。
- 2026-08-11：完成 schema v4、公共/Mock/Hikrobot、IPC、Console 和文档实现，并补齐迁移、能力、事务、回读、回滚和 UI 就绪测试。
- 2026-08-11：完成 Debug/Release 构建与非硬件 CTest、离屏 Console 冒烟、相关文件格式、MSVC 静态分析、SDK 边界、schema 和 diff 验证；计划状态改为 completed。

## 决策记录

- DEC-001：使用 `Off`/`Once`/`Continuous` 三态而非布尔值，避免丢失相机原生语义和回读信息。
- DEC-002：schema v2/v3 迁移默认 `Off`，保持旧配置和当前生产采集行为不变。
- DEC-003：完整参数写入顺序为 `ExposureAuto=Off`、`ExposureTime`、目标 `ExposureAuto`，兼顾手动基准和自动模式。

## 意外发现

- 旧实现除参数写入外，还在首次启动、更新后恢复和回滚后恢复三处强制关闭自动曝光，必须一并调整。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-11 | `git status --short` | 通过 | 任务开始时工作区干净 |
| 2026-08-11 | Debug/Release `cmake --build --preset ...` | 通过 | 两种配置均成功构建 |
| 2026-08-11 | Debug/Release `ctest --preset ... --output-on-failure` | 通过 | 两种配置均 30/30；硬件集成测试按预设排除 |
| 2026-08-11 | Debug `qt_console_smoke` | 通过 | 隔离重跑 1/1，5.61 秒；此前与 Release 构建并行时曾触及 10 秒超时 |
| 2026-08-11 | 配置 v4 schema 与默认配置校验 | 通过 | Draft 2020-12 schema 自检、默认配置验证及服务 `--validate-config` 均通过 |
| 2026-08-11 | 修改 C++ 文件 `clang-format --dry-run --Werror` | 通过 | 17 个本任务 C++ 文件全部通过 |
| 2026-08-11 | MSVC 静态分析 | 通过（任务范围） | 相机、配置、Console 目标及服务核心 `ClCompile` 通过；全依赖构建受未修改的 `src/storage/src/nvme_cache.cpp:73` C28020 阻断 |
| 2026-08-11 | `hikrobot_sdk_boundary` / `git diff --check` | 通过 | SDK 引用仍隔离在适配器；diff 无空白错误 |
| 2026-08-11 | 全仓 `format-check` | 受既有问题阻断 | 未修改的 `src/console/main.cpp` 存在既有 clang-format 差异；本任务文件已单独通过 |
| 2026-08-11 | 实体相机自动曝光 | 未执行 | 本轮不操作实体相机，不能声明模式收敛或图像质量通过 |

## 完成摘要

已增加 `Off`/`Once`/`Continuous` 三态自动曝光的配置持久化、能力探测、设备读写回读、事务回滚、
IPC 往返和 Console 下拉编辑。旧 v2/v3 配置迁移为 `Off`；启动和恢复取流不再覆盖已确认模式。
自动化、两种构建配置和任务范围静态分析均通过，唯一未完成项为需实体 MV-CS020-60GM 的模式收敛与图像验证。
