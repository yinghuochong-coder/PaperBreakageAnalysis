# 依赖与工具链基线

## 1. 文档状态与适用范围

| 项目 | 值 |
| --- | --- |
| 状态 | Accepted |
| 基线日期 | 2026-08-01 |
| 目标平台 | Windows 10/11 x64 |
| 语言标准 | C++20 |
| 决策记录 | [ADR-015：Windows 工具链与依赖获取基线](decisions/adr-015-windows-toolchain-dependencies.md) |

本文固定首期工程的工具链、直接依赖、获取方式、许可证和升级规则。它不创建 CMake 工程或安装依赖；M0 必须把本基线机械化为预设、vcpkg manifest、CI 检查和发布物料清单。

“批准”只表示技术上允许引入，不代替组织的法务、采购、安全和供应商分发授权。未列入本文的生产依赖必须先记录引入原因、版本、许可证、维护来源和替代方案，再进入实现。

## 2. 已接受决策摘要

1. 开发与 CI 使用 Visual Studio 2026 stable、MSVC v145、C++20、x64 和动态 MSVC 运行库。
2. CMake 最低版本为 4.2；当前已验证版本为 4.2.3。CMake 4.2 首次支持 `Visual Studio 18 2026` generator。
3. 开发、IDE 和 CI 统一使用 `Visual Studio 18 2026` x64 generator，并分别提供 Debug、Release 和静态分析预设。
4. Qt 6.10.2、OpenCV 4.12.0、Hikrobot MVS SDK 4.8.0.3 是外部 SDK，不由 vcpkg 下载或重新打包。
5. spdlog、nlohmann/json、GoogleTest、SQLite3 和可选 zstd 使用 vcpkg manifest mode；版本由提交的 baseline 和 manifest 约束。
6. 项目文件只引用逻辑变量、CMake imported target 和仓库相对路径。开发机安装根目录只能由环境或不提交的 `CMakeUserPresets.json` 注入。
7. CI 采用具备下述能力的 Windows 11 x64 自托管执行器。默认构建和测试只使用 Mock；MVS/实体相机任务在隔离的硬件 lane 执行。

## 3. 工具链

| 工具 | 接受基线 | 最低要求 | 用途与约束 |
| --- | --- | --- | --- |
| Visual Studio | 2026 stable，当前实证 18.6 | 18.x，含 Desktop development with C++ | 只支持 MSVC/x64；CI 记录完整安装版本 |
| MSVC Build Tools | v145，当前实证 `cl 19.51.36243` | v145 | `/std:c++20`；链接器不得早于输入库所用工具集 |
| Windows SDK | VS 2026 stable 通道所带受支持版本 | 支持 Windows 10/11 x64 | 精确版本写入构建元数据，不在源码中绑定开发机安装路径 |
| CMake | 当前实证 4.2.3 | 4.2 | `cmake_minimum_required(VERSION 4.2)`；支持 VS 2026 generator |
| C++ | C++20 | C++20 | 禁止依赖未批准的 C++23/26 行为 |
| vcpkg tool | VS 随附版本当前实证 2026-03-04 | 能解析锁定 baseline 的受支持版本 | 仅 manifest mode；不依赖全局 classic 安装状态 |

Visual Studio 2026 v145 可以链接使用较早 v143/v142 工具集生成的 Qt/OpenCV 库，依据是 Microsoft 对 v140～v145 的二进制兼容承诺；最终链接器必须是输入中最新的工具集。`/GL` 或 `/LTCG` 产物不跨工具集混用，若外部静态库启用这些选项，必须使用相同工具集重建或拒绝配置。

Qt 6.10 官方 Windows 支持表只列 MSVC 2022。项目使用 Qt 官方 `msvc2022_64` 二进制和 VS 2026 v145 属于基于 Microsoft ABI 保证的项目批准组合，不宣称它是 Qt 官方测试矩阵中的 MSVC 2026 配置。M0 必须完成链接、启动和部署 smoke test。

## 4. 外部 SDK

