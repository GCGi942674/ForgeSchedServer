# P10 批次停止

Clock 每次运行生成独立的 `batch_id`，记录在状态文件和每个 case 的
`payload.context.batch_id` 中。启动输出的 `BATCH:` 行可直接复制到 CLI：

```sh
./build/bin/gcrp_client query-batch BATCH_ID
./build/bin/gcrp_client cancel-batch BATCH_ID
```

取消在调度器锁内完成：同批 PENDING/QUEUED case 变成 CANCELLED，
ASSIGNED/RUNNING case 保持原状并等待工作机正常结果。已完成的 case 保留结果。
停止标记会拒绝此后同批的新提交；Clock 收到该响应后停止提交剩余 case，
并将状态文件的 `phase` 记为 `stopped`。重复取消返回当前统计，
`cancelled_now=0`。`complete=true` 只表示已创建的任务均已到终态。

目前批次与任务都在 Server 内存中，重启后不恢复。若取消或提交期间 Server
重启，应人工核对 Clock 状态文件和 Worker 结果，不要删除状态文件重跑。
单任务 `cancel TASK_ID` 仍只接受 PENDING/QUEUED；Worker 不接收停止进程指令。

旧版 Clock 创建的 case 使用日期和版本组合的批次 ID；新的唯一 ID 从下一次
运行开始生成，并写入状态文件。测试专用 `protect_versions_file` 不应充当正式
清理流程的版本保护文件。
