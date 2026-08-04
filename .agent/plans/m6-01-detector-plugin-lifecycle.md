# M6-01：检测器插件接口与生命周期 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-04
- 最后更新：2026-08-04
- 路线图条目：`docs/roadmap/development-roadmap.md` M6-01
- 关联需求：需求 4.7、4.9、7、11、13.2；架构 5.2、7～9、14、17、19

## 目的与可观察结果

提供不依赖 Hikrobot、存储、UI 或具体传统视觉实现的 `IBreakDetector` 边界，以及负责插件选择、初始化、事务式热更新、处理计时、异常翻译、重置和信息查询的宿主。编译期注册的 Mock 和后续传统视觉实现可在不改变采集/事件领域接口的前提下替换。自动化测试必须证明未知插件、工厂/初始化失败、配置回滚、插件异常、慢处理和重置语义。

## 范围

### 范围内

- `DetectorConfig`、`DetectionResult`、`DetectorInfo` 和 `IBreakDetector`；
- 进程内编译期插件注册表和单检测器宿主；
- 宿主生命周期状态、稳定业务错误、异常边界和同步处理耗时预算观测；
- 通过候选新实例实现配置更新失败时保留旧实例/旧配置；
- M5 Mock 检测器迁移并保持既有 `TriggerResult`/`ITriggerDetector` 源码兼容；
- ADR、单元测试和路线图完成证据。

### 范围外

- M6-02 传统视觉算法、阈值调参或 OpenCV 算法实现；
- M6-03 算法工作队列、积压跳帧、连续故障报警和服务级降级；
- M6-04 UI；
- 运行时 DLL ABI、第三方动态插件、进程外强制超时或沙箱；
- 改变采集、事件或持久化模块边界。

## 当前基线

- `paperbreak_algorithm` 仅有 M5 `TriggerResult` 和单方法 `ITriggerDetector`；Mock 在 `create` 时验证自身配置并由服务直接持有。
- 事件层只消费 `TriggerResult`，不依赖具体检测器；服务组合根直接调用 Mock。
- M6-00 保持 blocked，故本任务所有检测实现仍是原型，不能宣称正式算法验收通过。
- 工作树开始时干净，没有 M6-01 半成品。

## 前置条件与假设

- 第一版只注册随程序构建并由相同 MSVC/C++ 运行库编译的工厂；不跨 DLL 暴露 C++ ABI。
- `process` 是同步调用。宿主可以测量并报告超预算，但进程内 C++ 无法安全抢占正在执行的插件；硬超时隔离需要以后批准进程外方案。
- 每个宿主绑定一台逻辑相机；同一宿主的生命周期和 `process` 由上层串行调用。M6-03 才引入工作线程/队列。

## 设计说明

`DetectorConfig` 保存稳定身份、相机、修订号、同步处理预算和有界的通用参数集合；不暴露 JSON/OpenCV/MVS 类型。`DetectionResult` 扩充 M5 触发结果所需字段，并通过类型别名保持事件代码兼容。`IBreakDetector` 提供 initialize/process/updateConfig/reset/info。

`DetectorPluginRegistry` 只保存编译期工厂。`DetectorHost::load` 从注册表创建并初始化候选实例，全部成功后才替换活动实例。`updateConfig` 再创建候选实例，用当前配置初始化后对候选调用热更新；任何失败或异常都丢弃候选，原活动实例和修订保持不变。处理异常被翻译为稳定错误；超过配置预算的同步调用完成后返回超时错误并记录耗时，不伪装为可抢占超时。

### 线程和队列

本任务不创建线程或队列。注册表在装配阶段写入，运行后只读；每个 `DetectorHost` 由一个算法执行上下文串行调用。M6-03 的 `algorithm.frames[i]` 和 `algorithm.results` 有界队列不在本任务实现。

### 持久化与恢复

不持久化。插件 ID、配置修订和实现版本可由上层配置/事件记录；进程重启后由组合根重新注册并装载。热更新失败保留内存中的旧实例，不改磁盘配置。

### 错误和降级

- 未知插件/空工厂：`ALGORITHM_PLUGIN_LOAD_FAILED`；
- 初始化或热更新拒绝：保留插件业务错误，并由宿主补充阶段；
- 插件抛异常：`ALGORITHM_PLUGIN_EXCEPTION`；
- 未装载即处理/重置/查询：`ALGORITHM_NOT_READY`；
- 同步处理超预算：`ALGORITHM_PROCESS_TIMEOUT`，调用已经返回但结果被丢弃；
- M6-03 才根据连续故障实施报警和降级，本任务只返回可观测错误。

## 实施步骤

- [x] 1. 新增检测器数据模型、生命周期接口、配置/结果校验以及源码兼容别名。
- [x] 2. 实现编译期注册表和宿主，覆盖装载、事务式热更新、异常翻译、计时预算、重置与信息查询。
- [x] 3. 迁移 Mock 检测器实现新生命周期，同时保持既有 M5 行为和服务组合可构建。
- [x] 4. 新增脚本化测试插件，验证加载/初始化/更新/处理/慢调用/重置全部验收场景。
- [x] 5. 新增 ADR、更新路线图，运行格式、Debug/Release 构建、相关测试、完整非硬件 CTest 和差异检查。

