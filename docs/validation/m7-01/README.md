# M7-01 NVMe 格式与容量设计证据

本目录保存 M7-01 的机器可读设计合同。`nvme-block-format-v1.json` 中的参考工作负载来自当前默认配置，只用于让 CTest 复算格式和容量公式，不是目标 NVMe 或四路实体相机验收报告。

权威人工可读决策见 `docs/architecture/decisions/adr-011-nvme-rolling-cache-format-capacity.md`。v1 的关键结论是：每相机 1 秒块、原始帧不压缩、4096 B 头/尾页、96 B 定长索引、CRC32C 完整性和普通滚动写最多占目标卷实测持续写带宽的 80%。

解除硬件验证缺口至少需要记录最终相机数量、ROI、stride、像素格式、帧率、NVMe 型号/固件/容量/文件系统、近满盘热稳定持续写、并发事件写/flush，以及跨完整容量回绕的四路采集统计。在这些证据齐备前，`hardwareValidationStatus` 必须保持 `not-validated`。

