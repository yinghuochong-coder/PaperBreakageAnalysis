# 工控机端功能清单与开发方案



**目标平台：** Windows 10/11 x64\
**开发技术：** C++、Qt、CMake、海康机器人 MVS SDK\
**目标相机：** 海康 MV-CS020-60GM，单机最多接入4路

***

## 1. 建设目标



工控机端软件负责：

1. 管理1～4路工业相机；
2. 持续、高速、稳定地采集图像；
3. 执行图像预处理和断纸检测算法；
4. 保存断纸前后原始图像；
5. 生成断纸候选事件和正式事件；
6. 向上位机发送状态、报警、关键帧和事件文件；
7. 在上位机或中心网络离线时继续独立工作；
8. 提供本机配置、状态监测和低帧率预览界面。

系统必须满足以下基本原则：

* 图形界面退出不得影响相机采集；
* 上位机离线不得影响本地断纸检测；
* 中心网络中断时，事件先保存在本地；
* 网络恢复后自动补传；
* 相机掉线后自动重连；
* 软件异常后能够自动恢复；
* 原始高速图像不持续上传给上位机。

***

## 2. 技术基线

### 2.1 操作系统



优先采用： Windows 11 版本

### 2.2 编译与开发环境

批准以下基线；详细版本、获取、许可证和升级规则见 `docs/architecture/dependencies.md`：

```
开发环境：Visual Studio 2026 stable
编译器：MSVC v145 x64
C++标准：C++20
Qt：Qt 6.10.2 msvc2022_64
CMake：最低 4.2
构建工具：Ninja 或 Visual Studio Generator
架构：仅支持 x86_64
字符集：UTF-8
```

项目根目录提供 `CMakePresets.json`，统一 Debug、Release、测试及部署构建参数；开发人员的本地路径放入不提交版本库的 `CMakeUserPresets.json`。CMake 4.2 是支持 Visual Studio 18 2026 Generator 的最低版本。

### 2.3 主要依赖

外部 SDK 不由 vcpkg 下载，其安装根目录通过环境变量或不提交的 `CMakeUserPresets.json` 注入。禁止把以下 SDK 的开发机绝对路径写入提交的项目预设、Release 配置或安装产物。

必需外部 SDK：

Qt 6.10.2 `msvc2022_64`：

```
Qt6::Core
Qt6::Widgets
Qt6::Network
Qt6::Gui
Qt6::Concurrent（按需）
```

OpenCV 4.12.0：首期批准 core、imgproc、imgcodecs。

海康机器人 MVS SDK Development/Runtime 4.8.0.3。

海康机器人官网当前提供 Windows 平台的 MVS 软件及相机开发支持，项目应将 MVS SDK 封装在独立适配层内，禁止业务模块直接调用厂商 API。

批准通过固定 baseline 的 vcpkg manifest 管理：

```
spdlog          1.17.0#1   日志
nlohmann/json   3.12.0#2   配置和消息序列化
GoogleTest      1.17.0#3   单元测试
SQLite3         3.53.4     事件元数据和上传任务
zstd            1.5.7      事件数据压缩，可选且默认关闭
```



***

## 3. 总体软件架构

采用两个独立进程：

```
┌──────────────────────────────────────┐
│ PaperBreakEdgeService.exe            │
│ Windows后台服务                       │
│                                      │
│ 相机管理 / 图像采集 / 算法 / 缓存     │
│ 事件保存 / 状态监控 / 上位机通信       │
└──────────────────┬───────────────────┘
                   │ 本机IPC
                   │ QLocalServer/Socket
┌──────────────────▼───────────────────┐
│ PaperBreakEdgeConsole.exe            │
│ Qt桌面配置程序                         │
│                                      │
│ 系统托盘 / 参数配置 / 状态显示         │
│ 低帧率预览 / 日志和事件查看            │
└──────────────────────────────────────┘

```

### 3.1 为什么必须拆成两个进程

`PaperBreakEdgeService` 注册为自动启动 Windows 服务，由 Windows Service Control Manager 管理，适合从开机到关机持续运行的后台任务。

Windows 服务运行在隔离的 Session 0 中，不能直接提供交互式桌面界面，因此服务和 Qt 配置界面应分开实现。

本机进程间通信建议使用 `QLocalServer` 和 `QLocalSocket`。在 Windows 上，该机制底层可使用命名管道，适合传输控制命令、状态和低频预览数据。

***

# 4. Windows服务端功能清单

## 4.1 服务生命周期管理

优先级：P0

功能要求：

* 注册、卸载 Windows 服务；
* 支持自动启动；
* 支持启动、停止和重新启动；
* 正确处理系统关机；
* 启动时加载最后一次有效配置；
* 启动失败时记录明确原因；
* 向 SCM 上报启动中、运行中、停止中和已停止状态；
* 支持服务异常退出后自动重启；
* 服务停止时安全关闭相机并刷新事件文件；
* 禁止服务进程显示任何界面。

建议命令：

```
PaperBreakEdgeService.exe --install
PaperBreakEdgeService.exe --uninstall
PaperBreakEdgeService.exe --console
PaperBreakEdgeService.exe --validate-config

```

`--console` 用于开发调试，以普通控制台进程运行同一套核心代码。

***

## 4.2 配置管理

