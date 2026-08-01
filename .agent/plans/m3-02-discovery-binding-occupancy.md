# M3-02：发现、序列号绑定和占用检测 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M3-02
- 关联需求：需求 4.3、8、11、16；架构 5.1～5.4、6.1、13、15、16.3

## 目的与可观察结果

启用 Hikrobot 适配器后，生产 Provider 只调用 MVS GigE 枚举，把固定长度厂商字段安全映射为型号、序列号、当前 IP 和主机网卡 IP，并通过独占访问探测标记设备是否已被其他程序占用。厂商无关领域函数把最多四个 `CAM01`～`CAM04` 槽位按序列号与发现清单核对，明确报告就绪、缺失、占用及未配置设备，且拒绝非法逻辑 ID、重复配置序列号和重复发现序列号。

## 范围

### 范围内

- 公共相机领域中的逻辑槽位、发现结果、绑定状态与校验/核对函数；
- Hikrobot GigE 字段安全映射、独占访问探测和 `ICameraProvider::enumerate_devices`；
- 按精确序列号创建设备，连接时使用 M3-01 RAII 独占打开并翻译占用错误；
- 伪 MVS API 自动化测试、Mock-only 回归及真实 SDK 无硬件构建/link 验证。

### 范围外

- M3-03 参数能力、参数写入与回读；
- M3-04 取流、帧映射、状态机和自动恢复；
- M3-05 真机工具、四路吞吐和拔线测试；
- UI、IPC 或配置 schema 扩展。

## 当前基线

- 配置层已限制最多四项、唯一 `CAM01`～`CAM04`，并拒绝启用槽位的空/重复序列号；
- `paperbreak_camera` 已提供厂商无关描述符、库存重复检测和按序列查找，但没有槽位核对报告或占用状态；
- M3-01 已在 `paperbreak_camera_hikrobot` 内隔离 MVS 4.8.0.3，提供可注入 API 表、设备列表深拷贝、句柄 RAII 和错误翻译；尚无生产 Provider；
- 本机有 MVS Development/Runtime 4.8.0.3，可编译和运行伪 API/SDK smoke；未发现或访问实体相机；
- 任务开始时工作区包含父任务已完成但尚未提交的 M3-01 修改，本任务只叠加 M3-02 文件。

## 前置条件与假设

- MVS `MV_GIGE_DEVICE_INFO::nCurrentIp` 和 `nNetExport` 按 SDK 示例的高位到低位格式映射为点分十进制；固定数组只在数组边界内查找 NUL，并拒绝空值、非终止值和控制字符；
- `MV_CC_IsDeviceAccessible(..., MV_ACCESS_Exclusive)` 返回 false 视为“当前不可独占/被占用”；该 API 不提供原始错误码，因此占用是稳定业务状态，实际打开失败仍保留 MVS native code；
- 配置中没有物理端口或期望型号字段，“错误设备占槽”按序列号白名单实现：未配置序列号只列为 unexpected，绝不会绑定到任一逻辑槽位；
- 没有实体相机时，真机字段值、驱动占用竞争和多网卡行为只能标记未验证。

## 设计说明

- `CameraDeviceDescriptor` 增加厂商无关的独占访问状态；业务层不接触 MVS 类型、IP 不参与身份绑定。
- `reconcile_camera_slots` 接受最多四个 `{logical_id, serial_number}` 和发现描述符，先校验所有外部字符串及唯一性，再按序列号产生固定槽位报告。缺失/占用是可观察状态而不是丢失全清单的首次失败；非法 ID、空/超长值、重复配置或重复发现则返回稳定业务错误。
- Hikrobot 公开头只暴露 `create_hikrobot_camera_provider()`；SDK API 表、设备信息和句柄全部保留在适配器私有实现。Provider 每次枚举只请求 `MV_GIGE_DEVICE`。
- M3-02 设备实现只负责描述符、独占连接和断开；参数/取流接口返回稳定的“当前里程碑未实现”业务错误，不调用对应 SDK，避免提前实施 M3-03/M3-04。

### 线程和队列

不适用。本任务所有发现、核对、连接/断开均为调用线程上的同步操作，不新增工作线程或跨线程队列。

### 持久化与恢复

不适用。本任务不修改配置 schema、数据库或用户数据。槽位绑定来自已验证配置快照，每次发现重新核对。

### 错误和降级

- 非法槽位/描述符、重复配置/发现序列号：`CAMERA_CONFIG_FAILED`，通过 `reason` 区分；
- 配置设备缺失：槽位 `missing`；设备不可独占：槽位 `occupied`；未配置设备进入 `unexpected_devices`，不占槽；
- 枚举失败：`CAMERA_NOT_FOUND` + `hikrobot-mvs` native code；实际独占打开冲突：`CAMERA_ACCESS_DENIED` + native code；
- 字段映射失败携带字段和原因，不把无界/无效厂商值传播到业务层。

## 实施步骤

- [x] 1. 在 `paperbreak_camera` 增加访问状态和最多四槽位核对模型/函数；覆盖合法四槽、缺失、占用、unexpected、非法 ID、空/超长、配置重复和发现重复。
- [x] 2. 扩展私有 MVS API 表和字段映射，实现仅 GigE 的生产 Provider 与精确序列号设备创建；用伪 API 覆盖 IP/网卡、边界字符串、非 GigE、重复、缺失、占用和打开竞争。
- [x] 3. 更新 CMake 公共头/源及 SDK 边界检查，运行格式和定向测试后修复告警。
- [x] 4. 运行 Mock-only 与 Hikrobot-enabled Debug/Release 配置、构建、非硬件 CTest、格式、SDK 边界、静态分析和 diff 检查。
- [x] 5. 更新路线图状态、计划进度/决策/发现/证据和完成摘要，不开始 M3-03。

