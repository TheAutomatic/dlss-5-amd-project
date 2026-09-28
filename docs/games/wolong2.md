# 卧龙 2（Wo Long 2）

用户向兼容说明。代码里不按游戏名判断。

| 项 | 内容 |
|---|---|
| 症状 | lmxxf 开 NR 后，天空和高光严重泛白，泛白处会闪；有时整屏闪，有时只闪局部；极亮场景（陨石天空）会出现发黑和紫色色块。另外体感变慢 |
| 根因 1（格式契约） | 游戏送 `R16G16B16A16_TYPELESS` 的 FP16 线性 HDR。lmxxf 一侧原先把这个格式一律按 UNORM16 解释（依据是《浪人崛起》的 LDR 输出），而 daniel 侧与 FFX 侧按 FLOAT16 解释。同一个 `fmt=9` 被两端读成不同数值：FP16 的 `1.0` 按 UNORM 读只有约 0.234，编码器于是把画面推到过曝。**已修**：宿主在每帧按实际情况选择解释，view/codec/测光共用同一种（`fa308b3`，随包附 `typeless-float16.patch`、`codec-hue-safe-preexp.patch`） |
| 根因 2（细节混入高光） | 网络的 Detail（transfer）把高光继续推爆：Detail=0 或 `LmxxfDiagnostic=codec-passthrough` 时天空正常；Colour 与此无关（`f75297e` 起的对照结论） |
| 曝光 | 游戏没有曝光纹理（`preExposure` 恒为 1）。现在由运行时测光：每帧统计一次平均亮度并做时间平滑，`LmxxfAutoExposure` 控制，默认开（`e6ecf77`、`c6075d3`）。这取代了固定白点 8 |
| 现状 | **部分关闭**：格式契约与曝光两项已修，等实机复测确认泛白是否消失。高光里少混 Detail、闪屏、变慢（Streamline 报 `Exceeded VRAM budget`）未做 |
| 临时办法 | 复测前可下调 Detail，或改用 daniel 后端（0.3.3+）；游戏没有曝光时宿主写 `UseGameExposure=0`，走 daniel 自己的 auto |
| 测试约定 | 开始菜单就能复现。做对照时看 `lmxxf color` 日志行（`fmt=`、`exposure=`、`preExposure=`）。`LmxxfDiagnostic` 改了要重启 |
