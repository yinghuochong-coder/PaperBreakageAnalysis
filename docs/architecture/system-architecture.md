# 纸机断纸分析边缘系统架构

## 1. 文档信息

| 项目 | 内容 |
| --- | --- |
| 状态 | 架构基线 |
| 版本 | 1.0 |
| 基线日期 | 2026-07-31 |
| 适用版本 | 首期边缘工控机软件 |
| 需求来源 | `docs/requirements/edge-system-requirements.md` |
| 路线图来源 | `docs/roadmap/development-roadmap.md`，任务 G0-01 |
| 目标平台 | Windows 10/11 x64、Visual Studio 2026 MSVC、C++20、Qt 6、CMake |
| 目标设备 | 最多四台 Hikrobot MV-CS020-60GM |

本文档定义系统的模块边界和运行时约束。若实现需要改变这里规定的进程职责、依赖方向、线程所有权、持久化提交语义或厂商 SDK 隔离边界，必须先更新并评审本文档。

## 2. 架构目标与约束

### 2.1 首要质量属性

按优先级排序：

1. **采集连续性**：Qt 客户端、预览、上位机、磁盘滚动缓存或单路相机故障不得无条件停止其他相机采集。
2. **事件可靠性**：候选产生时立即保护前置缓存；已确认事件必须以可校验、可恢复的方式落盘。
3. **资源有界**：线程、帧池、内存缓存、队列、在途上传、日志和磁盘缓存均具有上限。
4. **确定性关闭**：所有工作线程和外部 I/O 都支持取消、超时和 join；服务停止不依赖无限等待。
5. **故障可诊断**：业务错误码稳定，同时保留厂商码、操作阶段和上下文；关键指标、队列深度和报警可查询。
6. **可测试性**：无实体相机、无上位机时，使用测试专用 Mock 运行绝大多数单元和集成测试；生产构建仍固定验证 MVS SDK。
7. **平台可控**：Windows、SCM、MVS 和具体网络协议代码位于适配层，领域与业务模块不直接依赖它们。

### 2.2 强制实现约束

- 使用 C++20、RAII 和明确所有权；
- 应用程序代码不直接使用原始 `new`/`delete`；
- 不以保存全局可变业务状态的单例组织系统；
- 所有跨线程队列都有容量、满载策略、关闭语义和指标；
- 相机采集回调/取流线程不执行磁盘写入、网络、JPEG、推理或阻塞式 IPC；
- MVS SDK 调用只存在于 Hikrobot 适配器目标；
- 厂商错误码不作为唯一业务错误码；
- 公开配置、IPC、事件、数据库和磁盘块格式都有独立版本；
- Release 构建不依赖开发机绝对路径；
- Windows 专用类型和 Qt Widgets 类型不得泄漏到领域接口。

### 2.3 当前未决项

以下内容是扩展点，不在本文档中臆定：

- PLC/现场 IO 的生产协议；
- 正式断纸算法、模型运行时和业务验收阈值；
- 目标机内存、相机最终 ROI、NVMe 性能对应的最终容量参数；
- 安装器技术选型。

这些未决项不得改变核心业务依赖方向；具体实现通过既有接口接入。

## 3. 系统上下文

```text
                              中心/上位机
                         状态、报警、配置、事件
                                  ⇅
┌──────────────┐      明文、无鉴权网络适配器      ┌────────────────────┐
│ PLC / 现场 IO │ ⇄ PlantIoAdapter ⇄             │                    │
└──────────────┘                                  │ PaperBreakEdge     │
                                                  │ Service            │
┌──────────────┐  GigE / MVS Adapter              │ Windows 后台服务    │
│ 1～4 路相机   │ ===============================> │                    │
└──────────────┘                                  │ 检测、缓存、事件、  │
                                                  │ 存储、监测、上传    │
┌──────────────┐  SCM 控制                        │                    │
│ Windows SCM  │ ===============================> │                    │
└──────────────┘                                  └─────────┬──────────┘
                                                            │ 本机 IPC
                                               QLocalServer / 命名管道
                                                            ⇅
                                                  ┌────────────────────┐
                                                  │ PaperBreakEdge     │
                                                  │ Console            │
                                                  │ Qt 配置客户端       │
                                                  └────────────────────┘
                                                            ⇅
                                                        本机操作员
```

系统还使用本机文件系统、SQLite、数据盘/NVMe 和 Windows 凭据/证书存储。它们都是服务端资源，Qt 客户端不能绕过服务直接修改生产配置、数据库或相机。

## 4. 进程架构

### 4.1 `PaperBreakEdgeService.exe`

后台服务承担全部生产业务：

- 服务生命周期和配置；
- 相机发现、绑定、控制、采集与重连；
- 图像预处理、算法检测和候选状态机；
- 内存/NVMe 缓存、事件冻结、关键帧与可靠落盘；
- SQLite 元数据、存储水位、上传任务；
- 上位机和 Plant IO 适配；
- IPC Server、健康监测、报警、日志和诊断。

服务不得创建任何界面。它支持两种宿主模式：

```text
WindowsServiceHost ─┐
                    ├─> ServiceRuntime（同一套服务核心）
ConsoleHost --------┘
```

- `WindowsServiceHost` 只负责 SCM 注册入口、状态上报和控制码转发；
- `ConsoleHost` 只负责开发时的控制台生命周期；
- 进程 `Bootstrap` 是唯一依赖装配组合根，负责装配 Hikrobot 生产适配器；
- `ServiceRuntime` 是注入完成后的业务生命周期和组件所有权根，不自行定位全局依赖；
- SCM 回调不得直接执行耗时停止，而是向控制通道投递高优先级停止请求；
- 两个宿主不能复制业务启动/关闭逻辑。

### 4.2 `PaperBreakEdgeConsole.exe`

Qt 桌面客户端只承担：

- 本机服务连接和协议适配；
- 系统托盘、页面导航和状态模型；
- 低帧率预览显示；
- 经服务校验的配置、控制、报警确认和事件复核；
- 诊断包请求与用户交互。

客户端不得：

- 链接 MVS SDK 或直接访问相机；
- 直接改写生产配置 JSON、SQLite 或事件目录；
- 保存高速原始帧；
- 把自身进程状态当作服务运行状态；
- 在断线后无提示地继续显示旧数据。

### 4.3 辅助工具

硬件测试、事件检查和服务安装可形成独立工具，但必须复用公开库接口，不复制业务实现：

- `camera-simulator`：控制模拟帧和故障注入；
- `event-inspector`：只读校验已提交事件；
- `PaperBreakUplinkSimulator`：Uplink v1 参考上位机，提供 GUI/headless、持久工作区、远程命令和故障注入；默认构建但仅进入独立 `UplinkSimulator` 安装组件，不进入 PaperBreakEdge 生产包；
- `service-installer`：若安装器需要独立帮助程序，只包装受控的服务安装操作。

辅助工具不属于长期运行的第三个业务进程。

## 5. 逻辑组件与模块边界

### 5.1 组件图

```text
┌──────────────────────── PaperBreakEdgeService ────────────────────────┐
│                                                                       │
│  WindowsServiceHost / ConsoleHost                                     │
│                    │                                                  │
│                    ▼                                                  │
│           ServiceRuntime（生命周期/所有权根）                           │
│   ┌──────────┬───────────┬──────────┬──────────┬──────────────┐       │
│   ▼          ▼           ▼          ▼          ▼              ▼       │
│ Config   CameraManager  Pipeline  EventMgr   Monitoring     IPC Server │
│              │           │  │        │             │            │      │
│      ICameraProvider     │  │        ├─ Storage    │            │      │
│             │            │  │        ├─ SQLite     │            │      │
│             ▼            │  │        └─ KeyFrames  │            │      │
│         Hikrobot         │  │                                      IPC│
│          Adapter         │  ├─ IBreakDetector                         │
│                          │  └─ PreviewEncoder                         │
│                          │                                           │
│                 IUplinkTransport        IPlantIoAdapter               │
│                     ├─ Mock                 └─ Mock                    │
│                     └─ 生产协议适配（M8 后批准）                        │
└───────────────────────────────────────────────────────────────────────┘
                                      ⇅
┌──────────────────────── PaperBreakEdgeConsole ────────────────────────┐
│ IPC Client → ClientStateStore → Widgets / Tray / Preview / Review     │
└───────────────────────────────────────────────────────────────────────┘
```

