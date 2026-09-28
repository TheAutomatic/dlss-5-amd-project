# 鬼武者：Way of the Sword（Onimusha）

用户向兼容说明。代码里不按游戏名判断。本文的安装路径以 XGP 版为例。

| 项 | 内容 |
|---|---|
| 安装位置 | `C:\XboxGames\Onimusha- Way of the Sword\Content\dxgi.dll`（这个目录可写）。**不要**写进 `WindowsApps`，安装器会拒绝。不要和 daniel 原生的 `version.dll` 放在同一目录 |
| 日志 | `Content\` 下有 `amd_presr.log`、`OptiScaler.log`、`amd_bridge.log`；daniel 自己的日志在 `…\_storage_\dlssnr_on_amd.log`，**不在** `Content\` 根目录 |
| 闪退或多秒长卡 | 来自系统 TDR，前面都跟着 daniel 日志里的 `SPIKE` 长任务，与本项目的调度无关。缓解办法是把 `TdrDelay` 调到 10 s |
| graphics 等待 | 实测第一个拒绝原因是 `viewport_unknown`（Create/Reset 之后 viewport 处于默认空状态），这时回退到 compute 等待，不影响出图 |
| 退出后进程残留 | 从菜单正常退出后，`OnimushaWotS` 还挂在后台。**不装插件也一样**，是游戏或 XGP 的退出路径问题，与本项目无关。用任务管理器结束即可 |
| 其他 | RE Engine 的 compute 状态恢复对 `onimushawots.exe` 和 demo 版都生效（上游 OptiScaler 的 quirk，见 `docs/NR-COMPATIBILITY.md`） |
