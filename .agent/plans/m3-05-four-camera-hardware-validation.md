# M3-05：四路硬件测试工具与记录 ExecPlan

## 元数据

- 状态：hardware-gate-incomplete
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M3-05
- 关联需求：需求 4.3～4.6、4.8、4.20～4.22、8、13、16；架构 5.2～5.4、6.1、7、9.1、10、13～16.3

## 目的与可观察结果

提供 Windows/MSVC 可构建的 `PaperBreakCameraHardwareTest`，使用厂商无关相机接口执行只读清点，以及经显式计划文件授权的绑定、参数回读、软触发、单路逐步扩展到四路的限时采集与资源采样；每次运行写出带 schema 版本、UTC 时间、环境、步骤状态、阈值和原始指标的 JSON 记录。提供不擅自执行物理动作的操作手册与记录模板，明确相机网线、交换机链路和服务重启的人工边界。无硬件时以 Mock/伪 SDK 验证工具逻辑和适配层，不把 M3-05 或 M3 标成完成。

## 范围

### 范围内

- 目标型号发现、序列号绑定、占用状态的只读探测和审计输出；
- Hikrobot `TriggerSoftware` 命令的适配器内实现、业务错误翻译与伪 SDK 测试；
- 参数计划的写入、完整回读和触发模式核对；
- 1～4 路限时采集、固定帧池/有界队列、持续消费和确定性关闭；
- 帧率、帧号间隙、不完整帧、超时、应用 CPU/内存与 Windows 网卡收发带宽采样；
- 下游慢消费/日志压力不阻塞采集的自动化隔离测试，以及真实预览尚未实现的限制记录；
- 真机操作说明、空白记录模板、门禁判定和本机硬件清点记录。

### 范围外

- M4 UI/预览功能、JPEG 编码、推理、事件落盘和上位机通信；
- 自动禁用网卡、拔线、重启交换机或安装/启动 Windows 服务；
- 修改生产配置 schema、服务装配或相机模块边界；
- M9 长期性能/故障注入和 168 小时稳定性测试。

## 当前基线

- M3-01～M3-04 已实现 SDK 隔离、发现/绑定、参数事务、固定池取流与恢复；公共采集队列固定容量且满载丢最旧，采集/恢复线程均可停止并 join。
- `HikrobotCameraDevice::software_trigger()` 仍返回 `not-implemented-in-m3-04`；MVS API 表尚未注入 `MV_CC_SetCommandValue`。
- `tests/hardware/hardware_baseline_tests.cpp` 只有 M0 跳过项，没有真机工具、记录 schema 或场景说明。
- 本机安装 MVS Development/Runtime 4.8.0.3；物理 Realtek 2.5GbE 当前协商为 1Gbps，另有 Siemens PLCSIM 虚拟网卡。PnP 清点未发现相机，ARP 不能证明设备类型或当前在线。
- 工作区已有父任务 M3-02～M3-04 尚未提交的修改；本任务只叠加 M3-05 所需文件和同文件小范围改动，不覆盖既有改动。

## 前置条件与假设

- 真机通过需至少一台 `MV-CS020-60GM`；四路吞吐/恢复通过需四台目标相机、目标交换机和生产等价网卡拓扑。
- 只读 `--probe` 可自动执行；参数/取流/软触发必须由操作者提供显式计划。拔插网线、交换机断链和服务重启必须人工执行并在记录中签名，工具不操控外部设备。
- 单次采样数、运行时长、帧池、队列和日志压力均设置上限；拒绝导致无界内存或无限运行的计划。
- 当前 M4 预览尚未实现，因此 M3-05 只能复用自动化证明有界慢消费者与采集隔离；日志队列的独立有界压力已有 M1 自动化，但不能声称真实 Qt/JPEG 预览与生产日志并发已验证。

## 设计说明