### 5.2 CMake 目标职责

| 目标 | 职责 | 可依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| `paperbreak_common` | 领域基础类型、时间、错误、Result、版本、通用有界容器 | C++ 标准库、批准的基础依赖 | Widgets、MVS、SCM、具体上传协议 |
| `paperbreak_platform` | 文件原子替换、ACL、凭据、进程指标和服务控制等窄平台端口 | common | Win32 头文件和具体平台实现 |
| `paperbreak_platform_windows` | Windows 平台端口及系统健康指标源实现 | platform、common、monitoring 接口、Windows API | 业务状态、Widgets、MVS |
| `paperbreak_config` | 强类型配置、schema 校验、版本、原子存储 | common、JSON、受控平台文件适配 | 相机实现、UI、上传实现 |
| `paperbreak_logging` | 日志门面、分类、脱敏、滚动和刷新 | common、spdlog | 业务模块反向依赖 |
| `paperbreak_camera` | 相机接口、能力、状态机、FramePacket、最多四路的运维控制会话 | common、monitoring 接口 | MVS 头文件、Widgets |
| `paperbreak_camera_mock` | 仅在 `BUILD_TESTING=ON` 时提供模拟设备、模拟帧和故障注入；不安装 | camera、common | MVS、生产应用 |
| `paperbreak_camera_hikrobot` | MVS 枚举、句柄、参数、取流和错误翻译 | camera、common、MVS SDK | UI、事件、上传 |
| `paperbreak_pipeline` | 预处理节点、帧路由、顺序和背压策略 | common、camera、algorithm 接口、event 接口 | MVS、SQLite、网络实现 |
| `paperbreak_algorithm` | `IBreakDetector`、结果模型、候选判定接口 | common、camera 的只读帧视图 | MVS、存储、UI |
| `paperbreak_algorithm_mock` | 手动/周期/灰度等模拟检测器 | algorithm | 相机或存储实现 |
| `paperbreak_algorithm_classical` | 批准的传统视觉算法 | algorithm、OpenCV | MVS、UI、SQLite |
| `paperbreak_event` | 候选状态机、窗口冻结、合并、关键帧策略、事件模型 | common、camera 帧视图、algorithm 接口 | MVS、具体数据库/网络 |
| `paperbreak_event_codec` | 关键帧 JPEG 图像编解码适配 | event、批准的 OpenCV core/imgproc/imgcodecs | MVS、UI、SQLite、网络；公开接口不得暴露 OpenCV 类型 |
| `paperbreak_storage` | 事件事务、SQLite、迁移、NVMe 块、保留策略 | common、event、SQLite、可选 zstd | UI、MVS、上传协议 |
| `paperbreak_uplink` | `IUplinkTransport`、Uplink v1 DTO/校验、单线程心跳/状态/命令编排和上传调度领域接口 | common、event 接口 | Qt Widgets、MVS、服务业务实现 |
| `paperbreak_uplink_mock` | 离线/慢速/失败脚本化传输 | uplink | 生产凭据 |
| `paperbreak_uplink_transport` | Uplink v1 明文 REST/WebSocket/HTTP 边缘适配器（M8-04） | uplink、Qt Network 或批准网络库 | 相机、UI |
| `paperbreak_uplink_simulator_core` | Uplink v1 参考服务端、SQLite 工作区、命令与故障模型 | uplink、Qt HttpServer/WebSockets、SQLite | 生产服务、MVS |
| `paperbreak_plant_io` | `IPlantIoAdapter` 和生产信号模型 | common | 具体未批准协议 |
| `paperbreak_monitoring` | 指标、报警、健康快照和诊断接口 | common、logging 接口 | UI、MVS |
| `paperbreak_ipc` | 版本化 IPC 编解码、Server/Client 传输 | common、platform_windows、Qt Core/Network、nlohmann/json | Widgets、MVS、业务实现 |
| `paperbreak_console_model` | 客户端连接/服务与相机快照、单写操作在途、代次和过期语义 | common、ipc、JSON | Widgets、相机/存储/算法实现、MVS |
| `paperbreak_service_core` | ServiceRuntime、用例编排、命令处理和生命周期 | 上述业务接口，不依赖具体适配器 | Widgets、MVS C API、具体网络/数据库句柄 |
| `paperbreak_windows_service_control` | 安装器和 Qt 客户端复用的窄 SCM 管理适配，含有界重启 | common、Windows API | Widgets、业务状态、MVS |
| `paperbreak_windows_service` | SCM 宿主与 Win32 服务适配 | service_core、windows_service_control、platform_windows、common | 业务模块内部实现 |
| `PaperBreakEdgeService` | Bootstrap、选择宿主、装配依赖、进程入口 | service_core、windows_service、批准的具体适配器 | Widgets |
| `PaperBreakEdgeConsole` | Qt UI、托盘、状态模型展示和用户发起的窄 SCM 重启 | common、console_model、logging、windows_service_control、Qt Widgets/Gui | camera/storage/algorithm 实现、MVS |

M0 路线图中的聚合名称可以作为以上目标的 `ALIAS` 或聚合目标，但不能把实现合并成一个目标从而破坏依赖检查。

### 5.3 依赖方向

```text
平台/厂商/传输实现
        │ 实现
        ▼
适配接口和领域模型
        ▲
        │ 使用
应用编排 / ServiceRuntime

UI ──仅通过 IPC──> 服务用例
```

允许的依赖必须形成有向无环图。核心规则：

1. `common` 不依赖任何业务模块；
2. 接口目标不依赖其实现目标；
3. `camera_hikrobot` 依赖 `camera`，反向依赖禁止；
4. `storage` 实现事件持久化端口，`event` 不依赖 SQLite；
5. `uplink_transport` 实现上传端口，事件模块不依赖网络；
6. `PaperBreakEdgeConsole` 不链接任何服务内部实现；
7. 只有进程组合根可以装配 Hikrobot 生产适配器；测试目标可显式注入 Mock；
8. 平台类型、SDK 句柄、Qt Widget、SQLite 句柄不得出现在领域公开头文件中。
9. 通用业务模块只依赖 `paperbreak_platform` 的窄端口；除 Hikrobot 适配器因 SDK 所需外，Win32 类型只允许出现在 Windows 平台实现和服务宿主。

### 5.4 禁止依赖的机械检查

实施时至少采用：

- 每个模块独立 CMake target 和最小 `target_link_libraries`；
- MVS include/lib 只添加到 `paperbreak_camera_hikrobot` 的私有属性；
- CI 扫描 MVS 头文件引用，只允许出现在 `src/camera/hikrobot`；
- CI 扫描 Qt Widgets 引用，只允许在 console/UI 目录；
- CI 扫描 `Windows.h`/Win32 句柄，只允许在 `src/platform/windows`、`src/service/windows` 和经记录的 Hikrobot 适配代码；
- 链接和安装测试证明所有生产配置包含 MVS 适配器与所需 Runtime，测试专用 Mock 不进入安装树；
- 架构测试或脚本检查禁止的 target 依赖；
- 公开头文件自包含编译测试。

## 6. 核心领域模型与所有权

### 6.1 帧模型

概念结构：

```cpp
struct FramePacket {
    CameraId cameraId;
    std::uint64_t cameraFrameNumber;
    std::uint64_t sequenceNumber;
    MonotonicTime monotonicTime;
    WallClockTime wallClockTime;
    FrameGeometry geometry;
    PixelFormat pixelFormat;
    std::shared_ptr<const FrameBuffer> buffer;
    FrameFlags flags;
};
```

架构规则：

- `FrameBufferPool` 由每个 `CameraSession` 或其帧资源上下文独占；
- 缓冲在启动/重新配置时按固定预算预分配；
- `shared_ptr` 使用返回对象池的受控删除器，跨线程只传只读视图；
- 裸 SDK 缓冲指针不能越过 Hikrobot 回调；
- 预处理需要新图像时从另一个固定池获取，不能在每帧路径反复申请大块内存；
- 内存环缓存和事件租约共享帧所有权，但总池容量不随事件增加；
- 池耗尽时按通道策略丢帧、计数并报警，禁止退化为无界堆分配。

### 6.2 时间语义

