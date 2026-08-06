# M4：本机普通用户授权调整 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-06
- 最后更新：2026-08-06
- 路线图条目：M4-05、M4-06 既有运行期 IPC 权限策略的用户授权调整
- 关联需求：`docs/requirements/edge-system-requirements.md` 第 12 节

## 目的与可观察结果

PaperBreakEdgeConsole 以未提升的普通 Windows 用户运行时，可以调用全部本机 IPC 查询和变更功能，不再因客户端不属于管理员组而收到 `IPC_UNAUTHORIZED`。未认证或非本机客户端仍被统一拒绝；服务停止、请求校验、乐观修订、审计和远程命令确认语义不变。

## 范围

### 范围内

- 统一本机 IPC 的身份基线为 `local && authenticated`。
- 去掉上位机、存储、算法、事件、相机、报警和配置重载的管理员身份要求。
- 安装/收敛服务配置时设置最小服务对象 ACL，使普通用户可从控制台重启该服务。
- 更新系统命令单元测试，使普通本机用户覆盖全部变更命令，并保留未认证/非本机拒绝测试。
- 更新需求、架构和 IPC 协议文档。

### 范围外

- Windows 服务安装、卸载、服务配置修改和删除；这些操作继续遵循 Windows 管理员权限。
- 数据目录 ACL、服务运行账户和供应商驱动安装权限。
- 远程 Uplink v1 的操作员确认、审计和网络安全策略。
- 实体相机或 SCM 提权测试。

## 当前基线

- `SystemCommandService::handle_with_source` 在六类业务分支读取 `PeerIdentity::administrator`。
- 相机命令已调整为已认证本机用户，但其他变更命令仍拒绝非管理员。
- 命名管道授权器能够识别本机来源、认证状态和客户端 SID。
- 工作区已有不属于本任务的 `src/service/main.cpp` 修改；必须保留。

## 前置条件与假设

- 普通用户通过本机命名管道访问后台服务，实际设备和文件操作仍由服务账户执行。
- 用户明确接受所有本机已认证账户具备生产配置修改能力。
- Windows 服务安装/卸载无法通过删除应用内管理员判断来降权。

## 设计说明

在公开的本机 `handle` 入口执行一次 `local && authenticated` 校验；远程 Uplink 继续通过专用 `handle_uplink_command` 入口，在进入共享调度器前完成确认、停止状态和审计校验。共享调度器删除管理员角色分支，保留各命令的停止令牌、DTO、修订和业务约束。服务安装适配器保留既有管理员安装门槛，并以 `SET_ACCESS` 收敛 INTERACTIVE 用户 SID 的服务对象权限为 `SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP`，不改变 SYSTEM/Administrators 等其他受托人的 ACE，也不向仅具网络登录令牌的远程用户授权。

### 线程和队列

不适用。只调整命令进入业务处理前的同步授权判断，不改变工作线程、队列容量、溢出策略或关闭路径。

### 持久化与恢复

配置仓储和事件数据库格式不变；变更命令继续使用既有原子更新、乐观修订和审计。无需迁移或数据恢复。

### 错误和降级

- 未认证或非本机本机-IPC请求：`IPC_UNAUTHORIZED`。
- 服务停止期间变更命令：保留 `SYS_SERVICE_STOPPING`。
- Windows SCM 安装/卸载访问被拒绝：保留现有 Win32/业务错误和管理员提示。

## 实施步骤

- [x] 1. 在 `SystemCommandService::handle` 增加统一本机认证检查，删除共享调度器中的管理员检查。
- [x] 2. 把各命令域的变更测试改为普通本机用户成功，并保留未认证/非本机拒绝覆盖。
- [x] 3. 更新需求、架构和 IPC 协议中的授权说明。
- [x] 4. 配置最小服务对象 ACL并覆盖安装收敛、失败回滚和普通用户重启所需权限。
- [x] 5. 运行格式检查、Debug/Release 构建、单元测试和完整非硬件 CTest，记录既有阻断。

## 验证计划

### 自动化测试

- 普通非管理员身份成功执行配置重载、上位机/存储/算法配置、算法测试、事件变更、报警确认和全部相机操作。
- 未认证与非本机身份在统一入口返回 `IPC_UNAUTHORIZED`。
- 远程 Uplink 仍要求 `operator_confirmed` 并记录审计。

