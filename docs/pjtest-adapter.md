# PJtest Adapter — P0/P1 安全修复

当前仅单 Worker、单 slot、单 case。真实 GalaxCore/Vivado 仍须内网验收。
未实现 SQLite、自动恢复或远程取消。ASSIGNED/RUNNING 取消仍被拒绝。

## 启动配置

生产模式必须指定 --pjtest-config。演示必须显式 --demo，且拒绝业务 payload。

```sh
python3 worker/forge_worker.py --worker-id pjtest-slot-1 --pjtest-config /local/worker.json
./build/bin/forgesched_client submit --target xcvu9p --revision 58231 --case cases/smoke/run.tcl --flow route --case-timeout 600
```

```json
{
  "test2_root": "/local/inputs/test2",
  "artifact_root": "/local/artifacts",
  "log_root": "/local/worker-runs",
  "flow_profiles": {
    "route": {"route_design": 1, "write_bitstream": 0, "bit_cmp": 0, "msk_cmp": 0, "bgn_cmp": 0}
  },
  "environment": {
    "PATH": "/opt/vivado/bin:/usr/local/bin:/usr/bin:/bin",
    "LD_LIBRARY_PATH": "/local/required-libraries"
  },
  "clean": true,
  "terminate_grace_seconds": 30
}
```

路径、环境与 flow 需由内网操作员核验。不会自动 source .cshrc。
log_root 是默认输出位置，--output 可覆盖。旧 lock_root 不再控制保护范围：
对规范化输入目录本身加内核 flock，锁与 Worker 名称无关。
target 当前是业务标签，不隐式改变 case/器件/flow。

## 构建发布契约

不再接受裸 ZIP。每个 revision 必须只有一个候选：
GalaxCore_REV.zip、Galaxcore_REV.zip、GalaxCore_rREV.zip、GalaxCore-REV.zip。
多个候选、链接、缺少清单或哈希不匹配都会失败，不回退旧安装。

ZIP 相邻文件 ZIP完整文件名.manifest.json 必须包含：
spec_version=1、revision、test2_revision、archive_sha256、test2_sha256。
两个哈希都是 64 位小写 SHA256。发布者完成并核验构建后，先将 ZIP 复制到临时名，
完成后原子改名，最后发布清单：

```sh
python3 worker/publish_artifact_manifest.py --archive /local/artifacts/GalaxCore_58231.zip --revision 58231 --test2-root /local/inputs/test2 --test2-revision 58231
```

工具原子发布且拒绝覆盖已有清单。版本标签必须来自可信构建流程，不能通过
重命名旧 ZIP 冒充新版本；工具不会读取 ELF 来猜测 SVN 版本，也不运行 automation。
不同 GalaxCore/test2 revision 可显式记录，但操作员必须审核兼容组合。
清单是完整性契约而非数字签名，发布目录只允许可信构建方写入。

test2 摘要覆盖路径、内容、大小、权限与目录结构，排除 .svn、__pycache__、
vivado_runner/runtime。符号链接和特殊输入文件拒绝。输入必须是固定干净快照，
输出不得放进输入树。

## 隔离与结果归属

每任务在输出目录创建私有 workspace/test2、workspace/bin、workspace/flow。
ZIP 先复制再验哈希；test2 复制后再次验摘要；完整解包到新目录，不叠加旧 flow。
不修改输入 flow_config，不运行输入目录的 clean.sh。pre/post-clean 只操作副本。
ZIP 上限为 2 GiB 压缩大小、2 GiB 解包大小、100000 项，拒绝越界、链接和重名目标。

每轮生成唯一随机 namespace，只读取该副本 namespace 中匹配 case 的 result.env。
不遍历其他 namespace，不靠 mtime 判断归属。所有后代退出后才读取结果。
后台延迟写入不被视为合法的任务完成。RUN_TCL 必须匹配副本 case；
结果最多 64 KiB，拒绝链接，安全解析普通 shell 转义，不 source/eval。
特殊 ANSI-C 引号尚未完整兼容，内网优先使用普通 ASCII 路径。

显式传入 --bg 1、--galaxcore、--flow-config 和 --timeout，并固定本轮 workspace/
namespace。旧的身份、workspace 和锁环境不能改变本轮选择。许可证/库等其他环境
仍需通过启动环境或本地 environment 配置提供。

副本不等于文件系统沙箱：可信脚本内部写死的绝对路径/外部服务副作用仍需检查。
执行目录保留用于诊断，会增加磁盘占用。当前不自动递归删除，操作员须在确认
进程退出后归档清理历史运行。

## 进程生命周期

每条命令由独立 Linux subreaper 监督进程拥有，覆盖 setsid、double fork 后代。
Worker SIGTERM、断连会关闭租约管道；Worker SIGKILL 时内核也关闭租约，
监督进程继续 TERM/KILL 并回收后代。正常父脚本退出而后代仍存活，回收后返回
execution_tree_incomplete，不能被 PASS 覆盖。

只有 waitpid 确认 ECHILD 才发送完成凭证。凭证缺失禁止后续清理/终态上报，
避免释放仍可能有进程的 slot。PID 文件记录监督进程 PID，stdout 的
execution_started 另记录 command_pid 与 argv。

不要主动 SIGKILL 监督进程。监督进程被杀、系统重启、/proc 不可访问等情况下，
不能保证自动回收；缺失凭证会触发安全停止。不可中断 I/O 子进程无法退出时，
宁可继续占用 slot，不能假装完成。这不是对抗恶意同 UID 进程的安全沙箱。

## 清理失败与诊断

保留 execution.json、result.json、原始有效 result.env、实际 flow_config、
manifest.json、stdout/stderr、ZIP 与私有 workspace。post-clean 前另保存最多
8 MiB testcase run。无效结果仍可能只留在私有 workspace，尚非完整历史服务。

post-clean 非零/异常、监督凭证缺失会隔离 Worker：不发送释放 slot 的 TASK_RESULT，
Worker 断开。Server 可能继续保持 RUNNING 和 slot 占用，后续任务排队。
这是安全停止，不是故障恢复。必须先核查进程和工作目录，不能换 Worker ID/
盲目重投绕过。CLI 目前不返回完整业务失败详情，需要查看 Worker 本地证据。

## 验证

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'test_python_worker|test_pjtest_adapter|test_worker_safety' --output-on-failure
```

tests/test_worker_safety.cpp 调用 Python 夹具，覆盖隔离、锁、哈希/清单/ZIP、
旧 flow、外来 namespace、低精度时间戳、清理隔离、Demo 拒绝、逃逸进程、
强杀 Worker、启动退出竞态和监督凭证丢失。reference 存在时还执行真实
run.sh/runner + 假 GalaxCore；未安装 reference 时明确 skip 该可选用例。
网络测试确认清理失败后任务仍 RUNNING，后续任务 QUEUED。

这不等于真实 GalaxCore/Vivado 验收；内网还需验证动态库、许可证、真实子进程树、
脚本绝对路径、共享文件系统 flock 支持、实际版本与磁盘容量。
