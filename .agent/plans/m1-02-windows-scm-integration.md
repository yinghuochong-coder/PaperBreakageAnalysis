# M1-02：Windows SCM 集成 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-02
- 关联需求：需求 4.1；架构 5.2、13、15、16、19

## 目的与可观察结果

为 `PaperBreakEdgeService` 增加可测试的 Windows SCM 安装、卸载、状态上报和异常恢复宿主。安装命令保存显式配置路径，服务复用 `ServiceRuntime`，控制回调不执行耗时关闭。自动化测试不修改真实 SCM；提升权限的隔离测试机验证单独记录。

## 范围

### 范围内

- `--install --config <path>`、`--uninstall` 和内部 `--service --config <path>`；
- SCM API 抽象、Win32 实现、安装配置收敛、幂等卸载；
- START_PENDING、RUNNING、STOP_PENDING、STOPPED 状态和停止控制；
- 自动启动、LocalService、5/15/60 秒恢复、24 小时失败计数重置和 30 秒预关机时限；
- 单元/CLI 测试、错误码、开发文档、路线图和验证证据。

### 范围外

- M1-03 强类型配置、默认 `%ProgramData%` 布局和 ACL 管理；
- IPC、相机、事件、上传及后续里程碑；
- 安装器、专用账户创建、实体相机、MVS SDK 和系统重启测试。

## 当前基线

- M1-01 已完成，`ServiceRuntime` 与控制台宿主分离；
- 服务只支持 console、配置校验和版本模式，配置路径必须显式提供；
- `paperbreak_windows_service` 仅封装 `SetConsoleCtrlHandler`；
- 2026-08-01 Debug 构建与默认非硬件 CTest 14/14 通过；任务开始时工作区干净；
- 当前终端属于管理员组但未提升，不能执行真实服务安装。

## 前置条件与假设

- 服务名和显示名固定为 `PaperBreakEdgeService`；
- 安装时把当前可执行文件和规范化绝对配置路径写入 SCM ImagePath；
- 服务使用 `NT AUTHORITY\\LocalService`，配置文件 ACL 由部署环境保证；
- 安装只注册服务，不启动；卸载先有界停止再删除；
- 用户明确要求忽略系统重启验证。

## 设计说明

`paperbreak_windows_service` 对外仅暴露标准 C++ DTO、Result 和可注入的 SCM API 接口。业务编排通过接口实现安装/卸载状态机，Win32 源文件负责句柄 RAII、宽字符转换和错误映射。SCM 宿主在 ServiceMain 注册控制处理器后创建与 console 模式相同的运行时；控制回调只记录首个停止原因、请求启动取消并唤醒宿主。

安装已存在的服务时更新 ImagePath、自动启动、LocalService、描述、恢复动作和预关机时限。新建服务后配置失败会删除新服务；已有服务更新失败返回明确错误，不谎报成功。卸载不存在或已标记删除为成功；运行中服务在 30 秒内未停止时保留注册并失败。

### 线程和队列

| 通道 | 生产者 | 消费者 | 容量 | 满载策略 | 停止/排空行为 | 指标 |
| --- | --- | --- | ---: | --- | --- | --- |
| SCM 停止请求 | Win32 控制回调 | ServiceMain 宿主线程 | 1 个合并槽 | 首个原因生效，后续幂等 | 唤醒宿主并进入 `ServiceRuntime::shutdown` | M1-06 接入 |
| pending 状态脉冲 | `std::jthread` | SCM | 无队列 | 每秒覆盖状态 checkpoint | 状态转换前 request_stop + join | M1-06 接入 |

不新增业务队列。状态线程只在启动/关闭阶段存在，持有停止令牌并确定性 join。

### 持久化与恢复

SCM 数据库保存服务 ImagePath、账户、描述、自动启动、失败动作及预关机时限。配置文件只读，不写入业务数据。失败动作是 5、15、60 秒重启，失败计数 24 小时后重置，并启用非崩溃失败恢复。

### 错误和降级

- 安装、卸载、宿主控制分别使用 `SYS_SERVICE_INSTALL_FAILED`、`SYS_SERVICE_UNINSTALL_FAILED`、`SYS_SERVICE_CONTROL_FAILED`；
- Win32 原始错误保留为 `nativeDomain=win32` 和十进制码；
- 参数/配置错误退出 2，SCM/运行时失败退出 1；
- 权限不足不自提升；服务启动失败向 SCM 上报 service-specific failure 并停止。

## 实施步骤

