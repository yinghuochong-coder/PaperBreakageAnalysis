# G0-03：记录依赖和工具链决策 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-07-31
- 最后更新：2026-07-31
- 路线图条目：G0-03
- 关联需求：需求第 2、9、14、16 节；架构第 5、16.3、19、20 节

## 目的与可观察结果

建立 Windows x64 工具链、Qt/OpenCV/MVS 外部 SDK、vcpkg 依赖、许可证、路径注入和 CI 执行器基线。完成后，评审者可以从 `docs/architecture/dependencies.md` 查到每项生产/测试依赖的用途、版本、来源、许可证和升级规则，并从 ADR 查到选择依据与替代方案。

## 范围

### 范围内

- Visual Studio 2026、MSVC v145、CMake、Ninja、C++20 和 x64 基线；
- Qt 6.10.2 组件与 MSVC 2022 二进制兼容策略；
- OpenCV 4.12.0 和 Hikrobot MVS SDK 4.8.0.3 外部 SDK 边界；
- vcpkg manifest、baseline、triplet 和二进制缓存策略；
- spdlog、nlohmann/json、GoogleTest、SQLite3 与可选 zstd 的版本、用途和许可证；
- 本地绝对路径隔离、Release 产物检查和 Windows CI 执行器类别；
- 路线图 DEC-001～DEC-003 的结论及 G0 退出门禁状态。

### 范围外

- 创建 CMake 工程、预设、vcpkg manifest、CI workflow 或构建脚本；
- 下载、升级或重新安装任何 SDK、工具或依赖；
- 实现 MVS 适配器、OpenCV 算法、日志、数据库或压缩功能；
- 注册具体 CI 平台 runner，执行真实相机或硬件集成测试；
- 开始 M0 或后续里程碑。

## 当前基线

- 仓库只有规划、架构与领域文档，没有源码、CMake 工程、预设、测试或 CI 配置；
- 本机已安装 Visual Studio Community 2026 18.6、MSVC 19.51.36243、CMake 4.2.3、Ninja 1.13.2；
- 本机存在 Qt 6.10.2 `msvc2022_64`，并包含 Core、Gui、Widgets、Network、Concurrent；
- 本机 OpenCV 头文件报告 4.12.0，MVS 注册表报告 Development/Runtime 4.8.0.3；
- 本机 Visual Studio 随附 vcpkg 2026-03-04，但独立 `vcpkg`、`ninja` 和 `cl` 未加入普通 PowerShell 的 PATH；
- 仓库无 Git remote，且没有已注册 CI runner 的证据；
- 工作区在任务开始时干净，既有 G0-01/G0-02 为已提交基线。

## 前置条件与假设

- 目标只支持 Windows 10/11 x64 和 C++20；
- Qt 使用官方 `msvc2022_64` 动态库；VS 2026 v145 依据 Microsoft 的 v143/v145 二进制兼容保证链接；
- Qt 商业许可证是否已采购未知；若采用 LGPLv3，发布前必须完成动态链接、通知、许可证文本、对应源码和可替换库等合规审查；
- MVS SDK 的分发权由供应商许可决定，未取得许可前不得把 SDK 安装包、头文件、库或运行时提交到仓库；
- CI 平台尚未选定，本任务只固定所需 Windows 执行器能力；注册和 workflow 属于 M0-04。

## 设计说明

项目级文件只记录版本、逻辑变量和相对路径。Qt、OpenCV、MVS 和工具安装根目录由环境变量或不提交的 `CMakeUserPresets.json` 注入。开源 C/C++ 依赖使用 vcpkg manifest mode，通过提交的 `builtin-baseline` 和 `x64-windows` triplet 形成可复现依赖图；升级必须通过单独评审并重新执行许可证、构建和测试检查。

工具链使用 VS 2026 stable、MSVC v145、C++20 和动态 MSVC 运行库。CMake 最低 4.2，因为该版本首次提供 Visual Studio 18 2026 generator；当前验证基线为 4.2.3。Ninja 是 CI 和日常预设的首选生成器，Visual Studio generator 作为受支持的开发入口。

### 线程和队列

不适用。本任务只记录依赖和构建决策，不新增线程或队列。spdlog 异步队列的容量、溢出和关闭行为属于 M0-03，仍须遵守架构中的有界队列规则。

### 持久化与恢复

