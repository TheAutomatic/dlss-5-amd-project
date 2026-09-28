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

## 开工前清单

- [ ] 渲染分辨率、场景、构建脚本、DLL SHA256 已记录
- [ ] 需要比较的变量能在同一会话内原地切换
- [ ] 有帧生成时 PresentMon 带 `--track_frame_type`
- [ ] 报 fps 用中位；任何比值先证明同期
- [ ] 流畅度看 `MsBetweenDisplayChange` 与 `MsAnimationError` 分布
- [ ] 窗口内有没有 TDR 已对过事件日志
- [ ] 报「没问题」前想一遍：这个查法会不会恒返回空或恒返回命中
