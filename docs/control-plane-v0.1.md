# ForgeSched v0.1 控制面

本阶段把 Client 和 Worker 接到同一个 TCP 服务端口，并复用现有 ProtocolRouter、
TaskService 和 Scheduler。未实现 SQLite、缓存、重启恢复、真实进程执行或 GUI。

## 构建与使用

在 WSL 的项目根目录运行（C++17，CMake >= 3.15.2）：

```sh
make
./build/bin/server
```

服务、CLI 和 Worker 读取同一格式的 `config/forgesched.conf`；
可用 `FORGESCHED_CONFIG=/absolute/path/forgesched.conf` 显式指定。
连接地址由 `network.server_ip` 指定，监听地址由 `server.bind_ip` 指定，
端口统一使用 `server.port`。缺少配置时启动失败；多机部署需在各机器上
配置可达的服务地址。
在另一终端运行：

```sh
./build/bin/forgesched_client submit --target demo --revision r1 --priority 0
./build/bin/forgesched_client query 1
./build/bin/forgesched_client cancel 1
```

上面的 1 必须替换为 submit 返回的 `result.task_id`。没有 Worker 在线时任务保持
QUEUED；连接 Mock Worker 后会自动派发。现有 `client` 和 `load_client` 仍是
Echo 基础设施测试工具，不是调度客户端。

CLI 选项：

- `--host`：临时覆盖配置中的数字 IPv4 地址；暂不支持 DNS/IPv6。
- `--port`：临时覆盖配置中的端口（1..65535）。
- `--timeout-ms`：临时覆盖配置中的超时（1..60000）；覆盖连接、完整发送和完整响应读取，
  收到少量数据不会重置截止时间。
- submit 需要 `--target` 和 `--revision`，任务类型固定 REGRESSION；
  `--priority` 范围 -100..100，默认 0。
- query / cancel 需要一个正整数 TaskId。
- `--help` 显示帮助。

stdout 输出一行 JSON 响应体（code/message/result），错误诊断写 stderr。
退出码：0 成功；2 服务端返回业务错误；1 网络或协议错误；64 参数错误。
客户端不自动重试。提交或取消超时不代表服务端没有执行，盲目重试提交可能重复建任务。
目前没有请求幂等去重机制。

可复用的 C++ API 位于 `client/include/ForgeClient.h`，链接
`forgesched_client_api`。每次请求新建一个连接；同一个实例不支持并发调用，
并发线程应分别创建实例。以后 GUI 可复用这层 API。

## 连接角色与权限

角色只存在于会话层，底层 Connection 仍负责传输。WorkerServer 保留原类名，
现在在其串行化边界内管理 ConnectionSession，再转给 WorkerSession 或 Router。

| 当前角色 | 允许入站消息 |
| --- | --- |
| UNKNOWN | SUBMIT_TASK / QUERY_TASK / CANCEL_TASK / WORKER_REGISTER |
| CLIENT | SUBMIT_TASK / QUERY_TASK / CANCEL_TASK |
| WORKER | WORKER_REGISTER / WORKER_HEARTBEAT / TASK_START / TASK_RESULT |

规则集中在 `common/include/protocol/ConnectionRole.h`：

- 合法 envelope 的首个 Client 类型请求绑定 CLIENT，即使其业务参数随后被拒绝。
- Worker 注册成功且绑定连接后才绑定 WORKER；注册失败仍为 UNKNOWN。
- malformed JSON、无效 envelope、未知消息类型不绑定角色。
- 绑定后不能切换角色；错误角色发消息返回 INVALID_REQUEST，不执行该操作。
- WORKER 身份匹配、重复注册、旧连接替换检查继续由 WorkerSession 处理。
- TASK_ASSIGN 和 RESPONSE 是服务端出站类型，任何角色都不能将它们作为请求。
- 不新增握手消息，不改变已有 Worker v1 wire 格式。

**角色隔离不是认证。没有 TLS、账号或租户权限；仅用于可信受控网络。
不要把当前服务直接暴露到公网。**

## 协议与返回值

每帧为 4 字节大端 body 长度 + UTF-8 JSON，body 上限 1 MiB。不是 HTTP，
也不是换行分隔 JSON。示例请求：

```json
{"version":1,"type":"submit_task","request_id":1,"data":{"task_type":"REGRESSION","target":"demo","revision":"r1","priority":0}}
```

成功响应（示例 TaskId）：

```json
{"version":1,"type":"response","request_id":1,"data":{"code":0,"message":"ok","result":{"task_id":1}}}
```

QUERY_TASK / CANCEL_TASK 的 data 为 `{"task_id":1}`。
QUERY_TASK 返回 result 中的状态、目标、revision、worker_id、priority 和时间字段。
合法请求的响应回传同一 request_id；无法解码的 envelope 使用 request_id=0。
客户端校验版本、响应类型、request_id、返回体结构和长度限制。

| code | 意义 |
| --- | --- |
| 0 | OK |
| 1 | INVALID_REQUEST：参数、协议或连接角色错误 |
| 2 | NOT_FOUND：任务不存在 |
| 3 | INVALID_STATE：例如取消终态任务或派发中暂不能取消 |
| 4 | INTERNAL_ERROR |

任务 target + revision 的 UTF-8 原始字节总长上限 128 KiB（不是字符数）。
Worker ID 上限 256 字节，hostname 上限 1024 字节。这些约束给查询和派发包
预留 JSON 转义空间；超限任务在生成 TaskId 和入队之前拒绝。

## 取消与执行边界

CANCEL_TASK 仅允许 PENDING/QUEUED，状态检查与取消在 Scheduler 同一把锁内完成。
ASSIGNED/RUNNING 返回 INVALID_STATE(code=3)，状态和 slot 保持不变；
终态任务不重复取消。远程 SIGTERM/ACK 尚未实现，不返回虚假的取消成功。

## 验收与后续

```sh
make test
(cd build && ctest -R 'test_control_plane|test_client_cli|test_client_transport|test_server_process' --output-on-failure)
```

- test_control_plane：全角色权限矩阵，真实 TCP Client 提交、Worker 注册、派发、
  TASK_START、TASK_RESULT、Client 查询 SUCCEEDED；含取消、错误角色、分片/粘包、
  超限输入、JSON 转义和并发提交。没有调用内部 createTask 来准备任务。
- test_client_cli：真正启动 CLI 子进程，验证参数、退出码、查询/取消和 Worker 闭环。
- test_client_transport：错配 request_id、错误响应类型、缺失字段、超长/截断帧、
  无响应及慢速分片截止时间。
- test_server_process：独立 server 可执行文件完成同样控制面闭环并正常响应退出信号。

后续顺序：先独立 Python Worker Adapter 执行 sleep/exit 等简单进程，
验证 PID、stdout/stderr 与退出码；随后才接 PJtest。SQLite/恢复、远程取消、
缓存、QUERY_REGRESSION 和 GUI 均未在本阶段实现。

本轮外网 WSL 验证：Release 16/16；Debug ASan + UBSan 16/16；
控制面、CLI、客户端协议、独立 server 进程、Worker 网络五项各连续 30 轮通过。
这不替代内网工具链或真实执行器的验证。
