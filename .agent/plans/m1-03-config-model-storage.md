# M1-03：配置模型、校验和原子存储 ExecPlan

## 元数据

- 状态：completed
- 负责人：Codex
- 创建日期：2026-08-01
- 最后更新：2026-08-01
- 路线图条目：`docs/roadmap/development-roadmap.md` M1-03
- 关联需求：需求 4.2、10、12、13.4；架构 5.2、6.3、12.1、15、16、19

## 目的与可观察结果

交付严格、版本化的强类型配置和可恢复配置仓储。服务能完整校验配置，以原子替换和有限历史保存配置，在版本、应用、写入或审计失败时保留最后有效快照，并明确报告立即生效与待重启字段。

## 范围

### 范围内

- schema v1、强类型模型、跨字段和路径校验；
- 配置仓储、版本冲突、幂等、历史恢复和回滚；
- 可注入配置应用、审计和原子文件端口及 Windows 实现；
- 服务启动/校验入口、测试、格式文档和路线图证据。

### 范围外

- 相机、算法、存储、uplink、Plant IO 的实际参数下发；
- IPC 和 Qt 客户端；
- 凭据内容存储、数据库 config_history 和安装器目录 ACL。

## 当前基线

- M1-01/M1-02 已完成，配置模块只校验 1 MiB、JSON 根对象和临时 `schemaVersion`；
- 服务要求显式 `--config`，console/SCM 共用 `ServiceRuntime`；
- 工作区开始时干净，Debug 构建及非硬件 CTest 17/17 通过。

## 前置条件与假设

- schema 严格拒绝未知字段；历史默认保留 5 份；
- 根版本采用领域模型规定的 `configSchemaVersion` 和 `configRevision`；
- 相机能力相关范围在本任务只做安全结构上限，M3 再按设备能力回读校验；
- 不增加生产依赖，不访问 MVS SDK 或实体硬件。

## 设计说明

`ConfigRepository` 持有不可变存储/有效快照，并串行化修改。候选内容依次经过有界读取、JSON/schema、强类型、依赖、版本、审计、组件应用/回读、历史和原子替换；任一步失败均逆序回滚已应用组件。待重启字段持久化但继续使用旧有效值，并以 JSON Pointer 列表显式报告。

### 线程和队列

不新增线程或跨线程队列。仓储使用互斥量串行化短时配置事务；未来组件通过注入的应用接口参与事务。

### 持久化与恢复

同目录临时文件完整写入并 flush 后原子替换；保留 5 份按修订命名的有效历史。主文件损坏时恢复最新可验证历史，残留临时文件不参与选择。历史回滚生成新修订。

### 错误和降级

使用既有 `SYS_CONFIG_INVALID`、`SYS_CONFIG_VERSION_CONFLICT`、`SYS_CONFIG_APPLY_FAILED`、`SYS_CONFIG_PERSIST_FAILED` 和 `SYS_CONFIG_SCHEMA_UNSUPPORTED`；Win32 原始码仅作为附加诊断。

## 实施步骤

- [x] 1. 建立 schema、强类型模型、解析和校验。
- [x] 2. 建立原子文件端口、Windows 实现和故障注入边界。
- [x] 3. 实现仓储事务、历史、恢复、回滚、应用分类和审计。
- [x] 4. 接入服务 CLI/启动，更新配置文档和测试数据。
- [x] 5. 增加单元/集成测试并完成 Debug 验证。
- [x] 6. 更新路线图、计划状态和验证证据。

## 验证计划

### 自动化测试

- 完整/非法 schema、边界、路径、敏感字段和依赖；
- 版本冲突、幂等、写入失败、应用失败、审计失败、待重启字段；
- 临时文件残留、历史恢复和显式历史回滚；
- CLI 完整校验和 console/SCM 启动。

### 构建与测试命令

```powershell
cmake --build --preset local-windows-vs2026-debug
ctest --preset local-windows-vs2026-debug
```

### 人工或硬件验证

不适用；本任务不访问实体相机、MVS SDK、SCM 注册或上位机。

## 回滚与恢复

代码回滚不得删除配置或历史。运行时失败通过旧快照、历史副本和原子替换恢复；不以截断写覆盖用户配置。

## 验收标准

- [x] 完整 schema 和强类型依赖校验可自动验证；
- [x] 配置可原子保存、恢复、回滚、审计且历史有界；
- [x] 待重启字段不伪装为已生效；
- [x] Debug 构建及默认非硬件 CTest 通过；
- [x] 文档、路线图和验证证据完整。

## 进度记录

- 2026-08-01：创建计划，状态 `in-progress`；基线 17/17 CTest 通过。
- 2026-08-01：完成 schema、模型、事务仓储、Windows 原子文件、服务启动恢复、文档和故障注入测试；状态更新为 `completed`。

## 决策记录

- DEC-001：schema v1 严格拒绝未知字段，不引入通用 JSON Schema 运行库。
- DEC-002：历史默认 5 份，回滚创建新修订。
- DEC-003：相机数值先做安全结构范围，设备能力校验留在 M3。

## 意外发现

- 首轮服务冒烟测试直接使用源目录配置，导致相对日志路径在测试数据目录下产生运行时文件；已改为在构建目录复制测试配置并从该副本启动。
- 首轮默认单元过滤器未包含 `ConfigRepository*`；已修正过滤器，最终默认单元入口运行 40 项，其中配置仓储 7 项。

## 验证证据

| 日期 | 命令/场景 | 结果 | 证据或限制 |
| --- | --- | --- | --- |
| 2026-08-01 | 任务前 Debug build + CTest | 通过 | 17/17，排除 hardware-integration |
| 2026-08-01 | `cmake --preset local-windows-vs2026-debug` | 通过 | 重新生成 Debug 工程和测试数据副本 |
| 2026-08-01 | `cmake --build --preset local-windows-vs2026-debug` | 通过 | MSVC `/W4 /WX` 构建完成 |
| 2026-08-01 | `ctest --preset local-windows-vs2026-debug` | 通过 | 17/17；默认 unit 内 40 项通过，排除 hardware-integration |

## 完成摘要

交付 schema v1、完整默认配置、强类型解析和依赖/路径/敏感字段校验；新增不可变配置快照、期望修订、幂等提交、历史恢复/回滚、热应用与待重启分离、同步脱敏审计，以及 prepare/apply-readback/persist/commit 和逆序回滚事务。Windows 文件端口使用同目录唯一临时文件、FlushFileBuffers 和写穿透原子替换。console/SCM 启动使用恢复后的有效快照，`--validate-config` 使用完整校验。未访问实体相机、MVS SDK、SCM 数据库、上位机或 PLC。