优先级：P0

功能要求：

* 从本地JSON文件读取配置；
* 对配置进行类型、范围及依赖关系校验；
* 配置包含版本号和修改时间；
* 写配置时使用临时文件加原子替换；
* 保留最近若干历史版本；
* 配置应用失败时回滚；
* 区分立即生效和重启后生效参数；
* 所有配置修改写入审计日志；
* 支持从上位机接收配置；
* 支持本地界面修改配置；
* 配置冲突时按版本号处理。

建议配置范围：

```
系统参数
相机参数
采集参数
预览参数
算法参数
事件参数
存储参数
上位机连接参数
PLC/IO参数
日志参数
健康监测参数

```

***

## 4.3 相机发现与绑定

优先级：P0

功能要求：

* 调用 MVS SDK 枚举 GigE 相机；
* 显示相机型号、序列号、IP和网卡信息；
* 按序列号绑定逻辑相机编号；
* 支持最多4路相机；
* 检测序列号重复；
* 检测配置相机缺失；
* 防止错误相机占用配置槽位；
* 支持相机连接、断开和重新连接；
* 记录相机连接错误码；
* 检查相机是否被其他程序占用。

逻辑编号示例：

```
CAM01：压榨部入口
CAM02：压榨部出口
CAM03：烘干部入口
CAM04：卷取部

```

业务逻辑只能使用逻辑编号，不直接依赖当前IP地址。

***

## 4.4 相机参数控制

优先级：P0

每路相机支持：

* 设备连接状态；
* 曝光时间；
* 增益；
* 帧率；
* ROI宽度、高度和偏移；
* 像素格式；
* 触发模式；
* 触发源；
* 触发延迟；
* GigE包大小；
* 网络传输延迟；
* 相机数字输入输出；
* 参数读取；
* 参数写入；
* 写入后回读验证；
* 保存用户参数集；
* 恢复默认参数。

配置界面修改参数时：

```
界面提交
  ↓
服务校验
  ↓
暂停对应相机采集（仅必要时）
  ↓
写入相机
  ↓
回读实际值
  ↓
恢复采集
  ↓
返回应用结果

```

不得仅依据 SDK 写入函数返回成功就认为参数已生效。

***

## 4.5 图像采集

优先级：P0

每路相机建立独立采集上下文：

```
CameraSession
├── 设备句柄
├── 相机状态
├── 采集线程
├── 有界帧队列
├── 帧统计
├── 重连控制器
└── 参数快照

```

采集功能：

* 连续采集模式；
* 硬件触发采集模式；
* 软触发测试模式；
* 获取相机帧号；
* 获取相机时间戳；
* 生成工控机接收时间戳；
* 检测帧号跳变；
* 检测接收超时；
* 检测图像尺寸变化；
* 检测像素格式变化；
* 统计实际帧率；
* 统计丢帧数；
* 统计接收带宽；
* 记录最近一帧时间；
* 支持采集线程优先级配置。

相机回调或取流线程中禁止执行：

* 图像编码；
* 磁盘写入；
* 网络发送；
* 深度学习推理；
* 大块内存反复申请；
* 阻塞式界面通信。

采集线程只负责获得帧、填充元数据并推入有界队列。

***

## 4.6 帧对象定义

建议核心帧结构：

```
struct FramePacket {
    std::string cameraId;
    uint64_t cameraFrameNumber;
    uint64_t sequenceNumber;

    std::chrono::steady_clock::time_point monotonicTime;
    std::chrono::system_clock::time_point wallClockTime;

    uint32_t width;
    uint32_t height;
    uint32_t stride;
    PixelFormat pixelFormat;

    std::shared_ptr<FrameBuffer> buffer;

    bool incomplete;
    bool timestampValid;
};

```

要求：

* 图像内存使用对象池；
* 使用RAII管理相机句柄和图像内存；
* 帧对象在管线内尽量零拷贝传递；
* 只有预览编码、事件落盘等分支在必要时复制；
* 不允许裸指针跨线程长期持有。

***

## 4.7 图像处理管线

优先级：P0/P1

建议数据流：

```
采集线程
   ↓
采集有界队列
   ↓
图像预处理
   ├──→ 算法检测
   ├──→ 内存环形缓存
   ├──→ NVMe滚动缓存
   └──→ 低帧率预览抽样

```

预处理功能：

* ROI二次裁剪；
* 灰度统计；
* 亮度归一化；
* 图像翻转或旋转；
* 坏点或固定噪声处理；
* 可选背景校正；
* 可选降采样；
* 画面有效性检测；
* 相机遮挡检测；
* 频闪异常检测。

每个处理步骤实现为可配置节点，避免把全部逻辑写进一个函数。

***

## 4.8 低帧率实时预览

优先级：P0

要求：

* 原始采集保持目标帧率；
* 预览独立抽样，不降低采集帧率；
* 默认预览2～5 fps；
* 支持单路和四宫格；
* 支持降低分辨率；
* 支持亮度、帧率、帧号和状态叠加；
* 支持算法ROI和检测结果叠加；
* 支持JPEG预览编码；
* 无界面连接时停止预览编码；
* 界面最小化后可自动降低预览帧率；
* 预览队列只保留最新帧，旧帧直接丢弃；
* 预览卡顿不得阻塞实时处理。

