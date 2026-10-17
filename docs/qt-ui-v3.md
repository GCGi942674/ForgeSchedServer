# Qt UI v3

> 当前安全策略：登录认证暂缓，正式模式下工作机暂停/恢复接单和 SVN 更新
> 按钮禁用，Server 同样拒绝直接管理请求。任务提交、查询和取消未执行案例
> 保持可用；删除记录继续禁用。下列管理界面描述保留为底层功能说明。

本轮以用户指定的 `gcrp-console-v3/index.html` 及其 CSS/JS 为唯一视觉参考，用原生 C++ / Qt Widgets 实现，不嵌入浏览器，不替换真实 Server 数据来源。

## 已还原

- 白色 196px 左侧菜单、品牌区、当前菜单高亮、顶部工作空间面包屑。
- 任务页四张统计卡片、批次列表、来源/业务结果筛选、完成进度与通过率分开统计、行内详情/停止后续/禁用删除。
- 批次详情 testcase 分页、成功/失败/超时/异常分类、原始 run 入口；TXT/CSV 导出弹窗可选结果类型，预览数量按全部匹配行而非本页计算。CSV 带 UTF-8 BOM，并处理引号及公式注入前缀。
- 工作机左侧物理服务器列表、右侧 slots；通信状态和接单状态分离，恢复只处理 ONLINE，SVN 更新沿用已有排空协议。
- 添加任务目录树、搜索、全选筛选结果、清空、独立批次表单、固定底部提交入口。flow_config 两列开关弹窗支持全选、关闭、模板默认、应用/取消。
- 每日回归继续使用 Server 的日期/模板/变化查询，人工任务不参与；提供每行两侧原始 run 入口。
- 原始 run 优先、左侧归档文件列表、深色正文、行号、片段搜索高亮、下一段与已上传归档下载。下载保留字节并按片段落盘，不把截断采集误称为完整源文件。
- 页面切换淡出、按钮按下反馈、点击短暂反馈、桌面弹窗淡入和操作提示；`GCRP_REDUCED_MOTION=1` 可减少动画。

## 真实数据边界

需要一起编译更新 **Server 和 GUI**，Worker 不需要因本轮 UI 修改而更新。

`QUERY_TASKS` 增加有界摘要：testcase、flow、context（来源/模板/名称/日期）、创建时间、执行结果摘要。不会返回完整 flow_config 或大日志。省略过长字段时标注 `summary_truncated`；可用任务详情查询完整参数。摘要按序列化字节限额控制，避免 50 行撑破 1 MiB 帧。

列表最多加载最近 5000 条任务；统计卡片、搜索、批次详情和导出均明确只覆盖该快照，不是全库汇总。分页检查 Server 实例和 snapshot_generation，数据变化则保留旧快照，避免混合统计。每日回归查询仍由 Server 完整统计并分页。

PASS 需要业务 `case_status=PASS`；仅有 SUCCEEDED 不自动伪造 PASS。CRASH / ERROR 按执行摘要区分。正式模式无真实记录时不填充演示数据；`--demo` 才启用示例。

删除记录和证据 ZIP 批量打包未开放，不用 GUI 模拟身份作为授权。日志不存在/过期/未上传的原因，当前 API 无法全部区分；不猜测故障 phase。

## 构建与运行

```sh
make GUI=ON
./build/bin/gcrp_gui --ip 192.168.10.11 --port 8080
```

窗口标题含 **UI v3**，请退出旧窗口再运行新二进制。仅查看演示：`./build/bin/gcrp_gui --demo`。

正式连接只接受 `--ip` 和 `--port`；GUI 不读取配置文件或 `GCRP_CONFIG`，
不再支持 `--config`。可以将程序复制到公共目录，从任意工作目录启动。
客户端仍需兼容的 Qt5 运行库和图形桌面。Server/Worker 的配置方式不变。

只有命令行的机器：

```sh
python3 -B tools/review_qt_ui.py
```

生成 `intranet-report/qt-v3/report.json` 和八张 PNG，不连接真实 Server/Worker。需安装 Qt offscreen 插件及可用中文字体。静态截图不能证明真实桌面的动画、鼠标、缩放或显示服务兼容性。

本地低内存验证使用单线程、关闭调试符号/优化的临时构建参数；没有更改生产 Make 默认的 Release 或 32 并行设置。

## 2026-10-16 验证记录

Debian Qt 5.15.15 主构建产物为 `build/bin/gcrp_gui`。相关测试 10/10 通过：Qt transport/gui/review/catalog、真实 C++ Server catalog + 65 条跨页任务摘要联调、ApiQueries、Protocol、CLI、RegressionService、RunLogArchive。八张实际 Qt 截图经离屏渲染和人工检查，报告为 `OFFSCREEN_PASS`。

尚未在内网 Qt 5.8 桌面实测，也没有用静态截图冒充动画流畅度或真实 GalaxCore 执行验收。真实 Server 联调使用临时数据库和隔离测试任务，不接入工作机，不启动 GalaxCore。