- 真机可执行文件只依赖公开 `ICameraProvider`/`ICameraDevice` 和 Windows 指标采样；唯一生产 MVS 调用仍在 `paperbreak_camera_hikrobot`。
- JSON 计划固定 schema version 1，最多四个唯一 `CAM01`～`CAM04`/序列号；运行记录每个场景使用 `passed`、`failed`、`not-executed`，硬件门禁由证据而非进程成功码推导。
- `--probe` 只枚举并记录，不打开设备。`--run` 按阶段创建 1～4 个设备，先连接/写回读，再启动每相机采集线程和有界消费线程；达到固定截止时间后按消费者、采集、停流、断开顺序关闭。
- 软件触发只在实际回读 `TriggerMode=On` 且 `TriggerSource=Software` 后调用 `MV_CC_SetCommandValue("TriggerSoftware")`；失败返回稳定业务码并保留 MVS 原始码。
- Windows 资源采样由工具线程按固定间隔读取当前进程 CPU/工作集和网卡累计字节，记录差分带宽；采样向量由运行时长/间隔预先计算并限制上限。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| 每路采集队列 | `AcquisitionWorker` | 工具消费线程 | 计划值，1～256，默认 16 | 丢最旧，采集不阻塞 | 先请求消费者/采集停止，关闭后限时 join | 深度/高水位/丢最旧/关闭拒绝 |
| 帧池 | 每路采集线程 | 帧包 RAII | 计划值，2～256，默认 24 | 无空闲缓冲时计入采集错误，不扩容 | 队列/消费者释放后析构 | 容量/可用/池耗尽 |
| 资源样本 | 运行协调线程 | 运行结束 JSON 序列化 | `ceil(duration/interval)+2`，硬上限 3601 | 超计划拒绝启动 | 固定截止时间后停止采样 | 样本数/CPU/工作集/网卡 Bps |

### 持久化与恢复

- 计划和结果 JSON 使用 `schemaVersion=1`；输出先写同目录临时文件，再原子替换，避免中断留下伪完整记录。
- 工具不保存相机用户集、不恢复默认值、不改生产配置。失败时尽力停采、断开，原始记录保留失败步骤和业务/厂商诊断。
- 人工场景使用提交的 Markdown/JSON 模板追加，不由工具伪造通过。

### 错误和降级

- 复用 `CAMERA_*` 稳定业务码；软触发错误使用参数/状态类业务码并保留 `native_domain=hikrobot-mvs`、原始十六进制码。
- 目标型号/绑定不满足、计划非法、记录无法原子写入均非零退出；不静默选择意外设备。
- 单路失败停止当前阶段，关闭所有已启动设备；其他物理场景保留 `not-executed`。
- 达不到阈值只记录 failed，不通过增大队列掩盖丢帧。

## 实施步骤

- [x] 1. 新增 M3-05 计划 schema、记录模型和 Windows/MSVC 真机测试可执行文件；完成只读探测、计划校验、原子 JSON 输出和无可信清单退出语义，以 CLI 负向测试和既有库存自动化覆盖边界。
- [x] 2. 在 Hikrobot 私有 API 表实现软触发命令，验证触发模式/源后调用，补充伪 SDK 的成功、错误模式和厂商失败翻译测试。
- [x] 3. 实现 1～4 路限时采集、参数写回读、软触发、固定消费/慢消费者模式及帧率/丢帧/超时/队列指标；复用并通过已有 Mock 四路、队列溢出与关闭自动化。
- [x] 4. 实现有界 Windows CPU/内存/网卡带宽采样，计划解析拒绝超时长、超采样和超内存预算；确认回调/取帧路径无磁盘、网络、JPEG 或推理。
- [x] 5. 新增真机操作说明、计划样例、记录模板和本机清点记录；把物理断链、交换机中断、服务重启、真实预览列为需人工/后续实现的未执行项。
- [x] 6. 执行 OFF/ON Debug/Release、CTest、格式、静态分析、SDK 边界、安装路径、缺 SDK 负向配置、工具负向/只读探测和 `git diff --check`，回写真实证据与路线图门禁状态。

## 验证计划

### 自动化测试

