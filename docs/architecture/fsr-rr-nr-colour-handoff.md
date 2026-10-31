# RR 合成颜色交给 NR 时的资源状态

2026-10-31：修正 FSR-RR 的 before-SR NR 接缝误用游戏颜色状态的问题。

`ColorResourceBarrier` 描述游戏的原始 Color 资源。UE 路径可以将其自动识别为 `RENDER_TARGET`；它不描述 FSR-RR 自己生成的合成纹理。合成输出复用内部 Motion 纹理，该纹理只有 UAV 标志，并由合成步骤返回 `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`。

此前接缝先按游戏 Color 配置转换内部纹理，NR 返回后再转回。原始 Color 状态为 `RENDER_TARGET` 时，这会给没有 Render Target 标志的内部纹理录制非法状态转换；首次 NR 因队列发现而跳过也会走到该转换。

修正后：

- 删除这对多余转换，内部纹理以已有的只读状态交接。
- `EvaluateAfterDenoise` 将明确的内部颜色状态沿宿主调用链传给 `AmdBridge`；桥接构建的 Frame 使用这个状态。仅改外层 barrier、不修桥接声明仍会给实际 NR 传错状态。
- 游戏原始 Color/Output 的配置处理保持原意。内部资源状态是资源所有者的契约，不改写用户配置，也不临时修改全局 Config。
- 普通 SR 的 NR 调用未指定内部状态，继续按原来的 Color/Output 设置选择状态。NR runtime ABI 未改变。

验证入口：

- `tests/host/run.cmd nr-rr-state`：颜色状态选择的 CPU 回归，包括游戏的 RT 状态、post-SR 输出状态和内部状态优先级。
- `tests/host/run.cmd nr-rr-state-warp`：额外在 WARP 上创建与 RR 相同格式的 UAV-only 纹理，覆盖首次 bypass、后续输入复制与状态恢复，提交并等待 fence，检查 D3D12 错误消息。
- `tests/host/test_rr_nr_colour_source.py`：检查内部状态传递覆盖完整宿主调用链，接缝不再使用游戏 Color 的 barrier 配置；接入 host `ci`。

CPU、WARP 和源码检查属于定向回归。最终是否解决游戏中的 RR+NR 闪退，仍须用修正后的宿主实测；这些检查不代表完整 RR/NR 管线或画质已通过验收。
