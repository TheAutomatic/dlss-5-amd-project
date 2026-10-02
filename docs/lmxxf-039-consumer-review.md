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

| 项目 | 决定与再次评估的验收条件 |
|---|---|
| Bridge 更新 / 输入轮询 | 保留 `54e14de` bridge 及产品补丁。新轮询缺少稳定整帧收益；移植前须适配 recording lease、watchdog、重放/丢弃、跨队列与设备丢失，再测精确输出及真实游戏 mean/p99 |
| 直接共享输入 | 上游 `DIRECT_IO` 与 HIP prefix direct-input 不同；先移植共享输入 API 的所有权和完成凭证，验证重建与资源复用，不能仅开一个 flags 值 |
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
