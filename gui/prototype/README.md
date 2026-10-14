# GCRP 本地 UI 原型

打开 `index.html` 即可试用，无需安装依赖或启动 Server。若浏览器限制本地文件，也可在本目录运行：

```bash
python3 -m http.server 8765
```

随后访问 `http://localhost:8765`。

左侧菜单包含工作机状态、任务管理、添加任务、每日回归对比。目录树勾选和人工任务提交可操作；新任务及删除/取消变更保存在浏览器 `localStorage`。回归对比只使用预置的 Clock 每日任务示例，人工任务不会进入该页。日期、版本、任务、slot 和日志均为演示数据，尚未连接 GCRP Server。

任务管理中，取消会保留记录；删除已结束任务的记录需要确认；仍在运行或排队的条目无法删除。原型里的“取消”不会真的停止进程。

如要恢复初始数据，在浏览器开发者工具中执行 `localStorage.removeItem('gcrp-prototype-tasks')`，然后刷新页面。
