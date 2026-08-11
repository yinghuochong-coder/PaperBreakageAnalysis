# 原始日志状态

实体 CAM01 启动哨兵取证已经执行，目录包含：

- `cam01-buffer-probe-final-25-rounds-20260811.log`：正式 20+5 轮完整采集日志；
- `cam01-ipc-control-final-25-rounds-20260811.log`：正式相机控制命令日志；
- `service-main-final-25-rounds-20260811.log`：正式服务启动/停止日志；
- `pilot-invalid-any-sentinel-classifier-20260811.log`：初版“任意哨兵即部分写入”判据的无效试运行。
  该文件只用于记录判据修正原因，不能作为根因证据。

后续复测只新增不可覆盖的完整日志，建议文件名：

```text
cam01-start-stop-<round>-<UTC>.log
cam01-connect-start-<round>-<UTC>.log
mvs-client-start-stop-<round>-<UTC>.<ext>
```

每个文件应保留进程启动、参数回读、全部 8 条 `operation=frame.startupBufferProbe`（取帧失败时
可少于 8 条扫描记录）、停止和断开上下文。
