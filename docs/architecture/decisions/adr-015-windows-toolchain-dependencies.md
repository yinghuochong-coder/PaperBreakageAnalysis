# ADR-015：Windows 工具链与依赖获取基线

- 状态：Accepted
- 日期：2026-07-31
- 修订日期：2026-08-01（统一为 Visual Studio 2026 generator）
- 决策者：项目架构基线
- 关联：G0-03、路线图 DEC-001～DEC-003

## 背景

项目要求 Windows 10/11 x64、Visual Studio 2026、C++20、Qt 6、CMake、OpenCV 和 Hikrobot MVS SDK。原始需求把 CMake 最低版本写为 3.27，同时允许 Visual Studio generator；但 CMake 直到 4.2 才加入 Visual Studio 18 2026 generator。需求还记录了开发机绝对 SDK 路径，Qt 目录名存在错误，并未固定 OpenCV/MVS 或开源依赖版本。

Qt 6.10.2 官方 Windows kit 使用 MSVC 2022 构建，Qt 官方支持表也只列 MSVC 2022；项目目标编译器则是 VS 2026 v145。Microsoft 明确保证 v140～v145 工具集的二进制兼容，但对 `/GL`/`/LTCG` 跨工具集混用和链接器版本有额外限制。

项目还需要在没有实体相机和 MVS SDK 的普通 CI 中运行 Mock 测试，同时保留供应商 SDK/硬件验证路径。依赖获取必须可复现，且 Release 不得依赖开发机绝对路径。

## 决策

### 工具链

- 使用 Visual Studio 2026 stable、MSVC v145、C++20、x64 和动态 MSVC 运行库；
- CMake 最低版本提高到 4.2，当前验证基线为 4.2.3；
- 开发、IDE 和 CI 统一使用 Visual Studio 18 2026 x64 generator；分别提供 Debug、Release 和静态分析预设；
- Qt 使用 6.10.2 `msvc2022_64` 官方动态库，依据 Microsoft ABI 保证由 v145 链接；禁止跨工具集混用 `/GL`/`/LTCG` 产物。

### 依赖获取

- Qt 6.10.2、OpenCV 4.12.0、Hikrobot MVS SDK 4.8.0.3 由官方/批准镜像作为外部 SDK 提供；
- spdlog、nlohmann/json、GoogleTest、SQLite3 和可选 zstd 使用 vcpkg manifest mode；
- vcpkg 初始 baseline 固定为 `6d811e820d5dd72ce2cc8a35596a33e67572d83d`，使用 `x64-windows`；
- 禁止普通构建通过浮动分支、隐式系统包或 vcpkg classic 全局状态解析依赖。

### 路径、许可和 CI

- SDK 安装根目录只通过环境或不提交的用户预设注入；项目预设和 Release 配置只保存逻辑变量/相对路径；
- Qt 的商业或 LGPLv3 路径必须在发布前明确，MVS Runtime 分发权必须由供应商条款确认；
- 默认 CI 使用 Windows 11 x64 自托管执行器能力，Mock-only 且不要求 MVS；MVS/实体相机使用隔离硬件 lane；
- 当前本机工具链可作为 runner 配置参考，但具体 CI 平台注册和 workflow 属于 M0-04。

完整版本、组件、许可证和门禁见 `docs/architecture/dependencies.md`。

## 结果

正面结果：

- VS 2026 IDE、编译器和 CMake generator 形成一致且可实证的基线；
- 开源依赖由 manifest/baseline 可复现解析，外部 SDK 保持供应商验证边界；
- 普通 CI 不依赖 MVS 或实体相机，硬件验证又不会被伪装为普通单元测试；
- Release 配置与开发机安装路径解耦，许可证和 SBOM 有明确门禁。

代价与风险：

- Qt 6.10.2 + v145 不是 Qt 官方列出的 MSVC 2026 测试组合，M0 必须用链接/启动 smoke test 验证；
- CMake 最低版本从 3.27 提高到 4.2，旧环境必须升级；
- 自托管 runner 需要补丁、凭据、缓存和隔离运维；
- 外部 SDK 不由 vcpkg统一管理，需要单独保存版本、签名、许可和离线镜像证据；
- Qt/MVS 的最终分发许可尚需组织审批，在此之前发布门禁保持阻塞。

## 被否决或推迟的方案

### 继续使用 CMake 3.27

否决。它不能生成 Visual Studio 18 2026 工程；只使用 Ninja 可以绕开 generator 缺失，但会让“支持 Visual Studio generator”的需求依赖隐含例外。

### 在 VS 2026 中强制使用 v143 构建全部项目

否决为默认方案。VS 2026 v145 是项目目标且能依据 Microsoft 保证链接较早 ABI 库；退回 v143 会削弱目标基线。若 M0 smoke test 发现供应商库实际不兼容，必须通过新 ADR 明确降级。

### 由 vcpkg 管理 Qt、OpenCV 和 MVS

否决。Qt/OpenCV 使用已批准的官方 Windows SDK，MVS 与驱动/相机工具链绑定且不适合公共包管理；混用来源会增加 ABI、许可和支持风险。

### 把 SDK 复制进仓库或写死安装路径

否决。会造成许可、仓库体积、升级、开发机差异和 Release 路径泄漏问题。

### 默认 CI 依赖装有相机的硬件 runner

否决。普通测试必须在无实体相机时可运行；硬件 lane 只用于明确标记的集成和验收。

## 验证要求

M0 将本决策落地时必须证明：

- CMake 4.2+ 能用 Visual Studio 18 2026 generator 配置 x64 Debug/Release；
- v145 能链接并启动 Qt 6.10.2 与 OpenCV 4.12.0 最小程序；
- Mock-only Debug/Release 构建不发现 MVS；
- 启用 MVS 的配置只把 SDK include/lib 暴露给 Hikrobot 适配器；
- 提交文件和安装树不包含开发机 SDK 绝对路径；
- vcpkg 解析版本、SBOM 和许可证清单与依赖基线一致。

真实相机取流、拔线、四路带宽和 7×24 小时测试不属于本 ADR 的已执行证据。

## 参考

- [Visual Studio 2026 release notes](https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-notes)
- [Microsoft C++ binary compatibility 2015–2026](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)
- [CMake 4.2 release notes](https://cmake.org/cmake/help/v4.2/release/4.2.html)
- [Qt 6.10 supported platforms](https://doc.qt.io/qt-6.10/supported-platforms.html)
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)
