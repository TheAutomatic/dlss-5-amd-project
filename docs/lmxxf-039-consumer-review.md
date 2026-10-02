# lmxxf 0.39 消费者审阅（2026-10-02）

审阅范围为 `c809efb0ea2960f148624730898da61b8fb55a45`（0.37）至
`82ce821f0ea1a12925d04c353cb2d1b9ee006c11`（0.39 后六个提交）。完成状态以
[UPSTREAM.md](../third_party/lmxxf/UPSTREAM.md) 和正常同步结果为准。
本记录不代表游戏验收、Actions 验收或正式发布。

## 审阅覆盖

完整检查上游变化路径，包括部署配置、生成器、实验源码、结果和发布脚本，以及
pinned bridge 的完整差异。逐项结论见
[upstream-review.json](../third_party/lmxxf/upstream-review.json)。扫描器补齐了仅在源码出现的
DLSS5 参数和非 HIP 前缀的宏，包含空定义、函数宏和跨行定义；扫描结果仍须人工追踪消费者。

判断链为环境变量/Options → 产品 Runtime/C ABI → 模块选择 → 实际生成配方 → 内核读写者。
HLSL 网络、独立 add-on 的拦截器/overlay、实验运行器中的参数不等于产品可用配置。
函数宏和导出模板也不等于每条分支都会执行。

## 实际生产配方

每个架构 31 个模块，gfx1200/gfx1201 共 62 个。构建器对所有模块添加
`HIP_ISA_HALF=1`，对 `-packed` 模块添加 `HIP_PREPACKED_WEIGHTS=1`；不能只看配方行或源文件默认值。
本次正常构建未开启 `RowOpts` 或 `ExtraOpts`。保留两份 padded-wave 模块的本地
`HIP_FFN_LINE_STORES=1`，不会推广到其他模块。

| 路径 | 实际选择与限制 |
|---|---|
| C32 wave | `CW_INPUT_HALF=7`、pre/post byte 配对、prefix/finish vector tail、`CW_HOIST_UP=3`；`CW_UP_LOW_BYTES=0`，block65 的 C32 up 低输入仍为 f32 |
| C64/C128 wave | `W2_QKV_FUSE=3`、query batch 4、hidden tiles 2、hoist mask 15；启用配对的 half tail 和 low-byte up；skip-byte/direct-up 保持关闭 |
| Persistent Swin | query batch 2、explicit window；复用主体但不导出普通 wave 入口；不继承 wave2 的全部 hoist/half-tail 配方 |
| C256 | W16 权重布局和匹配导出；小通道 `W16_SMALL=0`，不宣称 C64/C128 同样启用 |
| C512 | 实际 `C512_FFN_ONE=2`，不能沿用旧发布文字中的 1；保留 padding、独立 packed-weight 缓存键和 capability 检查 |
| ViT | 产品 `VIT_STREAM=3`，AV FP8 与 contract F16 配对；w5/f8 权重必须有对应导出；旧 boolean byte/half stream 不是同一条路径 |
| Decoder | wide epilogue 仅在 deep_fast-packed；`HIP_BYTE_F_ADD0=1` 仅在 deep_fast-packed 和 c512-m32-deep |
| 数值/诊断 | lossy FAST、ablation、KSPLIT、projection FB8 等未开启；不能用 timing-only、允许差异的实验结果证明精确一致 |

W2 packing 的分段 overflow 模式依赖经检查的转换区间及有限输入范围。
现有语料上的精确一致不证明任意 NaN/Inf 输入都与另一种 clamp 实现等价。

## 产品配置与日志

Ins 菜单 / INI 优先，flags 文件和外部环境仅补主机未设置的键，最后才用编译默认。
Style、compact 1088、RGBA32F/typeless float 解释已接入产品配置；模板和打包生成的 INI
同时维护，新增控制不使用菜单显示文字作为 INI 键。

产品有意保留既有 adaptive preset：开启、period 16、global 1、local 50、image 1、hotkey 开启。
这不同于网络未设置环境变量时的 fallback：mode 0、period 4、global 0.22、local 1、image 0.35。
该差异已在用户文档注明，不通过追更静默改动。adaptive idle 仍为 500 ms；实验覆盖值不是新菜单配置。

产品使用显式每帧 codec 参数，避免 LegacyParameters 或上游 flags 热重载覆盖菜单。
保留自身有界、非阻塞的帧时间采集；不会启用 add-on 的逐阶段同步探针。
`DLSS5_VIT_ADAPTIVE_LOG` 是显式选择的实验日志，会同步、读回并逐帧写文件；默认未设置，
产品计时开关不触发它。INIT/VRAM、SPAN 等诊断与正常日志分开。

## 保留与暂缓

### 输入直写本地增量

2026-10-02：在原 pin/模块上定向移植 shared-input UAV 接口；首次创建及重建都先 RequestDirectInput，RGB Create 关闭 tiles，并在首次录制前 RedirectOutput。共享输入仅在同一资源直写时免拷贝，COMMON→UAV→COMMON 边界不依赖录制次数；RecordingChain 持有 bridge，实际执行的队列完成凭证继续保护重放、重建和池复用。非直写的独立 bridge 调用者仍用原拷贝路径。

