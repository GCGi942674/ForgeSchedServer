# P15-P16 内网联合验收

P15 验证回归查询缓存的命中与失效，以及持久化路径没有回归。P16 验证按 `target + case + flow` 汇总数字 revision，并检测明确的 PASS→FAIL / FAIL→PASS。先在 Server 机器验收即可。

将完整 GCRP 项目同步到内网 Server 后，在项目根目录只运行：

```bash
python3 -B -u tools/intranet_p15_p16.py
```

脚本兼容 Python 3.6.8，自动选择内网 CMake 3.15.2（存在时）、配置构建、编译七项 P15-P16 测试，并在 `build/` 内调用 `ctest`。默认编译并行度为 8，可用 `--jobs 16` 调整。已经编译好时可用 `--no-build` 跳过构建。P15 缓存默认关闭；隔离测试会自行启用并验证，不修改真实服务端配置。异步持久化队列尚未实现，不在本轮范围。

报告在 `intranet-report/p15-p16/report.json`。如果失败，请把整个 `intranet-report/p15-p16/` 目录拷出。脚本会尝试只读查询运行中的 Server，采集真实回归查询缓存计数与 P16 API 结果：没有启动时总结果为 `OFFLINE_PASS`；有服务端但近期任务不足两个数字版本时为 `API_PASS_NO_COMPARABLE_DATA`；查询到两个版本且七项测试通过时为 `PASS`。缓存计数和 P15/P16 测试结果单独列在报告中，不把隔离测试当成生产负载性能结果。

脚本不提交任务、不启动真实 Worker、不改生产 SQLite。P16 目前按 revision 比较并会包含符合条件的人工任务；以后 GUI 所需的“只比较 Clock 每日批次、按日期”属于后续业务规则，不能拿本脚本结果代替该功能的验收。