## 验证计划

### 自动化测试

- 领域测试：四槽按序列绑定而非 IP；缺失、占用、unexpected；重复/非法输入；业务只接受 `CAM01`～`CAM04`；
- 伪 SDK：只传 `MV_GIGE_DEVICE`，完整字段映射，独占探测，固定数组非终止/空/控制字符，非 GigE 清单拒绝，Provider 精确序列创建及实际打开占用错误；
- 默认 OFF 与 ON 的既有全部非硬件测试回归。

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

另运行 `format-check`、`windows-vs2026-static-analysis`（ON/OFF 适用配置）、`hikrobot_sdk_boundary` 和 `git diff --check`。

### 人工或硬件验证

- 环境：需要安装 MVS 4.8.0.3、驱动及至少一台 GigE 相机；当前未提供实体相机。
- 步骤：MVS 管理程序占用相机时枚举；释放后枚举；核对型号、序列、IP、网卡；使用错误/正确序列绑定。
- 预期：占用状态切换，错误设备不绑定，正确设备落入指定 CAM 槽。
- 证据保存位置：未执行；待具备硬件时保存目标机日志和脱敏清单。本任务不会把未执行写为通过。

## 回滚与恢复

本任务只新增领域类型/函数、适配器 Provider/测试和文档。失败时撤销这些 M3-02 增量即可保留 M3-01 可构建基线；不删除配置、SDK 或用户数据。

## 验收标准

- [x] MVS 只枚举 GigE 并安全映射型号、序列号、IP 和主机网卡；
- [x] 最多四个逻辑槽位且业务 ID 只允许 `CAM01`～`CAM04`；
- [x] 检测重复序列号、配置设备缺失、未配置错误设备和其他程序占用；
- [x] IP 变化不影响按序列号绑定，错误设备不会占用槽位；
- [x] 所有 MVS 调用仍只在 Hikrobot 适配器且业务错误不只依赖厂商码；
- [x] 自动化测试不依赖实体相机，Debug/Release、CTest、格式、边界和静态分析通过；
- [x] 路线图和本计划记录真实验证证据及硬件限制。

## 进度记录

- 2026-08-01：完成规范、需求、架构、路线图、M3-01 ExecPlan、相关源码/CMake/测试及本机 SDK 字段定义检查；创建计划，状态 in-progress。
- 2026-08-01：完成领域槽位核对、Hikrobot GigE 映射/占用探测、精确序列设备创建及新增测试。
- 2026-08-01：完成 OFF/ON Debug/Release、非硬件 CTest、格式、边界、安装扫描和静态分析；回写路线图，状态 completed。

## 决策记录

- DEC-001：缺失、占用和 unexpected 作为完整发现报告中的状态，结构性非法/重复输入才使核对函数失败，以便一次发现展示全部四槽问题。
- DEC-002：未配置序列号的设备永不自动填入空槽；IP 和枚举顺序不参与身份绑定。
- DEC-003：M3-02 只实现 Provider 的发现与独占连接边界，其参数/取流方法显式返回未实现业务错误，避免越界到 M3-03/M3-04。

## 意外发现

- 现有配置层已经完成逻辑 ID、四路上限和启用序列号唯一性校验，M3-02 无需修改 schema。
- MVS 的可访问性 API 只返回布尔值，无法为“枚举时已占用”提供厂商原始码；真正打开冲突仍可保留原始码。
- Runtime 路径若以未经 CMake 规范化的反斜杠命令行值注入，会让生成的安装脚本把 `\P` 解析为非法转义；使用正斜杠路径重新配置后安装扫描通过，提交文件未写入本机路径。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | `git status --short` 与源码/SDK 字段检查 | 通过 | M3-01 增量已存在；本机 SDK 4.8.0.3 可读，无实体相机证据 |
| 2026-08-01 | ON Debug/Release configure、build、非硬件 CTest | 通过 | 两套均 19/19；适配器入口 14 项，unit 入口 142 项；只用伪 API 和 SDK 版本 smoke |
| 2026-08-01 | OFF Debug/Release configure、build、非硬件 CTest | 通过 | 两套均 18/18；默认构建不依赖 MVS |
| 2026-08-01 | `format-check`、`hikrobot_sdk_boundary`、安装树扫描、`git diff --check` | 通过 | SDK 符号未越界，安装产物无本机 SDK 路径泄漏 |
| 2026-08-01 | ON `local-windows-vs2026-static-analysis` configure/build | 通过 | MSVC `/analyze` 无构建失败 |
| 2026-08-01 | 实体 GigE 相机枚举/占用/连接 | 未执行 | 未提供实体相机；真实字段、驱动竞争和多网卡行为待硬件验证 |

## 完成摘要

新增 `CameraDiscoveryReport`，按配置序列号为最多四个 `CAM01`～`CAM04` 槽位报告 ready、missing 或 occupied，并单列未配置设备，IP 和枚举顺序不参与身份绑定。新增公开、无 SDK 类型的 Hikrobot Provider 工厂；适配器只枚举 GigE，以有界字段解析映射型号/序列/IP/主机网卡，探测独占访问状态，按精确序列创建设备并在独占打开冲突时返回 `CAMERA_ACCESS_DENIED` 与 MVS 原始码。新增 2 项领域测试和 3 项适配器测试；OFF/ON Debug/Release、非硬件 CTest、格式、边界、安装扫描、diff 检查和 ON 静态分析均通过。未访问实体相机，未验证真实发现/占用竞争/多网卡，且未开始 M3-03。