| 依赖 | 固定版本 | 允许组件/功能 | 获取与发现 | 许可证/发布约束 |
| --- | --- | --- | --- | --- |
| Qt | 6.10.2 `msvc2022_64` | 必需：Core、Gui、Widgets、Network；Concurrent 仅在具体 target 有批准用途时启用 | Qt 官方安装器；由 `PAPERBREAK_QT_ROOT` 或用户预设加入 `CMAKE_PREFIX_PATH` | 商业许可，或满足适用模块 LGPLv3/GPLv3 条款；未确认许可路径前不得发布 |
| OpenCV | 4.12.0 | 首期只批准 core、imgproc、imgcodecs；新增模块需记录理由 | OpenCV 官方 Windows SDK/批准的内部镜像；通过 `OpenCV_DIR` 注入 | Apache-2.0；发布时保留 NOTICE/许可证及实际打包模块的第三方通知 |
| Hikrobot MVS SDK | Development/Runtime 4.8.0.3 | 枚举、参数、取流、错误翻译；运行时仅部署核心控制与 GigE 传输组件；仅限 Hikrobot 适配器 | 供应商安装器/批准的内部镜像；通过 `PAPERBREAK_MVS_ROOT` 和 `PAPERBREAK_MVS_RUNTIME_DIR` 分别注入 Development 与 x64 Runtime | 供应商专有条款；随安装器提供的第三方许可通知必须保留，SDK 与 Runtime 分发权必须在 M9 发布前书面确认 |

补充规则：

- MVS 管理应用的本机版本是 5.0.1，但项目编译依赖版本是 Development/Runtime 4.8.0.3；二者不得混写。
- Qt 类型不得泄漏到领域接口，Widgets 只允许进入控制台/UI 目标。
- OpenCV 只允许进入批准的算法或图像编解码实现目标，不得成为相机接口、领域模型或 IPC 协议的公开类型。
- MVS 头文件、库目录、DLL 和 API 只允许进入 `paperbreak_camera_hikrobot` 的私有编译/链接/部署属性。
- 默认 Mock 构建不能要求 MVS 已安装。启用生产适配器时，缺少或版本不匹配必须在配置阶段失败。
- 外部 SDK 不提交到 `external/`、Git LFS、Release 配置或源代码树。

### 4.1 Hikrobot MVS 构建与运行时部署

`PAPERBREAK_ENABLE_HIKROBOT` 默认关闭；关闭时不读取任何 MVS 路径，也不创建或链接生产适配器。启用时：

- `PAPERBREAK_MVS_ROOT` 可以指向 MVS 安装根或 `Development` 根；未显式设置时允许读取供应商安装器创建的 `MVCAM_COMMON_RUNENV`；
- `PAPERBREAK_MVS_RUNTIME_DIR` 必须指向同时包含 `MvCameraControl.dll` 和 `MVGigEVisionSDK.dll` 的 x64 Runtime 目录，不能依赖 PATH 中碰巧先出现的 Win32/未知版本 DLL；
- 配置阶段要求 `MvCameraControl.h`、`MvErrorDefine.h`、`Libraries/win64/MvCameraControl.lib` 及上述两个 x64 Runtime DLL 全部存在，并逐个读取 DLL 文件版本拒绝非 4.8.0.3；适配层测试还调用 `MV_CC_GetSDKVersion()` 并执行不打开设备的 GigE 枚举 smoke；
- MVS include、import library、Runtime DLL 和 C API 只属于 `src/camera/hikrobot` 中的私有构建/部署边界；构建输出和安装树均把两个 DLL 放在调用程序同目录，运行不依赖开发机绝对路径；
- `MVGigEVisionSDK.dll` 是 `MvCameraControl.dll` 在 `MV_CC_EnumDevices(MV_GIGE_DEVICE)` 时动态加载的 GigE 传输组件，无法由静态 PE 依赖扫描自动发现。首期不部署未使用的 USB、采集卡或 GUI Runtime；目标机仍需安装批准的 MVS 驱动，签名、离线包和最终 SBOM/许可证物料由 M9 固化。

供应商安装目录中的许可通知（当前安装器文件名 `CLIENT_MVS_Win_license_notice.txt`）列出 Runtime 内嵌的 MIT、LGPL、BSD、zlib、libpng、IJG 等第三方组件。发布包必须随实际 Runtime 文件保留对应通知/许可证并在 SBOM 中逐项登记；供应商专有 SDK/Runtime 的复制和再分发授权必须由采购/法务书面确认。本文不是法律意见，授权未确认时发布门禁保持阻塞。

## 5. vcpkg 直接依赖

vcpkg 使用 `x64-windows` triplet 和动态 CRT。G0 选定的初始 registry baseline 为：

```text
6d811e820d5dd72ce2cc8a35596a33e67572d83d
```

M0 创建 `vcpkg.json` 时必须提交 `builtin-baseline`，并验证下表的解析结果；若 registry 已变化导致无法解析，不得静默改用最新版本，应以单独依赖升级评审更新本文与 baseline。

