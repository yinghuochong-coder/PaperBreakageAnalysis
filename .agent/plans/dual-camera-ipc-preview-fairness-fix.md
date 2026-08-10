# 双相机 IPC 超时与预览公平性修复 ExecPlan

## 元数据

- 状态：implemented（自动化验证通过；真机验收未执行；格式检查受既有文件阻断）
- 负责人：Codex
- 创建日期：2026-08-10
- 最后更新：2026-08-10
- 路线图条目：M3/M4 已完成功能的缺陷修复，不开始后续里程碑
- 关联需求：相机参数控制、图像采集、低帧率预览、本机 IPC、健康指标

## 目的与可观察结果

两台相机按各自配置帧率采集，低帧率预览独立抽样。第二台相机启动即使包含大帧池分配，也不会阻塞状态和指标查询；超时操作显示结果未知并通过状态快照对账。多路预览持续公平刷新，不再长时间成段切换。

## 范围

### 范围内

- MVS `AcquisitionFrameRateEnable` 的兼容写入、验证和回滚。
- 相机已验证参数缓存、采集中无 SDK 读参、启动资源顺序和幂等控制。
- IPC 串行控制与只读查询的有界隔离调度及分通道指标。
- Console 相机长操作超时和未知结果对账。
- 预览优先扇出、轮转公平性、条件变量唤醒和每路统计。
- 对应自动化测试和架构/IPC 文档。

### 范围外

- 修改当前生产相机帧率、ROI、包大小、包间延迟或日志等级。
- 更改配置 schema 或 IPC 线格式。
- 自动开始其他路线图里程碑。
- 在没有实际执行时宣称真机双相机测试通过。

## 当前基线

- `IpcServer` 当前只有一个全局命令工作线程，所有连接共享同一 FIFO。
- `CameraControlRuntime::read()` 每次调用设备能力和参数读取，采集时与 `get_one_frame_timeout` 争用同一设备锁。
- CameraClient 所有请求默认 5 秒超时。
- 预览在事件和 NVMe 提交之后，编码线程遍历无序映射并以 2 ms 轮询。
- 当前配置为两台 800×600、各 60 fps，预览 3 fps，`framePoolCapacity=2187`。
- 2026-08-10 日志显示实际约 138 fps、帧持续 `incomplete=true`；第二台 `camera.start` 完成时队列深度 25，随后 `camera.list` 单次阻塞约 50～70 秒，队列深度达到 111。
- 任务开始时工作区无未提交修改。

## 前置条件与假设

- 目标 MV-CS020-60GM 支持 `AcquisitionFrameRateEnable`；明确返回不支持时兼容无需该节点的设备。
- 继续保留现有帧池容量门禁和固定内存预算。
- SystemCommandService 的只读用例可并发执行；所有写用例仍通过单一控制通道串行。
- 真机验收需要实体相机、目标网卡/交换机和人工观察，自动测试仅使用 Fake/Mock。

## 设计说明

- `IRequestHandler` 增加默认返回 control 的请求分类接口。SystemCommandService 仅将已知非写请求分类为 query，未知命令进入 control。
- IPC 使用 control 1 worker/128 条和 query 2 workers/512 条。两个通道独立满载，汇总指标保留并增加分通道指标。
- Session 缓存设备描述、能力和最近成功回读参数。列表和采集中的配置查询不访问 SDK；显式连接/更新负责刷新缓存。
- 相机启动先构造帧池、队列和采集 worker 对象，再启动设备取流和工作线程；任一步失败按相反顺序清理。
- CameraClient 控制操作默认 30 秒。超时后记录 unknown outcome，立即刷新列表；开始/停止/连接/断开按目标状态对账。
- PreviewRuntime 保留每相机 latest-wins 单槽，增加固定相机顺序、轮转起点、条件变量和每路计数/最后投递时间。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| IPC control | IPC 事件线程 | 1 控制 worker | 128 | 返回 `IPC_BUSY` | 停止接收后排空已接受请求至共享截止 | depth/HWM/duration |
| IPC query | IPC 事件线程 | 2 查询 worker | 512 | 返回 `IPC_BUSY` | 停止接收后排空已接受请求至共享截止 | depth/HWM/duration |
| preview.latest[i] | 相机转发线程 | 1 预览 worker | 每相机 1 | 覆盖旧帧 | 停止时丢弃 pending 并唤醒退出 | per-camera sampled/replaced/encoded/delivered/last-delivery |

### 持久化与恢复

不适用。无线格式、配置 schema 或数据库变更；不修改生产配置文件。

### 错误和降级

- 帧率 enable 节点不支持：继续写帧率值；其他错误保持稳定 `CAMERA_PARAMETER_*` 并保留 MVS native code。
- 帧池分配或工作线程创建失败：相机保持 connected，不启动或立即停止取流。
- IPC 通道满：返回可重试 `IPC_BUSY`，不扩容。
- 相机控制超时：Console 显示结果未知并对账，不将迟到成功误报为失败。

## 实施步骤

