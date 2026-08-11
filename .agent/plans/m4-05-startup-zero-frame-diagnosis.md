# M4-05：相机启动全零帧根因取证（缓冲哨兵）ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-11
- 最后更新：2026-08-11
- 路线图条目：`docs/roadmap/development-roadmap.md` M4-05 跟进
- 关联需求：需求 4.5～4.7；架构 5.2～5.4、17.1

## 目的与可观察结果

在每次采集启动后的前 8 次取帧调用前，用 `0xA5` 填充已预分配的目标缓冲区，并仅在相机
Debug 诊断开启时对 SDK 成功返回的有效载荷做一次线性扫描。日志应能区分缓冲未写入、疑似
部分写入、完整零覆盖和正常覆盖，为事件
`EVT-019ff02f-0bb7-76b5-929e-fb7003fde980` 的启动全零首帧提供下一步真机取证依据。

## 范围

### 范围内

- 通用采集工作线程的启动缓冲哨兵、固定分类和 Debug 诊断日志；
- 完整保留哨兵帧的下游抑制及既有不完整帧统计；
- Mock 自动化测试、真机取证矩阵、记录模板和当前执行状态；
- Debug/Release 构建、非硬件 CTest 和 Hikrobot SDK 边界验证。

### 范围外

- 候选事件创建门槛、`consecutiveFrames` 语义或正式全零帧防误报策略；
- 丢弃已确认完整覆盖的全零帧；
- 新配置字段、公开 API、事件数据格式或业务错误码；
- 修改已有事件数据、默认生产配置或相机参数。

## 当前基线

- 首帧原始块和 JPEG 均为 2,013,760 字节全零，说明零数据在编码和落盘之前已经存在；
- `AcquisitionWorker` 从固定容量 `FrameBufferPool` 取得缓冲后直接调用
  `ICameraDevice::capture_into()`，成功后将帧放入有界丢最旧队列；
- Hikrobot `DeviceHandle::capture_into()` 调用 `MV_CC_GetOneFrameTimeout`，其调用前的
  `FrameBuffer::clear()` 只重置逻辑长度，不修改底层存储；
- 相机运行时已经注入 `DebugDiagnosticSink`，其 `enabled()` 由当前日志等级决定，默认生产日志
  等级仍为 `info`；
- 任务开始时工作树干净。

## 前置条件与假设

- `capture_into()` 成功时必须把 `FrameBuffer::size()` 设置为 SDK 报告的有效载荷长度；扫描不触及
  容量尾部；
- `0xA5` 也可能是合法像素值，因此只有有效载荷首部或尾部存在连续哨兵时才记录为
  `partial-write-suspected`；载荷内部的孤立 `0xA5` 只进入总数统计；
- 当前环境安装了 MVS SDK，但实体 CAM01、频闪和官方 MVS 客户端是否可安全占用需在软件验证后
  再确认；未实际执行的硬件项目必须标记“未执行”。

## 设计说明

- 工作线程启动时只调用一次诊断 `enabled()`。关闭时探针计数为 0，不填充、不扫描、不构造探针
  日志；开启时仅前 8 次 `capture_into()` 调用填充整个目标容量，失败返回不扫描但仍消耗一次探针
  尝试。
- 成功返回后只扫描 `buffer.bytes()`。记录零字节数、哨兵字节数、首尾连续哨兵长度、最小/最大值、
  有效载荷长度、相机帧号和原始丢包标志。
- 分类顺序固定为：全载荷哨兵 `unwritten-sentinel`；首部或尾部连续残留哨兵
  `partial-write-suspected`；无哨兵且全零 `all-zero-overwritten`；其余 `normal`。这样不会把
  图像内部自然出现的孤立 `0xA5` 误判为复制不完整。
- `unwritten-sentinel` 增加既有不完整帧计数并在序号、字节和下游入队统计之前跳过；其余分类
  完全沿用当前发布路径，尤其不丢弃 `all-zero-overwritten`。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 采集帧队列 | `AcquisitionWorker` | 相机转发线程 | 既有配置，默认 4 | 丢最旧 | 停止令牌退出，关闭后可排空 | 深度、高水位、入/出、丢弃 |

不新增线程或队列。探针扫描位于采集工作线程，只在启动诊断的最多 8 个成功载荷上执行，不进行
磁盘、网络、编码或推理。

### 持久化与恢复

