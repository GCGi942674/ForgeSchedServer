# Python Worker：真实进程最小闭环

这是 Linux / WSL 的单 slot 演示执行器，Python 3.8+，仅依赖标准库。
它不是 PJtest/Vivado Adapter，不用于生产任务。

```bash
python3 worker/forge_worker.py --worker-id demo-1 --output /tmp/forge-worker-output
build/bin/forgesched_client submit --target demo --revision r1
build/bin/forgesched_client query TASK_ID
```

默认子进程输出两行日志，等待 3 秒并返回 0。Worker 本地参数
`--demo-seconds`、`--demo-exit-code`、`--task-timeout`
用于验证成功、失败和超时。服务端 target/revision 不作为命令执行。

每次 Worker 启动创建独立 run-* 目录，每个任务保存 pid、stdout.log、
stderr.log、result.json。退出码和 PID 保存在本地；当前 Server 查询只提供
任务状态，不保存 TASK_RESULT.message 中的执行详情。

先等待 TASK_START 成功再启动进程；运行期间发送心跳；超时杀死进程组并回收；
SIGINT/SIGTERM 或连接错误同样清理子进程。结果响应和下一次派发允许交错。
断线不自动重连、不重放任务；未确认结果需人工核对，Server 暂无恢复机制。

重要限制：
- 暂不支持远程取消。Server 已强制拒绝 ASSIGNED/RUNNING 的取消请求，
  返回 INVALID_STATE 且不释放 slot。仅 PENDING/QUEUED 可取消。
- 退出或断线后，Server 可能保留 ASSIGNED/RUNNING；暂无 Worker lost recovery。
- 无认证/TLS，只在可信网络使用；worker-id 不得与其他在线 Worker 重复。
- 固定演示输出很小；接真实执行器前还需日志限额、资源隔离和取消协议。
- 本阶段不加入 SQLite、缓存、Vivado 或 PJtest。