| 时间 | 用途 | 禁止用途 |
| --- | --- | --- |
| `steady_clock` 单调时间 | 帧排序、超时、前后窗口、事件合并、性能耗时 | 用户展示、跨重启绝对时间 |
| `system_clock` 墙上时间 | 目录、审计、IPC 时间、用户展示 | 判断持续时间和超时 |
| 相机时间戳 | 设备诊断和多相机关联，标记有效性 | 在未同步/未验证时作为唯一事件时钟 |

系统时间跳变不改变已在进行的单调计时窗口，但必须产生健康事件并在 manifest 记录时钟状态。

### 6.3 配置快照

- `ConfigRepository` 拥有最后一次有效的版本化配置；
- 业务组件只持有不可变配置快照或经验证的局部配置；
- 配置应用是“准备 → 组件应用/回读 → 提交版本”；
- 任一必需组件应用失败时保留旧快照并回滚已修改组件；
- 需要重启的字段只保存为 pending，不伪装为已生效；
- 配置写入使用同目录临时文件、刷新和原子替换，并保留有限历史；
- 密码、Token、证书私钥只以安全引用存在于普通配置。

### 6.4 事件聚合

`EventAggregate` 是单个事件的一致性边界，包含：

- 全局事件 ID、状态和版本；
- 候选/确认/起止单调时间与墙上时间；
- 触发相机、帧号、原因、置信和算法版本；
- 各相机窗口租约；
- 关键帧选择结果；
- 持久化、复核和上传状态。

事件状态只能通过事件管理器串行转换，SQLite、UI 或上传器不能直接改写领域状态。

## 7. 服务运行时和线程模型

### 7.1 所有权树

```text
ProcessMain
└─ Bootstrap（唯一依赖装配组合根）
   └─ ServiceRuntime
      ├─ ConfigRepository
      ├─ LoggingRuntime
      ├─ MetricsRegistry / AlarmRegistry
      ├─ DatabaseRuntime
      ├─ StorageRuntime
      ├─ CameraManager
      │  └─ CameraSession[0..4]
      │     ├─ ICameraDevice
      │     ├─ FrameBufferPool
      │     ├─ AcquisitionWorker
      │     └─ AcquisitionQueue
      ├─ ProcessingRuntime
      │  ├─ PerCameraProcessor[0..4]
      │  ├─ DetectorWorkers
      │  └─ PreviewRuntime
      ├─ EventRuntime
      │  ├─ EventManager
      │  ├─ MemoryRing[0..4]
      │  ├─ KeyFrameWorker
      │  └─ EventWriter
      ├─ NvmeCacheRuntime
      ├─ UplinkRuntime
      ├─ PlantIoRuntime
      ├─ HealthMonitor
      └─ IpcServer
```

`ServiceRuntime` 通过成员对象或智能指针拥有组件，不通过全局单例访问。析构只是最后防线；正常关闭必须显式执行 `requestStop()` 和带截止时间的 `join()`。

### 7.2 工作线程

| 执行上下文 | 数量 | 主要职责 |
| --- | ---: | --- |
| 主控制线程 | 1 | 运行时状态转换、配置事务和生命周期 |
| SCM 回调线程 | Windows 管理 | 仅翻译控制码并投递请求 |
| IPC 事件线程 | 1 | 本机连接、解帧、请求关联和推送调度 |
| 相机采集线程 | 每启用相机 1 个，最多 4 | 获取帧、复制/接管到池、填元数据、入队 |
| 每相机预处理执行器 | 每启用相机 1 个，最多 4 | 保序预处理、内存缓存登记和分支路由 |
| 算法工作线程 | 每启用相机 1 个，最多 4 | 独占该相机的 `DetectorHost` 并串行检测；单 Lane 阻塞或降级不影响其他相机 |
| 预览编码线程 | 固定 1～2 | 抽样、缩放、覆盖层和 JPEG |
| 事件管理线程 | 1 | 候选状态、窗口租约、合并和事件状态串行化 |
| 关键帧/事件写线程 | 固定有界，首期各 1 | 关键帧编码和事件事务写入 |
| NVMe 写线程 | 1 | 顺序块写入、轮转和校验 |
| 上传线程 | 固定 1～2 | 心跳之外的持久化上传任务 |
| Plant IO 线程 | 0 或 1 | 有界轮询/写入；未配置时不创建 |
| 健康监测线程 | 1 | 周期采样、阈值和报警 |
| 日志后台线程 | 日志库固定 1 | 异步日志落盘和轮转 |

线程数量是配置上限而不是按任务动态无界增长。算法、预览、事件写和上传不得为每帧/每事件创建新线程。

### 7.3 每相机保序

相机帧的 `sequenceNumber` 在采集上下文中单调递增。每个相机只有一个预处理消费者，保证：

- 帧进入内存环缓存的次序稳定；
- 灰度/变化量等有状态节点按序执行；
- 向算法提交时记录序号；
- 多算法工作线程返回的结果由事件入口按相机序号重排；
- 缺失帧、跳帧和超出重排窗口均可统计。

不同相机之间不强制全局帧序；跨相机关联以单调时间窗口和时间同步质量为依据。

### 7.4 Qt 客户端线程

- Qt GUI 主线程独占所有 Widgets 和托盘对象；
- QLocalSocket 可运行在 GUI 事件循环，但解帧和状态更新必须短小，不执行 JPEG 解码、文件导出或数据库访问；
- JPEG 解码使用固定 1～2 个工作线程和每相机单槽 mailbox；
- 工作线程只产生不可变图像/状态结果，通过受控投递交给 GUI 线程，禁止直接更新界面；
- 服务端最多接受 8 条活动本机 IPC 连接；当前 Console 的状态、预览、相机、运维和事件客户端固定使用 5 条隔离连接，剩余容量用于有界的诊断或后续本机客户端；
- 客户端每连接最多保留 128 个待响应请求，达到上限时拒绝新请求并提示 busy；
- 不把高频 `preview.frame` 直接堆积为无界 Qt queued signal，状态推送按 key 合并，预览始终 latest-wins。

## 8. 跨线程通道与背压

### 8.1 容量原则

- 下表默认值是初始架构基线，不是未经测量的最终性能承诺；
- 所有容量在启动时根据相机数、分辨率、帧率、事件时长和内存预算校验；
- 容量只可在停止相关生产者后重新配置；
- 队列实现不在运行中自动扩容；
- 每条通道至少暴露当前深度、容量、高水位、入队数、出队数、丢弃/拒绝数和最长等待；
- 停止请求不依赖普通队列空位：使用 `stop_token`、关闭标志或保留的高优先级控制槽。

### 8.2 通道清单

