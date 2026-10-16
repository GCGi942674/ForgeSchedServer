# GUI 数据来源与排查

## 工作机数量

GUI 以当前连接的 Server 的注册、心跳为准，不按新项目的 SVN 目录数量推算。
`ONLINE` 表示心跳在线，`accepting_requested=false` 表示停止接新任务，不能混为一谈。
同一主机上旧 ForgeSched 与新 GCRP 可以因工作目录不同而同时运行；删除或移动目录不会停止已启动的进程。

发现额外在线 slot 时，先在对应主机核对：

```sh
ps -u "$USER" -o pid,ppid,etime,args | grep -E '[s]lot_manager.py|[g]crp_worker.py|[f]orge_worker.py'
```

确认没有任务且属于旧部署后，只向核实过的旧管理器 PID 发送 SIGTERM，等待排空；不要批量 pkill、删除目录或杀新版管理器。
GUI 默认隐藏无占用的离线历史 slot，可勾选显示历史。旧协议进程的维护能力会在提示中说明。
演示模式标题明确标记为演示；正式模式标题显示连接的 Server 地址。

## 添加任务自动加载

GUI 与 Server 均更新后，通过 `query_catalog` 获取目录，无需导出/复制 gui-catalog.json。
配置只由 Server 读取，GUI 不需要挂载 test2 或 ZIP 共享目录：

```ini
server.work_root=/home/chenggong/workspace/galaxcore/test2
server.templates_dir=./server/templates
server.artifact_root=/home/xshare/zhouwei_runcache/GalaxCore/zip
server.protect_versions_file=
```

- work_root 递归查找 run.tcl，不扫描 .svn、vivado_runner、runtime 或符号链接目录。
- templates_dir 读取 `*.json` 的 flow_config，模板名对应文件名前缀。
- artifact_root 展示最近 50 个数字版本的 GalaxCore_N.zip，以及保护文件中列出且实际存在的版本。
- ZIP 存在不等于内容有效；实际任务仍由 Worker 校验制品。该接口不解压/执行 ZIP。
- flow_config 列出标准阶段和模板额外选项，默认值来自所选模板；GUI 勾选只改变本次批次快照。
- 首次启动自动加载；修改目录内容后点“从 Server 刷新 testcase / ZIP / 模板”。目录扫描有时间/数量上限，分页最多 200 个 case/帧，不改变 1 MiB 协议帧限制。
- 保留最近四个目录快照，避免另一用户刷新导致正在分页的用户混入不同清单；超出保留窗口明确报错并要求刷新。
- 各来源独立报告错误：目录未配置、无权限、模板无效、没有 ZIP 等。缺任一必要内容时禁止提交，不用演示数据填充。
- 旧 Server 不支持接口时明确提示升级；仍保留手工导入旧清单作为兼容入口。

部署时重新编译并重启 Server，再重启 GUI。仅重新编译 GUI 不会给正在运行的旧 Server 增加接口。

```sh
make CMAKE_ARGS="-DGCRP_BUILD_QT_GUI=ON"
cd build
ctest -R '^(test_gui_catalog|test_api_queries|test_qt_gui|test_qt_server_catalog)$' --output-on-failure
```

Qt 端到端测试使用临时 test2/ZIP/SQLite 与真正的 Server，验证 405 个 testcase、分页、51 个版本（50+保护版本）、模板及 19 个阶段控件，不需要真实 GalaxCore。
