# Phase 6 — PJtest 接入审查与待实现契约

## 当前结论

基线 2411a88 已提交并推送。通用进程 Worker 不等于 PJtest Worker。
本轮根据 reference/PJTest/worker/worker_core/main.py 做静态审查；没有真实 PJtest 工作目录、
case、版本包或工具环境，因此未执行真实业务，也未宣称 Adapter 网络闭环完成。
该参考文件保持原样，不纳入 ForgeSched 提交。不要直接运行它：默认会联系旧内网 HTTP
调度器，准备 SVN slot、安装版本、清理工作目录，并非无副作用的演示脚本。

## 已核对的执行链

worker_loop 使用 HTTP 注册/心跳/pull/report，不兼容 ForgeSched framed TCP。
run_one_task 并非纯执行接口，它混合以下操作：

1. 获取与 run.sh 共用的 slot 锁。
2. prepare_task_for_slot：定位 slot SVN checkout，将 work_root 改为 test2。
3. 清理该 slot 遗留 GalaxCore 进程。
4. install_galaxcore_zip：按 revision 从共享目录找包，替换二进制和 flow。
5. update_flow_config_file：更新 flow_config。
6. pre-clean，启动 run.sh 单 case，等待执行。
7. 查找 result.env，classify_result 判定业务结果。
8. 可选保留失败证据，post-clean。
9. 写 pending report，通过旧 HTTP 重试上报。

不能直接调用 run_one_task 后再发 TASK_RESULT：会同时上报两个系统。
也不能用 demo Worker 外层 SIGKILL 包裹整个 main.py：run.sh 的 case 可以另建 session，
直接杀父组不保证这些 case 组全部退出。

## 建议的最小协议（尚未接入 DTO/CLI，不是现有可用参数）

保留 Task 核心字段；REGRESSION 的 payload 建议如下：

```json
{
  "spec_version": 1,
  "case": "examples/smoke/case1/run.tcl",
  "flow_profile": "route-smoke",
  "timeout_seconds": 600
}
```

- target 保留业务目标含义，不同时冒充设备名、case 路径和 shell command。
- revision 是受校验的版本标识；由 Worker 的本地共享目录映射到安装包。
- case 必须是 slot/test2 内的单一 run.tcl，拒绝目录、runlist、越界路径和 symlink 越界。
- flow_profile 是本地白名单配置名称，不接受任意 Tcl 文本或任意 flow_config 键值。
- spec_version 显式版本化；缺字段/未知字段/超长字段失败关闭。
- work_root、install_root、shell、共享目录、环境初始化由工作机本地配置决定。
- 不接受 cmd/command、网络传入的安装路径或环境脚本。
- 首轮一 case、一 flow、一 Worker、一 slot；不引入 batch/example/attempt 数据库模型。
- 当前 Task 无 payload，正式实现需贯通 Submit DTO → Task → assignment → query → CLI；
  不能只改 Worker，或悄悄把 JSON 塞进 target。

## Adapter 划分

PJtestAdapter.validate(spec, local_config) 验证参数与本地依赖，不执行业务。
prepare() 在持有 slot 锁后完成版本和 flow 准备；执行失败不得继续跑旧版本。
build_command() 返回本地受控 argv/cwd/env；使用现有 run.sh，不重写 vivado_runner。
execute() 与 TCP 心跳解耦，但仍保持单 slot；先收到 TASK_START ACK 再执行。
collect_result() 返回结构化结果，由 ForgeSched Worker 唯一负责 TCP 上报。

应从参考文件提取执行逻辑，不复制 HTTP 注册/pull/report、pending HTTP reports、
tmux 多 slot 管理。已有的锁、安装、配置、结果分类是复用候选；
是否原样复用需先通过下述安全检查。

## 从实际代码发现的接入风险

### 结果归属（必须修复后复用）

find_result_env 在精确路径和 RUN_TCL 匹配失败后，选择最新结果文件。
即使满足时间条件，也可能是另一个 case；新 Adapter 必须严格匹配当前 case，
并证明文件由本次执行产生，不能退回全目录最新文件。
旧 expected_result_env_path 只覆盖 runtime/status；实际部署可能使用 workspace namespace，
路径要从真实 runner 确认，不能凭参考文件假定。

### 退出码不是业务结果

classify_result 的优先级是外层超时、case PASS/TIMEOUT/FAIL、缺失结果失败。
PASS 可以覆盖非零工具退出码，这是原业务的 artifact-authoritative 语义，不宜擅改。
需要同时保存 raw_exit_code、归一化 exit_code、case_status、reason。
exit 0 但缺少本次结果不得报 SUCCEEDED。TIMEOUT 映射 ForgeSched TIMEOUT。
当前 C++ Router 不保存 TASK_RESULT.message，未来需受限的结构化 result 才能
在 CLI 查询里看到业务细节；仅把详情塞 message 不能算实现了结果查询。

### 进程清理

原 kill_process_group 先 TERM，再等 PROCESS_TERM_GRACE_SEC，必要时 KILL。
目的是给 run.sh 的 TERM trap 清理独立 case 组留时间；新 Adapter 不能直接强杀代替。
超时/断线/关闭都应回收父进程，并验证子组退出；真实验证前不宣称完全无残留。
本轮 Server 在 Scheduler 锁内禁止 ASSIGNED/RUNNING 取消。
使用现有 INVALID_STATE(code=3) 和明确提示，不引入未实现的 CANCEL_IN_PROGRESS。

### 准备与清理边界

安装会替换二进制及 flow，不能使用不受控路径或共享多 slot 安装目录。
pre-clean 返回码当前仅记录，是否继续运行需要明确定义；准备失败应停止任务。
失败证据默认不保留，且随后会 clean；接入时需在清理前保存所需证据。
参考 Worker 的 bash 分支和 csh 环境准备并不等价；不能在缺少 csh 时默默证明环境可用。
build_task_command 支持 cmd，但正常 prepare_task_for_slot 会重新生成 cmd；
适配层仍必须不接收网络 cmd，不能把该通用辅助函数暴露为远程执行接口。

## 验收门槛

离线测试：字段/路径白名单、版本包缺失、准备失败、flow 映射、结果缺失/陈旧/
错 case、PASS+非零退出码、超时、ACK 交错、取消 slot 不变。
真实环境提供后：CLI → Server → Adapter → run.sh → 新 result.env → TASK_RESULT →
CLI 最终状态，并留存命令 argv、环境摘要、版本标识、PID、日志、原始退出码与结果。

当前只完成基线提交推送、接入审查、取消安全限制；payload/Adapter 执行入口及上述
业务测试仍是下一步，不上 SQLite/缓存/恢复，不把固定命令测试算真实 Regression。
