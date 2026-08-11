# M3-03：参数读写与回读 ExecPlan

## 元数据

- 状态：completed（2026-08-11 单位语义纠正）
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-11
- 路线图条目：`docs/roadmap/development-roadmap.md` M3-03
- 关联需求：需求 4.4、8、11、16；架构 5.2～5.4、6.1、13、15、16.3

## 目的与可观察结果

已连接 Hikrobot 相机可查询曝光、增益、帧率、ROI、像素格式、触发模式/源/延迟、GigE 包大小、传输延迟及受支持数字 IO 的能力和当前值。参数应用严格执行服务校验、保存旧快照、必要时暂停、逐项写入、完整回读、必要时恢复采集并返回实际值；任一步失败时尝试恢复旧快照，恢复失败则锁定参数操作并返回稳定的明确故障码。

## 范围

### 范围内

- 公共参数模型的连续浮点范围、依赖校验和稳定参数应用/故障错误语义；
- Hikrobot GenICam 节点能力映射、参数读取、确定性写入顺序和回读；
- 取流中参数事务的暂停/恢复、失败回滚和不可恢复故障锁定；
- 伪 MVS API 自动化测试、默认 Mock 回归及真实 SDK 的编译/link 验证。
- 纠正 `inter_packet_delay_ns` 与 `GevSCPD` 的单位边界：公共字段使用真实纳秒，Hikrobot 私有适配器使用设备 timestamp tick。

### 范围外

- M3-04 的帧获取、帧元数据映射、断线恢复与业务状态机集成；
- M3-05 的测试工具、实体相机参数实测、四路吞吐和拔线测试；
- 用户参数集保存、恢复默认值、UI、IPC 或配置 schema 扩展。

## 当前基线

- 公共 `CameraCapabilities`/`CameraParameterSnapshot` 已覆盖目标字段，`validate_parameters` 已做多数范围和依赖校验，但浮点能力只接受正步长，且 `apply_validated_parameters` 只校验后直接调用设备；
- Mock 相机已有内存参数读写且禁止取流中修改，可用于公共回归，但不模拟 SDK 写入/回读/回滚失败；
- M3-02 Hikrobot Provider 能枚举、精确创建并独占连接，参数接口仍返回未实现；MVS API 表只含生命周期函数；
- 本机有 MVS Development/Runtime 4.8.0.3，可编译和运行伪 API及版本 smoke；未提供实体 MV-CS020-60GM；
- 工作区包含父任务已完成但未提交的 M3-01/M3-02 修改，本任务只叠加 M3-03 文件。

## 前置条件与假设

- 使用 SFNC/MVS 节点名：`ExposureTime`、`Gain`、`AcquisitionFrameRate`、`Width/Height/OffsetX/OffsetY`、`PixelFormat`、`TriggerMode/TriggerSource/TriggerDelay`、`GevSCPSPacketSize/GevSCPD`、`LineSelector/LineMode/LineStatus/UserOutputSelector/UserOutputValue`；节点不支持时能力中省略，不虚构支持；
- MVS 浮点能力只给最小/最大值，无离散步长，公共模型以 `increment == 0` 表示连续浮点范围；整数范围仍要求正步长；
- 数字 IO 仅暴露能由节点集合完整证明方向和读写方式的行，未知枚举不映射；`inter_packet_delay_ns` 始终表示真实纳秒，Hikrobot 适配器依据 `GevTimestampTickFrequency` 与 GenICam `GevSCPD` 原生 tick 双向换算；
- 没有实体相机时只证明节点映射与事务语义，不声明真实上下限、支持枚举或实际回读值。

## 设计说明

- 扩展私有 `MvsApi` 注入通用 Float/Integer/Enumeration/Boolean get/set。所有供应商符号和类型继续只在 `src/camera/hikrobot`。
- 参数适配器在已打开句柄的互斥区内运行。能力查询把不支持/访问受限节点映射为“能力不存在”，其他失败翻译为稳定 `CAMERA_PARAMETER_READ_FAILED` 并保留 native code。
- 参数应用前由公共入口查询能力并校验；适配器保存完整旧快照，若句柄正在取流则先停流，按会影响依赖的安全顺序写入，完整回读后恢复取流并返回实际快照。
- 写入、回读或恢复取流失败时，先用旧快照执行恢复并回读确认，再恢复原取流状态；恢复成功返回原始稳定业务错误，恢复失败则将设备参数状态锁定为 faulted，后续参数操作返回 `CAMERA_PARAMETER_FAULTED`，断开重连清除该会话故障。
- 像素格式和触发枚举使用显式白名单；未知厂商枚举不向公共层泄露。数字 IO 选择器修改后恢复原选择器。
- `GevSCPD` 仅在可取得正数 `GevTimestampTickFrequency`、且单 tick 可精确表示为整数纳秒时向公共层暴露；能力范围、当前值和写入值均执行检查溢出的精确换算，失败时返回稳定参数错误而不退回原生 tick。