### 构建与测试命令

```powershell
cmake --build --preset local-windows-vs2026-debug --target format-check
cmake --build --preset local-windows-vs2026-debug
ctest --test-dir out/build/local-windows-vs2026-debug -C Debug -R "^unit$" --output-on-failure
ctest --preset local-windows-vs2026-debug
cmake --build --preset local-windows-vs2026-release
ctest --test-dir out/build/local-windows-vs2026-release -C Release -R "^unit$" --output-on-failure
```

### 人工或硬件验证

- 未执行实体相机和真实 SCM ACL 写入；当前会话不安装或覆盖本机服务。
- 部署后应以普通 Windows 用户启动控制台，逐页验证保存和控制动作不再返回管理员权限错误。

## 回滚与恢复

代码和文档为无 schema 变更的逻辑调整，可通过恢复本计划列出的源文件和文档差异回滚。不得删除配置、数据库或用户数据。

## 验收标准

- [x] 运行期业务代码不再读取 `peer.administrator`。
- [x] 所有本机 IPC 功能由已认证普通用户调用成功。
- [x] 服务安装定义向交互式登录用户只授予查询、启动和停止权限。
- [x] 未认证和非本机请求仍被拒绝。
- [x] 相关文档与测试更新。
- [x] Debug 构建和相关单元测试通过；完整门禁的任何失败均有可复现说明。

## 进度记录

- 2026-08-06：创建计划，状态 `in-progress`；完成权限检查盘点。
- 2026-08-06：完成统一入口授权、业务域管理员检查删除、测试和文档调整，进入验证。
- 2026-08-06：发现控制台托盘直接通过 SCM 重启服务，将最小服务对象 ACL 纳入范围。
- 2026-08-06：Debug/Release 构建和串行 unit 通过；完整 Debug CTest 仅保留两个既有版本输出失败，计划完成。

## 决策记录

- DEC-001：统一授权放在公开本机 IPC 入口，避免各业务域再次产生权限漂移。
- DEC-002：保留 `PeerIdentity::administrator` 兼容字段和 Windows 角色探测，本任务只去掉功能授权依赖，避免扩大 IPC/平台接口改动。
- DEC-003：SCM 安装/卸载仍按 Windows 权限模型处理，不伪造普通用户可安装服务的承诺。

## 意外发现

- 全仓格式门禁已有 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 格式阻断。
- 工作区已有 `src/service/main.cpp` 的 `system("chcp 65001")` 修改，会导致两个版本输出比较测试失败。
- 并行启动 Debug/Release 两个完整 unit 入口会争用模拟器/IPC资源；改为项目原有串行方式后两套均通过。
- 完整 CTest 曾一次触发既有相机采集时序测试波动；该测试单独重复 20 次全部通过，随后完整 CTest 的 unit 入口通过。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-06 | 权限盘点 | 完成 | 六类管理员检查位于 `system_commands.cpp` |
| 2026-08-06 | 修改文件 clang-format / `git diff --check` | 通过 | 六个 C++ 文件格式合规，差异无空白错误 |
| 2026-08-06 | Debug / Release 构建 | 通过 | `cmake --build --preset local-windows-vs2026-{debug,release}` |
| 2026-08-06 | Debug / Release unit | 通过 | 串行运行；各 338 项测试 |
| 2026-08-06 | 完整 Debug CTest | 26/28 | `version_output_matches` 与安装树版本比较受既有 `chcp 65001` 输出影响 |
| 2026-08-06 | 全仓 format-check | 阻断 | 既有 `src/pipeline/include/paperbreak/pipeline/preview.hpp` 格式问题 |
| 2026-08-06 | 实体 SCM ACL / 相机 | 未执行 | 未安装、覆盖或重启本机生产服务，未访问实体相机 |

## 完成摘要

运行期本机 IPC 已统一为“本机且已认证”，不再使用管理员标志；普通用户测试身份覆盖配置、相机、算法、事件、存储、报警和诊断。服务安装/收敛新增 INTERACTIVE 用户最小启停 ACL，使托盘重启无需 UAC，同时保留安装、卸载、配置和删除的 Windows 管理员边界。文档、测试、Debug/Release 构建和串行 unit 已更新并验证；真实服务 ACL 需部署时由管理员执行一次安装/收敛后生效。
