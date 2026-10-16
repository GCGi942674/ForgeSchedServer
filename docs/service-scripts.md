# Linux 后台启动

在项目根目录执行；也可以从任意目录用脚本的绝对路径执行。需要 Bash、Python 3.6+。

工作机：

```sh
./worker/start.sh
./worker/status.sh
./worker/stop.sh
```

默认读取 `config/worker.conf`，兼容 `config/gcrp.conf`。只需配置 `worker.slots` 等部署字段，不需要手填 Worker ID。后台启动现有 slot_manager，不更换执行逻辑。修改 slot 数量后使用 `./worker/reload.sh`；该操作只是发送重载请求，是否接受以日志为准。

Server：

```sh
./server/start.sh
./server/status.sh
./server/stop.sh
```

需先编译 `build/bin/gcrp_server`；默认读取 `config/server.conf`，兼容 `config/gcrp.conf`。可用 `start.sh --config /绝对路径/配置文件` 或 `GCRP_CONFIG` 覆盖。Python 路径可用 `GCRP_PYTHON` 指定。

关闭终端不会停止后台进程。此脚本不是 systemd，不保证系统重启自启，也不自动重启崩溃的 manager/Server。现有 manager 负责各 slot 子进程的重试。Server start.sh 仅启动 C++ Server，不会额外启动 clock.py 或重复配置定时任务。

`stop.sh` 是优雅停止，不是临时暂停接单。工作机先排空任务，最多等待 30 秒；超时返回 2，保留进程和状态文件，不发送 SIGKILL。可以稍后查看状态，或 `./worker/stop.sh --wait 600`。临时暂停/恢复接单仍通过 GUI/CLI 管理接口操作，不用停止服务。

启动控制台输出保存在 `logs/server/YYYYMMDD/server-service.log` 或 `logs/workers/主机名/YYYYMMDD/worker-service.log`。日期是启动日期，长驻进程不自动每天重开这个文件；业务日志继续使用现有日志机制。

服务记录在 `data/services`，操作用 flock 串行化；发送信号前核对 PID、Linux 进程启动时刻及本项目启动入口，不用 pkill 或进程名批量终止。只管理本脚本启动的进程，不接管旧前台启动或其他部署的进程。状态只说明进程存在，不代表完成 SVN checkout、注册或网络就绪。

Linux 回归检查：`python3 -B tests/test_service_control.py`，使用独立临时假 Worker，不接触真实任务。