预览数据属于“尽力传输”，不是可靠事件数据。

***

## 4.9 断纸算法接口

优先级：P1

算法模块必须插件化，不与采集模块直接耦合。

建议接口：

```
class IBreakDetector {
public:
    virtual ~IBreakDetector() = default;

    virtual bool initialize(const DetectorConfig& config) = 0;
    virtual DetectionResult process(const FrameView& frame) = 0;
    virtual bool updateConfig(const DetectorConfig& config) = 0;
    virtual void reset() = 0;
    virtual DetectorInfo info() const = 0;
};

```

`DetectionResult` 至少包含：

```
是否异常
候选类型
置信度
异常区域
纸幅面积比例
画面变化量
算法耗时
模型版本
触发原因
调试指标

```

第一阶段允许使用模拟检测器：

* 手动触发候选事件；
* 固定周期触发；
* 根据平均灰度变化触发；
* 根据ROI纸幅占比触发。

先完成完整事件链，再替换为正式算法。

***

## 4.10 候选事件状态机

优先级：P1

建议状态：

```
Idle
  ↓
Suspicious
  ↓
Candidate
  ├──→ Confirmed
  ├──→ Rejected
  └──→ Timeout

```

状态定义：

* `Idle`：没有异常；
* `Suspicious`：单帧或短暂异常；
* `Candidate`：满足候选阈值，立即保护缓存；
* `Confirmed`：满足连续帧、模型或外部信号确认条件；
* `Rejected`：被判定为误报；
* `Timeout`：未在限定时间内完成确认。

候选事件生成时立即执行：

1. 分配全局事件ID；
2. 记录候选触发时间；
3. 锁定事件前缓存；
4. 开始收集事件后图像；
5. 保存候选关键帧；
6. 通知Qt界面；
7. 向上位机发送候选事件元数据；
8. 等待后续确认。

不得等到正式确认后才开始保护缓存。

***

## 4.11 断纸前后图像冻结

优先级：P1

配置项：

```
preEventSeconds：断纸前保存时长
postEventSeconds：断纸后保存时长
maxEventSeconds：单事件最大时长
mergeGapSeconds：相邻候选事件合并间隔

```

工作流程：

```
正常情况下循环覆盖缓存
          ↓
候选事件在时间T触发
          ↓
保护 T-preEventSeconds 至 T 的已有数据
          ↓
继续记录至 T+postEventSeconds
          ↓
形成完整事件片段
          ↓
异步落盘并生成清单

```

冻结是“禁止缓存数据被覆盖”，不是停止相机采集。

相邻事件窗口发生重叠时，应支持合并，避免重复保存大量相同图像。

***

## 4.12 内存环形缓存

优先级：P1

要求：

* 每路相机独立缓存；
* 使用固定容量对象池；
* 按帧数或时长配置容量；
* 正常情况下覆盖最旧帧；
* 事件触发时增加引用或转移所有权；
* 缓存满时不得无限扩张；
* 输出当前缓存时长；
* 输出内存使用量；
* 支持缓存不足告警。

第一版建议先实现内存环形缓存，再实现NVMe滚动缓存。

***

## 4.13 NVMe滚动缓存

优先级：P2

功能要求：

* 将连续图像按固定时间片写入NVMe；
* 例如每1秒或5秒生成一个数据块；
* 数据块附带索引和校验信息；
* 按总容量循环删除最旧数据块；
* 事件触发时保护相关数据块；
* 支持异常断电后的索引恢复；
* 支持磁盘写入限速；
* 支持剩余空间阈值；
* 支持缓存盘和系统盘分离；
* 磁盘不可用时降级为内存缓存；
* 不得因磁盘写入阻塞采集线程。

建议第一版文件格式：

```
Block Header
Frame Index Table
Raw Frame Data
CRC/Hash

```

不要在第一版自行设计复杂视频编码格式；优先保证逐帧可恢复和时间定位。

***

## 4.14 关键帧选择

优先级：P1

每个事件至少选择：

* 正常参考帧；
* 最早异常帧；
* 候选触发帧；
* 最大变化帧；
* 最高置信度帧；
* 正式确认帧；
* 断纸后状态帧。

关键帧选择接口：

```
class IKeyFrameSelector {
public:
    virtual std::vector<KeyFrame>
    select(const EventFrameSequence& frames,
           const EventMetadata& event) = 0;
};

```

关键帧优先生成JPEG，用于：

* Qt界面快速显示；
* 上位机快速报警；
* 事件列表缩略图；
* 人工复核；
* 报告生成。

完整原始序列仍需保留，关键帧不能替代原始事件片段。

***

## 4.15 事件保存

优先级：P1

事件目录建议：

```
data/
└── events/
    └── 2026/
        └── 07/
            └── 30/
                └── EVT-20260730-135501-000123/
                    ├── manifest.json
                    ├── thumbnails/
                    ├── keyframes/
                    ├── raw/
                    │   ├── CAM01/
                    │   ├── CAM02/
                    │   └── ...
                    ├── preview.mp4
                    └── event.log

```

`manifest.json` 至少包含：

