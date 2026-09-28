# 赛博朋克 2077（Cyberpunk 2077）

用户向兼容说明。代码里不按游戏名判断，也不按游戏设颜色默认值。

| 后端 | 症状 | 设置建议 |
|---|---|---|
| lmxxf | 旧版曾因混入网络色相导致绿色霓虹发棕；`fa308b3` 已修改通用色彩混合 | 当前 Colour strength 0–1 保留游戏色相，>1 才混入网络色；不再沿用旧版“必须设为 0”的建议。实现见 `third_party/lmxxf/shaders/native_codec_decode.hlsl` 的 `hueSafe` 混合 |
| daniel 0.3.3+ | Reinhard 色调曲线在过饱和的游戏里偏艳 | 在 Ins 菜单的 `Display (daniel 0.3.3+)` → `Tone curve` 选择 `ACES (filmic)`，再保存设置。OptiScaler.ini 对应 `[DlssNr] ToneCurve=1`；作者 ini 的 `aces` 字符串由保存逻辑转换，不要把它直接填入 OptiScaler.ini |

另外，encoder 不调用上游的 `LegacyParameters()`，所以本产品**不会**像上游那样把 2077 的 Colour strength 强制为 0，而是和其他游戏一样跟随滑条（默认 1）。