- [x] 1. 新增纯 C++ SCM DTO/API、安装/卸载状态机和命令行构造器，并以 fake API 覆盖幂等及失败路径。
- [x] 2. 新增 Win32 SCM API RAII 实现，配置自动启动、LocalService、描述、恢复动作、失败动作标志和预关机时限。
- [x] 3. 新增 SCM ServiceMain 宿主、状态脉冲和控制回调，复用 `ServiceRuntime` 装配。
- [x] 4. 扩展服务 CLI 并补充 parser/CLI 集成测试。
- [x] 5. 更新错误码、开发文档、路线图和本计划证据。

## 验证计划

### 自动化测试

- fake SCM：首次/重复安装、权限拒绝、配置失败回滚、卸载不存在/运行中/超时/标记删除；
- 命令行引用：Unicode、空格、反斜杠和双引号；
- 宿主状态：四种状态、checkpoint、控制码、首原因和异常屏障；
- CLI：模式冲突、配置必填、权限拒绝且不产生真实注册。

### 构建与测试命令

```powershell
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-release
ctest --preset local-windows-vs2026-release
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --preset local-windows-vs2026-static-analysis
cmake --build --preset local-windows-vs2026-static-analysis
git diff --check
```

### 人工或平台验证

- 环境：提升权限的隔离 Windows 10/11 x64 测试机；
- 步骤：重复安装、查询配置、启动/停止、强制终止观察恢复、重复卸载；
- 预期：自动启动、LocalService、状态流转、5/15/60 恢复、无界面且幂等；
- 系统重启验证按用户要求不执行；无提升测试机时记录“待目标机验证”。

## 回滚与恢复

新增代码只改变程序和 SCM 注册。新建服务配置失败时主动删除该服务；人工验证结束后执行幂等卸载。不得删除用户配置。代码回滚移除 SCM 源文件并恢复 CLI、测试和文档即可回到 M1-01 可构建状态。

## 验收标准

- [x] CLI 和 SCM 安装/卸载语义符合批准方案；
- [x] 服务状态、控制和异常恢复配置正确且可由 fake API 验证；
- [x] Win32 类型不泄漏至核心业务接口，回调不执行耗时关闭；
- [x] Debug/Release、默认 CTest、格式和静态分析通过；
- [x] 文档和证据完整，真实 SCM 未验证时明确标记限制。

## 进度记录

- 2026-08-01：创建计划，状态 `in-progress`；确认 Debug 基线 14/14 通过。
- 2026-08-01：完成 SCM 封装、Win32 宿主、CLI、测试和文档；状态 `completed`，真实提升权限平台验证待目标机执行。

## 决策记录

- DEC-001：安装配置路径必填，不提前固化 M1-03 默认路径。
- DEC-002：服务账户使用内置低权限 LocalService，不在本任务修改 ACL。
- DEC-003：恢复策略为 5/15/60 秒，24 小时重置，后续使用最后一项动作。
- DEC-004：安装只注册不启动；卸载停止超时不删除服务。

## 意外发现

- 当前 PowerShell 未提升，真实 SCM 安装验证需隔离提升环境。
- Win32 文档确认 LocalService 必须使用非本地化名称 `NT AUTHORITY\LocalService`；密码参数使用空字符串。
- SCM 在失败次数超过动作数组长度后重复最后一项，三项恢复动作满足后续持续 60 秒重启策略。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | M1-02 前 Debug build + CTest | 通过 | 14/14 默认非硬件测试通过 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | `/W4 /WX` Debug 构建成功 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 17/17；unit 中 34 个用例通过 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-release` | 通过 | Release 构建成功 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-release` | 通过 | 17/17 默认非硬件测试通过 |
| 2026-08-01 | Debug `format-check` | 通过 | clang-format 无违规 |
| 2026-08-01 | static-analysis configure + build | 通过 | MSVC `/analyze` 构建成功 |
| 2026-08-01 | 非提升 `--install`、`--uninstall` | 通过 | 均退出 1，分别返回安装/卸载业务码和 `win32:5`；前后服务均不存在 |
| 2026-08-01 | `git diff --check` | 通过 | 无空白错误；仅 Git LF→CRLF 提示 |
| 2026-08-01 | 提升权限 SCM 安装/启动/停止/恢复 | 待目标机验证 | 当前终端为中等完整性级别；未声称通过 |
| 2026-08-01 | 系统重启验证 | 未执行 | 用户明确要求忽略 |

## 完成摘要

新增纯 C++ SCM DTO/API、Win32 句柄适配和 ServiceMain 宿主；服务 CLI 支持显式配置路径的幂等安装、管理员幂等卸载及内部 SCM 模式。服务注册策略为自动启动、LocalService、5/15/60 秒恢复、24 小时重置和 30 秒预关机；pending 状态由可停止并确定性 join 的 `std::jthread` 更新 checkpoint。新增 fake SCM、状态、控制、命令行引用及 CLI 集成测试，更新错误码和开发文档。未执行提升权限的真实 SCM、Session 0 和强杀恢复验证，已登记待隔离目标机验证。
