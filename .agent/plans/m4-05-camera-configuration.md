# M4-05：相机配置与实际值回显 ExecPlan

## 元数据
- 状态：completed（2026-08-03 GigE Runtime 部署缺陷修复）
- 负责人：Codex
- 创建日期：2026-08-03
- 最后更新：2026-08-03
- 路线图条目：M4-05
- 关联需求：4.1～4.8 相机控制与采集、5 Qt 客户端、6 IPC

## 目的与可观察结果

控制台“相机配置”页通过本机 IPC 列出最多四个逻辑相机，展示配置、连接/采集状态、设备描述与服务回读的实际参数。用户可在确认危险操作后执行发现、连接、断开、开始、停止、快照和软件触发，并提交受能力约束的参数更新；结果清楚标示已保存、已下发、已应用、失败或需重启。

2026-08-03 整改补充：修复 `camera.discover` 服务/客户端字段不一致、空配置吞掉发现结果和常用构建未启用生产适配器的问题；新增从真实发现清单绑定空闲逻辑槽位，并为控制端提供跟随系统、浅色和暗黑三种可持久化主题，确保所有输入控件和状态文本具备明确前景/背景色。

2026-08-03 GigE Runtime 补充：修复安装树只部署 `MvCameraControl.dll`、遗漏其在 GigE 枚举时动态加载的 `MVGigEVisionSDK.dll`，导致安装版服务返回 `MV_E_LOAD_LIBRARY (0x8000000C)` 的缺陷。部署范围保持为目标 GigE 相机所需的最小组件，不引入 USB、采集卡或 GUI Runtime。

## 范围

### 范围内
- 在相机领域接口之上增加服务端受限控制协调器及十一个 M4-05 IPC 命令。
- 使用已存在的 `ICameraProvider`/`ICameraDevice`，使 Mock 相机可作自动化端到端验证；不让 MVS SDK 泄漏至服务或 UI。
- 增加客户端相机状态模型和 Qt 配置页，并将总览正常相机数接入实际快照。
- 更新 IPC 文档、路线图和测试。
- 增加 `camera.bind`、发现后自动展示/绑定、Hikrobot 显式构建预设以及应用级主题控制器。
- 修复 Hikrobot 构建输出和安装树的最小 GigE Runtime 部署，增加真实 SDK 只读枚举 smoke 与安装文件门禁。

### 范围外
- MVS SDK 的实体设备验证（M3 硬件验证范围）；无 SDK 或硬件时不得模拟成真机。
- M4-06 的报警、日志、诊断导出；M5 的持久化事件快照与 M6 算法配置。

## 当前基线

- `camera` 已有独立 `ICameraProvider`、`ICameraDevice`、能力查询、参数校验与 Mock 实现；本任务已在其上装配相机控制协调器。
- 服务 IPC 已有有界请求/响应队列；控制台状态模型已有独立状态连接，预览有独立连接。
- 默认配置没有已启用相机；工作区已有未跟踪的 `config/data/`，不属于本任务且不修改。

## 前置条件与假设

- 启用 Hikrobot 构建时，生产服务在适配器边界创建并注入 MVS 提供者；服务核心与控制台仍只依赖厂商无关接口。
- 所有写操作均要求现有 IPC 本机管理员身份，且请求停止时拒绝。
- `updateConfig` 以当前配置修订号作为并发前提，设备回读是实际值的唯一来源。

## 设计说明

服务核心新增一个可注入的相机控制门面。门面在自身互斥锁保护的有限（最多四项）会话表中保存设备对象；所有操作同步执行在 IPC 命令工作线程，不创建无界队列。设备的连接、流控制及参数读写均返回稳定业务错误，保留领域层的来源信息。`captureSnapshot` 仅返回元数据，不进行磁盘写入、JPEG 编码或事件落盘；实际持久化快照属于 M5。