不改变任何持久化格式或生产配置。诊断日志由既有日志运行时写出；硬件取证摘要和人工记录保存在
`docs/validation/m4-05/`，原始运行日志按轮次复制到该目录或记录不可变来源路径。

### 错误和降级

- SDK 错误和超时仍沿用现有业务错误映射、重试和停止语义；
- 哨兵未写入不新增业务错误，只增加 `incomplete_frames` 并跳过下游；
- 部分写入和全零覆盖仅记录诊断，不改变帧标志或发布语义；
- 未安装/不可占用实体硬件时只完成软件分类能力，不能确定相机侧根因。

## 实施步骤

- [x] 1. 在 `src/camera/src/acquisition.cpp` 增加私有扫描统计、分类和前 8 次调用门控。
- [x] 2. 在 `tests/unit/camera_acquisition_tests.cpp` 扩展脚本相机，覆盖四类结果、8 次上限、关闭路径和未写入不入队。
- [x] 3. 创建 `docs/validation/m4-05/` 取证说明，记录硬件矩阵、MVS 版本、参数回读字段、原始日志位置和未执行项。
- [x] 4. 更新路线图跟进与本计划证据，运行格式、Debug/Release 构建、CTest、专项测试、SDK 边界和 diff 检查。

## 验证计划

### 自动化测试

- 正常完整覆盖：分类 `normal`，零哨兵残留，帧进入队列；
- 完整保留哨兵：分类 `unwritten-sentinel`，首尾/总哨兵长度等于载荷，不进入队列且不完整计数加一；
- 部分覆盖：分类 `partial-write-suspected`，记录残留数量和尾部连续长度，仍进入队列；载荷内部
  自然出现孤立 `0xA5` 仍分类为 `normal`；