```
schemaVersion
eventId
eventState
candidateTime
confirmedTime
startTime
endTime
cameraIds
triggerCameraId
triggerFrameNumber
triggerReason
confidence
preEventSeconds
postEventSeconds
keyFrames
rawFiles
algorithmName
algorithmVersion
configVersion
machineId
productionLineId
paperType
paperSpeed
uploadState
fileChecksums

```

写入要求：

* 先写临时目录；
* 文件全部完成后生成manifest；
* 最后通过原子重命名提交事件；
* 程序崩溃后能识别未完成事件；
* 对未完成事件执行恢复或标记损坏；
* 不允许上位机读取仍在写入的事件目录。

***

## 4.16 本地事件数据库

优先级：P1

SQLite只保存元数据，不保存高速原始图像。

建议表：

```
events
event_cameras
key_frames
event_files
event_retention
upload_jobs
device_status_history
config_history
alarm_history
audit_logs

```

数据库功能：

* 事件分页查询；
* 按时间、状态、相机筛选；
* 上传任务管理；
* 配置版本管理；
* 日志索引；
* 数据库损坏检测；
* 自动备份；
* 数据库迁移版本管理。

每次数据库结构变更必须提供迁移脚本。

***

## 4.17 本地存储管理

优先级：P1

功能要求：

* 检测磁盘总容量和剩余容量；
* 设置预警水位；
* 设置严重水位；
* 设置停止保存水位；
* 优先删除已上传、已确认可删除的最旧事件；
* 未上传事件默认禁止自动删除；
* 删除事件前更新数据库状态；
* 删除失败时报警；
* 定期清理临时文件；
* 支持事件保留天数；
* 支持事件容量上限；
* 支持人工锁定重要事件；
* 支持导出事件。

推荐策略：

```
磁盘正常：
    正常保存

达到预警水位：
    上报告警，清理已上传旧事件

达到严重水位：
    停止普通滚动缓存，保留正式事件

达到停止水位：
    禁止新增大文件，持续检测并输出严重报警

```

***

## 4.18 上位机通信

优先级：P2

业务模块不得直接绑定某一种通信协议，定义：

```
class IUplinkTransport {
public:
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool sendHeartbeat(const Heartbeat&) = 0;
    virtual bool sendEventMetadata(const EventMetadata&) = 0;
    virtual bool uploadFile(const UploadFileRequest&) = 0;
    virtual void setCommandHandler(CommandHandler) = 0;
};

```

正式 Uplink v1 采用：

* 明文 HTTP/REST：会话、事件元数据和分块文件上传；
* 明文 WebSocket：心跳、状态、报警、命令、确认、命令结果及低帧率 JPEG 预览；
* 无 TLS、无应用鉴权，参考服务端默认监听全部网卡；
* 请求/消息/分块幂等、SHA-256 校验和断点续传。

完整端点、信封、限制、命令能力和二进制预览格式见 `docs/uplink-protocol-v1.md`。该安全方案不防窃听、伪造命令或中间人攻击，只能部署在隔离 VLAN，并由防火墙、ACL 和物理访问控制限制来源。`PaperBreakUplinkSimulator` 是 M8-00 参考服务端和人工联调工具，不是完整上位机；M8-01 只完成传输抽象与 Mock，心跳/命令编排、持久上传和真实 HTTP/WebSocket 边缘适配仍由 M8-02～M8-04 实现。

通信要求：

* 心跳；
* 自动重连；
* 指数退避；
* 连接状态显示；
* 发送队列持久化；
* 文件断点续传；
* 文件校验；
* 服务端确认；
* 重复消息幂等；
* 上位机离线时本地排队；
* 恢复连接后按优先级补传。

上传优先级：

```
1. 报警元数据
2. 关键帧
3. 事件清单
4. 低码率回放文件
5. 原始图像文件

```

***

## 4.19 PLC及现场IO接口

优先级：P2

定义抽象接口：

```
class IPlantIoAdapter {
public:
    virtual PlantSignals readSignals() = 0;
    virtual bool writeVisionAlarm(const VisionAlarm&) = 0;
};

```

预留支持：

* Modbus TCP；
* OPC UA；
* 数字量IO；
* 自定义PLC协议。

采集的生产信号可以包括：

* 纸机运行状态；
* 纸机速度；
* 张力；
* 生产纸种；
* 班次；
* PLC断纸信号；
* 引纸状态；
* 停机状态。

现场安全停机不得只依赖上位机通信。

***

## 4.20 健康监测

优先级：P0/P1

系统级指标：

* 服务运行时间；
* CPU使用率；
* 内存使用率；
* 线程数量；
* 句柄数量；
* 系统盘和数据盘空间；
* NVMe写入速率；
* 数据库状态；
* 上位机连接状态；
* 最后心跳时间；
* 当前事件数量；
* 待上传任务数量。

相机级指标：

* 连接状态；
* 实际帧率；
* 丢帧数量；
* 接收超时次数；
* 最近一帧时间；
* 图像亮度；
* 图像尺寸；
* 相机温度，若设备支持；
* 重连次数；
* 当前曝光和增益；
* 当前采集带宽。

算法级指标：

* 每帧处理耗时；
* 平均耗时；
* 最大耗时；
* 处理队列深度；
* 跳过帧数量；
* 候选事件数量；
* 正式事件数量；
* 误报数量。

***

## 4.21 故障与报警

优先级：P0/P1

至少定义以下报警：