| 通道 | 生产者 → 消费者 | 初始容量/上限 | 满载策略 | 停止与排空 | 严重性 |
| --- | --- | --- | --- | --- | --- |
| `service.control` | SCM/Console/IPC → 主控制线程 | 128 条；停止信号独立 | 普通命令返回 `SYS_BUSY`；停止不丢弃 | 停止接收新普通命令，处理已接受控制事务 | Warning |
| `camera.command[i]` | 主控制 → 相机会话 | 32 条/相机 | 拒绝新普通命令；stop/close 走优先控制 | 取消未开始操作，当前 SDK 调用必须有超时 | Warning |
| `acquisition.frames[i]` | 采集 → 每相机预处理 | 16 帧/相机 | 丢弃最旧未处理帧，记录序号缺口；绝不阻塞采集 | 停采后关闭生产端，消费者排空至截止时间 | Warning，持续超限升级 Error |
| `algorithm.frames[i]` | 预处理 → 算法执行器 | 8 帧/相机 | 丢弃最旧待检测帧并记录 `algorithmSkipped` | 停止新提交；可在截止时间内处理已接受帧 | Warning，持续超限升级 Error |
| `algorithm.results` | 算法执行器 → 事件管理 | 256 条 | 不静默覆盖；拒绝新结果并触发 Error，后续帧进入降级状态 | 关闭算法生产端后排空并完成排序窗口 | Error |
| `preview.latest[i]` | 预处理/结果叠加 → 预览编码 | 每相机 1 个槽 | `latest-wins`，覆盖旧帧并计数 | 无订阅立即清空；停止时直接丢弃 | Info |
| `preview.encoded[i]` | 编码 → IPC 推送 | 每相机 1 个槽；单 JPEG 有字节上限 | `latest-wins` | 断开/停止直接丢弃 | Info |
| `event.commands` | 事件检测/人工/复核 → 事件管理 | 256 条 | 事件触发不能静默丢弃；拒绝并触发 Critical；复核命令返回 busy | 停止新候选后处理已接受状态转换 | Critical（候选/确认） |
| `event.persist` | 事件管理 → 事件写入 | 默认 8 个事件，不超过 `maxConcurrentEvents` | 不扩容；无法接收新事件时保护现有租约并触发 Critical | 截止时间内提交；超时保留可恢复临时目录 | Critical |
| `keyframe.jobs` | 事件管理 → 关键帧线程 | 默认 32 个任务 | 拒绝并标记事件不完整，触发 Error；原始事件仍优先 | 尝试完成已接受任务，超时由恢复流程补偿 | Error |
| `nvme.blocks` | 预处理 → NVMe 写线程 | 默认 2 个时间块/相机 | 停止普通滚动块、记录缺口并报警；不反压采集 | 停止新块，完成当前块或留下可扫描尾块 | Warning/Error |
| `ipc.outbound[client]` | 服务模块 → IPC 线程 | 128 条且总计 32 MiB/客户端；其中推送最多 32 条 | 状态按 key 合并；预览 latest-wins；事件/报警推送可丢但客户端通过版本游标补查 | 停止新推送，发送服务停止通知后限时关闭 | Warning |
| `console.previewDecode[i]` | Qt IPC/GUI 事件循环 → JPEG 解码线程 | 每相机 1 个槽 | `latest-wins`，覆盖尚未解码的旧帧 | 断线、暂停或退出时直接清空 | Info |
| `upload.inflight` | 持久化调度器 → 上传线程 | 每线程 1 个，线程默认 2 | 不从 SQLite 领取更多任务，不丢持久任务 | checkpoint 当前分块，释放租约后退出 | Warning |
| `plantio.commands` | 服务 → Plant IO | 32 条 | 拒绝普通写并报警；安全动作不能依赖此通道作为唯一手段 | 取消轮询，限时完成/取消当前 I/O | Error |
| `logging.async` | 全部模块 → 日志线程 | 默认 8192 条 | acquisition/performance 日志丢弃并计数；Error/Critical 写应急有界通道或 Windows Event Log，仍不得阻塞采集 | 停止业务生产后限时刷新 | 按日志等级 |

容量单位和配置范围必须进入配置 schema。帧池容量不是简单等于帧队列容量，还需包含环缓存、事件租约、算法、预览和写入的并发引用预算。

### 8.3 内存预算

每路相机的启动校验至少计算：

```text
bytesPerFrame = stride × height
ringFrames = ceil(preEventSeconds × configuredFrameRate) + safetyMargin
postEventInFlight = ceil(postEventSeconds × configuredFrameRate)
pipelineFrames = acquisitionCapacity + algorithmCapacity + previewSlots
eventLeaseBudget = 根据 maxConcurrentEvents 和窗口重叠策略计算的最坏额外引用
requiredBytes = bytesPerFrame × 所有池化缓冲数 + 元数据与编码工作区
```

同一原始帧被多个事件引用时共享缓冲，不重复原始图像内存。若计算超过配置的进程/相机内存预算，服务拒绝启动该配置并返回稳定错误，而不是运行中尝试无界扩张。

### 8.4 优先级

资源竞争时按以下原则降级：

1. 停止和数据一致性控制；
2. 已触发事件保护与事件提交；
3. 相机采集和必要预处理；
4. 候选检测；
5. 报警与关键状态；
6. NVMe 普通滚动缓存；
7. 上传；
8. 预览；
9. 调试日志。

优先级不意味着在采集回调执行事件 I/O；它决定队列准入、线程调度建议和降级顺序。

## 9. 图像与事件数据流

### 9.1 正常采集

```text
ICameraDevice（生产为 Hikrobot，自动化测试可注入 Mock）
        │ SDK 回调或限时取流
        ▼
FrameBufferPool → FramePacket 元数据
        │
        ▼ acquisition.frames[i]（有界）
每相机预处理执行器（保序）
        ├──────────────> MemoryRing[i]（O(1) 登记共享引用）
        ├──────────────> algorithm.frames[i]（可跳帧）
        ├──────────────> preview.latest[i]（latest-wins）
        └──────────────> nvme.blocks（M7，可降级）
```

采集线程到 `acquisition.frames` 入队后立即返回，不等待任何下游分支。

### 9.2 算法与候选

```text
algorithm.frames[CAMxx]（每相机有界、drop-oldest）
      ▼
algorithm-worker-camxx（该 Lane 的 DetectorHost 串行调用；相机 ID 后缀转为小写以满足日志线程名约束）
      ▼
AlgorithmResultEnvelope（禁用、降级、失败和跳帧也产生时间推进信封）
      ▼
algorithm.results（全局有界 256，不阻塞 Lane）
      ▼
event-processing（单线程稳定排序和状态转换）
      ▼
Idle → Suspicious → Candidate → Confirmed/Rejected/Timeout
                           │
                           └─ Candidate 时立即申请缓存窗口租约
```

结果稳定顺序键为单调时间、相机 ID、序号。事件线程以所有 Lane 队列和在途帧中的最早单调时间作为安全水位，只提交水位之前的结果，避免慢 Lane 的旧结果在窗口已经冻结后到达。候选状态、跨相机合并窗口和冻结决策仍只由 `event-processing` 串行管理。`algorithm.results` 满载时拒绝来源结果、记录 `ALGORITHM_RESULT_QUEUE_FULL` 并立即把来源 Lane 降级为 `manual-trigger-only`；其他 Lane 保持自动检测。

停止顺序固定为：禁止新提交，逐 Lane 排空并停止 worker，排空结果入口并冻结剩余窗口，随后关闭内存环、JPEG 和持久化运行时。重配置先构造完整检测器/Lane/队列，并创建停在启动门后的候选事件线程和 Lane worker；只有候选线程组全部准备成功，才排空旧线程组并切换。

不得等到 Confirmed 才保护缓存。算法更新采用先初始化新实例、验证后原子切换；失败继续使用旧实例。

### 9.3 事件冻结与提交

候选首次取得窗口管理器分配的规范 Event ID 后，服务必须立即在 SQLite 建立
`Collecting` 行并发布 `event.lifecycleChanged`，不能等待原始证据写盘。合并触发只增加
同一行的 `triggerCount`；源判定按 `Confirmed > Candidate > Timeout > Rejected` 聚合。
冻结、JPEG 编码、进入容量 8 的写队列、单线程写入和目录提交分别推进
`Encoding`、`Queued`、`Writing`、`Committed`。任一阶段失败进入 `Incomplete` 并报警。
只有 `Committed` 行允许读取 manifest、缩略图、正式目录、导出、上传和人工复核。

```text
候选时刻 T
   │
   ├─ MemoryRing 获取 [T-pre, T] 的只读租约
   ├─ 订阅并收集至 [T, T+post]
   ├─ 重叠窗口按 mergeGap 合并/共享
   ▼
EventAggregate 完整
   ├─ KeyFrameSelector / JPEG 工作线程
   ├─ 原始帧异步写入
   ├─ 文件校验
   ├─ 最后生成 manifest
   ▼
同卷临时目录 ──原子重命名──> 已提交事件目录
   ▼
SQLite 索引/对账 → 上传任务
```

事件目录只有原子提交后才能被 UI、导出器或上传器读取。数据库写入和目录提交无法形成单一文件系统事务，因此启动/周期对账负责修复“目录已提交但索引缺失”或“数据库记录指向缺失目录”。

### 9.4 预览

```text
最新帧槽 → 订阅抽样 → 缩放/覆盖层 → JPEG → 最新编码槽 → IPC
```

- 无订阅者不抽样、不编码；
- 客户端最小化可降低订阅帧率；
- 暂停显示只取消/降低订阅；
- 编码和 IPC 卡顿只覆盖旧预览，不影响采集、算法或事件；
- 原始高速图像不经预览通道上传。

### 9.5 上位机上传

```text
已提交事件 / 报警 / 状态
          ▼
SQLite upload_jobs（持久）
          ▼
优先级调度 → 有限在途上传 → 服务端校验/确认 → 提交完成状态
```

