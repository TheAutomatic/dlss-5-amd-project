# 卧龙 2（Wo Long 2）

用户向兼容说明。代码里不按游戏名判断。

| 项 | 内容 |
|---|---|
| 修复前报告 | lmxxf 开 NR 后，天空和高光泛白、闪烁；极亮场景出现发黑和紫色色块，并有变慢报告。以下排查记录来自修复前，不能直接当作当前缺陷清单 |
| 根因 1（格式契约） | 游戏送 `R16G16B16A16_TYPELESS` 的 FP16 线性 HDR。lmxxf 一侧原先把这个格式一律按 UNORM16 解释（依据是《浪人崛起》的 LDR 输出），而 daniel 侧与 FFX 侧按 FLOAT16 解释。同一个 `fmt=9` 被两端读成不同数值：FP16 的 `1.0` 按 UNORM 读只有约 0.234，编码器于是把画面推到过曝。**已修**：宿主在每帧按实际情况选择解释，view/codec/测光共用同一种（`fa308b3`，源码补丁由 `tools/lmxxf-sync/patches/` 维护） |
| 历史对照 | `f75297e` 时期曾观察到 Detail=0 或 `LmxxfDiagnostic=codec-passthrough` 可消除天空泛白；该对照早于后续格式与曝光修复，不能据此要求当前版本下调 Detail |
| 曝光 | 当前优先使用游戏曝光 / pre-exposure；没有可用曝光时由 `LmxxfAutoExposure` 控制运行时测光（默认开）。旧采集里没有曝光纹理、`preExposure=1`，不代表所有版本或场景都如此 |
| 当前发布记录 | [README](../../README.md) 的 1.9.6 更新已记录修复部分高光泛白与闪屏；不再把修复前现象统一列成待修复问题。新问题应附当前版本、场景和日志重新确认 |
| daniel 已知报告 | 0.4.3 / 0.5.0 在卧龙 2 Demo 可能闪退，原因尚未确定，见 [README](../../README.md)。不把换用 daniel 当作无条件绕过办法 |
| 排查 | 看 `lmxxf color` 日志行（`fmt=`、`exposure=`、`preExposure=`）。`LmxxfDiagnostic` 改了要重启；历史复现点在开始菜单，新报告需记录实际场景 |