- Mock：非法/重复槽位、错误目标型号、缺失/占用、1～4 路阶段、参数回读差异、软件触发、慢消费导致有界丢弃但不阻塞采集、停止时线程/队列关闭。
- 伪 MVS：`TriggerSoftware` 成功、非软件触发拒绝、命令失败业务/native 诊断。
- 记录：schema/version、UTC、环境、步骤状态、帧/资源/网卡/队列指标齐全，运行中断/失败不能记录为 passed。
- 负向：无绑定、超过四路、无界时长/容量、输出不可写、缺 SDK 配置明确失败。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=OFF
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release -DPAPERBREAK_ENABLE_HIKROBOT=OFF
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release

cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=ON
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --preset local-windows-vs2026-release -DPAPERBREAK_ENABLE_HIKROBOT=ON
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
```

另运行 `format-check`、ON/OFF 静态分析、`hikrobot_sdk_boundary`、安装树路径泄漏、缺 SDK 预期配置失败、工具无硬件/非法计划负向检查及 `git diff --check`。

### 人工或硬件验证

- 环境：Windows 10/11 x64、MVS 4.8.0.3、1～4 台 MV-CS020-60GM、目标网卡/交换机，关闭其他占用相机的程序。
- 步骤：只读清点；按序列号绑定；连续模式逐路扩展；参数回读；软件/实际硬件触发；人工拔插单相机网线、断开/恢复交换机上联、重启服务；预览/日志压力期间观察采集指标。
- 预期：只绑定白名单目标机；实际值可审计；各阶段达到批准阈值；故障只影响约定范围并恢复；CPU/内存/网卡/帧/队列指标完整。
- 证据保存位置：`docs/validation/m3-05/records/`（只提交空白模板/说明；实际记录由目标机操作者归档）。
- 当前状态：未执行。没有证据证明本机连接了任何目标相机，也没有四路目标硬件/测试交换机；不会擅自执行物理断链或服务控制。

## 回滚与恢复

撤销 M3-05 新增工具、测试、文档和软触发增量即可回到 M3-04 可构建状态；不删除 SDK、配置或用户数据。工具失败会确定性停止全部线程、停流并断开；原子记录临时文件可人工保留排障，不覆盖既有正式记录。

## 验收标准

- [x] 工具能只读发现并按目标型号/序列号生成可审计绑定记录；
- [x] 工具能经显式计划执行参数写回读、软触发和 1～4 路限时采集；
- [x] 记录包含帧率、丢帧、超时、CPU、内存、网卡带宽和队列指标；
- [x] 所有资源有上限且所有线程确定性关闭，MVS 调用仍只在适配器；
- [x] 自动化 Mock/伪 SDK、OFF/ON Debug/Release 与质量门禁通过；
- [ ] 至少一台目标相机完成功能验证；
- [ ] 四路目标吞吐、恢复与预览/日志隔离在生产等价环境留有通过证据；
- [x] 硬件证据不完整时路线图保持 M3-05/M3“硬件门禁未完成”。

## 进度记录

- 2026-08-01：完整阅读项目规则、需求、架构、路线图、计划规范、M3-01～M3-04 ExecPlan、相关相机/平台/测试/CMake；创建计划，状态 in-progress（硬件门禁未完成）。
- 2026-08-01：只读清点本机 MVS、PnP、网卡/IP/邻居；确认 MVS 4.8.0.3 可用，物理网卡已连接，但没有可审计证据证明存在目标相机或四路测试网络。
- 2026-08-01：完成真机工具、参数实际回读、软触发、1→4 路有界采集、资源指标、不可覆盖 JSON、操作说明和模板；真实只读 probe 因完整序列号重复被库存校验拒绝，无法确认目标型号/实际数量。
- 2026-08-01：OFF Debug/Release 18/18、ON Debug/Release 22/22、格式、ON/OFF 静态分析、SDK 边界、安装泄漏、缺 SDK 与非法计划负向检查通过；状态更新为 hardware-gate-incomplete。

## 决策记录

- DEC-001：真机工具通过公共相机接口工作，禁止为了测试方便在工具内直接调用 MVS。
- DEC-002：物理断链和服务控制只提供计时/记录窗口，不由工具执行，避免未经授权操控外部设备或 SCM。
- DEC-003：M4 预览尚不存在；本任务验证有界慢消费者/日志压力隔离并明确保留真实预览门禁，不虚构通过。
- DEC-004：硬件证据缺失时，即使工具和自动化全部通过，M3-05 与 M3 仍为“硬件门禁未完成”。

## 意外发现

- 本机 `MVCAM_COMMON_RUNENV` 指向 Development 根，Runtime 位于 `C:\Program Files (x86)\Common Files\MVS\Runtime\Win64_x64`；二者需要分别注入。
- `192.168.11.222` 属于 Siemens PLCSIM 虚拟网卡；同子网 ARP 邻居不能作为 Hikrobot 相机发现或型号证据。
- 首次工具编译使用 `MIB_IF_TABLE2/GetIfTable2` 时受 Windows SDK 声明条件影响；改为 Windows 10/MSVC 兼容的 `GetIfTable`，分配前先读取大小并限制为 1 MiB。其 32 位累计计数适合当前最多 2 秒的采样间隔，但记录仍标明是活动非环回网卡汇总而非单相机精确流量。
- probe 的重复判断使用完整、边界校验后的序列号字符串；错误详情调用脱敏函数只保留后四位 `8674`。它证明 MVS 至少返回两个相同序列号的描述项，不证明实体设备数；可能的多接口重复仅为待网络负责人核实的推测。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | MVS Development/Runtime 布局检查 | 通过 | 已安装批准的 4.8.0.3；只证明 SDK 可构建 |
| 2026-08-01 | `Get-NetAdapter`/`Get-NetIPAddress`/`Get-PnpDevice`/邻居只读清点 | 未发现目标相机证据 | Realtek 物理链路 1Gbps；PnP 无 Hikrobot/MV-CS 条目；未调用设备打开或取流 |
| 2026-08-01 | 一台/四台实体相机、物理断链、服务重启 | 未执行 | 当前无可确认硬件且物理动作需人工 |
| 2026-08-01 | `PaperBreakCameraHardwareTest --probe --output out/m3-05-probe-20260801.json` | 未通过清单校验，退出 4 | MVS 至少返回两个完整序列号相同的描述项；仅披露后四位；未打开设备或取流 |
| 2026-08-01 | OFF Debug/Release configure、build、CTest | 通过 | 两套均 18/18；通用 unit 148 项、simulation 使用四路 Mock |
| 2026-08-01 | ON Debug/Release configure、build、CTest | 通过 | 两套均 22/22；Hikrobot 27 项、CLI 3 项、通用 unit 148 项，未包含实体相机功能测试 |
| 2026-08-01 | 软触发伪 SDK | 通过 | 实际模式/源核对、命令成功、模式不符不发命令、厂商失败保留业务/native 码 |
| 2026-08-01 | ON/OFF MSVC `/analyze` | 通过 | 生产目标及 ON 工具无构建失败 |
| 2026-08-01 | `format-check`、SDK 边界、安装路径泄漏、`git diff --check` | 通过 | 工具安装参与 ON 安装；MVS 引用仍只在适配器 |
| 2026-08-01 | 缺 MVS 根、缺计划、无界计划 | 预期失败 | 分别退出 1/2/2，具有明确诊断且无相机写操作 |

## 完成摘要

已实现 Windows/MSVC 真机验证器、MVS 软件触发、版本化有界计划、不可覆盖的原子 JSON 记录、1→4 路固定资源采集和资源指标；新增操作说明、计划样例、人工场景模板和本机清点记录。OFF/ON Debug/Release、全部非硬件 CTest、格式、静态分析及负向门禁通过。真实 probe 调用了 MVS 枚举但因完整序列号重复被拒绝，无法确认目标型号和实际设备数量；没有打开设备、写参数、取流或执行物理动作。至少一台目标机功能、四路吞吐/恢复、服务重启与真实预览/日志并发仍未执行，M3-05/M3 保持硬件门禁未完成。
