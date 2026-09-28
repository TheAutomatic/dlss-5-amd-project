# 高光闪烁诊断（测试 runtime）

用途：分辨局部时序变化首先出现在游戏输入、编码、模型还是 NR 合成阶段。
这不是闪烁修复，也不是帧率测试包。普通构建不包含采集代码。

## 给测试者

1. 退出游戏，备份并替换原位置的 `dxgi.dll` 和 `LmxxfNrRuntime.dll`。
   沿用自己的配置、权重、modules 和 `version.dll`。
2. 进入能稳定复现高光闪烁的场景，把闪烁位置放在画面中央，尽量保持镜头静止。
3. 按一下 **F9**，保持场景至少 5 秒。一次 runtime session 只采集一次，最多 64 次 Evaluate；
   如需重测，重启游戏。F9 不会被拦截，游戏自身的 F9 绑定仍可能生效。
4. 打开资源管理器，在地址栏输入 `%TEMP%`，找到最新的
   `lmxxf-highlight-进程号-时间.nrhl`，连同本次 `OptiScaler.log` 一起提供。
   日志的 `lmxxf runtime snapshot` 会显示 `highlight=F9-ready/capturing/writing/saved/failed` 和路径。
5. 测完恢复备份。采集期间有额外拷贝、资源分配和 CPU 回读，不用这段测 1% low。

文件包含画面中心和四个象限的局部原始像素（每块最多 32×32），没有整屏截图。
采集文件可能包含这些位置的游戏 UI 内容，分享前按游戏画面资料处理。
只在用户按 F9 后开始，磁盘写入在 CPU 后台线程完成，无逐帧文本日志。

## 记录范围与解释

- `game_input`：游戏交给 NR 的 Color。
- `encoded_input`：编码后的 FP16 工作表面，在 RGB 解包和模型前。
- `neural_output`：模型输出转换得到的 FP16 工作表面。
- `nr_composite`：解码、合成后的 NR 输出，**还没经过 SR、FG、后处理或显示**。
- `exposure`：本帧实际绑定的曝光纹理值；无纹理时没有这一条。
  每条记录同时携带 `pre/scale/paper`、颜色/迁移强度、debug view 和曝光来源：
  1=游戏纹理，2=自动测光纹理，3=无纹理（游戏 preExposure 或固定 paper white）。

只采集 RGBA16_FLOAT 色彩表面及 R16/R32_FLOAT 曝光；不支持的表面会缺少对应阶段，
不能把缺失理解为全黑。支持的目标是当前鸣潮日志中明确的 FLOAT16 路径。
网络表面的区域位于适配后的有效图像范围内；不同阶段分辨率和编码不同，区域只是
近似对应，不能直接相减判错。相同阶段连续帧的变化也可能来自镜头移动、抖动或正常光照变化。
小采样块没覆盖闪烁位置时，结果无法排除问题。

GPU 完成由实际 consumer 提交后的 fence 确认；记录命令不算完成。
取消的样本不会回读，资源不回收复用；session 安全 drain 后释放。
最多约 11 MiB readback 加约 11 MiB CPU 暂存；设备丢失时沿用 session 的保留资源策略。

## 构建与分析

在 MSVC x64 开发者命令行中，只对本次测试 runtime 构建启用：

```bat
set "_CL_=/DLMXXF_NR_HIGHLIGHT_DIAGNOSTICS"
call tools\build\build-lmxxf-runtime.cmd exports\highlight-runtime
set "_CL_="
```

不要将诊断 runtime 作为正式发布件。

```text
python tools/diag/analyze-highlight.py capture.nrhl --out summary.csv
```

CSV 的 mean/p95/max 是该阶段采样区域的亮度统计；曝光阶段就是曝光标量。
`mean_abs_delta` 只比较相同区域且 Evaluate 序号连续的两次采样。
NaN/Inf 单独计数，几何变化或漏帧不生成差值。原始二进制保留所有采样像素，便于后续分析。
WARP 测试已接入 `tests/lmxxf/run.cmd warp`，分析器测试接入 `abi`。