该路径不读取上游 `DLSS5_DIRECT_IO` bitmask，不增加产品开关或旧 ABI 兼容。FP16 输出原本就直接返回 private_output；非 FP16 的必要格式转换保留。整体强度和稳定器继续消费原解码结果。关闭 tiles 在 1920×1152 processing 尺寸下每条链减少 33.75 MiB 分配及每帧重复写入；RedirectOutput 保留原 color 分配，不能把它也计作显存节省。

验证：MSVC 两个候选运行库编译成功；旧版/仅关闭 tiles/完整直写在 720、900、1080 各 12 帧输出哈希逐帧一致（分别为 `9e99616bb5014312`、`799c164a4a0daf6c`、`fe40c904da05472e`）。现有 runtime recording GPU 专项在 PDL=1/0 均通过，覆盖跨队列、丢弃/重放、曝光与尺寸重建、NR off 后旧录制、新旧 session 并存、失败 Signal 保留资源，D3D12 debug 无错误。SourcePatchTests 两项通过，补丁由独立原始快照重放匹配。

本机 RX 9070 XT，复用录制的顺序为旧版→仅关 tiles→直写→直写→仅关 tiles→旧版，每次 600 帧弃前 100，表内每项合并 1000 个样本。口径为 producer 提交至 consumer 完成的 CPU 墙钟，包含 CPU/GPU 交接及等待，不是游戏帧时长或单纯 NR GPU 时间。

| 网络档 | 旧版 avg / p99 (ms) | 仅关闭 tiles | 完整直写 |
|---|---:|---:|---:|
| 900 | 8.915791 / 12.6723 | 8.905803 / 12.6279 | 8.888980 / 12.5973 |
| 1080 | 12.204966 / 15.5093 | 12.215411 / 15.5301 | 12.151442 / 15.4883 |

完整改动本批平均省约 0.027/0.054 ms，合并 p99 略降；单轮仍有波动。先前每帧重建录制/上传输入的对照波动更大（例如 1080 旧版两轮 avg 21.03 与 19.44 ms），不用于量化提速。结论是保留此分支内接入供游戏验证，不宣称稳定游戏帧数收益。未运行全量 CI、gfx1200 实机、游戏后端切换/效果菜单组合；发布前另做最终验证。没有更改模块配方、shader 算法、输入轮询或 IO fusion。

### 剩余保留项

| 项目 | 决定与再次评估的验收条件 |
|---|---|
| Bridge 更新 / 输入轮询 | 保留 `54e14de` bridge 及产品补丁。新轮询缺少稳定整帧收益；移植前须适配 recording lease、watchdog、重放/丢弃、跨队列与设备丢失，再测精确输出及真实游戏 mean/p99 |
| 直接共享输入 | 已作本地定向接入，见下文“输入直写本地增量”；上游 `DIRECT_IO` bitmask 不作为产品配置，游戏性能验收仍待完成 |
| IO fusion | 当前产品没有 UseNeuralBuffer 消费者；需要匹配 compose shader、资源 pin 和录制生命周期，独立验证输出及 mean/p99 后再考虑启用 |
| Gather fold / projection FB8 | 主机与模块配对开关均保持关闭；前者已有变慢证据，后者改变残差存储。后续必须验证配对布局、精确/AE 输出和整帧收益 |
| 老实验 Options | 如 sparse VMM、旧 ViT stream、window fused、替代 FFN；记录中逐项说明依赖/冲突。VMM 须先解决多 chunk 驱动正确性，不能仅凭减少分配量启用 |

## 修复与实际验证

接入阶段已修复 Style 初始化异常后的 HIP module 泄漏，使用独立原始上游夹具维护补丁。
故障注入对原始实现失败、补丁后通过。格式 fallback 在创建视图前检查设备
Texture2D/SHADER_LOAD 支持；重建时正确清理 geometry/Style 状态。

已通过正常双架构模块构建、MSVC host/runtime、MinGW runtime、完整 CI、device 和 RX 9070 XT GPU 测试。
审计重新核对全部 62 个二进制哈希及 5 份规范化文本元数据哈希，并检查实际 defines/opts。

| GPU 检查 | 输出哈希 |
|---|---|
| EXACT | `fe40c904da05472e` |
| AE / scale16 | `79233836b6257864` |
| R10 | `8ba14ef2db0dddfe` |
| Recording | `9e99616bb5014312` |
| Style 0 / 2 | `b9d51f33f1cea174` / `cb30fe1e72358339` |
| Compact 1088 | `6215e046d94a7eb0` |
| RGBA32F + AE | `baea37576a56eeb3` |

Style 1、1152 rows 恢复 EXACT；每次设置变化只重建一次，重复帧稳定。
未进行 gfx1200 实机、完整游戏矩阵、新 Actions 或新测试包验收。
正式发版仍须遵守 [release.md](release.md)。

