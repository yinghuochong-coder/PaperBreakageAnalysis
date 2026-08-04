# M6-02：传统视觉算法初版 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M6-02
- 关联需求：需求 4.7、4.9、7、11、13.2；架构 5.2、5.3、6～9、14、17、19；ADR-016

## 目的与可观察结果

新增可由 M6-01 `DetectorHost` 装载的 `classical-vision` 进程内编译期插件。它在经验证的 Mono8 只读帧上执行 ROI 灰度统计、帧间变化、纸幅占比和有界背景比较，返回完整 `DetectionResult`，且不修改采集或事件模块。由于 M6-00 缺少冻结数据集和批准阈值，插件及所有默认阈值明确标记为原型，不宣称正式断纸算法通过验收。

## 范围

### 范围内

- 独立 `paperbreak_algorithm_classical` CMake 目标，OpenCV 仅作为私有实现依赖；
- `classical-vision` 插件工厂/注册入口、强类型内部配置和参数解析；
- ROI、均值/帧间变化、纸幅占比、背景差分、置信度/区域/面积/原因/调试指标；
- 初始化、事务式热更新兼容、重置和版本/原型标记；
- 内存帧自动化测试、构建和路线图证据。

### 范围外

- M6-00 数据集冻结、指标/阈值批准和正式算法验收；
- M6-03 算法工作队列、连续帧确认、冷却、积压跳帧、报警和服务降级；
- M6-04 UI、配置 schema 或生产配置选择；
- 外部背景模板/模型制品、训练/调参、GPU 推理；
- 修改采集、事件、存储或 IPC 模块边界。

## 当前基线

- M6-01 已提供 `IBreakDetector`、`DetectorPluginRegistry` 和强回滚 `DetectorHost`；Mock 保持 M5 事件链兼容。
- 架构已列出 `paperbreak_algorithm_classical`，允许依赖 algorithm 与 OpenCV，禁止 OpenCV 类型进入公开接口。
- OpenCV 4.12.0 的 core/imgproc/imgcodecs 已由项目依赖基线批准并在本机预设配置。
- 工作树开始时干净；M6-00 状态为 blocked，缺少冻结数据、批准数值、目标 ROI/像素格式及目标机预算。

## 前置条件与假设

- 原型只接受 `PixelFormat::mono8`；目标生产像素格式尚未批准，其他格式返回稳定业务错误而不隐式错误解码。
- ROI 参数为像素坐标；宽高同时为零表示处理整帧，否则必须同时为正且落在当前帧内。
- 默认阈值只用于确定性原型和自动化测试，不代表生产批准值。
- 背景参考从 reset/初始化后的首个有效 ROI 建立；不读取外部模板。背景状态受固定像素上限约束。

## 设计说明

公开头文件只暴露插件 ID、创建和注册函数，不暴露 `cv::Mat`。实现用带 stride 的只读 `cv::Mat` 视图包装 `FrameView`，不复制输入帧。每帧计算归一化灰度均值、相邻均值变化、阈值分割后的纸幅占比，以及与背景参考的归一化绝对差和变化像素比例。低纸幅占比、背景变化、均值突变按确定顺序选择主触发原因，并输出归一化置信度；全部指标仍写入调试指标。

背景参考只在首帧复制 ROI，之后在非异常帧按固定可配置学习率更新；ROI 像素数超过编译期上限时拒绝处理。`reset`、初始化和热更新清除序号、均值与背景状态。配置解析拒绝未知、重复（宿主已拒绝）、错类型和越界参数。

### 线程和队列

本任务不创建线程或队列。插件与 M6-01 宿主一样由上层按相机串行调用；M6-03 才实现 `algorithm.frames[i]`/`algorithm.results` 有界通道。OpenCV 临时工作区仅存在于单次同步调用内。

### 持久化与恢复

不适用。背景参考和上一帧统计只保存在进程内，重启、reset 或配置切换后由首个有效帧重建；没有 schema、磁盘制品或用户数据迁移。

### 错误和降级

- 配置参数未知、类型错误、数值越界：`SYS_CONFIG_INVALID`；
- 未初始化、相机不匹配、帧不完整、非 Mono8、ROI 越界、序号/时间回退、ROI 超出内存上限或 OpenCV 失败：`ALGORITHM_PROCESS_FAILED`；
- 异常仍由 `DetectorHost` 的 M6-01 边界转为稳定插件异常；
- M6-03 才根据连续失败和积压执行服务级降级，本任务不伪造降级完成。

## 实施步骤

- [x] 1. 新增 classical 插件公开工厂/注册入口、私有配置解析与独立 CMake 目标。
- [x] 2. 实现 Mono8 ROI 统计、纸幅占比、帧间变化、有界背景比较、结果语义和生命周期状态。
- [x] 3. 新增确定性内存帧测试，覆盖注册/信息、正常基线、各触发分支、ROI/stride、热更新/reset 和错误输入。
- [x] 4. 运行定向测试、Debug/Release 全量构建与 CTest、静态分析/格式/差异检查，更新计划和路线图证据。

## 验证计划

### 自动化测试

- 通过注册表和 `DetectorHost` 装载，信息完整且 `prototype_only=true`；
- 首帧建立背景不误触发，结果含 ROI、均值、纸幅/变化/背景指标、耗时和版本；
- 低纸幅占比、均值变化、背景局部变化分别输出预期类型、来源、置信度和区域/面积；
- 非紧密 stride 与子 ROI 只统计批准区域；
- 参数未知/错类型/越界、非 Mono8、不完整帧、ROI 越界、序号/时间回退返回稳定错误；
- reset 和热更新清除背景/上一帧状态，失败更新由宿主保留旧实例。