| vcpkg port | 初始解析版本 | 范围 | 引入原因 | 许可证 | 发布要求 |
| --- | --- | --- | --- | --- | --- |
| `spdlog` | 1.17.0#1 | 生产 | 异步日志、分类 sink、滚动文件和统一格式；业务代码通过项目日志门面使用 | MIT | 携带版权与许可证；记录其实际解析的 fmt 等传递依赖 |
| `nlohmann-json` | 3.12.0#2 | 生产 | 配置、事件清单和版本化消息的 JSON 解析/序列化；不替代 schema 与强类型校验 | MIT | 携带版权与许可证 |
| `gtest` | 1.17.0#3 | 测试 | GoogleTest/GoogleMock 单元、组件和模拟集成测试 | BSD-3-Clause | 不进入 Release 安装集；测试分发时保留许可证 |
| `sqlite3` | 3.53.4 | 生产 | 嵌入式事件元数据、上传任务和迁移；不存高速图像 BLOB | blessing/public domain | 在 SBOM 中记录版本和来源；数据库 schema 独立版本化 |
| `zstd` | 1.5.7 | 可选生产 | 仅用于经性能/恢复验证后的事件数据压缩 | BSD-3-Clause OR GPL-2.0-only；项目选择 BSD-3-Clause 路径 | 默认关闭；启用时保留 BSD 版权与许可证，不启用 GPL 路径 |

版本号包含 vcpkg port revision（`#N`）时，revision 与上游版本一起构成可复现标识。spdlog 的 fmt 等传递依赖由同一 baseline 解析，必须进入 SBOM 和许可证清单，不允许以另一个全局 fmt/spdlog 副本覆盖。

### 5.1 功能开关

- `zstd` 不属于最小构建，只有存储格式 ADR、性能数据和恢复测试通过后才能启用；
- GoogleTest 只在 `BUILD_TESTING=ON` 时解析/构建；
- 不启用 SQLite 可装载扩展；生产构建应在 M5 固定所需 compile options 并测试；
- 不使用 vcpkg 获取 Qt、OpenCV 或 MVS，以避免与供应商验证的二进制/驱动组合产生双重来源。

## 6. 获取、缓存与离线重建

1. vcpkg 只使用 manifest mode。禁止把开发机 classic mode 中已安装的包当作构建输入。
2. `vcpkg.json`、baseline、必要的 overlay port/triplet 和许可证清单必须提交；`vcpkg_installed/` 不提交。
3. CI 可以使用受访问控制的 vcpkg binary cache，但 cache key 必须至少包含 baseline、triplet、编译器、CRT、配置和相关 feature。
4. 外网不可用的生产网络通过批准的内部镜像/离线包提供 vcpkg 源与外部 SDK；镜像必须保存来源、校验值、许可证和签名信息。
5. 禁止 CMake `FetchContent` 在普通配置或构建阶段从浮动分支下载生产依赖。
6. 依赖下载失败不得回退到系统 PATH 中版本未知的库。

## 7. 路径和 Release 可移植性

允许注入的逻辑入口：

| 逻辑项 | 推荐入口 | 是否可提交具体值 |
| --- | --- | --- |
| Qt kit 根目录 | `PAPERBREAK_QT_ROOT` / `CMAKE_PREFIX_PATH` | 否 |
| OpenCV CMake 目录 | `OpenCV_DIR` | 否 |
| MVS Development 根目录 | `PAPERBREAK_MVS_ROOT` | 否 |
| MVS x64 Runtime 目录 | `PAPERBREAK_MVS_RUNTIME_DIR` | 否 |
| vcpkg 根目录 | `VCPKG_ROOT` 或 CI toolchain 设置 | 否 |
| vcpkg baseline/triplet | `vcpkg.json` / 项目预设 | 是 |

规则：

- 提交的 `CMakePresets.json` 只能包含相对路径、`${sourceDir}`、环境引用和可复现版本；本机值放入被 Git 忽略的 `CMakeUserPresets.json`。
- 不在源码、生成的默认配置、安装清单、服务参数或发布脚本中写入驱动器盘符、用户名目录、Qt/OpenCV/MVS 安装目录。
- imported target 的绝对路径只允许存在于构建树的 CMake cache/import 文件，不得复制到提交文件或安装后的运行配置。
- Release 从安装树启动，只从应用安装目录、系统运行时或明确批准的系统组件加载 DLL；不得依赖开发机 `PATH`。
- M0/M9 对提交文件和安装树执行路径泄漏扫描。PDB 等调试制品如需发布，必须单独存放并按组织的源码路径脱敏策略处理。

## 8. CI Windows 执行器

### 8.1 默认 lane

采用 provider-neutral 的 Windows 11 x64 自托管执行器，能力标签至少包括：

```text
windows
x64
vs2026
msvc-v145
cmake-4.2
mock-only
```