### 线程和队列

不新增线程或队列。参数事务与 MVS 句柄生命周期共用每设备互斥锁，同步串行；SDK 取流线程只被 stop/start 边界影响。M3-04 的采集线程与有界帧队列不在本任务实现。

### 持久化与恢复

不修改配置 schema、数据库或用户参数集格式。现有 `interPacketDelayNs` 值按既有字段契约解释为纳秒，不做全局乘 8 迁移；仅将 CAM01 为绕过旧缺陷临时写入的 `50` 恢复为 `400`。事务内旧快照只驻留内存；成功后返回回读值。故障锁仅属于当前连接会话，断开销毁句柄后清除。

### 错误和降级

- 校验失败：`CAMERA_CONFIG_FAILED`，包含参数与原因；不调用 SDK；
- 能力/读取失败：`CAMERA_PARAMETER_READ_FAILED` + `hikrobot-mvs` 原始码；
- 写入/回读不一致：`CAMERA_PARAMETER_WRITE_FAILED`，尝试旧快照恢复；
- 暂停或恢复取流失败：稳定流错误；仍执行适用的恢复；
- 旧快照恢复或确认失败：`CAMERA_PARAMETER_FAULTED`，参数接口锁定，不以厂商码作为唯一错误。

## 实施步骤

- [x] 1. 扩展公共错误与浮点连续范围/依赖校验，补充拒绝非有限能力、非法完整快照和服务入口不越过校验的测试。
- [x] 2. 扩展 MVS API 表与内部参数节点助手，实现可选能力映射和完整快照读取，覆盖目标参数及支持的数字 IO。
- [x] 3. 实现确定性参数事务：旧快照、必要暂停、写入、回读、恢复采集、实际值返回；实现失败回滚和 faulted 锁定。
- [x] 4. 将 Hikrobot `ICameraDevice` 参数接口接入内部事务，保持保存用户集/恢复默认和取帧仍为后续里程碑未实现。
- [x] 5. 用伪 SDK 覆盖能力映射、读写顺序、回读量化、暂停恢复、写失败回滚、恢复失败锁定及错误翻译。
- [x] 6. 运行 OFF/ON Debug/Release、CTest、格式、静态分析、SDK 边界、路径泄漏及必要负向检查。
- [x] 7. 更新路线图、计划进度/决策/发现/证据和完成摘要，确认未开始 M3-04。
- [x] 8. 纠正 `GevSCPD` tick 与真实纳秒的能力、读、写和回滚换算，覆盖 125 MHz、1 GHz、非法频率、步长与溢出测试。
- [x] 9. 恢复 CAM01 的 400 ns 配置，更新 IPC/路线图说明并重新执行 Debug/Release、CTest、格式和静态分析门禁。

## 验证计划

### 自动化测试

- 公共层：连续浮点范围、整数步长、ROI 边界、触发依赖、数字 IO 支持/唯一性、校验失败不写设备；
- 伪 SDK：完整与部分能力、未知枚举、不支持节点、完整读取、量化回读；
- 事务：非取流顺序、取流暂停/恢复、写入失败恢复、回读失败恢复、恢复取流失败、旧快照恢复失败进入 faulted；
- OFF/ON 全部非硬件回归、真实 SDK 版本/link smoke、SDK 边界扫描。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release

cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=ON -DPAPERBREAK_MVS_ROOT="$env:MVCAM_COMMON_RUNENV" -DPAPERBREAK_MVS_RUNTIME_DIR="<本机 MVS x64 Runtime>"
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

另运行 OFF/ON Release、`format-check`、适用静态分析、`hikrobot_sdk_boundary`、安装路径泄漏扫描和 `git diff --check`。

### 人工或硬件验证