M8-03 将 `upload_jobs` 升级为 schema v4：每个任务有独立幂等键、可选事件 ID、资源类型、固定优先级、逻辑 ID、相对路径、声明上传字节、状态、尝试次数、下次时间、checkpoint 和稳定错误码。事件资源引用既有 `events` 行，报警元数据可独立排队；幂等入队不创建或修改事件，同键同内容返回原任务，同键不同内容拒绝。

领取顺序固定为报警元数据、关键帧、manifest、低码率回放、原始文件，再按创建时间和任务 ID 稳定排序。未完成任务（包括永久失败和人工处理）默认最多 10000 条、硬上限 1000000 条，默认声明待上传字节上限 1 TiB、硬上限 16 TiB；全部任务历史默认最多 50000 条、硬上限 2000000 条，满载时只淘汰最旧 `Completed` 记录，不删除事件文件或尚待处置的失败记录。任务状态为 `Pending`、`InProgress`、`RetryWait`、`Completed`、`PermanentFailed` 和 `ManualIntervention`。

网络断开只延迟上传。可重试失败使用带抖动、有最大间隔和最大次数的指数退避；永久失败与需人工处理不再自动领取，`event.retryUpload` 可把指定事件的失败任务显式放回队列。进程启动把遗留 `InProgress` 原子转为立即到期的 `RetryWait`，保留原任务 ID、事件 ID 和 checkpoint。幂等键至少包含设备 ID、事件 ID、文件逻辑 ID。M8-04 执行器查询服务端已确认分块，只补传缺块，并在整文件完成确认后提交任务完成。

## 10. 状态机

### 10.1 相机状态

```text
Disabled
   │ enable
   ▼
Disconnected → Connecting → Connected → Starting → Streaming
                      ▲                         │          │
                      │                         │          ▼
                      └──────── Recovering ◀────┴──── fault/timeout
                                      │
                                      └─ retries exhausted → Faulted
```

规则：

- 所有转换由 `CameraSession` 串行执行；
- 退避采用 1、2、5、10、30、60 秒上限序列，恢复成功后重置；
- 停止请求可从任意状态到达受控停止路径；
- `Faulted` 需要显式重试、配置变更或重新启用；
- 一路状态机不持有其他相机锁。

### 10.2 候选事件状态

```text
Idle → Suspicious → Candidate ─┬→ Confirmed
                               ├→ Rejected
                               └→ Timeout
```

状态转换具有事件版本。来自 UI 或上位机的确认/拒绝必须携带期望版本，重复命令幂等，过期命令返回冲突。

### 10.3 服务状态

```text
Created → Starting → Running → StopRequested → Draining → Stopped
              │                                      │
              └──────────── Failed ◀─────────────────┘
```

启动阶段失败按已启动组件的逆序回滚。服务向 SCM 报告 checkpoint 和合理等待提示，但内部每个阶段仍需自己的截止时间。

## 11. IPC 架构

### 11.1 边界

- 服务是 QLocalServer，客户端是 QLocalSocket；
- Windows 下使用本机命名管道语义并设置访问控制；
- UI 不复用服务内存对象，跨进程只传版本化 DTO；
- 每连接有独立解码状态、请求表和有界推送队列；
- 客户端重连后通过版本/游标重新查询状态，不假定漏失推送会重发。

### 11.2 帧格式

逻辑格式：

```text
uint32 headerLength
uint32 binaryLength
header JSON bytes
optional binary bytes
```

实现前必须固定字节序、最大 header、最大 binary、协议版本不兼容行为和校验方式。默认建议：

- JSON header 不超过 1 MiB；
- 普通二进制负载不超过 16 MiB；
- 超限在分配负载内存前拒绝；
- 预览二进制大小还受订阅分辨率限制；
- 不通过 IPC 传完整原始事件序列。

消息支持 request、response 和 push，包含 `protocolVersion`、`requestId`、`command/eventName`、时间和 payload。写操作始终由服务重新校验和审计。

### 11.3 命令路由

IPC 层只负责：

- 鉴别连接权限；
- 解帧和 schema 基础校验；
- requestId、超时和响应关联；
- 将命令映射为服务用例；
- 编码响应和推送。

IPC 层不直接操作相机、数据库或配置文件。业务用例返回稳定业务错误、可读消息和结构化上下文。

## 12. 持久化架构

### 12.1 配置

```text
读取 → JSON/schema 校验 → 强类型校验 → 依赖校验 → 不可变快照

修改 → 版本冲突检查 → 组件预应用/回读 → 临时文件 → flush
     → 原子替换 → 提交内存版本 → 审计
```

应用失败时保留旧文件和旧生效快照。启动时若主配置损坏，可从最近有效历史恢复，但必须报警和记录恢复来源。

### 12.2 事件目录

事件 manifest schema v2 不兼容 v1。`manifest.json` 使用 `decisionState`、算法确认时间、
`triggerCount` 和 `rawBlocks`；每个块记录相机、墙上/单调时间范围、帧号/序号范围、帧数、
文件大小、CRC32C 与 SHA-256。原始证据按相机及单调时钟 1 秒窗口分块，单块最多 256 帧，
格式复用 `PBNVME1` v1 的 4 KiB 头页、固定帧索引、逐帧 CRC32C、尾页和提交标记。

块写入顺序固定为：临时块主体和尾页、持久刷新、最后 8 字节提交标记、再次持久刷新、
同卷原子发布块名。全部块、关键帧和 `event.json` 完成后最后写 manifest，再原子提交事件
目录；取消或失败保留 `.transactions` 内的可恢复目录，正式目录绝不能暴露伪提交。

部署 schema v2 前必须由用户人工归档旧 `events.db` 和旧事件目录并提供空事件根目录。
代码不删除、不移动旧数据；检测到非空旧库、schema v1 manifest 或无法识别的旧 manifest
时拒绝启动并报告 `EVENT_SCHEMA_UNSUPPORTED`。

- 事件根目录与临时目录必须位于同一卷，以保证最终 rename 语义；
- 所有文件先写入唯一临时目录；
- 每个文件写完后记录长度和校验；
- manifest 最后生成；
- 必要数据和目录元数据刷新后再原子重命名；
- 未完成目录在启动时恢复、隔离或明确标损；
- 正式目录不可被原地修改；复核和上传状态主要记录在数据库，必要的事件版本更新采用新版本元数据文件而非破坏原始证据。

### 12.3 SQLite

- SQLite 只保存元数据，不存高速图像 BLOB；
- 单一数据库写入协调器串行化迁移和关键写事务；
- 读连接有数量和查询时限；
- 每次 schema 变更都有递增版本和迁移测试；
- 启动执行完整性检查策略，定期备份；
- 文件系统事件目录是原始文件事实来源，数据库通过对账恢复索引。
- `event_retention` 独立保存人工锁定、允许删除和可恢复删除状态；不得改写已提交
  manifest 来表达可变保留状态。

### 12.4 NVMe 滚动缓存

M7 按 ADR-011 实现 NVMe v1 块：每相机 1 秒、原始帧不压缩、显式小端字段，文件由
4096 字节头页、96 字节定长帧索引、原始数据区和 4096 字节提交尾页组成。逐帧及块区域
使用 CRC32C 检测意外损坏；缺少合法提交标记/尾页或任一边界、长度、CRC 不一致的块不得
作为正常已提交块使用。完整布局、容量和带宽公式见
`decisions/adr-011-nvme-rolling-cache-format-capacity.md`。架构约束：

- 固定总容量，按块循环复用；
- 事件通过租约保护块；
- 单写线程顺序提交；
- 尾块可在断电后扫描；
- 索引可从块文件重建；
- 磁盘失败降级到内存缓存并报警；
- 普通滚动缓存的失败不得阻止已触发事件优先落盘；
- 普通滚动写需求不得超过目标卷实测持续写带宽的 80%，生产容量必须按最终 ROI、stride、
  像素格式、帧率和相机数复算，不能使用平均压缩率或厂商峰值标称值。

M7-02 的运行时使用单一写线程；每相机一个当前组装块和最多两个待写块，队列项只持有
`FrameView` 的共享只读所有权；连同写线程当前块，每相机最多四块的共享引用计入帧池启动
预算。完整块队列满时立即拒绝普通块并记录缺口，不在相机交付路径
等待或执行文件 I/O。写线程按配置速率和单块总截止时间提交；任何创建、预分配、写入、
`FlushFileBuffers`、发布或回绕删除失败都进入显式 `memory-degraded`，事件内存环保持运行。

