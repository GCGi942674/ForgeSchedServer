# 已确认的目录与配置方案

```text
server/{include,src,templates,Tasks.yaml,clock.py,daily_log.py}
worker/                     # Python，现有工作区不动
gui/qt/{include,src,tests,CMakeLists.txt}
client/                     # CLI：脚本、无桌面操作及排障
common/                     # 公共协议、配置、日志代码
config/{server.conf,worker.conf}
database/                   # SQLite 和未来可选的磁盘缓存文件
logs/{server,workers,daily}
build/
tests/
tools/
third_party/
docs/
```

## 两份配置，不设 common.conf 或 gui.conf

Server、CLI、clock 和目录导出默认读取 server.conf；Worker、slot_manager 及工作机验收入口默认读取 worker.conf。GUI 只通过 `--ip 地址 --port 端口` 连接，不读取配置文件。
GCRP_CONFIG 显式指定优先；空值或不存在文件报错，不悄悄回退。自动发现从当前目录向上查找，每层先找角色配置，后找旧 gcrp.conf；Qt 额外从可执行文件目录查找。
GUI 读取 Server 配置中的连接/展示参数，业务数据仍从网络接口获取。

相对路径**继续相对进程工作目录**，此次不改变旧路径含义。请从项目根启动，跨工作目录部署则使用绝对路径。

## 现有部署

从项目根运行 `python3 -B tools/split_config.py`，从旧 gcrp.conf 生成两份角色配置，不删除旧文件，不改变 DB、journal、SVN、输出和归档路径。任意目标已存在就拒绝覆盖。
服务器和工作机分别运行一次拆分工具即可，然后修改各自主机需要的值。无需新增 GUI 配置。
新部署可复制 config/server.conf.example 或 worker.conf.example；不要覆盖已有配置。

## 数据及日志

- 新部署示例使用 database/gcrp.db、logs/server、logs/workers；已有部署路径原样保留。
- 移动 SQLite 必须停写、备份、checkpoint 和校验，不允许在线只搬主文件而遗漏 WAL/SHM。本轮不自动搬数据库。
- 内存缓存代码仍在服务端，未来磁盘缓存文件可以放 database 子目录；不引入第二份任务真相源。
- 每日日志：logs/daily/YYYYMMDD/clock-HHMMSS-PID-随机标识.log；server.daily_log_root 可调整根目录。
- 每日日志保存提交、版本、批次、跳过、停止、错误和每 100 条的进度，避免打印数千条成功信息。
- 按 log.retention_days（默认 30）只清理严格匹配程序命名的过期普通日志文件，不跟随符号链接，不清理未知文件。
- Worker 身份、运行 journal、未上报结果不是普通日志，不随 30 天策略删除。

Worker 的内部输出/归档 task_id/run_id 布局保持兼容，不为目录外观重写日志协议。后续添加机器/日期/slot 归档索引时须同步改查询与保留策略。
远程上下线、更新 SVN、带权限删除记录仍需独立服务端接口，不因目录整理而视为完成。
