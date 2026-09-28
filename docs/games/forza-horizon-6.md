# 极限竞速 地平线 6（Forza Horizon 6）

用户向兼容说明。**本文内容来自社区补丁说明，我们没有实机验证。**

| 项 | 内容 |
|---|---|
| 早期命令列表 | 游戏在 swapchain 创建之前就建 command list。lmxxf 默认的早期包装白名单包含 Unreal 引擎和 exe 名里带 `forza` 的进程（`c4690e5`）。`LmxxfEarlyExeWrap=true/false` 可以强制打开或关闭 |
| enhanced barrier | 游戏的 list 使用 D3D12 enhanced barrier。默认 fail-closed，拒绝切分，NR 不运行；原因是 layout/access 和 `SYNC_SPLIT` 都没有建模。需要时设 `[DlssNr] LmxxfAllowEnhancedBarriers=true` 显式放开，风险自担：切分可能把一个 barrier group 劈开 |
| 现状 | 等实测。另外，OptiScaler 本体对 Forza Horizon 5 有 `dbghelp` 导出方面的处理（上游已有），与本条无关 |