M7-03 在缓存根 `.index` 中维护 schema v1 SQLite 派生索引；块文件仍是唯一恢复事实来源，
索引可由调用方提供的已验证块摘要事务式重建。每条摘要包含块 ID、相机、UTC 起止、序号
范围、块内缺口和提交时头/索引/数据/尾 CRC32C。查询按相机和 UTC 时间窗返回有界的块引用，
并报告块内/跨块序号缺口、重叠和提交校验状态，不返回截断后伪称完整的结果。活动事件租约
固定最多 64 条；当前进程用单调时间把既有和随后提交的重叠块绑定到租约，持久层只保存 UTC、
块 ID 和引用，不序列化可跨重启误用的 `steady_clock` 值。回绕选择、文件删除和租约变更通过
独立索引操作互斥串行化，不持有相机提交队列互斥量；全部块受保护时普通缓存显式降级，绝不
覆盖事件块。事件目录和 SQLite 元数据均成功提交后释放相关来源租约；编码、准入、文件或
元数据失败时保留租约供恢复/人工处理。

M7-04 在 NVMe 写线程启动前执行 v1 恢复扫描。扫描同时受 100000 个候选文件、64 MiB 摘要
内存和 5 分钟总截止时间限制，原始负载以固定 1 MiB 缓冲流式校验，不把整块读入内存。完整
`.pbnvme` 只有在头页、保留位、声明长度、每项索引 CRC、逐帧负载 CRC、索引/数据区域 CRC、
尾页 CRC 和提交标记全部一致时才接纳。`.partial` 若头页有效且能证明至少一个连续有效帧前缀，
则按该前缀重建尾页，依次持久化尾页主体和提交标记后在同卷原子发布；已经包含完整提交标记但
尚未改名的临时块只需重新验证并发布。已发布块的任何损坏、未知版本以及无法证明安全前缀的
临时块均非覆盖地移动到 `.quarantine`，不删除证据，也不进入正常回绕。

全部候选扫描成功后，已验证摘要才事务式替换派生块表；持久 UTC 租约重新绑定恢复块，新事件
租约也用 UTC 绑定启动前块，同时继续用当前进程单调时间保护随后提交的重叠块。运行时从恢复
摘要恢复物理占用和每相机下一代次，容量超限时仍只回收零租约块。扫描、修复、隔离或索引提交
失败时普通滚动缓存进入 `memory-degraded`，事件内存链保持运行；未完整扫描的集合不会被提交为
新的索引事实。

### 12.5 存储水位

| 状态 | 行为 |
| --- | --- |
| Normal | 正常事件和滚动缓存 |
| Warning | 报警；清理已上传、可删除且未锁定的最旧事件 |
| Critical | 停止普通滚动缓存；保留正式事件能力 |
| Stop-save | 禁止新增大文件，持续检测和 Critical 报警；不谎报事件已保存 |

默认禁止自动删除未上传或人工锁定事件。删除采用数据库状态转换、文件操作和后续对账，不在采集线程执行。

### 12.6 上位机心跳与命令编排

M8-02 的 `UplinkRuntime` 位于传输接口一侧，不依赖 IPC、相机、存储或 Qt 网络类型。一个工作线程独占会话建立、心跳、状态和命令结果发送；同步传输调用不得出现在相机采集回调。服务端命令回调只写入固定容量队列，默认 64、最大 4096 条，同时受默认 8 MiB、最大 64 MiB 字节上限约束；任一上限满载都拒绝最新命令并计数。`request_stop()` 先关闭命令入口、触发停止令牌并调用 `disconnect()` 唤醒传输 I/O；`join()` 接收统一关闭截止时间。M8-04 的正式适配器为每次网络 I/O 设置上限，`disconnect()` 可取消当前 HTTP 或 WebSocket 等待。

连接失败、心跳失败、状态失败或传输侧断开都进入同一有上限指数退避，成功会话后重置退避。会话必须协商协议 v1、相同 `machineId` 和 1～3600 秒心跳间隔。连接成功立即上报状态，以后每个心跳周期同时发送心跳和新状态快照；状态提供器只返回不超过 1 MiB 的 JSON 对象。

远程命令由 `SystemCommandService` 映射到既有用例 dispatcher，复用字段白名单、配置 schema、乐观修订、相机能力回读、事件复核和停止门禁。配置写入上下文按入口区分 `local_ipc`/`uplink`；远程变更还要求协议人工确认和可用的 `audit` 日志，明文网络可达性不作为认证。命令结果按 `commandId` 保存在默认 1024、最大 4096 条且默认 16 MiB、最大 64 MiB 的进程内 FIFO 去重缓存；相同内容重放，不同内容冲突拒绝。M8-02 不增加 SQLite schema，跨重启上传任务和文件传输分别留给 M8-03/M8-04。

### 12.7 持久上传调度

M8-03 的 `PersistentUploadScheduler` 位于 uplink 编排层，SQLite 状态转换由 storage 层的 `EventMetadataDatabase` 封装。调度器只有一个工作线程，不建立进程内任务队列；每次从 SQLite 原子领取一项任务并调用注入的执行器。执行器边界隔离 M8-04 的真实传输和分块实现，且必须遵守既有传输会话所有权、单次 I/O 上限和停止令牌。

调度器停止后不再领取新任务，正在执行的回调收到停止令牌；中断任务以 `RetryWait` 和现有 checkpoint 落库。执行器返回成功、可重试、永久失败或需人工处理四种结果，仓库只接受从 `InProgress` 发起的完成/失败转换，避免过期执行器覆盖其他状态。仓库访问失败会记录指标并尝试收回遗留租约，不进行紧循环。

### 12.8 Qt 传输与分块执行器

`paperbreak_uplink_transport` 是唯一依赖 Qt Network/WebSockets 的生产上行适配器，公开头不暴露 Qt 类型。WebSocket 由一个专用 Qt 事件线程持有，同步发送最多一个在途等待；HTTP 调用由互斥门串行化，每次使用有截止时间和 1 MiB 响应上限的局部事件循环。停止令牌或 `disconnect()` 会中止当前等待，不创建进程内文件任务队列。

文件执行器在 M8-03 单调度线程中工作，以事件根目录解析受限相对路径，流式验证长度和整文件 SHA-256，按配置的 64 KiB～4 MiB 块大小上传。服务端 checkpoint 是恢复事实源；本地任务 checkpoint 只保存有界诊断和已确认块。预校验读盘和网络分块阶段均按同一非零速率上限节流，等待可取消，默认 20 MiB/s，不占用相机采集回调。服务组合根在 uplink 启用时装配运行时、调度器和执行器，为现有及新提交事件幂等建任务；`confirmed` 策略在复核确认后建任务。状态指标公开连接状态、最后心跳、待上传任务和字节数。

## 13. 启动与关闭

### 13.1 启动顺序

```text
1. 创建进程级停止源和最小应急日志
2. 加载版本、路径和安全上下文
3. 读取、校验配置；确定最后有效版本
4. 启动正式日志和指标/报警登记
5. 打开 SQLite，执行迁移、健康检查和备份策略
6. 扫描恢复临时事件、事件/数据库对账、NVMe 索引恢复
7. 创建存储、帧池、队列和工作执行器（尚不接收帧）
8. 启动 IPC，只发布 Starting/受限命令
9. 创建相机 Provider，发现/绑定并逐路启动会话
10. 启动处理、事件、健康和已启用的上传/Plant IO
11. 进入 Running，向 IPC/SCM 发布实际状态
```

启动失败时，按已完成步骤的逆序停止。配置、数据库或必要事件恢复失败是否阻止采集必须由错误分类明确决定；不能在无法保证事件保存时悄然进入“正常”状态。

### 13.2 关闭顺序

```text
1. ServiceRuntime 原子进入 StopRequested；停止接受配置和外部写命令
2. 停止上位机命令和新上传任务领取；持久化上传 checkpoint
3. 通知 IPC 客户端服务正在停止；停止新预览订阅
4. 请求全部相机停止采集，取消重连和 SDK 等待
5. 关闭 acquisition.frames 生产端
6. 在截止时间内排空预处理与已接受算法帧；禁止产生新候选
7. 事件管理器封闭当前窗口，完成或标记现有事件
8. 关键帧、事件写入和 NVMe 当前块完成提交，或留下可恢复状态
9. 提交数据库状态并关闭连接
10. 取消 Plant IO、健康和上传 I/O，等待线程 join
11. 停止 IPC，关闭客户端
12. 刷新并关闭日志
13. 向宿主/SCM 报告 Stopped
```

