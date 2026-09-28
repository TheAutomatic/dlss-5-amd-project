# 赛博朋克 2077（Cyberpunk 2077）

用户向兼容说明。代码里不按游戏名判断，也不按游戏设颜色默认值。

| 后端 | 症状 | 设置建议 |
|---|---|---|
| lmxxf | 霓虹发棕：Colour strength 为 1 时，超分前送进游戏调色的色相会把绿色霓虹变成棕色 | Colour strength 调到 0：只改亮度，不改色相 |
| daniel 0.3.3+ | Reinhard 色调曲线在过饱和的游戏里偏艳 | 作者建议 `ToneCurve=aces`。宿主目前没有这个菜单项，只能在 daniel 的 overlay 或 `dlssnr_on_amd.ini` 的 `[DlssNrOnAmd]` 里改 |

另外，encoder 不调用上游的 `LegacyParameters()`，所以本产品**不会**像上游那样把 2077 的 Colour strength 强制为 0，而是和其他游戏一样跟随滑条（默认 1）。
