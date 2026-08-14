# R0-02 时间与 Uplink 契约样例

本目录固定 R0-02 的线上 JSON 验证向量，不表示 T1、E3 或 O4 生产实现已经完成。

有效样例：

- `strict-session-request.json` / `strict-session-response.json`：能力请求与服务端确认；
- `break-event-triggered.json`：候选窗口保护后的立即通知；
- `event-lock-command.json`：无人工确认的自动 T0 锁定命令；
- `event-lock-ack.json`：包含逐相机部分结果的 ACK；
- `status-time.json`：工控机及相机时间状态投影；
- `preview-time-header.json`：协商时间能力后的二进制预览 JSON 头。

拒绝样例：

- `invalid-unknown-field.json` → `UPLINK_PROTOCOL_ERROR`；
- `invalid-version.json` → `UPLINK_PROTOCOL_VERSION_UNSUPPORTED`；
- `invalid-accepted-capability.json` → `UPLINK_PROTOCOL_ERROR`。

独立校验命令：

```powershell
powershell -NoProfile -File tools/validate-r0-02-contract.ps1
```

2026-08-14 已运行该命令并通过；`local-windows-vs2026-debug` 配置、构建和最终全量 CTest
也已通过。后续生产 parser 实现必须复用这些向量增加 C++ 单元/集成测试，不能把本脚本作为
生产输入校验器。实体相机、PTP/Grandmaster 和上位机联调不属于本契约门禁，未在此验证。