```
相机离线
相机被占用
连续接收超时
丢帧率超限
采集帧率过低
算法处理积压
频闪或亮度异常
画面遮挡
服务配置无效
数据盘空间不足
NVMe写入失败
数据库异常
事件保存失败
上位机断开
事件上传失败
PLC通信失败
系统时间异常

```

报警包含：

```
alarmCode
severity
source
firstOccurredAt
lastOccurredAt
active
occurrenceCount
message
details
acknowledged

```

等级建议：

* Info；
* Warning；
* Error；
* Critical。

系统托盘消息只能作为辅助提醒，不能作为关键报警的唯一显示方式。Qt官方也说明托盘消息可能受系统设置影响而不显示。

***

## 4.22 日志系统

优先级：P0

日志分类：

```
service
camera
algorithm
event
storage
uplink
ipc
ui
audit
performance

```

要求：

* 异步日志；
* 按日期和大小滚动；
* 支持日志等级；
* 每条日志包含时间、线程、模块和错误码；
* 不记录原始密码和密钥；
* 支持界面查看最近日志；
* 支持一键导出诊断包；
* 支持自动删除过期日志；
* 关键事件单独生成事件日志。

诊断包建议包含：

```
当前配置脱敏副本
最近日志
系统信息
相机信息
网络信息
数据库健康信息
最近报警
软件版本
依赖版本

```

***

# 5. Qt配置客户端功能清单

## 5.1 系统托盘

优先级：P0

使用 `QSystemTrayIcon` 实现。Qt提供托盘图标、右键菜单、点击事件及通知消息接口。

托盘状态颜色：

```
绿色：服务正常，所有启用相机正常
黄色：存在警告或部分设备异常
红色：严重故障或服务未运行
灰色：无法连接本地服务

```

右键菜单：

```
打开控制台
显示当前状态
暂停/恢复预览
重启后台服务
打开事件目录
导出诊断包
关于
退出界面

```

“退出界面”只关闭 Qt 客户端，不停止后台服务。

双击托盘图标打开主窗口。

***

## 5.2 主界面

优先级：P0

建议页面：

```
总览
实时预览
相机配置
算法配置
事件配置
存储配置
上位机配置
设备状态
报警记录
事件记录
系统日志
系统维护

```

主界面顶部显示：

* 服务状态；
* 当前时间；
* 工控机编号；
* 上位机连接状态；
* 正常相机数量；
* 当前报警数量；
* 数据盘剩余容量；
* 软件版本。

***

## 5.3 总览页面

显示：

* 1～4路相机状态卡片；
* 实际帧率；
* 丢帧数；
* 图像亮度；
* 最近一帧时间；
* 预览缩略图；
* 当前检测状态；
* 当前候选事件；
* CPU、内存和磁盘；
* 上位机连接；
* 待上传事件数；
* 最近报警。

***

## 5.4 实时预览页面

功能：

* 单画面；
* 2×2四宫格；
* 全屏；
* 自适应缩放；
* 1:1显示；
* 暂停显示；
* 单帧抓图；
* 显示ROI；
* 显示算法检测区域；
* 显示帧率、帧号和时间；
* 选择预览帧率；
* 选择预览分辨率；
* 支持相机状态覆盖层。

“暂停显示”不能暂停后台采集。

***

## 5.5 相机配置页面

每路相机显示：

* 逻辑编号；
* 安装位置；
* 型号；
* 序列号；
* IP；
* 当前状态；
* 曝光；
* 增益；
* 帧率；
* ROI；
* 像素格式；
* 触发模式；
* 触发源；
* 网络参数；
* 采集启用状态。

操作：

```
搜索设备
绑定相机
连接
断开
开始采集
停止采集
应用参数
读取参数
恢复上次参数
抓取测试图
执行软触发

```

危险操作需要二次确认。

***

## 5.6 算法配置页面

功能：

* 算法启用；
* 算法类型；
* 检测ROI；
* 候选阈值；
* 确认阈值；
* 连续帧数；
* 冷却时间；
* 模型路径；
* 模型版本；
* 运行设备；
* 调试可视化；
* 算法性能统计；
* 测试当前图像；
* 恢复默认参数。

算法参数修改后应显示：

```
已保存
已下发
已应用
应用失败
需要重启

```

***

## 5.7 事件配置页面

配置：

* 事件前保存秒数；
* 事件后保存秒数；
* 最大事件时长；
* 事件合并间隔；
* 关键帧数量；
* 原始数据保存开关；
* 预览视频生成开关；
* 上传策略；
* 保留策略；
* 人工事件触发按钮。

人工触发功能用于系统联调，必须在事件中标记：

```
triggerSource = ManualTest

```

***

## 5.8 状态和报警页面

功能：

* 当前报警；
* 历史报警；
* 按级别筛选；
* 按来源筛选；
* 报警确认；
* 查看详细信息；
* 跳转至对应配置页；
* 导出报警记录。

Qt客户端与服务断开时，界面要明确显示“后台服务连接中断”，不能继续展示过期状态而没有提示。

***

## 5.9 事件查看页面

第一阶段实现：

* 事件列表；
* 事件时间；
* 事件状态；
* 触发相机；
* 置信度；
* 上传状态；
* 关键帧缩略图；
* 打开事件目录；
* 查看manifest；
* 标记确认或误报；
* 重新上传；
* 导出事件。