IPC 响应分别返回保存配置、下发请求、设备实际回读与重启要求。控制台模型每种查询最多一个在途请求，以连接代次屏蔽旧响应；写操作亦单一在途。配置页只渲染模型快照，不直接访问相机。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| IPC 命令 | Qt 客户端 | IPC 服务命令线程 | 既有 64 | 满时返回 `IPC_BUSY` | 服务停止时拒绝写操作 | IPC 既有指标 |
| 相机控制会话表 | IPC 命令线程 | IPC 命令线程 | 最多 4 | 超限拒绝 | 服务析构时有序停止/断开 | 每相机状态/最后错误 |
| 客户端相机快照 | Qt 回调 | Qt UI | 每相机 1，最多 4 | 新快照覆盖旧快照 | 停止时取消请求 | 连接代次、过期标记 |

### 持久化与恢复

参数配置经既有 `ConfigRepository` 的乐观修订、审计和原子写入保存。设备参数仅在连接后下发；保存成功但设备未连接或不支持热应用时明确标为“需重启/待应用”。本任务不写入图像或事件文件。

### 错误和降级

- 不存在、未连接或状态不允许的相机返回已有稳定 `CAMERA_*` 错误；MVS 原始码仅可由适配器作为详情保留。
- 参数或 DTO 越界返回 `IPC_REQUEST_INVALID` 或 `CAMERA_CONFIG_FAILED`，不改变已保存配置。
- 无装配设备提供者时，查询显示未装配，危险控制操作返回 `SYS_NOT_SUPPORTED`，不伪造设备实际值。

## 实施步骤

- [x] 1. 新增相机控制协调器，采用已存在领域接口实现枚举、会话生命周期、能力/实际参数回读及安全状态限制；添加 Mock 单元测试。
- [x] 2. 扩展服务命令分发、配置写入和 IPC 文档，严格校验全部 M4-05 DTO，并覆盖权限、停止、冲突、失败和实际值回读。
- [x] 3. 增加控制台相机状态/命令模型及相机配置页，完成操作确认、状态标签和总览相机计数；增加离屏 UI 与模型测试。
- [x] 4. 更新路线图/本计划，运行 Debug、Release、CTest、格式检查与差异检查，回填证据。

## 验证计划

### 自动化测试
- Mock IPC 验证 list/discover/connect/disconnect/start/stop/getConfig/updateConfig/captureSnapshot/softwareTrigger，及无效 DTO、未授权、停止和过期响应。
- 验证保存值、设备实际值、失败和需重启状态不混淆。
- 离屏 Qt smoke 验证相机页面、确认动作和断线过期提示。

### 构建与测试命令
```powershell
cmake --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug
cmake --preset windows-vs2026-release
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-release
```

### 人工或硬件验证
- 已执行只读 `--probe` 和启用 Hikrobot 服务的临时配置 IPC 发现；真实绑定需要提升后的管理员身份，本次令牌未提升，因此按权限模型被拒绝。未写生产配置、未写相机参数、未取流。

## 回滚与恢复

本任务只通过既有配置仓储写入配置；若回滚代码，使用仓储保留历史恢复上一个有效修订。不会删除用户事件或图像数据。

## 验收标准

- [x] 十一个 IPC 命令与权限/DTO/停止语义已测试。
- [x] UI 通过 IPC 显示保存、下发和实际回读值，危险操作有二次确认。
- [x] Mock 自动化覆盖实际值回读与失败隔离。
- [x] Debug/Release 构建与相关 CTest 已实际执行并记录限制。
- [x] Hikrobot 构建输出与安装树同时包含 4.8.0.3 的 `MvCameraControl.dll` 和 `MVGigEVisionSDK.dll`，安装目录只读枚举与实际 IPC 发现成功。

## 进度记录

