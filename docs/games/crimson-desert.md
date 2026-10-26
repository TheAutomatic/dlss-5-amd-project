# Crimson Desert（红色沙漠）

Xbox/MSStore 版 `CrimsonDesert.exe`（`packages/bin64`）。v1.13.3.4 起经实机验证（RX 9070 XT，2026-10-26）。

## 引擎帧结构（NR 相关）

- 升采样走游戏自带 FFX（`Upscalers.Dx12Upscaler: ffx`），NR 挂在 FFX evaluate。
- 目标命令列表（list 10/18/23 轮换）使用 **D3D12 增强屏障**，且把 FFX evaluate 包在一个**跨 evaluate 开着的 `PIPELINE_STATISTICS` 查询**里（heap 固定、type=3、index=5）。
- 推论：普通 split 会被 `enhanced_barrier` + `open_query` 两道闸全拒（查询 Begin/End 对不能跨两个原生命令列表）。

## 默认策略（GetNrGamePolicy：`crimsondesert` 匹配）

- `enhancedBarriers=true` → `LmxxfAllowEnhancedBarriers` auto=on。
- `splitQuerySplice=true` → `NrSplitQuerySplice` auto=on（切分查询拼接：producer 补 EndQuery / continuation 补 BeginQuery，游戏查询结果只覆盖切分后半段）。
- 显式 true/false 始终优先于 auto；两项菜单里均为三态，实时生效。

## 已验证

- split + 拼接约 19,000 次切分无异常（0 failed / 0 bypass），`split_accepted` 每窗口 ~200。
- inline 模式同样可用（inline 准入不看查询），但 split+auto 策略是默认推荐路径。
- 一次照相模式 GPU 挂死（`0x887A0006`，关闭 NR 约 10 秒后进入照相模式时），未复现；
  见 work/notes/2026-10-26-crimson-desert-inline-visual-inactive.md 崩溃一节。
- "NR 下人物区域视觉变化弱"未闭环：候选解释为引擎把人物层画在 NR 所挂 pre-SR color
  之外，判别方法：`RunBeforeSR=false`（SR→NR）下观察人物是否变化。