## 验证计划

### 自动化测试

- 配置和结果模型边界值；
- 未知插件、工厂失败、初始化失败不替换活动实例；
- 热更新成功切换修订，失败/异常仍由旧实例继续处理；
- `process` 抛 `std::exception`/未知异常被隔离；
- 慢处理完成后按实测耗时返回预算超限；
- 重置成功、失败和异常；
- 既有 Mock 手工/周期/灰度/ROI 行为不回归。

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

- 不需要实体相机或 MVS 运行时行为验证；全部新增测试使用内存帧和脚本化检测器。
- 未执行冻结数据集、四路目标机性能或真实断纸验证；M6-00 仍阻塞，不能声明算法质量或硬件性能通过。

## 回滚与恢复

撤销新增检测器宿主/测试/ADR，并恢复 Mock 对旧单方法接口的实现即可回到 M5 基线。无数据格式、数据库或用户配置迁移，不删除任何生产数据。

## 验收标准

- [x] `IBreakDetector` 及四个数据/信息模型可由 Mock 和未来传统视觉实现复用；
- [x] 编译期插件装载策略和 ABI 边界有 Accepted ADR；
- [x] 初始化、处理、热更新、重置和版本查询可通过宿主调用；
- [x] 加载/初始化失败及配置更新失败不会破坏已有活动实例；
- [x] 处理异常和慢处理转换为稳定、可观测业务错误；
- [x] 自动化测试、构建和非硬件 CTest 已运行且结果如实记录；
- [x] 未修改采集/事件模块边界，未进入 M6-02。

## 进度记录

- 2026-08-04：阅读需求、架构、路线图、计划规范、M6-00 结论和 M5 算法/事件/服务基线；确认工作树干净，创建计划，状态 in-progress。
- 2026-08-04：完成接口、编译期注册表、串行宿主、候选实例强回滚、异常/软超时隔离、Mock 生命周期和 ADR；Debug/Release 全量构建及 CTest 通过，状态 completed。

## 决策记录

- DEC-001：首版采用进程内编译期工厂注册，不开放运行时 DLL C++ ABI，避免 MSVC/STL/Qt/OpenCV 跨模块 ABI 和供应链风险。
- DEC-002：热更新以候选实例完成“旧配置初始化 + 新配置 updateConfig”后原子交换，牺牲少量初始化成本换取宿主可保证的强回滚。
- DEC-003：同步进程内调用只提供软预算观测；慢调用完成后丢弃结果并报超时，不声称能够安全抢占插件。

## 意外发现

- 静态分析预设按项目基线不配置 `BUILD_TESTING`，因此不存在 `PaperBreakTests` 目标；算法与 Mock 库本身的静态分析构建成功，测试代码仍由 Debug/Release `/W4 /WX` 编译覆盖。
- 全仓 `format-check` 首先失败于本任务未修改的 `src/console/main.cpp:467` 等既有格式差异；本任务 8 个 C++ 文件已用同一 clang-format 执行 `--dry-run --Werror` 并通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-04 | 初始工作树和代码基线检查 | 通过 | 工作树干净；M6-01 尚无代码或计划。 |
| 2026-08-04 | `PaperBreakTests.exe --gtest_filter='AlgorithmDetector*:AlgorithmMockTrigger*'` | 通过 | 15/15；覆盖注册、装载、初始化、强回滚、异常、软超时、重置、版本及 Mock 回归。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | Debug `/W4 /WX` 全量构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-debug --output-on-failure` | 通过 | 24/24，含 unit、simulation、无相机 MVS smoke、Qt/服务/安装树和 M6-00 阻塞门禁。 |
| 2026-08-04 | `cmake --build --preset local-windows-vs2026-release` | 通过 | Release `/W4 /WX` 全量构建成功。 |
| 2026-08-04 | `ctest --preset local-windows-vs2026-release --output-on-failure` | 通过 | 24/24。 |
| 2026-08-04 | 静态分析预设构建 `paperbreak_algorithm`、`paperbreak_algorithm_mock` | 通过 | MSVC `/analyze` 未报告本任务库错误；该预设不生成测试目标。 |
| 2026-08-04 | 本任务 C++ 文件 clang-format `--dry-run --Werror`；`git diff --check` | 通过 | 8 个相关 C++ 文件格式通过，无空白错误。 |
| 2026-08-04 | 全仓 `format-check` | 既有阻塞 | 首个失败为未修改的 `src/console/main.cpp:467`；未进行无关格式重构。 |
| 2026-08-04 | 实体相机、冻结数据集、四路目标机性能、正式断纸验收 | 未执行 | M6-01 不依赖硬件；M6-00 仍缺少批准数据和指标，检测器保持原型。 |

## 完成摘要

完成内部 `IBreakDetector` 生命周期边界、编译期插件注册表和事务式宿主；失败和异常不会替换活动实例，慢处理以实测软预算错误和指标呈现。Mock 已迁移且 M5 事件链保持兼容，ADR-016 固定首版 ABI/装载决策。Debug/Release 及全部非硬件自动化验证通过；没有进入传统视觉算法、算法队列/降级或 UI 范围，M6-00 的外部验收阻塞保持不变。