- 2026-08-03：创建计划，状态 in-progress；已确认没有服务端相机控制协调器，且默认配置不启用相机。
- 2026-08-03：完成控制门面、十个 IPC 命令、原子配置保存、生产适配器接线、控制台模型与配置页；状态 completed。
- 2026-08-03：用户真机复核发现 M4-05 整改缺陷；只读 probe 枚举到 `MV-CS020-60GM`/`DB1888674`/`192.168.11.115`，主机接口 `192.168.11.102`，但独占访问不可用。确认普通 Debug/Release 缓存为 Mock-only，客户端错误要求 `transportId` 而服务返回 `networkInterface`，且空配置 UI 在渲染发现清单和操作错误前提前返回。计划状态重新置为 in-progress；不修改用户已有日志，不结束未知占用进程。
- 2026-08-03：完成发现字段、空配置 UI、`camera.bind`、拓扑待重启、显式 Hikrobot 预设和三模式主题整改。最新只读 probe 显示设备已恢复独占可用；启用 Hikrobot 的临时服务 IPC 返回完整发现字段，空配置 list 返回修订与拓扑状态。当前令牌未提升，真机绑定按设计返回 `IPC_UNAUTHORIZED`，临时与生产配置均未改变。计划状态置为 completed。
- 2026-08-03：安装版再次复现 `camera.discover` 失败；安装树中的硬件 probe 返回 `MV_E_LOAD_LIBRARY (0x8000000C)`，而使用供应商 Runtime 的构建树 probe 成功。隔离验证确认补齐 `MVGigEVisionSDK.dll` 后，安装版 probe 和服务 IPC 均恢复发现。计划状态重新置为 in-progress，仅修复 GigE Runtime 部署和相应门禁。
- 2026-08-03：完成两项必需 Runtime 的存在性/版本配置门禁、服务/适配器测试/硬件工具构建输出部署、安装清单门禁和真实 SDK GigE 枚举 smoke。Hikrobot 与 Mock 的 Debug/Release 全量构建和 CTest 均通过；从安装目录运行的只读 probe 与实际 IPC `camera.discover` 均成功，计划状态置为 completed。

## 决策记录

- DEC-001：控制门面只依赖 `camera` 领域接口；MVS SDK 调用继续限定在 Hikrobot 适配器模块。
- DEC-002：本任务的 `captureSnapshot` 只返回内存帧元数据，图像持久化留给 M5，以避免在控制路径加入不受控 I/O。
- DEC-003：只部署 4.8.0.3 的 `MvCameraControl.dll` 与目标 GigE 相机所需的 `MVGigEVisionSDK.dll`；不复制约 128 MB 的完整 Runtime，避免扩大未使用组件、安装体积和许可证清单。

## 意外发现

