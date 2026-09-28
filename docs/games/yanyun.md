# 燕云十六声（Where Winds Meet / yysls）

用户向兼容说明。代码里不按游戏名判断。

| 项 | 内容 |
|---|---|
| 安装位置 | 代理 DLL 放在 `Engine\Binaries\Win64r - NR\`，文件名 `winmm.dll` |
| daniel | 游戏是纯 DLSS，A 卡上 daniel **原生**接不进去，只能通过本项目桥接。负载高，建议 **≥3 槽**：2 槽会大量跳过 NR 帧，看起来快但没有降噪 |
| lmxxf：切档显存暴涨（已修） | 切换 DLSS 档位或反复开关菜单时，显存涨到约 21.6 GB。原因是游戏丢弃了未提交的 list，bridge 卡在 pending 状态，session 反复重建并泄漏。已由 `CancelUnsubmitted` / `NotifyOutputSubmittedIfRecorded` 修复 |
| lmxxf：整幅发糊（已修） | Color 迟了 1 帧（private list staging），改为同帧执行后解决 |
| lmxxf：早期包装会崩 | 在 swapchain 之前包装 `CreateCommandList` 会崩。默认白名单只含 Unreal 和 Forza，燕云不在其中。**不要**设 `LmxxfEarlyExeWrap=true` |
| 其他 | 游戏不经过 FFX 的 `ffxDispatch` 导出，挂那条路径的方案无效（已排除） |
