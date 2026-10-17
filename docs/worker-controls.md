# 工作机管理 V1

> 当前发布策略：身份认证另做，远程暂停/恢复接单和 SVN 更新一律由 Server
> 拒绝，GUI 对应按钮禁用。删除记录继续禁用。下面保留的是底层实现与未来
> 认证接入后的行为契约，不代表当前能通过 CLI 或网络调用这些操作。
> 普通任务提交、查询、取消未执行案例不受影响。详见
> [生产加固与部署注意事项](production-hardening.md)。

Server、CLI 使用 server.conf，工作机使用 worker.conf。GUI 使用 `--ip 地址 --port 端口`，不读取配置文件。Server 和 Worker 都需更新到本版本。TCP+JSON framing 不变。

## 行为

- 停止接单不关闭进程、不杀 testcase、不提前释放 slot。ASSIGNED / RUNNING 继续完成。
- ONLINE/OFFLINE 是通信状态；accepting_tasks 是接单状态。暂停的在线 slot 仍发心跳。
- 整机 pause 设置当前和历史子 slots，暂停策略供后续新增 slots 继承；整机 resume 只恢复请求当时 ONLINE 的 slots，不改 OFFLINE / DRAINING 或未来 slot 的策略。响应列出实际处理与跳过的 worker_ids。单 slot resume 同样要求 ONLINE。
- 更新请求先封锁该 slot 的派发，等待执行与结果上报完成，再通过心跳交付到 Worker。
- 更新范围只有 test2 或 all，Worker 自行构造 `svn update --non-interactive -- 本机副本目录`。不接受任意命令、路径或 shell 字符串。
- SVN 使用工作副本源仓库的当前版本，与任务的 ZIP revision 不是同一概念；本次操作不会改变任务指定的 GalaxCore ZIP 版本。
- 采用与 testcase 相同的工作目录内核锁、独立进程监督器、心跳和退出证据。更新后做 SVN 冲突检查，不做 revert、删除或自动解决冲突。
- 更新成功恢复此前的接单意向（更新期间再次 pause 会保留暂停）；失败保持停派。检查 SVN 副本后可重试更新，或明确恢复接单。
- 进程退出未证明时不回报完成，隔离该 slot。Worker 重启发现未完成维护日志时，必须具备监督器的清理证据，否则拒绝复用副本。
- 离线主机不能从 GUI 远程开机。更新要求在线且支持维护 V1 的单 slot Worker；旧 Worker 可以停派，但不能冒充更新成功。

## 状态与持久化

`IDLE → PENDING → RUNNING → SUCCEEDED / FAILED`。PENDING 时有活动任务则显示“等待排空”，RUNNING 才显示“更新中”。请求接受不等于更新完成。
主机/slot 策略、维护状态和 operation_id 去重记录写入 SQLite。数据库不可写则不发布内存策略；批量请求先整体验证，再单次持久化。
Server 重启后恢复策略，Worker 重连不会自动解除暂停或维护隔离。Worker 本地 maintenance.json 保存结果，丢失 ACK 后重发结果而非重复运行 SVN。

**数据库 schema 升至 3（新增 worker_management 表）。部署前备份数据库；旧版 08e9314 不识别 schema 3，回退时须用升级前备份，不可直接把旧二进制对着新版 DB 启动。**
本版本不自动升级正在运行的生产服务、不移动数据库或 SVN 副本。

## GUI

工作机页左侧选物理主机，右侧显示 slot 通信、接单、维护状态及操作。整机和单 slot 均可操作。状态过期、接口不支持时禁止发起管理操作。维护列提示 operation_id 和错误，不把“已接受”显示成“已完成”。

## CLI

```sh
./build/bin/gcrp_client worker-control --machine-id novarch4 --action pause
./build/bin/gcrp_client worker-control --worker-id novarch4-slot1 --action resume
./build/bin/gcrp_client worker-control --machine-id novarch4 --action update --scope test2
./build/bin/gcrp_client worker-control --worker-id novarch4-slot1 --action update --scope all
./build/bin/gcrp_client workers --machine-id novarch4
```

CLI 自动生成 operation_id。若网络超时，先刷新核对；确需重试时，用返回过的同一 `--operation-id` 和完全相同的参数。不同参数复用同一 ID 会拒绝。CLI/GUI 均不自动重试写操作。

## 验证

```sh
cd build
ctest -R '^(test_worker_controls|test_slot_maintenance|test_worker_control_process)$' --output-on-failure
```

第三项使用实际 TCP、Python Worker 和本地临时 SVN 仓库，检查运行中停派、更新、重放、重启、恢复接单、SVN 冲突；不用 GalaxCore。它不能代替内网真实工作副本验收。
总验收脚本新增 WORKER_MANAGEMENT 分组，报告沿用 intranet-report/full-acceptance/report.json。

## 权限边界与暂缓项

现有协议角色不是身份认证。工作机管理仅用于可信内网，不应暴露到公网。
按用户决定，删除记录保持禁用，能力声明 record_deletion=false；以后必须由服务端验证管理员身份，而不是 GUI 自封管理员。