本任务不写生产数据。vcpkg manifest/baseline 和构建元数据属于可重建配置；升级失败时恢复上一 baseline 和版本约束。SQLite 数据库 schema 与 SQLite 库版本独立演进，不因库升级自动迁移业务 schema。

### 错误和降级

- 缺少必需工具或 SDK 时配置阶段失败并报告逻辑依赖名，不回显不必要的绝对路径；
- 未启用 MVS 时必须仍可构建 Mock 测试；MVS 目标不得以静默 stub 冒充生产适配器；
- zstd 默认关闭，缺少时事件存储使用未压缩格式，不改变事件完整性；
- 许可证或分发权未通过审查时阻止发布，不通过下载其他包静默替代；
- CI 默认测试不依赖实体相机，硬件任务独立标记并在缺少硬件时 skipped。

## 实施步骤

- [x] 1. 阅读需求、架构、路线图 G0-03 和 ExecPlan 规范，检查工作区及现有 ADR 索引。
- [x] 2. 盘点本机 VS、MSVC、CMake、Ninja、Qt、OpenCV、MVS 和 vcpkg 的真实版本与组件。
- [x] 3. 编写 `docs/architecture/dependencies.md`，记录工具链、依赖、许可证、获取、路径、升级和 CI 规则。
- [x] 4. 编写 ADR，并同步需求技术基线、系统架构 ADR 索引和路线图 DEC-001～DEC-003/任务状态。
- [x] 5. 运行文档契约、版本一致性、链接和绝对路径检查，回写验证证据和完成摘要。

## 验证计划

### 自动化测试

- 检查依赖文档、ADR 和必需章节存在；
- 检查 VS/CMake/Qt/OpenCV/MVS 版本在需求、依赖文档、ADR 和路线图中一致；
- 检查五项指定依赖均有用途、版本、来源和许可证；
- 检查外部 SDK、vcpkg manifest/baseline、`x64-windows` 和可选 zstd 规则齐全；
- 检查提交文件没有开发机 SDK 绝对路径作为配置值；
- 检查 ADR 索引、DEC-001～DEC-003 和 G0-03 状态可追踪。

### 构建与测试命令

```powershell
cmake --preset windows-vs2026-debug
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug
```

仓库尚无 CMake 工程或预设，本任务不会为绕过缺失而提前实施 M0-01。上述命令预计无法执行，验证证据必须如实记录。

### 人工或硬件验证

- 环境：当前 Windows 开发机和官方文档；
- 步骤：核对已安装工具/SDK 版本、Qt 组件、MSVC 兼容性、许可证和 CI 能力清单；
- 预期：决策可由本机事实与官方来源复核，默认测试不依赖 MVS 或实体相机；
- 证据保存位置：本 ExecPlan 的“验证证据”和依赖文档来源章节；
- 实体相机/MVS 运行测试：未执行且不属于本任务。

## 回滚与恢复

本任务只修改 Markdown，不更改已安装工具、SDK、程序或生产数据。评审不通过时恢复 G0-03 修改前的文档内容，保留 G0-01/G0-02 基线；不得删除本机 SDK 或用户配置。未来依赖升级失败时恢复上一已批准 vcpkg baseline、外部 SDK 版本和锁定文件。

## 验收标准

- [x] 存在依赖文档和已接受 ADR；
- [x] VS 2026、MSVC、CMake 最低版本、Qt 版本与组件明确；
- [x] Qt、OpenCV、MVS 明确为外部 SDK，其他批准依赖使用 vcpkg manifest；
- [x] spdlog、nlohmann/json、GoogleTest、SQLite3、可选 zstd 的用途、版本和许可证齐全；
- [x] 外部 SDK 根目录只允许通过环境或用户预设注入；
- [x] Windows CI 执行器能力、默认 Mock lane 和硬件 lane 边界明确；
- [x] DEC-001～DEC-003 有结论，G0 退出门禁可审查；
- [x] 文档验证通过，并如实记录未执行的构建、CI 注册和硬件测试。

## 进度记录

- 2026-07-31：读取任务基线、盘点本机环境并创建计划，状态 in-progress。
- 2026-07-31：完成依赖清单、ADR、需求/路线图同步和文档验证，状态 completed。

## 决策记录