- 安装测试会先清空 `test-install`；原安装脚本的运行时依赖搜索目录缺少 OpenCV `bin`，因此即使旧安装目录曾有 DLL，重新安装仍会在复制前解析失败。已将 `opencv_core` 的目标目录加入搜索路径。
- 显式 Hikrobot 预设首次暴露环境变量反斜杠进入安装脚本，以及运行时依赖解析未搜索 MVS x64 Runtime。现已在适配器边界规范路径并将 Runtime 加入部署依赖搜索；路径泄漏扫描显式包含 MVS 根目录和 Runtime，跳过与 PDB/ILK 同类的 MSVC Debug `.lib` 开发产物。
- CMake 的静态运行时依赖解析能复制 `MvCameraControl.dll` 的导入依赖，但看不到 SDK 在 GigE 枚举阶段通过 `LoadLibrary` 加载的 `MVGigEVisionSDK.dll`；仅执行 `--version` 或 `MV_CC_GetSDKVersion()` 无法覆盖该故障，必须增加真实 SDK 枚举 smoke。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-03 | 基线检查 | 通过 | 发现未跟踪 `config/data/`，未修改 |
| 2026-08-03 | `cmake --preset windows-vs2026-debug` | 未通过 | 环境变量未提供 OpenCV 4.12.0 的 `OpenCVConfig.cmake` |
| 2026-08-03 | `cmake --preset local-windows-vs2026-debug`; `cmake --build --preset local-windows-vs2026-debug --target paperbreak_camera` | 通过 | 确认本机本地预设可构建现有相机模块；未执行全量测试 |
| 2026-08-03 | Debug/Release `cmake --build --preset local-windows-vs2026-*` | 通过 | 两种配置全量构建成功 |
| 2026-08-03 | Debug/Release `ctest --preset local-windows-vs2026-*` | 通过 | 完整非硬件 CTest 两种配置均 18/18；通用 unit 入口 164 项 |
| 2026-08-03 | Mock Debug/Release 全量构建与 CTest | 通过 | `local-windows-vs2026-debug/release` 均全量构建成功；非硬件 CTest 均 18/18，通用 unit 入口 167 项 |
| 2026-08-03 | Hikrobot Debug/Release 全量构建与 CTest | 通过 | 新增 `windows-vs2026-hikrobot-debug/release` 预设；两种配置构建成功，非硬件 CTest 均 22/22，适配器 27 项，SDK 边界与安装运行树通过 |
| 2026-08-03 | Qt 离屏 smoke | 通过 | 空配置仍显示发现设备；拓扑待重启禁用设备操作；系统/浅色/暗黑切换、非法设置回退、持久化及成对色值 4.5:1 对比检查通过 |
| 2026-08-03 | 任务文件 clang-format dry-run；`git diff --check` | 通过 | 项目全局 `format-check` 仍命中无关既有 `preview_client.cpp` 格式差异 |
| 2026-08-03 | `PaperBreakCameraHardwareTest --probe` | 部分通过 | 只读发现目标设备与 IP/网卡，最新状态可独占；硬件门禁仍为 incomplete，未执行取流或故障场景 |
| 2026-08-03 | 启用 Hikrobot 的临时服务 IPC | 部分通过 | `camera.discover`/空配置 `camera.list` 返回准确结构；当前令牌非提升管理员，`camera.bind` 返回 `IPC_UNAUTHORIZED`，临时配置修订保持 1 |
| 2026-08-03 | Hikrobot Debug/Release 全量构建与 CTest（GigE Runtime 修复后） | 通过 | 两种配置均全量构建成功且 CTest 23/23；适配器 28 项，新增真实 SDK `MV_GIGE_DEVICE` 只读枚举 smoke 通过；安装树强制包含两个 DLL |
| 2026-08-03 | Mock Debug/Release 全量构建与 CTest（GigE Runtime 修复后） | 通过 | 两种配置均全量构建成功且 CTest 19/19；未读取或部署供应商 Runtime，缺失 GigE DLL 的配置诊断测试通过 |
| 2026-08-03 | 安装目录 `PaperBreakCameraHardwareTest --probe` | 通过 | 两个 DLL 文件版本均为 4.8.0.3；只读发现 `MV-CS020-60GM`/`DB1888674`、相机 IP `192.168.11.115`、主机网卡 `192.168.11.102`，`exclusiveAccessAvailable=true`，不再出现 `0x8000000C` |
| 2026-08-03 | 安装目录服务实际 IPC `camera.discover` | 通过 | 从子进程 `PATH` 排除 MVS 目录后启动安装树服务，响应成功并返回型号、序列号、IP、网卡和独占可用状态；未执行绑定、打开、参数写入或取流 |
| 2026-08-03 | 变更文件 clang-format dry-run；SDK 边界/安装路径泄漏扫描 | 通过 | 变更 C++ 文件格式通过；SDK 边界与两项 Runtime 安装清单/路径泄漏扫描均通过。全局 `format-check` 仍仅命中无关既有 `src/console/src/preview_client.cpp` 格式差异 |

## 完成摘要

已完成厂商无关的相机控制会话、十一个服务 IPC 命令、配置原子保存与设备回读、控制台模型、绑定页面和应用级主题。启用 Hikrobot 的生产构建会注入现有 MVS 提供者，并在服务、测试、硬件工具和安装树中就地部署版本锁定为 4.8.0.3 的核心控制与 GigE 传输 DLL；未启用 SDK 时不读取或部署 MVS 文件。Mock/Hikrobot Debug/Release、Qt 离屏、SDK 边界、运行时安装树、安装目录只读 probe 和实际 IPC 发现均已验证；硬件门禁仍为 incomplete，未执行或宣称绑定、相机打开、参数写入、取流、断链及其他故障场景通过。