## 2026-10-02 本地效果与计时修正复核

上游范围、模块配方和 26 项暂缓不变；本次只复核本地最终 diff，沿用未变化的上游证据。
审阅组织按 tools/lmxxf-sync/README.md 改为功能分组，逐项清单只负责覆盖；生成器、
部署配置与 pinned bridge 仍在覆盖范围，生产无关的实验不再要求重复展开原始记录。

- 稳定器：shader 在保存残差前除以每帧 preExposure，外层因此不再因正常 preExposure
  变化清空历史；保留 reset、无效输入、尺寸、exposureScale、深度/抖动约定和超时保护。
  状态区分建立历史和可用历史，不宣称每个像素都通过重投影。
- 菜单：共享 Reset 恢复 Config 声明的默认值并失效历史，lmxxf 整体 Reset 同样覆盖；
  Lighting / Appearance 局部 Reset 保持原范围。整体强度计算未变。
  Page Up 保留第一行 NR 字段，编解码与输出效果独立换行，宽度计算随之更新。
- 计时：NR_NETWORK_TIMING_AVAILABLE=0 同时阻断生产桥接事件启用和样本收集；
  网络字段显示 N/A (paused)。保留 ABI、配置键及独立的 CPU/codec/effects 测量，
  日志仍默认关闭且摘要限频。待上游正式网络计时接口可用后再接入。

验证：tests/run-all.cmd --tier ci --out exports/nr-fixes-tests 全部通过；包含连续曝光
变化下的残差滤波、提交历史保留、显式场景重置和状态回归，以及 host、ABI、WARP、
安装/打包、同步测试。tools/build/build-release-local.cmd --fast 宿主编译通过，
该编译独立于上述完整 CI，不作为替代。无新增 GPU 硬件测试、游戏画质/菜单实测或发布包验收；
上面的历史 GPU 结果只保留为未变化上游算法的既有证据。

## 2026-10-02：定向接入 GetTimings、移除快捷键与旧 ABI

本次定向移植 upstream `fe4d1d734aa6e5aaa1e940229f11803bfe7e190f` 的计时功能，
来源为 include/LmxxfNrApi.h、src/LmxxfNrRuntime.cpp 和 Development/HIP/hip_d3d12_bridge.h。
完整上游 pin 仍为 82ce821f；没有接入同期 LLVM23/RowOpts/PrebuiltDir 编译实验，
也没有改变模块配方或现有 62 个模块。当前 pinned bridge 的录制所有权补丁继续保留。

- 接口：24 字节 LmxxfNrTimings 载荷沿用上游，GetTimings 接在本产品录制函数之后。
  整包交付只接受当前函数表及 FrameInfo，移除 ABI v1、历史尺寸和宿主降级重试。
- 事件：移植四槽异步 HIP event-query 采样。产品补充关闭采集、启用 epoch，
  错误后不复用未完成事件。默认不分配事件；UI 读缓存，不直接调用 HIP。
- 计时修复（2026-10-02）：早期 8 帧结果不足以归因 PDL。扩大测试后 PDL 开关两侧均有
  坍缩区间。end event 记录后、外部信号及完成标记之前，立即非阻塞 query 一次可修正读数；
  保留异步采集、默认 PDL 和完成标记，不增加同步等待。解除 PDL 专属禁用。
  显示仍丢弃低于 0.01 ms 的区间及重建后首样本，至少 3 个样本后取最近 5 个中位数。
  原始 GetTimings 载荷保留，正常日志默认关闭且仍限频。
- 快捷键：移除 ViT F8 控件、配置读写及上游轮询，保存在 reference-network.patch。
  NR 默认 End，FG 默认未绑定，避免新安装默认重复触发；用户显式配置保持有效。
- 流程：日常按最终 diff 审阅和具体风险做专项验证；完整测试集中在发版前。

早期（计时修复前）验证：MSVC runtime/host 编译；lmxxf ABI/C 冒烟/22 项 runtime 验证；
NR 统计中位数/异常区间与 PDL 状态单测；SourcePatchTests 原始夹具重放；
安装包模板/实际解包安装专项；实际 HIP bridge 录制/重放/关闭/epoch/错误测试；
runtime 录制 GPU 测试分别验证默认 PDL 不发布错误样本及 PDL-off 的网络样本。
最后非 PDL 场景 8 帧中 1 个坍缩区间被丢弃，7 个有效样本，输出 baseline
仍为 9e99616bb5014312。未重跑完整 CI、gfx1200 实机、游戏菜单/快捷键或性能矩阵；未打包发布。

2026-10-02 补充：旧的 PDL-only 归因和禁用策略已撤回。修复通过最终 runtime 的
1080p PDL 开／关各 1000 帧及 720p、900p、SPAN_PROBE、codec/debug 定向采样，
并验证录制生命周期。此项不改变原上游 26 项暂缓清单，也不代替游戏验收。
