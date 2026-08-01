# M3-01：SDK 构建隔离和 RAII 封装 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M3-01
- 关联需求：需求 2.3、4.3～4.6、11、16；架构 2.2、5.2～5.4、6.1、13、15、16.3、19

## 目的与可观察结果

默认 Mock-only 配置不查找或链接 MVS；显式启用 `PAPERBREAK_ENABLE_HIKROBOT` 后，CMake 只给 `paperbreak_camera_hikrobot` 私有注入批准的 MVS 4.8.0.3 头文件、x64 import library 和 Runtime DLL。适配器内部通过 RAII 管理设备列表值、设备句柄和取流会话，析构按停止取流、关闭设备、销毁句柄的顺序尽力清理；任何图像回调异常均在 C ABI 边界转换为有界诊断状态，不越过 SDK。

## 范围

### 范围内

- MVS Development/Runtime 的 CMake 查找、版本/架构诊断、独立目标和安装部署隔离；
- `src/camera/hikrobot` 内部 SDK API 表、设备列表、句柄、取流 RAII、稳定业务错误翻译和异常安全回调蹦床；
- 使用伪 API 的适配层生命周期测试、真实已安装 SDK 的链接/版本 smoke，以及 Mock-only 隔离检查；
- MVS 版本、运行时部署和许可证/分发门禁文档。

### 范围外

- M3-02 的 GigE 字段映射、序列号绑定、占用检测和业务 Provider；
- M3-03 的参数能力、读写与回读；
- M3-04 的帧元数据映射、取帧管线、断线恢复和状态机集成；
- M3-05 的相机测试工具、实体相机、四路吞吐、拔线和目标机验证。

## 当前基线

- 顶层已有默认 `OFF` 的 `PAPERBREAK_ENABLE_HIKROBOT`，但启用后在依赖解析阶段固定报错，尚无适配器目标；
- `paperbreak_camera` 与 `paperbreak_camera_mock` 已独立，默认非硬件 CTest 共 17 个入口；
- `ICameraProvider`/`ICameraDevice`、稳定相机业务错误和可选 native diagnostics 已存在且不暴露 MVS 类型；
- `docs/architecture/dependencies.md` 已批准 MVS SDK Development/Runtime 4.8.0.3，并要求 SDK 仅进入适配器私有属性；
- 工作区开始时 `git status --short` 为空；本机发现 MVS 安装根 `C:\Program Files (x86)\MVS`，Development/Runtime 文件版本为 4.8.0.3，但未发现或访问实体相机。

## 前置条件与假设

- 目标仅支持 Windows x64/MSVC，MVS import library 使用 `Development/Libraries/win64/MvCameraControl.lib`；
- Development 根可由 `PAPERBREAK_MVS_ROOT` 或供应商 `MVCAM_COMMON_RUNENV` 注入，Runtime x64 目录由 `PAPERBREAK_MVS_RUNTIME_DIR` 注入；提交文件不记录具体绝对路径；
- CMake 配置校验 SDK 文件布局，运行测试调用 `MV_CC_GetSDKVersion` 校验实际加载版本为 4.8.0.3；
- 本机 SDK 可用于编译和无设备 smoke，不等于完成实体相机、驱动、取流或四路性能验证。

## 设计说明

- `Hikrobot::MVS` 为仅在开关开启时创建的 imported shared target；include、implib 和 runtime location 不进入 `paperbreak_camera`、Mock、UI 或业务目标。
- 适配器内部 `MvsApi` 保存所需 C API 函数指针，生产表指向供应商函数，测试表指向无硬件伪实现。这样可验证失败和清理顺序而不伪造实体硬件。
- `DeviceList` 按值持有 SDK 列表并在销毁时清零；SDK 未提供独立释放函数，因此不虚构释放调用。
- `DeviceHandle` 工厂只在 `MV_CC_CreateHandle` 成功后取得所有权；共享内部状态使未结束的 `StreamSession` 阻止底层句柄过早销毁。最后一个所有者析构时先尽力停流，再关闭设备，最后销毁句柄。
- `StreamSession` 只有 `MV_CC_StartGrabbing` 成功后成立；析构/显式停止幂等调用 `MV_CC_StopGrabbing`。
- C 回调蹦床为 `noexcept`，分别捕获 `std::exception` 和未知异常，仅更新预分配/固定上限的原子诊断，不进行磁盘、网络、编码或推理。

### 线程和队列