- 完整全零覆盖：分类 `all-zero-overwritten`，零字节数等于载荷，仍进入队列；
- 连续 9 个成功帧：只有前 8 次填充和扫描；
- 诊断关闭：不填充、不扫描、不记录启动探针，正常采集行为保持不变。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-debug -R "^(hikrobot_adapter_unit|hikrobot_sdk_boundary)$"
ctest --preset local-windows-vs2026-release -R "^(hikrobot_adapter_unit|hikrobot_sdk_boundary)$"
git diff --check
```

### 人工或硬件验证

- 环境：CAM01、MV-CS020-60GM、MVS Development/Runtime 4.8.0.3、当前曝光/增益/频闪参数；
- 步骤：临时改为 Debug 日志并关闭算法，至少执行 20 次 start/stop 和 5 次 connect/start；每轮保存
  首 8 次探针日志与参数回读。若出现完整零覆盖，再用官方 MVS 客户端按相同参数重复启停；
- 预期：依据用户给定矩阵区分 SDK 未写、复制不完整、相机/频闪时序或本程序调用顺序；
- 证据保存位置：`docs/validation/m4-05/`；未执行项不得写成通过。

## 回滚与恢复

只撤销本任务对采集实现、测试和取证文档的增量即可恢复原行为。任务不迁移配置、不修改事件或
相机持久化数据，无数据恢复步骤。

## 验收标准

- [x] Debug 开启时每次采集启动仅探测前 8 次取帧，日志字段和四类名称固定；
- [x] Debug 关闭时不填充、不扫描、不产生探针日志；第 9 次起同样无探针工作；
- [x] 仅 `unwritten-sentinel` 被抑制并计入不完整帧；全零覆盖仍按当前行为发布；
- [x] 自动化测试覆盖计划中的全部场景；Debug/Release 构建和非硬件测试实际运行；
- [x] 硬件矩阵、日志位置和执行状态已记录，未将未执行的硬件项目声称为通过。

## 进度记录

- 2026-08-11：阅读需求、架构、路线图、ExecPlan 规范及采集/Hikrobot/测试基线；创建计划，状态 `in-progress`。
- 2026-08-11：实现前 8 次取帧缓冲哨兵、四类日志与完整哨兵下游抑制；新增三项采集工作线程测试。
- 2026-08-11：首次真机试运行发现合法图像内部会自然出现孤立 `0xA5`，收紧部分写入判据并补回归测试。
- 2026-08-11：完成 Debug/Release 全量构建与非硬件测试；使用 CAM01 完成 20+5 轮启停，保存 200 条正式探针记录，全部为 `normal`。
- 2026-08-11：完成取证文档和路线图更新；根因因未复现而保持未确定，状态改为 `completed`。

## 决策记录

- DEC-001：探针放在通用采集工作线程，不向公共相机接口增加诊断参数；这样能在 SDK 调用前填充
  实际目标池缓冲，并复用 Mock 自动化验证。
- DEC-002：诊断启用状态在工作线程启动时取一次；这对应“启动探针”，并避免关闭诊断时逐帧执行
  新的动态检查。
- DEC-003：完整哨兵帧不分配服务序号、不累计有效字节、不更新最后有效帧，避免把未写入负载伪装
  成下游帧；只增加既有不完整帧计数。
- DEC-004：`partial-write-suspected` 只由有效载荷首部或尾部的连续哨兵残留触发；内部孤立
  `0xA5` 仍计数但归为 `normal`，避免把合法 Mono8 像素误判为复制不完整。
- DEC-005：官方 MVS 对照只在程序复现 `all-zero-overwritten` 后执行。本轮正式 200 条记录均为
  `normal`，因此保持该项目“未执行”，不以没有复现推断相机侧根因。

## 意外发现

- Hikrobot 适配器的 `destination.clear()` 不清零存储，因此不会破坏调用方预填的哨兵。
- 路线图 M4-05 已完成，本任务作为同一条目的故障取证跟进，不改变既有里程碑状态。
- 首次真机试运行的 192 条探针记录中，59 条图像内部自然出现 1～2 个 `0xA5`，但首尾连续
  哨兵均为 0；按“任意哨兵即部分写入”的初版判据会产生明显误报，因此改为只依据首尾连续
  残留分类，并增加回归测试。该批试运行不用于根因结论。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-11 | `git status --short` | 通过 | 任务开始时工作区干净 |
| 2026-08-11 | 首次定向 Debug 构建 | 失败后修正 | 测试误用了 C++23 `std::string::contains`；改为 C++20 `find` 后通过 |
| 2026-08-11 | Debug 配置、全量构建、`ctest --preset local-windows-vs2026-debug` | 通过 | 非硬件测试 30/30；硬件集成按预设排除 |
| 2026-08-11 | Release 配置、全量构建、`ctest --preset local-windows-vs2026-release` | 通过 | 非硬件测试 30/30 |
| 2026-08-11 | Debug/Release `hikrobot_adapter_unit` 与 `hikrobot_sdk_boundary` | 通过 | 两个预设均为 2/2 |
| 2026-08-11 | 本任务 C++ 文件 `clang-format --dry-run --Werror` | 通过 | Visual Studio LLVM clang-format 22.1.3 |
| 2026-08-11 | 全仓 `format-check` | 被既有文件阻断 | 未修改的 `src/console/main.cpp` 存在既有格式差异；未越界修改 |
| 2026-08-11 | `PaperBreakCameraHardwareTest.exe --probe` | 通过 | 发现唯一 CAM01，型号 MV-CS020-60GM，序列号 DB1888674，独占访问可用 |
| 2026-08-11 | 初始临时 16 帧池服务启动 | 配置校验拒绝 | 事件窗口要求 1818 帧；在任何相机 IPC 操作前拒绝，随后仅调整仓库外临时诊断配置为有界 160 帧池 |
| 2026-08-11 | 初版任意哨兵判据试运行 | 判据无效 | 192 条中 59 条含内部自然 `0xA5`，首尾残留均为 0；日志仅保留为判据修正证据 |
| 2026-08-11 | CAM01 20 次同连接 start/stop + 5 次 connect/start | 完成，未复现 | 200/200 为 `normal`，载荷均为 2,013,760 字节，丢包 0，首尾哨兵残留 0 |
| 2026-08-11 | 官方 MVS 同参数对照 | 未执行 | 计划触发条件 `all-zero-overwritten` 未出现 |
| 2026-08-11 | `git diff --check` | 通过 | 仅报告工作副本 LF/CRLF 转换警告，无空白错误 |

## 完成摘要

已完成仅由 Debug 诊断启用的前 8 次取帧缓冲哨兵、固定四分类日志、完整哨兵帧抑制和自动化覆盖；
未新增配置、公开 API、数据格式或业务错误码，默认 `info` 配置未改。Debug/Release 非硬件测试均为
30/30，通过两个 Hikrobot 专项入口。实体 CAM01 完成计划要求的 25 轮启停，正式 200 条探针记录均为
`normal`。本次没有复现全零首帧，不能确定 SDK、驱动、相机或频闪中的根因；官方 MVS 对照因触发
条件未出现而明确标记为未执行。
