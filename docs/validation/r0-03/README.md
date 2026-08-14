# R0-03 PBNVME3 / manifest v4 格式证据

本目录保存 R0-03 的机器可读字段合同和黄金文件，不表示 D2 生产读写器已经实现。

权威决策：

- `docs/architecture/decisions/adr-018-immutable-time-evidence-model.md`：原始、接收、校正时间及模型身份；
- `docs/architecture/decisions/adr-019-pbnvme3-manifest-v4-format.md`：二进制布局、manifest v4、完整性和兼容规则。

黄金场景：

- `golden/minimal`：一帧、完整且具有校正 UTC；
- `golden/multi-frame`：三帧、序号/数据连续；
- `golden/incomplete-frame`：一帧带 incomplete 标志，事件质量明确降级；
- `golden/uncorrected-time`：保留原始 ticks 和接收 UTC，但校正 UTC/offset/uncertainty 不可用。

每个场景包含 `block.pbnvme3` 和 `manifest-v4.json`。manifest 中的长度、SHA-256、CRC、范围和时间可用性必须与二进制一致。二进制由检查器的 `-Regenerate` 模式确定性生成；日常验证只读已提交黄金文件。

验证：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate-r0-03-formats.ps1
```

检查器还在内存副本上注入头、索引和负载损坏、尾页截断、数据偏移越界、未来块版本、未来 manifest schema 和 SHA 不匹配，并要求稳定拒绝。它同时只读检查现有 PBNVME2/manifest v3 设计合同，禁止自动迁移或重写旧事件。

实体相机、PTP/Grandmaster、生产 NVMe、物理断电、六路吞吐和正式上位机导入未在本任务执行，仍待 T1/D2/V5。
