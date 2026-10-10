# PJTest 定时任务迁移（第一版）

`server/clock.py` 读取 `server/Tasks.yaml` 与 `server/templates/*.json`，
一次选定一个 `GalaxCore_N.zip` 版本，展开 runlist 或目录中的 `run.tcl`，
然后通过 ForgeSched TCP+JSON 提交任务。每个 case 是独立的 ForgeSched Task；
目前尚没有 PJTest 原来的父任务、attempt、重试和汇总报告。不要把此实现理解为
旧 `clock.py` 完全等价替换。

使用前在服务端配置 `server.work_root`（完整 GalaxCore checkout 中的 test2）、
`server.artifact_root`（ZIP 所在共享目录）、`server.protect_versions_file`
（构建清理脚本读取的保护版本文件）和 `server.clock_state_file`。
`server.tasks_yaml` 指向任务表，`server.templates_dir` 指向 JSON 模板目录。
`Tasks.yaml` 里目前保留原 PJTest 的五项日常任务名称，但镜像没有对应的五份
模板，且 runlist 路径是原机路径。必须从内网核对后补齐这些模板和路径；
缺少任何一项时 `--dry-run` 会失败，不会猜测 flow 参数或提交任务。
仓库现有 `route.json` 只对应已验收的单 case route 配置，不等价于
`route_design.json` 等日常模板。

先检查配置，再启用定时：

```sh
python3 -B server/clock.py --dry-run
python3 -B server/clock.py --check-only
python3 -B server/clock.py --interval 30
```

需要 Python 3.6+ 和 PyYAML。可由系统 cron 每天调用一次，但首次启用前请确认
dry-run 成功、Worker 在线、Server 无未完成任务以及磁盘容量足够。
同一天重复调用会跳过。提交前写保护版本文件；提交中断或响应丢失时把状态
记为 uncertain，后续调用拒绝自动重试，须人工核对任务 ID、任务数与运行状态，
避免重复执行。Server 当前不持久化任务；Server 重启后无法从旧任务 ID
自动恢复日任务，必须人工处理，不能直接删除时钟状态文件来绕过。
当前强制 `max_retry=0`，不支持旧 PJTest 的失败重试语义。