- 环境：MVS 4.8.0.3、驱动、目标 MV-CS020-60GM、目标网卡；当前没有实体相机。
- 步骤：连接后记录能力和旧值；分别在停止/取流状态应用各参数与组合；核对回读、帧几何/格式及失败恢复；验证数字 IO 实际电平。
- 预期：真机上下限/枚举与 MVS 一致，写入返回实际回读值，采集恢复，失败不留下半配置。
- 证据保存位置：当前未执行；具备硬件后保存脱敏参数快照和目标机日志，不能把伪 SDK 结果写成真机通过。

## 回滚与恢复

本任务只修改参数领域、Hikrobot 私有适配器、测试和文档。失败时撤销 M3-03 增量即可保留 M3-02 的发现/绑定基线；不删除配置、SDK 或用户数据。运行期事务失败时优先恢复旧快照，无法恢复则锁定当前连接的参数操作并要求断开重连。

## 验收标准

- [x] 全部目标参数均有受支持能力映射、读取、写入和回读路径；
- [x] 外部参数先做类型、范围、枚举、ROI 与触发/IO 依赖校验，失败不触达 SDK；
- [x] 事务顺序严格为校验、必要暂停、写入、回读、恢复采集、返回实际值；
- [x] 写入/回读/恢复失败可恢复旧快照，恢复失败进入明确 faulted 状态；
- [x] 厂商错误转换为稳定业务错误并保留原始诊断；SDK 调用仍只在 Hikrobot 适配器；
- [x] 伪 SDK 测试覆盖能力、事务和故障路径，OFF/ON Debug/Release 与静态检查通过；
- [x] 未开始 M3-04，未声称实体相机上下限、回读或 IO 测试。

## 进度记录

- 2026-08-01：完成需求、架构、路线图、计划规范、M3-01/M3-02 计划、公共相机/Mock/Hikrobot/CMake/测试基线检查；创建计划，状态 in-progress。
- 2026-08-01：完成公共连续浮点范围和参数错误语义、MVS 节点能力/读写映射、事务暂停回读恢复、失败回滚/faulted 锁定及 7 项参数专用伪 SDK 测试。
- 2026-08-01：完成 OFF/ON Debug/Release、非硬件 CTest、ON/OFF 静态分析、格式、SDK 边界、路径泄漏、缺 SDK 负向配置和 diff 检查；路线图与错误码文档已更新，状态 completed。
- 2026-08-11：确认 UI/配置的 `400 ns` 被旧实现原样写成 `GevSCPD=400`；开始按真实纳秒契约纠正，工作区原有 `config/default-config.json` 包大小、ROI、修订和时间戳修改均保留。
- 2026-08-11：完成动态频率换算、能力/步长/溢出校验、读写回滚测试、CAM01 配置恢复和协议/路线图更新；Debug/Release 构建及最终非硬件 CTest 通过，状态 completed。

## 决策记录

- DEC-001：浮点 `increment == 0` 表示 SDK 报告的连续范围，整数范围仍禁止零步长，避免伪造 MVS 未提供的曝光/增益步长。
- DEC-002：事务和 faulted 锁位于 Hikrobot 每设备连接会话；公共层负责服务校验，适配器负责唯一能安全观察的 SDK 取流状态与原子恢复。
- DEC-003：不为完成参数暂停而提前实现 M3-04 取帧；仅复用已有句柄 `streaming` 状态并用伪 API 验证事务边界。
- DEC-004：不硬编码目标型号的 8 ns/tick；适配器读取 `GevTimestampTickFrequency`。无法精确映射到整数纳秒时拒绝暴露/写入该能力，避免再次伪造单位。
- DEC-005：`interPacketDelayNs` 的字段名、类型和 schema 不变；这是实现纠错而非协议迁移。当前 CAM01 临时值 `50` 恢复为 `400`，其他持久化值不做自动换算。

## 意外发现