后续实现：

* 多相机同步回放；
* 逐帧查看；
* 时间轴；
* 算法结果叠加；
* PLC数据曲线。

***

# 6. 服务与Qt客户端IPC设计

## 6.1 基本原则

* 服务作为 IPC Server；
* Qt客户端作为 IPC Client；
* 使用长度前缀消息；
* 消息体使用JSON；
* 大尺寸预览图采用二进制负载；
* 每条请求具有唯一 `requestId`；
* 支持请求、响应和服务端推送；
* 所有配置写操作由服务端校验；
* Qt客户端不得直接读写相机；
* Qt客户端不得直接修改生产配置文件。

***

## 6.2 消息封装

```
{
  "protocolVersion": 1,
  "messageType": "request",
  "requestId": "uuid",
  "command": "camera.updateConfig",
  "timestamp": "2026-07-30T13:47:00.123+08:00",
  "payload": {}
}

```

响应：

```
{
  "protocolVersion": 1,
  "messageType": "response",
  "requestId": "uuid",
  "success": true,
  "errorCode": "",
  "message": "",
  "payload": {}
}

```

***

## 6.3 IPC命令清单

系统命令：

```
system.getStatus
system.getVersion
system.getMetrics
system.reloadConfig
system.exportDiagnostics

```

相机命令：

```
camera.list
camera.discover
camera.connect
camera.disconnect
camera.start
camera.stop
camera.getConfig
camera.updateConfig
camera.captureSnapshot
camera.softwareTrigger

```

预览命令：

```
preview.subscribe
preview.unsubscribe
preview.updateOptions

```

算法命令：

```
algorithm.getConfig
algorithm.updateConfig
algorithm.getStatus
algorithm.reset
algorithm.testFrame

```

事件命令：

```
event.list
event.get
event.manualTrigger
event.confirm
event.reject
event.retryUpload

```

日志和报警：

```
alarm.list
alarm.acknowledge
log.tail

```

服务端推送：

```
status.changed
camera.statusChanged
camera.frameStats
preview.frame
alarm.raised
alarm.cleared
event.created
event.updated
upload.progress

```

***

# 7. 线程模型

建议线程划分：

```
主控制线程
IPC线程
每路相机一个采集线程
图像处理工作线程池
预览编码线程
事件管理线程
磁盘写入线程
上传线程
健康监测线程
日志后台线程

```

关键规则：

1. 所有跨线程队列必须有容量上限；
2. 队列满时必须有明确策略；
3. 事件数据优先于预览数据；
4. 预览队列满时丢弃旧帧；
5. 算法队列积压时允许按策略跳帧；
6. 事件保存队列满时产生严重报警；
7. 任何线程不得无限等待；
8. 所有线程必须支持停止令牌；
9. 服务停止时按顺序关闭；
10. 禁止由工作线程直接更新Qt界面。

建议关闭顺序：

```
停止接收新配置
停止上位机命令
停止相机采集
等待处理队列排空
完成正在写入的事件
保存数据库状态
停止上传任务
停止IPC
刷新日志
退出服务

```

***

# 8. 相机状态机

```
Disabled
  ↓
Disconnected
  ↓
Connecting
  ↓
Connected
  ↓
Starting
  ↓
Streaming
  ↓
Recovering
  ├──→ Streaming
  └──→ Faulted

```

状态说明：

* `Disabled`：配置中未启用；
* `Disconnected`：等待连接；
* `Connecting`：正在连接；
* `Connected`：已连接但未采集；
* `Starting`：正在配置和开始采集；
* `Streaming`：正常取流；
* `Recovering`：发生超时或掉线，自动恢复；
* `Faulted`：达到最大恢复次数，需要人工处理。

自动重连采用退避策略，例如：

```
1秒、2秒、5秒、10秒、30秒、60秒

```

恢复成功后重置退避时间。

***

# 9. 推荐项目目录

```
paper-break-edge/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
├── config/
│   ├── default-config.json
│   └── schemas/
├── docs/
│   ├── architecture.md
│   ├── ipc-protocol.md
│   ├── config-schema.md
│   └── event-format.md
├── external/
├── src/
│   ├── common/
│   │   ├── core/
│   │   ├── logging/
│   │   ├── config/
│   │   ├── ipc/
│   │   └── models/
│   ├── camera/
│   │   ├── interfaces/
│   │   ├── hikrobot/
│   │   └── mock/
│   ├── pipeline/
│   ├── algorithm/
│   │   ├── interfaces/
│   │   ├── mock/
│   │   └── classical/
│   ├── event/
│   ├── storage/
│   ├── uplink/
│   ├── plant_io/
│   ├── monitoring/
│   ├── service/
│   └── console/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── simulation/
├── tools/
│   ├── service-installer/
│   ├── event-inspector/
│   └── camera-simulator/
└── packaging/

```

建议生成以下目标：

```
paperbreak_common
paperbreak_camera
paperbreak_pipeline
paperbreak_algorithm
paperbreak_event
paperbreak_storage
paperbreak_uplink
PaperBreakEdgeService
PaperBreakEdgeConsole
PaperBreakTests

```

***

# 10. 配置文件初始结构