执行器必须安装 VS 2026 stable 的 C++ 工作负载、v145、Windows SDK、CMake 4.2+、Qt 6.10.2 和 OpenCV 4.12.0，并能访问锁定 vcpkg registry/cache。默认 lane：

- 配置、构建并运行 Debug/Release 的非硬件 CTest；
- 不要求 MVS SDK、相机、PLC 或上位机在线；
- 明确禁用 Hikrobot 生产适配器，使用 Mock Camera；
- 每次作业输出 VS、MSVC、CMake、Qt、OpenCV、vcpkg baseline 和直接依赖版本；
- runner 不满足精确能力时失败，不降级到其他编译器或未锁定依赖。

当前开发机已验证具备 VS 2026 18.6、MSVC v145、CMake 4.2.3、Qt 6.10.2 和 OpenCV 4.12.0，可作为 runner 镜像/配置参考。仓库尚无远端和 CI 注册信息，因此这里只确认执行器能力与本机可供给性，不声称 runner 已在某一 CI 平台上线；注册、凭据和 workflow 由 M0-04 完成。

### 8.2 硬件 lane

Hikrobot/MVS 和实体相机测试使用独立的受控自托管执行器，并额外标记 `mvs-4.8.0.3`、相机型号和数量。它不得成为普通 PR 的必需前置，必须：

- 检查 SDK/Runtime/驱动版本并保存原始记录；
- 缺少硬件时报告 skipped，而不是 passed；
- 禁止不受信任变更直接访问生产网段、相机或长期凭据；
- 四路相机、拔线、带宽和稳定性验收只在批准的目标工控机执行。

## 9. 许可证与供应链门禁

每次 Release 至少生成：

- 直接和传递依赖的名称、版本、来源、哈希、许可证与链接方式；
- Qt 实际模块及其 SPDX SBOM/第三方组件清单；
- OpenCV 实际模块及随包第三方通知；
- MVS Runtime 文件清单、版本和供应商分发授权依据；
- MSVC Redistributable 和其他运行库的版本/分发依据；
- 漏洞扫描、许可证扫描和人工例外审批记录。

Qt 许可路径在首个外部分发包之前必须由项目所有者/法务二选一并留证：

1. 使用有效的 Qt 商业许可证；或
2. 仅使用适用的 LGPLv3 模块并完整履行义务，包括动态链接、显著通知、许可证文本、对应源码供应和用户替换/重链接权利。

本文不是法律意见。若组织无法确认 LGPLv3 或供应商 SDK 的分发义务，发布门禁保持阻塞。

## 10. 升级和例外流程

依赖升级作为独立逻辑变更，至少：

1. 更新版本/baseline 和本文；
2. 阅读上游发布、安全、ABI 与迁移说明；
3. 重新核对直接及传递许可证；
4. 运行 Debug/Release、CTest、安装树路径扫描和许可证/SBOM 检查；
5. 对 Qt/OpenCV/MVS 执行适配器/启动 smoke test；MVS 变更还需目标相机验证；
6. 数据格式相关依赖升级不得隐式修改配置、事件或数据库 schema；
7. 保存前后版本、验证命令和回滚点。

紧急安全更新可以提高 patch 版本，但不能跳过评审和可恢复性。未通过验证时恢复上一 baseline/SDK 版本；不得删除用户数据或用数据库降级覆盖已迁移数据。

## 11. 来源

- [Visual Studio 2026 release notes](https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-notes)
- [Microsoft C++ binary compatibility 2015–2026](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)
- [CMake 4.2 release notes](https://cmake.org/cmake/help/v4.2/release/4.2.html)
- [CMake Visual Studio 18 2026 generator](https://cmake.org/cmake/help/v4.2/generator/Visual%20Studio%2018%202026.html)
- [Qt 6.10 supported platforms](https://doc.qt.io/qt-6.10/supported-platforms.html)
- [Qt licensing](https://doc.qt.io/qt-6/licensing.html)
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)
- [vcpkg versioning reference](https://learn.microsoft.com/en-us/vcpkg/users/versioning)
- [OpenCV releases and license](https://opencv.org/release/), [Apache-2.0 licensing](https://opencv.org/license/)
- [spdlog](https://github.com/gabime/spdlog), [nlohmann/json](https://github.com/nlohmann/json), [GoogleTest](https://github.com/google/googletest), [SQLite](https://www.sqlite.org/copyright.html), [zstd](https://github.com/facebook/zstd)

本机 MVS 版本和许可文件来自已安装供应商软件；供应商许可及下载页面需要在 M3/M9 使用组织批准的账号和合同复核。