- MVS `MVCC_FLOATVALUE` 只有当前值、最小值、最大值，没有步长字段；现有公共模型必须明确连续范围语义。
- 公共字段 `inter_packet_delay_ns` 与 MVS `GevSCPD` 的原生 tick 单位不能等同；旧实现虽在计划中记录该限制，仍把原生 tick 直接暴露为 ns，构成单位错误，必须在适配器边界纠正。
- 初次执行 `format-check` 时 `clang-format` 不在 PATH；定位到 Visual Studio 自带 x64 工具并显式加入 PATH 后检查通过，属于本机工具发现问题而非代码失败。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | `git status --short` | 通过 | 基线仅含父任务未提交的 M3-01/M3-02 文件，本任务不覆盖无关修改 |
| 2026-08-01 | 实体相机参数与数字 IO | 未执行 | 未提供目标相机，真实能力、单位、上下限与回读待验证 |
| 2026-08-01 | OFF：本机 Debug/Release configure、build、CTest | 通过 | 两套非硬件 CTest 均 18/18；通用测试可执行文件 146 项，不查找/链接 MVS |
| 2026-08-01 | ON：本机 MVS 4.8.0.3 Debug/Release configure、build、CTest | 通过 | 两套非硬件 CTest 均 19/19；Hikrobot 可执行文件 21 项，包含 7 项参数/故障专用测试和真实 SDK 版本/link smoke |
| 2026-08-01 | 参数伪 SDK：连续能力、完整快照、量化回读、暂停/恢复、写失败、回滚失败、恢复采集失败、数字输出、错误翻译 | 通过 | 仅证明封装与事务语义，不代表真机节点或电平 |
| 2026-08-01 | ON/OFF `local-windows-vs2026-static-analysis` configure/build | 通过 | MSVC `/analyze`，生产目标无构建失败 |
| 2026-08-01 | `format-check`、`hikrobot_sdk_boundary`、安装路径泄漏、`git diff --check` | 通过 | MVS 符号仍只在 Hikrobot 适配器；ON 安装树无注入路径文本 |
| 2026-08-01 | 缺失 Development/Runtime 路径配置 | 预期失败 | 退出码 1，明确报告 `PAPERBREAK_MVS_ROOT` 目录不存在 |
| 2026-08-11 | `PaperBreakHikrobotTests` Debug 定向运行 | 通过 | 49/49；新增 7 项覆盖 125 MHz 的 8 ns/tick、400 ns→50 tick、400 tick→3200 ns、非对齐拒绝、1 GHz、频率缺失/非法、溢出和回滚 |
| 2026-08-11 | Debug/Release configure、build、非硬件 CTest | 通过 | 两套最终均为 30/30；Release 首轮 `unit` 聚合入口波动失败，隔离重跑及随后全量重跑通过 |
| 2026-08-11 | `PaperBreakEdgeService --validate-config --config config/default-config.json` | 通过 | schema v3、revision 60；CAM01/CAM02 均为 400 ns |
| 2026-08-11 | 任务 C++ 文件 clang-format、`git diff --check` | 通过 | 全仓 `format-check` 继续被未修改的 `src/console/main.cpp` 既有格式问题阻断 |
| 2026-08-11 | `paperbreak_camera_hikrobot` 静态分析目标 | 通过 | 全仓静态分析继续被未修改的 `src/storage/src/nvme_cache.cpp` C28020 阻断 |
| 2026-08-11 | 实体 MV-CS020-60GM 的频率、寄存器和帧率验证 | 未执行 | 未操作实体相机，不能声明真机 `GevTimestampTickFrequency`、`GevSCPD` 回读或 60.3 fps 已通过 |

## 完成摘要

新增参数专用稳定业务错误和连续浮点能力语义；扩展 Hikrobot 私有 MVS API 表，映射全部 M3-03 参数能力、当前快照和写入路径。每设备互斥事务先校验并保存旧快照，必要时暂停取流，确定性写入、完整回读、恢复取流并返回实际值；失败时恢复并确认旧快照，无法恢复则锁定当前连接会话为 `CAMERA_PARAMETER_FAULTED`。新增伪 SDK 覆盖实际回读、暂停恢复、回滚/faulted、数字输出和错误翻译；OFF/ON Debug/Release、非硬件 CTest、静态分析及边界检查均通过。未访问实体相机，真实能力、上下限、传输延迟单位、写后量化和数字 IO 电平仍未验证；未开始 M3-04。

2026-08-11 纠正：`inter_packet_delay_ns`/`interPacketDelayNs` 现严格表示真实纳秒。适配器读取 `GevTimestampTickFrequency`，精确换算 `GevSCPD` 能力、当前值、写入值和回滚值；125 MHz 下 400 ns 写为 50 tick，原生 400 tick 回读为 3200 ns。缺失/非法频率、非设备步长或溢出均稳定拒绝，不再退回原始 tick。CAM01 临时 50 已恢复为 400，其他用户配置修改保留。自动化和构建门禁结果见上表；实体相机未执行。