```
{
  "schemaVersion": 1,
  "configVersion": 1,
  "machineId": "EDGE-001",
  "productionLineId": "PM-01",

  "cameras": [
    {
      "id": "CAM01",
      "enabled": true,
      "serialNumber": "",
      "location": "PressEntry",
      "exposureUs": 100,
      "gainDb": 0.0,
      "frameRate": 60.0,
      "pixelFormat": "Mono8",
      "triggerMode": "Hardware",
      "previewFps": 3
    }
  ],

  "event": {
    "preEventSeconds": 10,
    "postEventSeconds": 10,
    "maxEventSeconds": 60,
    "mergeGapSeconds": 3
  },

  "storage": {
    "eventRoot": "D:/PaperBreakData/events",
    "cacheRoot": "D:/PaperBreakData/cache",
    "warningFreeSpaceGb": 200,
    "criticalFreeSpaceGb": 100
  },

  "uplink": {
    "enabled": false,
    "serverUrl": "",
    "heartbeatSeconds": 5
  },

  "logging": {
    "level": "info",
    "directory": "D:/PaperBreakData/logs",
    "retentionDays": 30
  }
}

```

所有路径必须支持包含中文和空格。

密码、Token和证书私钥不得明文直接放入普通JSON配置文件。

***

# 11. 错误码设计

错误码采用稳定字符串，不直接将厂商错误码暴露为业务错误码。

示例：

```
SYS_CONFIG_INVALID
SYS_SERVICE_STOPPING
CAMERA_NOT_FOUND
CAMERA_OPEN_FAILED
CAMERA_ACCESS_DENIED
CAMERA_CONFIG_FAILED
CAMERA_STREAM_START_FAILED
CAMERA_FRAME_TIMEOUT
CAMERA_FRAME_INCOMPLETE
PIPELINE_QUEUE_FULL
ALGORITHM_INIT_FAILED
EVENT_WRITE_FAILED
EVENT_DELETE_FAILED
STORAGE_LOW_SPACE
DATABASE_ERROR
UPLINK_DISCONNECTED
UPLOAD_CHECKSUM_MISMATCH
IPC_PROTOCOL_ERROR

```

同时保留：

```
业务错误码
厂商原始错误码
可读错误信息
上下文参数

```

***

# 12. 安全与权限

要求：

* 服务使用专用低权限账户；
* 仅授予相机网卡、数据目录和必要网络权限；
* Qt客户端只允许已认证本机用户访问；
* 已认证本机用户可执行相机运行控制、绑定和参数修改，其他生产参数修改要求管理员权限或应用内登录；
* 高风险参数修改写审计日志；
* IPC限制为本机连接；
* IPC服务设置访问权限；
* Uplink v1 按 ADR-012 使用明文、无鉴权通信；部署方必须通过隔离 VLAN、防火墙和物理访问控制降低窃听、伪造命令和中间人风险；
* 若后续版本引入证书和密钥，必须单独安全存储并通过新协议版本迁移；
* 禁止日志输出密码、Token和私钥；
* 软件升级包进行签名校验；
* 数据目录禁止普通用户随意修改。

***

# 13. 非功能要求与验收标准

## 13.1 稳定性

* 服务可连续运行至少7天；
* Qt界面反复打开、关闭不影响服务；
* Qt界面崩溃不影响采集；
* 上位机断开不影响本地检测；
* 中心网络中断后自动恢复；
* 相机临时掉线后自动重连；
* 服务异常退出后由 Windows 自动恢复；
* 无持续内存增长；
* 无持续句柄泄漏。

## 13.2 性能

在最终工控机上验证：

* 同时接入4路目标相机；
* 达到配置帧率；
* 采集线程不被预览阻塞；
* 预览默认2～5 fps；
* 算法平均处理速度满足目标吞吐；
* 事件触发后正确保存前后图像；
* 磁盘写入期间不出现持续采集阻塞；
* 所有队列深度可监测；
* 任何丢帧均有统计。

不得只在模拟器或单路相机上完成验收。

## 13.3 事件正确性

给定触发帧号 `N`：

* 事件清单记录正确触发帧；
* 能定位触发前配置时长的图像；
* 能定位触发后配置时长的图像；
* 帧序号连续性可检查；
* 关键帧来源可追溯；
* 文件校验值正确；
* 未完成事件能够恢复或明确标记损坏。

## 13.4 配置正确性

* 非法参数不能下发；
* 应用失败时保留旧配置；
* 参数写入后必须回读；
* 配置版本可查询；
* 配置历史可回滚；
* Qt界面显示实际生效值，而非只显示用户输入值。

***

# 14. 分阶段开发计划

## 阶段M0：工程骨架

交付：

* CMake工程；
* CMakePresets；
* 服务端和Qt客户端空程序；
* 公共库；
* 日志；
* 单元测试框架；
* CI构建脚本；
* 版本信息生成。

验收：

```
Debug和Release均可构建
服务可用控制台模式启动
Qt客户端可启动并显示托盘
单元测试可运行

```

***

## 阶段M1：Windows服务与IPC

交付：

* Windows服务安装、启动和停止；
* 控制台调试模式；
* QLocalServer/QLocalSocket；
* 请求、响应和推送协议；
* Qt客户端显示服务状态；
* 服务与客户端断线重连。

验收：

