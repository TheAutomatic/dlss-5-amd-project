# 鬼武者：Way of the Sword（Onimusha）

用户向兼容说明。代码里不按游戏名判断。本文的安装路径以 XGP 版为例。

| 项 | 内容 |
|---|---|
| 安装位置 | `C:\XboxGames\Onimusha- Way of the Sword\Content\dxgi.dll`（这个目录可写）。**不要**写进 `WindowsApps`，安装器会拒绝。不要和 daniel 原生的 `version.dll` 放在同一目录 |
| 日志 | `Content\` 下有 `amd_presr.log`、`OptiScaler.log`、`amd_bridge.log`；daniel 自己的日志在 `…\_storage_\dlssnr_on_amd.log`，**不在** `Content\` 根目录 |
| 历史长卡报告 | 旧采集中曾有 TDR 与 daniel `SPIKE` 长任务同时出现。新版本的闪退或长卡需对齐事件日志与采集窗口后归因；不沿用旧记录统一归咎于后端，也不把修改 `TdrDelay` 当作默认解决办法 |
| graphics 等待 | 旧采集曾记录 `viewport_unknown` 后回退到 compute 等待；这是当次拒绝原因，不能代表每个场景的当前状态 |
| 退出后进程残留 | 旧采集在不装插件时也观察到 `OnimushaWotS` 残留。新报告仍需带插件 / 不带插件对照，不直接推广旧结论 |
| 其他 | RE Engine 的 compute 状态恢复说明见 [NR-COMPATIBILITY.md](../../OptiScaler-DLSSNR-PreSR-Multipass-main/docs/NR-COMPATIBILITY.md) |