每一步接收同一总截止时间的剩余预算，而不是各自无限等待。所有阻塞 SDK、网络、文件和数据库调用必须具备超时、取消或可被关闭句柄唤醒的路径。

### 13.3 超时与恢复

- 超时组件返回稳定错误和未完成阶段；
- 未提交事件保留临时目录供下次恢复，不能直接删除；
- 上传任务在数据库中恢复为待处理；
- 普通预览和调试任务可直接丢弃；
- 服务宿主不得用未受控的线程强杀掩盖无确定性关闭缺陷；
- 若外部 SDK 确实无法取消，适配器必须限定最大等待并在硬件测试中验证，不能只作假设。

## 14. 故障隔离与降级

| 故障 | 局部处理 | 系统行为 |
| --- | --- | --- |
| Qt 客户端退出/崩溃 | 释放订阅和 IPC 资源 | 服务、采集和事件继续 |
| 单路相机掉线 | 当前会话进入 Recovering，退避重连 | 其他相机继续；报警 |
| MVS 参数写入失败 | 回读、恢复旧快照或 Faulted | 不发布伪成功配置 |
| 采集队列持续满 | 丢弃并统计、升级报警 | 不阻塞采集线程 |
| 算法单帧异常 | 捕获为业务错误、跳过该帧 | 采集与缓存继续 |
| 算法持续失败/积压 | 禁用或降级检测，显式报警 | 采集、缓存和人工触发仍可用 |
| 预览编码/客户端慢 | 覆盖旧预览 | 不影响采集和事件 |
| 内存缓存不足 | 保存可用窗口并标记缺口，Critical 报警 | 禁止宣称完整事件 |
| 事件写入失败 | 保留临时状态、重试有上限 | Critical；采集继续到资源策略上限 |
| 数据库异常 | 关闭写入、备份/恢复/对账 | 事件文件优先可靠提交；状态显示 degraded |
| NVMe 滚动缓存失败 | 停止普通块写入，降级内存 | 正式事件路径优先 |
| 数据盘到停止水位 | 禁止新大文件 | 持续检测、报警；事件明确未保存 |
| 上位机断开 | upload_jobs 持久排队、退避 | 本地检测和保存继续 |
| PLC/Plant IO 失败 | 有界重试和报警 | 视觉系统不能声称完成现场安全停机 |
| 系统时间跳变 | 记录报警，继续单调计时 | manifest 标记时间质量 |
| 服务异常退出 | SCM 恢复策略重启；启动执行恢复 | 不依赖 GUI |

“继续采集”受物理内存和磁盘安全上限约束；达到无法安全运行的条件时必须进入可诊断的受控降级/故障状态，而不是耗尽系统资源。

## 15. 错误模型

统一错误至少包含：

```text
businessCode      稳定字符串，如 CAMERA_OPEN_FAILED
severity          Info/Warning/Error/Critical
message           面向操作员的可读信息
module            camera/event/storage/...
operation         当前操作阶段
sourceId          CAM01、eventId、fileId 等
nativeCode        可选厂商/Win32/SQLite/网络原始码
details           脱敏结构化上下文
retryable         是否可重试
timestamp         墙上时间
```

规则：

- C 风格 SDK 回调边界捕获所有异常；
- 线程入口捕获顶层异常并转换为组件故障；
- 不使用异常表达高频正常状态，如队列满和超时；
- 错误日志、报警和 IPC 响应引用同一业务码；
- 密码、Token、私钥和完整敏感配置不进入 details；
- 重试必须有次数/时间上限和退避，不允许紧循环。

## 16. 安全边界

### 16.1 本机

- 服务使用专用低权限账户；
- 数据、配置、日志和证书目录采用最小 ACL；
- IPC 只接受本机连接并设置命名管道访问权限；
- 已认证本机操作员可执行全部运行期查询、控制和生产参数修改，不依赖管理员组或 UAC 提升；
- 服务对象 ACL 只向交互式登录用户开放查询、启动和停止，安装、卸载、配置变更和删除仍由管理员执行；
- 安装、服务重启、高风险相机参数和删除/导出操作写审计；
- UI 请求打开目录不能接受任意未校验路径。

### 16.2 网络与凭据

- Uplink v1 正式使用明文 HTTP/WS 且无应用鉴权；部署必须隔离网络并限制端口来源，明确接受窃听、伪造命令和中间人风险；
- 若未来其他协议或 Uplink 新版本使用证书和密钥，必须存于 Windows 安全存储或批准的受限位置；
- 普通 JSON 只保存 secret reference；
- 网络消息、文件名、URL、分块范围和响应大小全部校验；
- 重放保护和幂等语义在 M8 协议评审中固定。

### 16.3 供应链与部署

- 记录生产依赖的版本、引入理由和许可证；
- Release 不从开发机绝对路径加载 DLL；
- 安装/升级包执行签名校验；
- Mock、调试端点和人工触发在生产中可配置禁用或受权限控制。

## 17. 可观测性

### 17.1 指标

至少提供：

- 服务：运行时间、CPU、内存、线程、句柄、版本、状态；
- 磁盘：总量、剩余、写入速率、水位、事件/NVMe 占用；
- 数据库：状态、schema、写事务失败、备份时间、对账差异；
- 相机：状态、实际帧率、相机/服务序号缺口、超时、不完整帧、最后帧、亮度、尺寸、温度（若支持）、重连、曝光、增益、带宽；
- 算法：单帧/平均/最大耗时、队列、高水位、跳帧、异常、候选/确认/误报；
- 事件：活跃窗口、租约帧数、写队列、提交耗时、失败、缺口；
- IPC：连接、请求耗时、协议错误、推送丢弃和每客户端积压；
- 上位机：连接、心跳、待上传数/字节、重试、速率和最后成功；
- 所有队列：容量、深度、高水位、入/出、拒绝/丢弃、等待。

指标采样读取快照，不在采集热路径进行昂贵聚合。

### 17.2 报警

`AlarmRegistry` 是活动报警的单一状态源，支持 raise、合并、clear 和 ack。M1-06 将活动报警和已清除历史分别限制为 1024/4096 条，并只保留在进程内；M5-07 再接入 SQLite 历史持久化。报警推送可能丢失，但 UI 可按版本/游标查询恢复。托盘通知只是辅助。

至少覆盖需求列出的相机离线/占用/超时/帧率、算法积压、画面异常、配置、磁盘、NVMe、数据库、事件、上位机、上传、PLC 和系统时间异常。

### 17.3 日志

- 分类：service、camera、algorithm、event、storage、uplink、ipc、ui、audit、performance；
- 单一 `logging-worker` 异步线程、有界队列和 `overrun_oldest` 策略；业务线程只入队，不执行文件 I/O、轮转或刷新；
- 每个已注册功能线程分别按本地日期和大小滚动。服务文件为 `paperbreak-service-<thread-name>-YYYY-MM-DD.log[.N]`，控制台文件为 `paperbreak-console-<thread-name>-YYYY-MM-DD.log[.N]`；同名线程的后续实例在同一天继续追加；
- 进程内功能线程名必须唯一，只含小写字母、数字和连字符且不超过 63 字符。非法、重复、未注册或超过 64 个文件状态的写入进入 `unregistered-thread-<tid>` 应急文件并产生错误记录；
- 日志后台线程同时维护默认 2048 条的结构化近期日志环；IPC 查询只复制有界内存快照，不读取滚动文件；
- 文件和近期日志使用带本地偏移的 RFC 3339 毫秒时间；事件、配置等领域时间继续使用 UTC；
- 每条含逻辑线程名、线程 ID、模块、业务码和关联 ID；结构化扩展字段最多 16 个；
- `info` 是生产默认等级。逐帧数据流只允许写 `debug`，通过默认空操作的诊断回调注入且先检查等级，禁止记录原始图像、完整网络正文或凭据；
- 关键事件有独立事件日志；
- 诊断包使用一致快照并脱敏。