```
界面退出后服务继续运行
服务重启后界面自动重连
IPC异常消息不会导致服务崩溃

```

***

## 阶段M2：相机抽象与模拟器

交付：

* `ICameraDevice`；
* `ICameraProvider`；
* 模拟相机；
* 模拟帧；
* 相机状态机；
* 帧对象池；
* 帧统计。

验收：

```
可模拟4路相机
可配置帧率和分辨率
可模拟掉线、超时、丢帧

```

***

## 阶段M3：海康相机接入

交付：

* MVS SDK适配；
* 相机枚举；
* 序列号绑定；
* 连接、取流和停止；
* 参数读取和写入；
* 自动重连；
* 4路相机测试工具。

验收：

```
能够识别目标型号
能够稳定获取图像
能够统计帧率和丢帧
相机拔网线后可恢复

```

***

## 阶段M4：Qt配置和低帧率预览

交付：

* 总览页面；
* 相机配置页面；
* 实时预览；
* 系统托盘；
* 状态和报警；
* 配置保存与回读；
* 单路和四宫格。

验收：

```
预览开启不影响原始采集
界面关闭不影响服务
参数应用结果明确显示

```

***

## 阶段M5：内存缓存与事件链

交付：

* 内存环形缓存；
* 模拟断纸检测器；
* 候选事件状态机；
* 前后图像冻结；
* 关键帧选择；
* 事件目录；
* manifest；
* SQLite事件索引。

验收：

```
人工触发事件后保存正确时间窗口
缓存继续运行且不停止采集
事件文件可校验和回放

```

***

## 阶段M6：正式算法接口

交付：

* 算法插件接口；
* 传统视觉算法初版；
* 算法参数界面；
* 算法耗时监测；
* 候选和确认逻辑；
* 调试结果叠加。

验收：

```
算法可独立替换
算法异常不会导致采集进程退出
算法积压有明确降级策略

```

***

## 阶段M7：NVMe滚动缓存

交付：

* 分块缓存格式；
* 文件索引；
* 循环删除；
* 事件块保护；
* 崩溃恢复；
* 磁盘水位策略。

验收：

```
缓存容量固定
正常数据自动覆盖
事件数据不被覆盖
断电残留数据可扫描恢复

```

***

## 阶段M8：上位机通信

交付：

* M8-00 Uplink v1 协议与 `PaperBreakUplinkSimulator` 参考服务端（GUI/headless、独立安装组件）；
* Uplink抽象；
* 心跳；
* 状态上传；
* 事件元数据上传；
* 关键帧上传；
* 大文件分块上传；
* 断点续传；
* 持久化重试队列。

验收：

```
网络中断不丢事件
恢复后自动补传
重复上传不会产生重复事件
文件校验失败可重试

```

***

## 阶段M9：稳定性与部署

交付：

* 安装程序；
* 服务注册；
* 配置初始化；
* 运行账户和目录权限；
* 日志导出；
* 性能压测；
* 7×24小时稳定性测试；
* 运维手册；
* 故障排查手册。

***

# 16. Codex必须遵守的代码约束

* 使用现代C++和RAII；
* 禁止业务代码直接调用 MVS SDK；
* 禁止全局可变单例保存业务状态；
* 禁止裸 `new/delete`；
* 禁止无界队列；
* 禁止采集线程执行磁盘和网络操作；
* 禁止异常穿越 C 风格SDK回调边界；
* 所有线程必须可停止；
* 所有外部输入必须校验；
* 所有文件写入必须处理失败；
* 所有公开接口需要注释；
* 所有模块必须有单元测试；
* 模拟相机必须始终可用；
* 没有真实相机时也能运行集成测试；
* 配置和事件格式必须带版本号；
* 数据库升级必须提供迁移；
* 业务错误码保持稳定；
* Windows平台相关代码集中隔离；
* 编译警告视为错误；
* 启用静态分析；
* Release构建不得依赖开发机绝对路径。

***

# 17. 第一版明确不做的内容

为避免首期范围失控，第一版暂不实现：

* 复杂多机深度学习融合；
* 完整上位机业务系统；
* 云端训练平台；
* 自动模型训练；
* 长时间全量原图中心存储；
* 自定义视频编解码器；
* 多用户复杂权限系统；
* Qt界面直接控制PLC安全停机；
* 工控机之间的直接协同；
* 无人工确认的自动根因结论。

第一版重点是：

> 四路稳定采集、配置管理、低帧率预览、候选事件、前后缓存冻结、事件可靠落盘和可恢复运行。

***

# 18. 首期完成定义

首期版本满足以下条件才算完成：

1. Windows开机后服务自动运行；
2. 不登录桌面也能正常采集；
3. Qt客户端可以随时打开和关闭；
4. 托盘图标正确显示服务状态；
5. 能配置并连接最多4路相机；
6. 能持续显示低帧率预览；
7. 能统计每路实际帧率和丢帧；
8. 能模拟或手动触发断纸候选事件；
9. 能保存断纸前后指定时长图像；
10. 能生成事件清单和关键帧；
11. 网络断开不影响事件保存；
12. 相机掉线能够自动恢复；
13. 磁盘不足能够报警并执行策略；
14. 服务异常退出后能够自动重启；
15. 经过目标工控机和真实4路相机稳定性测试。