- [x] 1. 扩展 MVS Fake 和参数事务，写入/回读/回滚 `AcquisitionFrameRateEnable`。
- [x] 2. 重构 CameraControlRuntime Session 快照缓存、幂等控制及启动资源顺序。
- [x] 3. 扩展 IPC handler 分类、双有界队列/worker、分通道指标和停止路径。
- [x] 4. 扩展 CameraClient 控制超时、unknown outcome 和状态对账。
- [x] 5. 将预览置于扇出首位并实现固定轮转、条件变量和每路统计。
- [x] 6. 补齐定向测试并更新架构、IPC 和开发说明。
- [x] 7. 运行 Debug/Release 构建、非硬件 CTest、格式和差异检查；格式检查记录既有文件阻断。
- [x] 8. 真机验证未执行：当前任务环境未安排实体双相机和人工场景。

## 验证计划

### 自动化测试

- Hikrobot Fake：节点存在、不支持、enable 写失败、帧率写失败和回滚。
- CameraControlRuntime：采集期间列表不调用 SDK 读参、幂等开始/停止、启动失败清理。
- IPC：阻塞控制期间查询仍返回、两通道容量、指标和确定性关闭。
- CameraClient：短超时 unknown outcome、后续状态确认和真实失败。
- PreviewRuntime：双路高频输入下两路在两个预览周期内均持续投递。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行项目格式检查目标（按 CMake 暴露名称）、`git diff --check` 和任务文件范围检查。

### 人工或硬件验证

- 环境：两台 MV-CS020-60GM、800×600、配置 60 fps、预览 3 fps。
- 预期：实际帧率接近配置；不完整帧/缺口不持续增加；启动第二台时状态查询小于 5 秒；队列回落到 0；两路预览连续刷新。
- 证据：保存指标导出、相关线程日志和场景记录。
- 未实际执行时必须标记“未执行：需要实体相机和人工场景”。

## 回滚与恢复

无线格式或数据迁移。回滚代码即可恢复原行为；不删除配置、日志、事件或缓存数据。若双通道并发暴露只读线程安全问题，先将对应命令分类回 control，不扩大锁范围到采集回调。

## 验收标准

- [x] 配置 60 fps 时 MVS 限帧 enable 事务有自动测试覆盖。
- [x] 采集中的 `camera.list/getConfig` 不访问 MVS 参数节点。
- [x] 慢相机控制不阻塞系统状态和指标查询。
- [x] 相机操作超时显示未知并可由快照确认。
- [x] 双路预览自动化测试无单路饥饿。
- [x] Debug/Release 构建和非硬件 CTest 通过。
- [x] 文档与公开接口同步更新。
- [x] 真机状态明确为未执行。

## 进度记录

- 2026-08-10：创建计划，状态 in-progress；完成日志、配置和代码基线诊断。
- 2026-08-10：完成 MVS 限帧事务、相机缓存/锁拆分、IPC 双通道、客户端超时对账和预览轮转实现及定向测试；进入全量验证。
- 2026-08-10：Debug/Release 配置、构建与 29/29 CTest 全部通过；差异检查通过；真机未执行；项目格式检查受未修改的 `src/camera/src/camera.cpp` 既有格式问题阻断。

## 决策记录

- DEC-001：实际采集帧率继续按配置可调，预览帧率独立。
- DEC-002：写命令保持单线程顺序，只读请求使用独立双 worker，避免破坏配置事务顺序。
- DEC-003：不降低 2187 帧池容量；用长操作超时与 IPC 隔离处理启动耗时。
- DEC-004：IPC 线格式与配置 schema 保持不变。
- DEC-005：相机会话对象在进程期保持稳定地址；目录锁只用于查找/创建，会话控制锁和缓存锁分离，使慢启动期间同路缓存查询也可返回。

## 意外发现

- 当前逐帧 debug 日志量很大，但本任务不擅自修改生产日志等级。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-10 | 基线日志只读分析 | 失败基线已确认 | 实际约 138 fps、持续 incomplete、IPC queueDepth 最高 111 |
| 2026-08-10 | `cmake --preset local-windows-vs2026-debug` | 通过 | 生成 Debug 工程 |
| 2026-08-10 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | 全部目标编译链接成功 |
| 2026-08-10 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | 29/29；`unit` 371 项通过，Hikrobot 适配器 34 项通过 |
| 2026-08-10 | `cmake --preset local-windows-vs2026-release` | 通过 | 生成 Release 工程 |
| 2026-08-10 | `cmake --build --preset local-windows-vs2026-release` | 通过 | 全部目标编译链接成功 |
| 2026-08-10 | `ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | 29/29 |
| 2026-08-10 | `cmake --build --preset local-windows-vs2026-debug --target format-check` | 被既有问题阻断 | 仅报告本次未修改的 `src/camera/src/camera.cpp` 行 81、399～407；未扩大本任务范围修改该文件 |
| 2026-08-10 | `git diff --check` | 通过 | 无空白错误；仅有 Git 的 LF/CRLF 工作区提示 |
| 2026-08-10 | 双相机真机验收 | 未执行 | 需要两台实体 MV-CS020-60GM、目标网络环境和人工场景 |

## 完成摘要

已完成 MVS 限帧 enable 事务、相机已验证参数缓存和启动资源顺序、IPC 控制/查询双通道、CameraClient 长控制超时对账，以及预览优先扇出和轮转公平调度。公开 IPC 线格式、配置 JSON 和 schema 版本均未变化。Debug/Release 构建与非硬件 CTest 全部通过；真机验收未执行。唯一剩余的仓库级验证限制是格式检查命中未修改的既有 `src/camera/src/camera.cpp`。