服务固定线程名包括 `service-main`、`ipc-event`、`ipc-command`、每相机的
`camera-acquisition-cam01`～`cam04` 和 `camera-forward-cam01`～`cam04`、
`event-processing`、`event-keyframe`、`event-persistence`、`preview-encoder`、
`nvme-writer`、`storage-maintenance`、`uplink-session`、`uplink-transport`、
`upload-scheduler`、`health-monitor`。控制台固定线程名为 `console-gui`、
`console-service-restart`、`console-event-export` 和 `console-diagnostics-export`。
其他自有线程按“模块-功能-实例”命名并在入口用 RAII 注册，同时同步 Windows 线程描述。
SCM 启停阶段的 `service-scm-status` 在线程日志运行时创建之前工作，只设置 Windows 原生描述且不写业务日志。

## 18. 目录结构

计划结构：

```text
/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ cmake/
├─ config/
│  ├─ default-config.json
│  └─ schemas/
├─ docs/
│  ├─ architecture/
│  │  ├─ system-architecture.md
│  │  ├─ domain-model.md
│  │  ├─ dependencies.md
│  │  └─ decisions/
│  ├─ requirements/
│  ├─ roadmap/
│  ├─ ipc-protocol.md
│  ├─ config-schema.md
│  └─ event-format.md
├─ src/
│  ├─ common/
│  ├─ config/
│  ├─ logging/
│  ├─ platform/
│  │  ├─ interfaces/
│  │  └─ windows/
│  ├─ camera/
│  │  ├─ interfaces/
│  │  ├─ mock/
│  │  └─ hikrobot/
│  ├─ pipeline/
│  ├─ algorithm/
│  │  ├─ interfaces/
│  │  ├─ mock/
│  │  └─ classical/
│  ├─ event/
│  ├─ storage/
│  ├─ uplink/
│  ├─ plant_io/
│  ├─ monitoring/
│  ├─ ipc/
│  ├─ service/
│  │  ├─ core/
│  │  └─ windows/
│  └─ console/
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  ├─ simulation/
│  └─ hardware/
├─ tools/
├─ packaging/
└─ .agent/
   └─ plans/
```

目录不能代替 target 边界；禁止通过相对路径包含其他模块私有头文件。

## 19. 测试架构

### 19.1 测试层次

| 层次 | 目标 | 默认外部依赖 |
| --- | --- | --- |
| 单元测试 | 状态机、队列、配置、时间、错误、选择器、迁移函数 | 无 |
| 组件测试 | Camera Mock、Pipeline、Event、Storage、IPC 编解码 | 临时目录/内存数据库 |
| 模拟集成 | 四路模拟相机、故障注入、事件全链、离线上传 | Mock Camera/Uplink/Plant IO |
| 平台集成 | SCM、ACL、安装、路径和 Windows 关闭 | 隔离 Windows 测试机 |
| 硬件集成 | MVS 枚举、参数回读、取流、拔线恢复、四路带宽 | 目标工控机和相机 |
| 系统验收 | 性能、磁盘/网络故障、事件正确性、168 小时 | 生产等价环境 |

默认 CTest 不依赖真实相机；硬件测试有明确标签和环境检查，缺少硬件时报告 skipped，不伪装 passed。

### 19.2 故障注入点

接口需允许测试注入：

- 相机掉线、超时、丢帧、尺寸/格式变化和 SDK 错误；
- 队列满、池耗尽和线程停止竞争；
- 算法慢、异常、配置失败和无效结果；
- 文件创建/写入/flush/rename 失败、磁盘满和校验损坏；
- SQLite 事务失败、损坏和迁移中断；
- IPC 拆包、粘包、超长、慢客户端和重连；
- 网络离线、慢速、重复确认、分块校验错误；
- 系统时间跳变和服务阶段性启动/关闭失败。

## 20. 架构决策记录索引

独立 ADR 后续存放于 `docs/architecture/decisions/`。未创建独立 ADR 前，本表是决策索引。

| ADR | 状态 | 决策 | 依据/后续 |
| --- | --- | --- | --- |
| ADR-001 | Accepted | 后台服务与 Qt 客户端拆为两个进程 | Windows Session 0、GUI 生命周期隔离 |
| ADR-002 | Accepted | ServiceRuntime 与 SCM/Console 宿主分离 | 可测试性和单一生命周期实现 |
| ADR-003 | Accepted | MVS SDK 使用独立实现目标隔离 | 防止厂商类型和调用泄漏 |
| ADR-004 | Accepted | 所有跨线程通道固定容量并观测背压 | 稳定性和内存上限 |
| ADR-005 | Accepted | 帧用池化只读共享所有权 | 跨分支零拷贝与 RAII |
| ADR-006 | Accepted | 单调时间用于窗口，墙上时间用于展示 | 抵抗系统时间跳变 |
| ADR-007 | Accepted | 事件使用临时目录、manifest 最后写、原子提交 | 防止读取半成品并支持恢复 |
| ADR-008 | Accepted | SQLite 只存元数据，事件文件独立保存 | 避免高速图像 BLOB 和数据库膨胀 |
| ADR-009 | Accepted | 本机 IPC 使用 QLocalServer/QLocalSocket | Qt 集成和本机命名管道 |
| ADR-010 | Proposed | 初版算法采用进程内接口实现，保留以后隔离进程的可能 | M6 在 ABI/故障隔离评审时确认 |
| ADR-011 | Accepted | 每相机 1 秒原始块、显式小端头/索引/提交尾页、CRC32C、上界容量和 80% 持续带宽准入 | `decisions/adr-011-nvme-rolling-cache-format-capacity.md` |
| [ADR-012](decisions/adr-012-uplink-v1-plaintext-protocol.md) | Accepted | Uplink v1 明文无鉴权协议、断点续传与参考模拟器 | M8-00 |
| ADR-013 | Deferred | Plant IO 生产协议 | DEC-005/另行批准 |
| ADR-014 | Deferred | 安装器技术 | M9-01 |
| ADR-015 | Accepted | VS 2026/v145、CMake 4.2、外部 SDK 与 vcpkg manifest 基线 | `decisions/adr-015-windows-toolchain-dependencies.md` |

## 21. 需求追踪

| 需求章节 | 架构落点 |
| --- | --- |
| 第 3 节：两个进程 | 第 3、4 节 |
| 第 4.1～4.4 节：服务、配置和相机控制 | 第 4、5、6、10、12、13 节 |
| 第 4.5～4.8 节：采集、帧、管线和预览 | 第 6～9 节 |
| 第 4.9～4.14 节：算法、事件、缓存、关键帧 | 第 5～10、12 节 |
| 第 4.15～4.17 节：事件、数据库、存储 | 第 9、12、14 节 |
| 第 4.18～4.19 节：上位机和 Plant IO | 第 5、9、14、16 节 |
| 第 4.20～4.22 节：监测、报警和日志 | 第 8、14、17 节 |
| 第 5 节：Qt 客户端 | 第 4.2、5、9、11 节 |
| 第 6 节：IPC | 第 4、8、11、16 节 |
| 第 7 节：线程模型 | 第 7、8、13 节 |
| 第 8 节：相机状态机 | 第 10.1 节 |
| 第 9 节：目录 | 第 5.2、18 节 |
| 第 11 节：错误码 | 第 14、15 节 |
| 第 12 节：安全权限 | 第 11、16 节 |
| 第 13 节：稳定性、性能和正确性 | 第 2、8、12～14、17、19 节 |
| 第 16 节：代码约束 | 第 2、5～8、13、15、19 节 |

## 22. 架构合规门禁

每个后续任务的代码评审至少回答：

1. 新代码属于哪个目标，是否只使用允许依赖？
2. 是否引入新线程、队列、缓存或在途任务？若是，容量、溢出、指标和停止行为是什么？
3. 是否在采集热路径加入了 I/O、编码、推理、阻塞或大块分配？
4. 是否把平台、MVS、Qt Widgets、SQLite 或网络实现类型泄漏到领域接口？
5. 是否改变配置、IPC、事件、数据库或块格式版本？
6. 故障时是否可能静默丢事件、伪报成功或影响无关相机？
7. 服务停止时如何取消当前 I/O 并在截止时间内 join？
8. 无实体相机和上位机时如何自动测试？
9. 公开行为或模块边界变化是否已更新本文档和相关 ADR？

违反第 2.2 节强制约束的实现不得合并。架构中标记为 Proposed/Deferred 的决策在对应里程碑前不能被实现默认为 Accepted。