- DEC-001：采用 Visual Studio 2026 stable、MSVC v145、C++20 和 x64；记录精确编译器版本但允许受控安装安全/修复更新。
- DEC-002：CMake 最低 4.2，当前验证基线 4.2.3；Ninja 为首选生成器，Visual Studio 18 2026 generator 为受支持入口。
- DEC-003：Qt 6.10.2 `msvc2022_64`、OpenCV 4.12.0 和 MVS SDK 4.8.0.3 作为外部 SDK；开源基础依赖由固定 baseline 的 vcpkg manifest 管理。
- DEC-004：CI 需要 Windows 11 x64 自托管执行器能力；默认 lane 使用 Mock，不安装/调用 MVS，硬件 lane 独立。

## 意外发现

- 需求中的 Qt 路径含错误目录片段 `msvc2022\_64`，本机实际 kit 为 `msvc2022_64`；
- CMake 3.27 不支持 Visual Studio 18 2026 generator，官方从 4.2 才加入，必须提高最低版本；
- Qt 6.10 官方列出的 Windows 编译器是 MSVC 2022；VS 2026 v145 的使用依据是 Microsoft 的 v143/v145 二进制兼容保证，需在 M0 做链接/启动测试；
- MVS 应用版本 5.0.1 与开发/运行 SDK 版本 4.8.0.3 不同，项目依赖应记录后者；
- 当前机器具备 runner 所需工具，但仓库没有远端或 CI 注册证据，不能声称 CI 已上线。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-07-31 | `vswhere`、`cl`、`cmake --version`、`ninja --version` | 通过 | VS 18.6、MSVC 19.51.36243、CMake 4.2.3、Ninja 1.13.2 |
| 2026-07-31 | Qt `qtpaths` 和模块 Config 检查 | 通过 | Qt 6.10.2；Core/Gui/Widgets/Network/Concurrent 均存在 |
| 2026-07-31 | OpenCV 版本头与 MVS 注册表检查 | 通过 | OpenCV 4.12.0；MVS Development/Runtime 4.8.0.3 |
| 2026-07-31 | 官方 CMake/Qt/Microsoft/vcpkg/依赖来源核对 | 通过 | 版本、兼容性、manifest 和许可证依据已取得 |
| 2026-07-31 | pinned vcpkg baseline 五项 port manifest 核对 | 通过 | spdlog 1.17.0#1、nlohmann-json 3.12.0#2、gtest 1.17.0#3、sqlite3 3.53.4、zstd 1.5.7 |
| 2026-07-31 | PowerShell G0-03 文档契约检查 | 通过 | 工具链、外部 SDK、五项依赖、许可证、路径、CI lane、ADR 和 DEC 14/14 检查通过 |
| 2026-07-31 | 代码围栏、本地链接、提交配置路径和 `git diff --check` | 通过 | 围栏/链接正确；仓库尚无构建配置候选；无空白错误，仅有 Git 的 LF→CRLF 提示 |
| 2026-07-31 | `cmake --help` | 通过 | 本机 CMake 4.2.3 包含 `Visual Studio 18 2026` generator |
| 2026-07-31 | `cmake --preset windows-vs2026-debug` | 未通过 | 已执行，退出码 1；M0-01 尚未创建 `CMakePresets.json` |
| 2026-07-31 | `cmake --build --preset windows-vs2026-debug` | 未通过 | 已执行，退出码 1；缺少 `CMakePresets.json`，没有可构建工程 |
| 2026-07-31 | `ctest --preset windows-vs2026-debug` | 未通过 | 已执行，退出码 1；缺少 `CMakePresets.json`，没有已配置测试 |
| 2026-07-31 | CI runner 注册与 workflow | 未执行 | 仓库无 remote/CI 配置；属于 M0-04，不声称 runner 已上线 |
| 2026-07-31 | 实体相机/MVS SDK 运行或硬件测试 | 未执行 | 本任务只读取 SDK 版本，不实现或运行适配器 |

## 完成摘要

新增 `docs/architecture/dependencies.md` 和 ADR-015，固定 VS 2026/v145、CMake 4.2、Qt 6.10.2、OpenCV 4.12.0、MVS SDK 4.8.0.3、vcpkg baseline、五项依赖版本/许可证、路径注入和 CI lane。需求、架构 ADR 索引及路线图 DEC-001～DEC-003 已同步。文档检查通过；因 M0-01 尚未创建 CMake 工程/预设，三个约定构建测试命令已执行但均因缺少 `CMakePresets.json` 未通过。未注册 CI runner，未执行硬件测试。