### 构建与测试命令

```powershell
cmake --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug -R unit --output-on-failure
ctest --preset local-windows-vs2026-debug --output-on-failure
cmake --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release --output-on-failure
git diff --check
```

### 人工或硬件验证

- 不需要实体相机或 MVS 调用；自动化测试使用内存 `FrameView`。
- 冻结数据集、目标工控机四路 240 frame/s 性能、真实断纸准确性和生产阈值验证均未执行；原因是 M6-00 的外部输入仍未获批准。

## 回滚与恢复

移除独立 classical 目标、测试源和路线图 M6-02 记录即可回到 M6-01 基线。没有持久化格式或用户数据变更，不需要数据回滚，也不得删除生产数据。

## 验收标准

- [x] `classical-vision` 可经 M6-01 注册表/宿主装载，不改采集或事件代码；
- [x] ROI、灰度/变化量、纸幅占比和背景比较均有实现及自动化测试；
- [x] 结果包含异常、类型、置信、区域、面积比例、变化量、耗时、版本、原因和调试指标；
- [x] OpenCV 仅为实现目标私有依赖，公开头文件无 OpenCV 类型；
- [x] 状态内存有明确上限，reset/热更新行为确定；
- [x] Debug/Release 构建和非硬件 CTest 已运行并如实记录；
- [x] 插件与路线图保持“原型”，未绕过 M6-00 门禁或进入 M6-03/M6-04。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、计划规范、M6-00/M6-01 计划及现有检测器/帧/CMake/测试基线；确认工作树干净，创建计划，状态 in-progress。
- 2026-08-04：完成独立 OpenCV classical 目标、插件工厂、参数合同、ROI/灰度/纸幅/背景算法、生命周期和 9 项内存帧测试；Debug/Release、CTest、静态分析与任务格式检查通过，状态 completed。

## 决策记录

- DEC-001：使用独立 classical 静态目标，OpenCV 仅 PRIVATE 链接，保持 ADR-016 的内部 C++ 插件边界。
- DEC-002：在缺少批准背景模板时以首个有效 ROI 建立进程内参考，reset/热更新重建；不引入未登记外部制品。
- DEC-003：像素格式仅接受 Mono8，并对背景状态设置固定像素上限；不猜测尚未批准的 Mono10/Mono12/Bayer 打包和转换合同。
- DEC-004：置信度直接报告未经数据集校准的归一化异常分数，不用原型阈值伪造概率；纸幅、背景和均值分支分别使用缺失比例、背景平均差和均值差。
- DEC-005：缺纸帧不建立背景，背景只由首个满足纸幅条件的有效 ROI 建立，并仅在非异常帧按有界学习率更新，避免明显异常污染参考。

## 意外发现

- 首次 Debug 编译发现 `cv::Scalar` 的列表初始化会把 `uint8_t` 视为收缩转换；显式转换为 `double` 后通过 `/W4 /WX`。
- `ctest -R unit` 同时匹配 Hikrobot adapter unit；这是现有 CTest 命名匹配行为，未触碰实体设备，只执行 SDK 枚举 smoke。
- 全仓 `format-check` 首先失败于本任务未修改的 `src/console/main.cpp:467` 等既有格式差异；6 个任务 C++ 文件用同一 clang-format 单独检查通过，未进行无关格式重构。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | 初始工作树和代码/文档基线检查 | 通过 | 工作树干净；M6-02 尚无实现或计划；M6-00 仍 blocked。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter=AlgorithmClassicalVision.*` | 通过 | 9/9；覆盖装载/版本、ROI/stride、纸幅/均值/背景三分支、异常背景保护、reset/热更新、配置和帧错误。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | Debug `/W4 /WX` 全量构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | 24/24，包含 255 项 unit、simulation、无相机 MVS smoke、Qt/服务/安装树和 M6-00 阻塞门禁。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-release` | 通过 | Release `/W4 /WX` 全量构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | 24/24。 |
| 2026-08-04 | 静态分析预设构建 `paperbreak_algorithm_classical` | 通过 | MSVC `/analyze` 未报告 classical 或其依赖目标错误。 |
| 2026-08-04 | 任务 C++ 文件 clang-format `--dry-run --Werror`；`git diff --check` | 通过 | 6 个相关 C++ 文件格式通过，无空白错误。 |
| 2026-08-04 | 全仓 `format-check` | 既有阻塞 | 首个失败为未修改的 `src/console/main.cpp:467`；未进行无关格式重构。 |
| 2026-08-04 | 实体相机、冻结数据集、四路目标机性能、正式断纸验收 | 未执行 | M6-00 缺少批准数据/指标/目标输入与硬件预算；插件保持 `prototype_only=true`。 |

## 完成摘要

完成独立 `classical-vision` 原型插件及参数/结果合同；OpenCV 仅在实现目标内部使用，最大 ROI 和三份 8 位工作区有固定上限。算法输出纸幅占比、均值变化和健康背景比较结果，异常不会污染背景，初始化/reset/热更新均确定性重建状态。9 项定向测试、Debug/Release 全量构建、两套完整 CTest、MSVC 静态分析、任务文件格式和差异检查通过；采集与事件模块未修改，M6-03/M6-04 未开始。M6-00 仍 blocked，未执行的数据集、目标机和实体相机验证不被宣称通过。
