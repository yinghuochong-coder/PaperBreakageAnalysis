# M3-05 四路硬件验证操作说明

## 状态与用途

本目录提供 M3-05 的真机验证方法和空白记录输入。自动化测试、SDK 链接 smoke 或 Mock 结果不能代替实体相机证据。只有至少一台 `MV-CS020-60GM` 完成功能验证，且四路最终吞吐与恢复获得生产等价环境记录后，才能解除 M3 硬件门禁。

`PaperBreakCameraHardwareTest` 仅在 `PAPERBREAK_ENABLE_HIKROBOT=ON` 时构建。它通过公开相机接口工作，工具源码不直接调用 MVS。`--probe` 只枚举；`--run` 必须显式提供计划，才会打开设备、写入参数、回读、取流和发软件触发命令。输出记录若已存在会拒绝覆盖。

## 构建

```powershell
$env:PAPERBREAK_MVS_ROOT = 'C:\Program Files (x86)\MVS'
$env:PAPERBREAK_MVS_RUNTIME_DIR = 'C:\Program Files (x86)\Common Files\MVS\Runtime\Win64_x64'
cmake --preset local-windows-vs2026-release -DPAPERBREAK_ENABLE_HIKROBOT=ON
cmake --build --preset local-windows-vs2026-release --target PaperBreakCameraHardwareTest
```

实际路径应放在本机环境或未提交的用户预设中，不要写入生产预设。

## 0. 前置清点

记录 Windows 版本、CPU/内存、MVS 4.8.0.3、网卡型号/驱动/协商速率、交换机型号/端口速率、相机供电与拓扑。关闭 MVS GUI 或其他可能独占相机的程序。

相机网段不得同时配置在多个活动接口上。当前目标拓扑为：

- Realtek 相机网卡保持 `192.168.11.102/24`；当前只读探测到的相机地址为 `192.168.11.117`，相机绑定仍以序列号为准；
- Siemens PLCSIM 虚拟网卡使用独立的 `192.168.12.0/24`，建议主机地址 `192.168.12.222/24`，不配置默认网关；
- 依赖 PLCSIM 旧地址的仿真配置由操作者同步修改，工具不自动禁用网卡或改写系统网络设置。

调整后先执行只读路由清点，确认只有 Realtek 接口拥有 `192.168.11.0/24` 直连路由，且 PLCSIM 不再提供默认路由：

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object InterfaceAlias -in @('以太网', '以太网 2')
Get-NetRoute -AddressFamily IPv4 |
  Where-Object DestinationPrefix -in @('192.168.11.0/24', '192.168.12.0/24', '0.0.0.0/0')
```

若两个接口仍处于 `192.168.11.0/24`，不得开始项目探测后的参数或取流测试。仅修改接口 metric 或添加主机路由不能消除 GigE 广播发现歧义，也不得通过代码静默合并同序列号条目。

```powershell
PaperBreakCameraHardwareTest.exe --probe --output records\probe-<rig>-<UTC>.json
```

审核输出中的型号、完整序列号、IP、主机接口和独占可访问性。重复序列号、意外设备、占用或缺失均不得继续写参数测试；先纠正路由/虚拟网卡/交换机拓扑后重新生成新记录，不修改旧记录。

## 1. 计划审批

复制 `hardware-test-plan.example.json`，填写实际序列号和经设备/带宽预算批准的参数。计划约束：

- `CAM01` 起连续排列，最多四路，序列号唯一；
- 运行 1～3600 秒，资源样本不超过 3601；
- 队列/池容量最多 256，缓冲字节数必须覆盖实际 payload；
- `minimumFpsRatio` 是目标帧率验收比例，不得通过扩大队列掩盖丢帧；
- 连续吞吐先使用 `continuous`；软件触发另建 `software` 计划；实际硬件触发需现场触发源，不能用软件命令冒充。

## 2. 参数与逐路吞吐

```powershell
PaperBreakCameraHardwareTest.exe --run `
  --plan hardware-test-plan.approved.json `
  --output records\run-<rig>-<UTC>.json
```

工具按 1、2、3、4 路依次执行固定时长阶段。每路写入计划参数并保存实际完整回读，使用预分配帧池和有界丢最旧队列；采集线程只取帧并入队。记录包含实际帧率、帧号间隙、不完整帧、超时、相机接收带宽、队列/池指标、进程 CPU/工作集和 Windows 所有活动非环回网卡的汇总收发速率。

网卡速率是系统短区间累计值，可能包含同机其他流量；审核时须结合隔离网络和相机接收带宽，不能把它解释为单一相机精确流量。

## 3. 触发模式

- 软件触发：计划回读必须为 `TriggerMode=On`、`TriggerSource=Software`，工具才发 `TriggerSoftware`；每个资源采样周期每相机一次。记录实际回读和触发失败。
- 硬件触发：计划使用 `hardware` 和实际 `Line0`（或经能力核实的输入源）。由有权限的现场人员提供电气触发，并记录触发源、频率、电平、线缆和计数。工具不会用软件触发冒充。
- 连续模式：用于逐路目标吞吐基线。

## 4. 人工故障场景

以下动作工具不会自动执行。需现场负责人确认安全、影响范围并操作；每项使用 `manual-scenario-record.template.md` 单独记录开始/结束 UTC、操作者、预期和观测。

1. 单相机网线拔出并恢复：只影响该路；记录断流业务/native 码、状态转换、退避、恢复耗时、其他路帧率。
2. 交换机链路中断并恢复：记录受影响范围、所有路状态、重连退避与恢复后参数回读。
3. 服务重启：必须在已安装且获授权的目标机由管理员执行；记录 SCM 命令、停止期限、句柄/线程清理、启动后重新绑定和取流。

不得由本工具禁用网卡、操控交换机、调用 SCM 或提示操作者在不安全工况下拔线。

## 5. 预览与日志隔离

M3-05 工具的 `consumerDelayMs` 可模拟有界慢消费者，证明队列满时丢最旧而不反压采集；资源/记录写入只在非采集线程。当前路线图 M4 尚未实现真实 Qt/JPEG 预览，因此真实“预览开/关”和生产日志并发只能记为 `not-executed`，不得用模拟慢消费者结果替代。M4 可用后需在同一目标配置复测并把记录关联回本门禁。

## 6. 门禁判定

- 自动化适配层、Mock、OFF/ON Debug/Release 与质量检查全部通过；
- 至少一台目标相机的发现/绑定、参数回读、连续/软件/实际硬件触发有通过记录；
- 单相机断链、交换机链路和服务重启有人工签名的恢复记录；
- 真实预览/日志并发有生产实现后的验证记录。

任一证据缺失时，M3-05/M3 保持“硬件门禁未完成”。
