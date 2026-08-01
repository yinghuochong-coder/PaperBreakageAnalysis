# M3-05 本机硬件可用性只读清点（2026-08-01）

## 结论

状态：硬件门禁未完成。

本机存在批准的 MVS Development/Runtime 4.8.0.3，能够构建和运行适配器；Realtek 2.5GbE 物理网卡当前协商速率为 1Gbps。PnP 清点未提供 Hikrobot 型号证据。首次真实 `PaperBreakCameraHardwareTest --probe` 调用 MVS GigE 枚举后，在厂商无关库存校验处拒绝清单：MVS 至少返回两个描述项，其完整序列号字符串相同；记录调用脱敏函数后仅披露后四位 `8674`。固定数组解析会要求边界内 NUL 终止并保存完整字符串，因此后四位不是字段截断。仍然无法判断这些描述项代表多台实体设备还是同一设备经不同接口重复可见，也无法可信确认实际设备数量、型号或绑定，更没有证据证明具备四台目标相机和测试交换机。

可能原因包括同一设备经物理网卡与 Siemens PLCSIM 虚拟网卡重复可见，但这只是推测。没有禁用网卡、修改路由、打开相机或取流；应由目标机网络负责人清理/隔离测试拓扑后重新执行只读探测。

## 已执行只读检查

- MVS 安装根和 Runtime DLL 文件版本；
- `Get-NetAdapter`、`Get-NetIPAddress`、`Get-PnpDevice` 和 IPv4 邻居；
- `PaperBreakCameraHardwareTest.exe --probe --output out/m3-05-probe-20260801.json`。

探测进程退出码为 4，审计 JSON 记录 `CAMERA_CONFIG_FAILED`、`reason=duplicate-serial-number`，人工场景全部为 `not-executed`。`out/` 是本机构建/证据目录，不作为已经评审签字的目标机记录提交。

## 未执行

- 目标型号/完整序列号确认和绑定；
- 打开设备、参数写入回读、连续/软件/硬件触发；
- 单路至四路吞吐、CPU/内存/网卡趋势；
- 相机网线拔插、交换机链路中断、服务重启；
- 真实预览/日志并发。

以上项目均不得报告为通过。
