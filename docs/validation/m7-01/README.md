# M7-01 NVMe 格式与容量设计证据

本目录保存 M7-01 的机器可读设计合同。`nvme-block-format-v1.json` 中的参考工作负载是 M9-00 六路 1624×1240 Mono8@60 FPS 容量基线，只用于让 CTest 复算格式和容量公式，不是目标 NVMe 或六路实体相机验收报告。默认部署配置仍为四路且滚动缓存关闭。

权威人工可读决策见 `docs/architecture/decisions/adr-011-nvme-rolling-cache-format-capacity.md`。v1 的关键结论是：每相机 1 秒块、原始帧不压缩、4096 B 头/尾页、96 B 定长索引、CRC32C 完整性和普通滚动写最多占目标卷实测持续写带宽的 80%。

当前写入格式已由 ADR-017 升级为 `PBNVME2`：保留布局、结构 CRC、容量公式和 80% 带宽准入，
逐帧/Data CRC 字段写零，使用普通缓冲写；v1 JSON 留作历史只读兼容合同。

解除硬件验证缺口至少需要记录最终相机数量、ROI、stride、像素格式、帧率、NVMe 型号/固件/容量/文件系统、近满盘热稳定持续写、并发事件写/flush，以及跨完整容量回绕的六路采集统计。在这些证据齐备前，`hardwareValidationStatus` 必须保持 `not-validated`。六路参考滚动写为 725,037,312 B/s，80% 门槛要求目标盘实测至少 906,296,640 B/s；默认 600 MiB/s 限制不足，必须根据实测能力显式配置。