本任务不新增线程或跨线程队列。SDK 回调可能由供应商线程调用；回调边界只读取已注册处理器并更新原子计数，生命周期要求停止取流完成后再销毁回调状态。真正的帧投递和有界队列属于 M3-04。

### 持久化与恢复

不适用。本任务不修改配置、数据库、事件格式或用户数据。Runtime DLL 仅作为安装时依赖复制，失败时配置/安装立即失败，不写业务状态。

### 错误和降级

- 创建、打开、启流失败转换为现有稳定相机业务码，`native_domain=hikrobot-mvs`、十六进制 `native_code` 保留诊断；
- 清理函数为 `noexcept` 且幂等，保存第一次清理失败供诊断，绝不从析构或回调抛出；
- Mock-only 开关关闭时完全不检查 MVS；开启但 SDK 头/lib/dll 缺失时配置阶段明确失败，不静默回退或从未知 PATH 链接；
- 回调异常计数并标记类别，当前帧被丢弃；后续告警/恢复编排不在 M3-01 范围。

## 实施步骤

- [x] 1. 更新依赖解析并新增 `src/camera/hikrobot/CMakeLists.txt`，建立开关控制、严格文件诊断、私有 imported target、Runtime 安装和 SDK 引用扫描。
- [x] 2. 实现内部 API 表、设备列表、设备句柄、取流会话和错误翻译，覆盖成功、部分失败、幂等停止与析构逆序清理。
- [x] 3. 实现 `noexcept` 图像回调边界，捕获标准/未知异常并用原子有界状态记录，禁止回调内其他工作。
- [x] 4. 新增 Hikrobot 专用测试目标，通过伪 API 验证所有权/清理/错误语义，通过真实 SDK 函数验证版本和链接；默认测试继续只运行 Mock/non-hardware。
- [x] 5. 运行 Mock-only 与 Hikrobot-enabled Debug/Release 配置、构建、CTest、格式、静态分析及缺 SDK 配置失败检查。
- [x] 6. 更新依赖/构建说明、路线图状态、计划证据和完成摘要，确认未开始 M3-02。

## 验证计划

### 自动化测试

- Mock-only Debug/Release 全部非硬件 CTest，证明缺少/未指定 SDK 不影响现有构建；
- 伪 API：创建失败不接管、打开失败仍销毁、启流失败不生成会话、显式/析构停止幂等、停止/关闭/销毁逆序和首个清理错误；
- 回调正常、抛 `std::exception`、抛非标准异常，均不得逃逸 C 回调边界；
- 业务错误码和 `hikrobot-mvs` 原始码翻译；
- 真实 SDK 4.8.0.3 版本/link smoke，不枚举或打开设备；
- SDK 头/符号路径扫描只允许 `src/camera/hikrobot`，公开相机头不出现 MVS 类型。

### 构建与测试命令

