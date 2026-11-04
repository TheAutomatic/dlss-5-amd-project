# 测量纪律

报帧率、比值、A/B 结论之前先读这份。下面每条都来自实际犯过的错。git/grep 查证工具的坑在 [dev-environment.md](dev-environment.md#git-查证的坑)。

## 总原则

- **我们只测桥接。** 后端（daniel / lmxxf 网络本体）的性能用上游作者的口径引用，不自建后端基准。见 [decisions.md](decisions.md)。
- 每个数字都附：构建脚本、产物 `OptiScaler.dll` 的 SHA256、渲染分辨率、场景、会话。
- 结论分清「排除」和「未测出」：「没测出差异」不等于「没有差异」，「没看到 X」不等于「X 不存在」。

## 读数规则

| 规则 | 说明 |
|---|---|
| 比值先证明分子分母同期 | 日志和 PresentMon CSV 的起止不会自动对齐。只能靠采集脚本的标记点或日志里的切换标记对齐；对不上就只报原始计数。反例：一次「31.8% 帧未降噪」的数字，分子取自约 66 s 日志、分母取自 44.9 s CSV，已进过 README、tooltip 和 Release 正文，花了几轮才清掉 |
| 典型 fps 报中位数 | 分布偏斜时均值不可用：同一段数据均值 48.8 / 中位 44.3；另一段只因 1 帧 2018.7 ms 卡顿，均值被拉低约 3 fps。报均值必须说明是否被 SPIKE 拉低 |
| 写清渲染分辨率 | 对外统一写「4K FSR 超级性能档（约 720p）」：1280×720 是 3840×2160 超级性能档喂给超分的分辨率。不要裸写「720p」 |
| 只比同一会话、原地切换 | 不同会话的绝对数字不能搬来比；比较槽数、后端、选项都在同一次会话、同一站位原地切 |
| 同场景、相近路线 | 跑动会抬高 GPU 负载；不要一边跑一边比 |
| 采集是否作废看 TDR 是否落在窗口内 | 精确到秒对照事件日志；不看文件名，也不因为「窗口邻接 TDR」整段作废（曾有窗口内 2 次 TDR 恢复而数据零掉帧的采集） |

## PresentMon 列语义

| 列 | 回答什么 |
|---|---|
| `MsBetweenPresents` | 应用调用 `Present()` 的节奏（提交吞吐） |
| `MsBetweenDisplayChange` | 实际显示的节奏。问「玩家看到的是否流畅」用它 |
| `MsBetweenAppStart` | CPU 帧起点的节奏 |
| `MsCPUBusy` / `MsGPUBusy` / `MsGPUTime` | 帧工作持续时间，**不是利用率**，也不能拆 A/B 两边的成本。`MsGPUTime = MsGPUBusy + MsGPUWait` |
| `MsGPUWait` = 0 | 只说明 PresentMon 在该跨度内没检测到 idle，不说明 GPU 满载 |
| `MsAnimationError` | 看分布、标准差、绝对值 P95，不看均值（正负抵消）。它能捕捉长短帧交替（2 槽时标准差约 10.5 ms，3 槽约 1.8 ms） |
| `MsUntilDisplayed` | 从 `Present()` 起算，不是从 CPU 帧起点；不要自造「与帧周期比 0.5×」这类判据丢数据 |

其它：

- 三列均值接近只是某条路径的实测关系，不是语义相同。某批 CSV 里 `MsCPUWait` 逐行等于 `MsInPresentAPI`、`AnimationTime` 等于 `CPUStartTimeInMs`，都不能推广成列定义。
- 某列「几乎全 NA」时写实际有效个数，不写「恒 NA」。
- HAGS 状态会影响 GPU 类时序，采集时记录下来；这些列只适合本机同会话相对比较。
- 组件归因（谁占了多少）要用 PIX / GPU profiler 或模块自己的 timestamp query，CSV 做不到。
- 涉及帧生成时，采集必须带 `--track_frame_type`，否则分不出应用帧和生成帧。

## RR 离线画质的通道口径

- `tests/fsr-rr/offline_chain.cpp` 的 RMSE、PSNR、细节梯度、平均偏差、能量比和 `noise_std` 只衡量 **RGB 辐射值**；RGBA 四通道仍全部检查非有限值。Alpha 不属于颜色误差，不能因输出贴图有四个通道就混入统计。
- `noise_std` 是所有 RGB 真值残差的总体标准差，不是分离出的随机噪声；空间模糊、重建偏差和通道差异也会影响它。亮度 RMSE 改善不保证每个空间 ROI 都改善。
- `run.json` 必须声明 `metric_channels_contract = "rgb-radiance-v1;nonfinite=rgba"`，比较器拒绝缺失、旧 RGBA 口径和不同版本。旧 CSV 必须重新生成，不能只补元数据标签；相同错误口径的两次运行也不能据此通过。
- 每次构建离线 runner 后，`run.cmd` 自动执行 `--metric-self-test`：直接调用实际 `ComparePlanes`，检查有限 alpha 不影响六项画质指标，NaN/Inf alpha 仍计入非有限值。此模式在 GPU 初始化前退出，不提供 GPU 执行或画质通过证据。

## 原生 RR 漫反射对照

- `offline_chain.exe --mode native-diffuse --manifest <manifest.ini> --out <dir>` 默认按顺序运行 `native_diffuse_flat`（常量 albedo、常量法线的有限平板）和 `native_diffuse_detail`（有限棋盘平板＋有界漫反射球）。`--native-scene flat|detail|both` 可单独选择，默认 `both`。每个 clean/noisy 序列长度为 `warmup_frames + measured_frames`，分析排除预热；不覆盖反射、运动或真实低采样 RT。
- 两场景共享相机：平板 view-Z=12、半宽高 14×9，near=0.1、far=32；覆盖全部 jittered render / center / target supersample 射线，没有天空、无限地面、horizon 或 far-bypass 评估像素。任何 miss、超出 RR bounds 或 converter `log(z+1)/log(far+1)>=0.99` 都使诊断失败，不用伪造的远处表面填充真值。原生 miss 编码仍显式定义：黑色、零 albedo/MV、signal alpha=-1、超 far 的 signed depth、neutral oct normal/rough1/material3；正常评估区域必须零 miss。flat 整幅（尤其避开图像边缘的内部 ROI）为恒定法线／albedo；主平板法线不在 oct fold 的零符号 seam 上。
- 原生 DirectDiffuse 独立生成 `E * max(N·L,0) / pi`，与转换路径共用同一 lognormal 噪声样本和 UNORM8 量化材质；原生 guides 不来自转换器，输出由独立 GPU pass 乘共享 diffuse albedo。两路使用独立 RR/SR 历史及相同 provider 配置。原生模式专用 converter / RR 最大尺寸及两路物理 guide / signal 均为实际 render 256×160，不再使用通用 544×336 ceiling；SR 最大尺寸为实际 render / target 256×160 / 512×320。其他模式和产品 API 默认值不变。
- target RGB 真值直接积分解析场景：每 output pixel 固定 4×4 midpoint grid，共 **16 个无噪声空间样本**、box filter，不对带噪图做 bilinear。render / pre-SR 参考仍为匹配输入 jitter 的单点解析真值，不能把两种阶段的误差混为同一滤波口径。
- `--native-signal-alpha 0|65504` 只改原生 active DirectDiffuse 的 alpha（默认合法 `0`，不是 hit distance）；生产转换输出保持原样。`--native-history normal|reset-every-frame` 默认 `normal`；诊断值同时 reset 两路 RR/SR 和 composition history，抑制跨帧累积以区分 accumulating darkening，而不是画质验收方案。两项均仅允许 native mode。Jitter 默认仍沿用既有 pixel-space Halton 约定，未因 AMD sample 的 NDC 说法切换；不能把此诊断当作已证明所有 title 的 jitter 单位。
- `metrics.csv` 同时记录 `native_presr/native_sr` 与转换路径 `presr/sr`；`frames/` 导出实际两路 signal/depth/packed-normal/motion、共享 albedo、真值、逐帧来源。R10G10B10A2 normal PFM 为归一化打包通道（RG oct、B roughness、A material），不是解码后的 XYZ。`guide_comparison.json` 记录物理尺寸、实际 translated alpha 范围及 quantization-aware delta：depth 1e-3、normal 2/1023、MV 1e-4 的诊断阈值和超阈分量数；阈值不是产品画质容差，超阈需检查 fold seam / 半精度输入，不能假称 bit-identical。depth 覆盖检查强制零 miss / out-of-bounds / farSkip。
- `run.json.native_comparison` / 逐帧 provenance 声明实际控制值、16-spp 真值和配对范围。API 成功不等于原生参考验收；必须检查实际 guides、资源尺寸、flat 内部 ROI 和时间序列，不能只凭整图均值宣称优于游戏原生 FSR RR。这不是普通旧／新版 `compare_runs.py` 验收输入。`segments.csv` 的 `clean_native/noisy_native` 仅为串行带验证的诊断时间戳，不作正式吞吐或游戏帧率结论。

短跑：先从 reviewed manifest 复制一份 `native-short.ini`，只把 `warmup_frames=4`、`measured_frames=8`、`frames=12`（保留 adapter / DLL / hash / seed）改为短序列。以下在 repo root、已有新构建的 `build-native/offline_chain.exe` 下运行；本次源码环境修正未执行构建或 GPU 验证：

```bat
build-native\offline_chain.exe --mode native-diffuse --manifest native-short.ini --native-scene flat --out exports\finite-flat-a0-normal
build-native\offline_chain.exe --mode native-diffuse --manifest native-short.ini --native-scene flat --native-signal-alpha 65504 --out exports\finite-flat-a65504-normal
build-native\offline_chain.exe --mode native-diffuse --manifest native-short.ini --native-scene flat --native-history reset-every-frame --out exports\finite-flat-a0-reset
build-native\offline_chain.exe --mode native-diffuse --manifest native-short.ini --native-scene flat --native-signal-alpha 65504 --native-history reset-every-frame --out exports\finite-flat-a65504-reset
build-native\offline_chain.exe --mode native-diffuse --manifest native-short.ini --native-scene detail --out exports\finite-detail-a0-normal
```

先比较 flat 四格实验 measured frames 4..11 的两路 presr 能量／内部 ROI 时间序列与实际 guide delta；再独立看 detail 的 SR 相对 16-spp truth 误差，不把 reset 的单帧重建与正常 temporal 累积直接排画质名次。

## 开工前清单

- [ ] 渲染分辨率、场景、构建脚本、DLL SHA256 已记录
- [ ] 需要比较的变量能在同一会话内原地切换
- [ ] 有帧生成时 PresentMon 带 `--track_frame_type`
- [ ] 报 fps 用中位；任何比值先证明同期
- [ ] 流畅度看 `MsBetweenDisplayChange` 与 `MsAnimationError` 分布
- [ ] 窗口内有没有 TDR 已对过事件日志
- [ ] 报「没问题」前想一遍：这个查法会不会恒返回空或恒返回命中