```powershell
cmake --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug
cmake --preset windows-vs2026-release
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-release

cmake --preset local-windows-vs2026-debug -DPAPERBREAK_ENABLE_HIKROBOT=ON -DPAPERBREAK_MVS_ROOT="$env:MVCAM_COMMON_RUNENV" -DPAPERBREAK_MVS_RUNTIME_DIR="<MVS-x64-runtime>"
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

本机依赖路径由现有 `local-windows-vs2026-*` 用户预设或环境注入；具体路径不提交。

### 人工或硬件验证

- 环境：本机存在 MVS SDK/Runtime 4.8.0.3，但无已提供的 MV-CS020-60GM 和目标四路网络。
- 步骤：本任务只执行版本/link smoke；不枚举、连接或取流。
- 预期：编译和生命周期测试通过只能证明 SDK 构建边界与 RAII 语义，不证明真机功能。
- 证据保存位置：本计划验证证据表；实体相机证据由 M3-02～M3-05 后续任务归档。

## 回滚与恢复

本任务只新增适配器源码/测试并修改 CMake/文档。失败时撤销本任务文件和目标登记即可回到 M2 Mock-only 基线；不删除 SDK、Runtime、用户配置或任何生产数据。

## 验收标准

- [x] CMake 选项可真实启用独立 Hikrobot 适配器，缺文件/错误架构有明确配置错误；
- [x] 开关关闭时不查找 MVS，Mock 与全部非硬件测试继续通过；
- [x] MVS include/lib/dll/API/错误翻译仅存在于 `src/camera/hikrobot` 及其私有目标属性；
- [x] 设备列表、句柄和取流生命周期均由无原始 `new/delete` 的 RAII 类型管理，部分失败和析构清理有测试；
- [x] C API 回调捕获全部异常且测试证明异常不越界；
- [x] 文档记录 SDK 4.8.0.3、Runtime 部署、第三方许可通知和供应商分发授权门禁；
- [x] Debug/Release、格式、非硬件 CTest、Hikrobot 专用测试和静态分析完成；
- [x] 未开始 M3-02 或后续行为，未声称实体相机测试。

## 进度记录

- 2026-08-01：完成需求、架构、路线图、计划规范、现有相机/CMake/测试与本机 MVS 布局检查；创建计划，状态 in-progress。
- 2026-08-01：完成独立目标、SDK 文件/版本诊断、Runtime 部署、设备列表深拷贝、句柄/取流 RAII、错误翻译、回调边界和 11 项专用测试。
- 2026-08-01：完成 OFF/ON Debug/Release、非硬件 CTest、格式、边界/路径扫描、缺 SDK 失败诊断和静态分析，状态 completed。

## 决策记录

- DEC-001：RAII 通过可注入的 MVS C API 表测试，既调用真实 SDK 类型又不依赖实体相机。
- DEC-002：Hikrobot 测试作为独立目标，仅开关开启时构建，避免 SDK include/link 属性污染默认 `PaperBreakTests`。
- DEC-003：M3-01 只提供内部生命周期基元和回调边界，不提前实现 `ICameraProvider` 的发现、参数或取流行为。

## 意外发现

- 本机已安装 MVS 管理应用 5.0.1，其 Industrial Camera SDK/Runtime 文件版本为批准的 4.8.0.3；管理应用版本不能当作 SDK 编译版本。
- MVS Development 与 x64 Runtime 位于不同根目录，因此需要分别注入并在 CMake 中逐文件校验。
- SDK 设备列表本身没有配套释放 API；为避免保留 SDK 管理的裸指针，RAII 包装在枚举成功后对最多 256 项设备信息做有界深拷贝。
- 首次清理失败测试的预期少计了一次析构重试；实现会在显式停止、会话析构和最终句柄清理三个边界尽力停止，持续失败时仍继续关闭并销毁句柄，测试已按该确定性行为修正。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | `git status --short` | 通过 | 任务开始时工作区干净 |
| 2026-08-01 | MVS 安装布局与文件版本检查 | 通过 | SDK/Runtime 为 4.8.0.3；未访问相机 |
| 2026-08-01 | 实体相机/四路取流 | 未执行 | 无任务授权及硬件条件，属于 M3-02～M3-05 |
| 2026-08-01 | 默认 OFF：本机 Debug/Release configure、build、CTest | 通过 | 两套非硬件 CTest 均 18/18；unit 入口 140 项，不查找/链接 MVS |
| 2026-08-01 | ON：本机 MVS 4.8.0.3 Debug/Release configure、build、CTest | 通过 | 两套非硬件 CTest 均 19/19；专用入口 11 项，包含真实 SDK 版本/link smoke，不枚举设备 |
| 2026-08-01 | ON：缺失 Development/Runtime 路径配置 | 预期失败 | 退出码 1，明确报告 `PAPERBREAK_MVS_ROOT` 目录不存在 |
| 2026-08-01 | `format-check`、`hikrobot_sdk_boundary`、安装树路径泄漏扫描 | 通过 | MVS 符号仅位于适配器；ON 安装树含 4.8.0.3 核心 DLL 且无注入路径文本 |
| 2026-08-01 | ON：`local-windows-vs2026-static-analysis` configure/build | 通过 | MSVC `/analyze`，适配器目标无构建失败 |

## 完成摘要

新增独立 `paperbreak_camera_hikrobot` 静态库及其私有 SDK imported target；默认 OFF 完全保留 Mock-only 构建。启用时严格检查批准的 Development/x64 Runtime 文件和 DLL 4.8.0.3 版本，把核心 Runtime DLL 部署到安装 `bin`。内部 API 表支持无硬件伪实现；设备列表持有有界深拷贝，设备句柄和取流会话以共享状态保证停止、关闭、销毁逆序且幂等清理，厂商错误转换为稳定业务码并保留 `hikrobot-mvs` 原始码。图像回调蹦床捕获标准和未知异常，仅发布原子诊断。新增 11 项专用测试及 SDK 边界检查；OFF/ON Debug/Release、非硬件 CTest、格式、路径扫描、负向配置和静态分析均通过。未访问实体相机，未验证驱动、发现、连接、取流、带宽或拔线恢复，未开始 M3-02。
