# lmxxf 0.41 接入审阅

固定范围：0.40 `c81a88bc8534f7193df08ec3cae21d06d10d285d` → 已发布 0.41 源码及发行说明
`b687e13a8fcb8efd5be905ebbd0c9d70e15d88e3`。上游没有 0.41 Git tag，按完整 SHA 集成。
本页为功能证据；全部 1213 个变化路径和开关逐项清单保存在 `third_party/lmxxf/upstream-review.json`。
上游实验数据与本地验证分开，不能把上游 FPS 或 PSNR 当成本产品实测。

## 配置与多层

对照三份 `scripts/hip-*-flags.txt`、`CONFIGURATION*.md`、
`Development/results/{predict-default-20261004,package-041-20261005}/README.md`，
最终发布默认 MULTI_PASS=1、PREDICT=1、SKIN_PROTECT=0。早期预测实验默认0已被后续发布决定替代。
产品继续默认自由分辨率开启、NR→SR；用户已有 INI 选择保留。

两项新键注册到 ConfigKeys/kKnown，经 Config 读写和 PutEnvAlias 发布，再由
Runtime 的独立 CRT 同步、requestedOptions 重建标识进入 Network。菜单标签不作 INI 键。
flags 仅补未设置值；两项均在 Model 页、流程概览、reset、源 INI 和生成 INI 中说明。
GetStatus 读取已构建 bridge，报告请求层数、真实网络层数和预测/肤色选项。

`Network::MultiPassRest` 在3层且预测开启时计算两遍网络，16×16 tile 拟合增益，
正相关且 cos≥0.5 才使用，增益限制0..1并双线性插值，最终 clamp。
`hip/multi_pass_predict.hip` 的 RGB/stride 两种入口读取同一首遍/次遍数据。
这是有损预测；关闭才运行真实三遍。1/2层不变，后续跳块仍作用于第二遍及以后。
`multi_pass_skin.hip` 用原输入的 YCbCr 色域椭圆、chroma 下限和3×3平滑生成权重，
在2/3层将首遍与最终结果混合；不修改网络输入、seed 或历史。它不是语义分割，默认关闭。
来源：`multi-pass-predict-20261004`、`skin-protect-20261004` 的结果和对应 experiments。

所有层都在同一次后端 Record 内；已实现的 PostSr/SrPlacement 共用适配器先准备输入，
整条网络链后执行输出效果与 SR 写回各一次。三后端不需要分别实现处理位置。
之前的 SR→NR/1..3层验证见 [处理顺序](post-sr-nr.md)，不等同新增0.41的游戏验收。

## RGBA与资源生命周期

非末遍 C32 post 在原数学顺序后直接写 float4、alpha=1；末遍保持 RGB。
`DirectRgbaAllowed` 要求 wave-owned、raw chain、融合 finish/post、对应 RGBA 导出，
预测/肤色开启时还检查其 stride 导出。缺入口只能走同版模块内的既有路径；产品完整包仍拒绝缺模块。
`RunGraph` 的 graph_output_stride 随实际出口设置，下一遍和混合按该值消费。
`MultiPassFeed` 的旧 RGB 路仍按2^19行分块，规避 Windows pitched copy 的行数限制。

上游现在在构造期预备两个 feed。删除0.40本地“二层只轮换一个buffer”的补丁；
保留异步 adaptive reset、录制租约、PDL preflight及诊断。预测模块/符号和肤色资源预备在 producer wait 前。
预测 gain 和临时张量的初次池分配由本产品 PrepareFrame 的完整 Warmup 消化；
新选项走重建，不能在未完成的旧录制上调用上游热 setter。
`DLSS5_LAB_DIRECT_RGBA` 默认启用且影响生产，按实际消费者接入；
`DLSS5_LAB_FEED_DUP` 只是复制成本对照，默认1，0会省略必要复制，不提供玩家控制。
`DLSS5_MP_RAW_EXPORT` 还受未启用的 HIP_MP_RAW_EXPORT 编译条件保护，不属于产品路径。
来源：`multipass-direct-rgba-20261004` 和 `skin-protect-20261004`，后者记录并修复首帧未满足producer栅栏时的同步问题。

## 数值、布局与编译

0.41每架构38模块、合计76：新增 deep_fast-packed-fast、vit-stream-fast、multi-pass-skin、multi-pass-predict。
模块表、manifest、安装器夹具、ABI断言及发布新鲜度数字一起更新，不支持旧runtime/新模块混装。
模块来自该固定SHA的原版配方；五个LLVM23行/架构、其余33行COMGR，保留产品两个LINE_STORES覆盖。
LLVM23.1.2/ICU的来源和复现步骤沿用 [0.40审阅](lmxxf-040-consumer-review.md#数值模块与编译器)。
两个阶段均禁用real-true16，C64两行保留HIP_BARRIER_FENCE=1。
`module-script-encoding.patch`仅恢复上游脚本首行UTF-8 BOM，遵守本仓库PowerShell5.1编码要求；
raw夹具仍是固定SHA的无BOM原文件。配方、宏、命令和内核源字节不变，复用已核对来源的76模块。
配方指纹只规范化`.ps1`开头的UTF-8 BOM，其他脚本/内核/二进制字节仍完整参与；
专项回归覆盖BOM等价、真实配方变化、二进制不规范化和pending重试仍拒绝陈旧模块。

FAST_NUMERIC=1保留C32/C64/C128近似，并选择上述两个fast twin：
deep VIT_FAST_NUM=15（attention去half/裸rcp及projection去RNE half），vit-stream=4。
本地FastTwin补丁仅查询配方实际交付的四种twin，避免每次初始化把不存在的实验MH twin误报为安装缺失。
另修复上游无条件twin循环覆盖rtz_tall结果的问题：仅FAST1重写twin；FAST0的1920×1152/1088
保留c32-wave1-rtz，其他几何保持普通C32。这延续0.40既有优化，不新增模块或数值算法。
`VIT_FAST_H` 由bit2选择，不是独立开关。关闭FAST使用普通模块；不能保证跨版本FAST=1输出相同。
C512_FAST_PROJ默认0及VIT_FAST_NUM的f16残差导出位不启用：上游两种残差实现精确但更慢，
生产配方和宿主分支未收，不能将它们算作已启用优化。
来源：`fast-vit-c512-20261003` 和 `experiments/fast-vit`。

`C512_DIRECT_PACK=3` 只进入c512-m32-deep：mix省去F解码/重编码，contract直接输出byte。
原c512_hq、Hrtz、WMMA顺序和外部residual保留，编译要求native/branchless F与BYTE_F_ADD0匹配。
单独bit1/bit2实验有慢轮，采用已完成整网核对的组合3。
来源：`c512-direct-pack-20261004` 的域/实际FFN证明及 `c512-direct-whole-20261004` 的最终接受记录。

`VIT_CONTRACT_BYTE_EDGE=1` 只在两个vit-stream模块。host必须同时找到contract_bout、
qkv_bin_w5f8、project_n64_bb，且stream=3、QKV/contract的packed/fragment门通过。
contract缓冲从每token512个float容量变256，后两消费者分别直接读byte/精确decode。
`DLSS5_LAB_VIT_BYTE_EDGE` 默认启用的生产门保留。F输出有限的E4M3值，不对任意NaN byte宣称等价。
来源：`vit-contract-byteedge-20261004`、`vit-byteedge-formal-20261004`。

原生2560×1440补到2560×1472后，C256融合wholeblock尺寸门扩大到该尺寸，
已有模块/数学顺序不变；`DLSS5_LAB_C2561440=0`可对照旧split路径，默认启用。
ViT960继续用动态tokens核，640核名不代表只有640容量；未合入960常量化候选，未硬开Swin1440。
来源：`native-1440-optimization-20261004` 与 `vit960-20261004`。

## Bridge与宿主边界

检查本次pinned bridge差异及旧pin以来的整个差异，更新到同一0.41原始头后重套产品bridge补丁。
新增enqueue线程hipSetDevice(hip_device)，保留LUID匹配、失败封闭、跨队列完成凭证、租约、
输出COMMON封存、非阻塞计时/回收。Current device是线程状态，初始化线程设置不足以覆盖提交线程。
来源：`issue4-igpu-20261004`；双HIP设备复现来自上游提交者，本地没有该硬件，不冒称复现。
旧INPUT_POLL/IO_FUSE等不重新接入，沿用 [既定决定](decisions.md)。

上游add-on的PRE_UPSCALE=auto由首帧tail探针选择前后位置；产品明确Processing order与共享
SR适配器负责此事，不引入第二套自动规则。native_hot_flags的文件轮询/F9热切也不进入产品。
RE9 runtime把文件强度优先于host的改变不采纳。保留host标志优先和产品0..3范围，
只收有限值校验与严格双值解析；无host值时外部合法值可回退补充，非法值用默认。
上游src/runtime_strength_config.h是另一宿主的实现，不复制其覆盖策略或ABI1函数表。
来源：`pre-upscale-auto-20261003`、`strength-config-20261004` 与最终src diff。

## 全路径清点与沿用证据

1213变化路径中，719项为native-1440测量，165项为experiments脚本/生成器。
逐项记录路径与功能组，原始图表/CSV/日志只用于来源清点，不把每个样本另写一次批准。
各组发布配置/生成器与生产宏的关系已按上面调用链核对。

- 生产相关实验：fast-vit、multi-pass-predict、skin-protect、multipass-direct-rgba、native-1440、
  c512-direct-pack/whole、vit-contract-byteedge/formal、issue4-igpu，对应上文各组；采用的是最终生产配方。
- c32-small、vit960、c512-activation-lut均有慢轮，候选只在实验patch，不进生产；
  issue13原版oracle/prefix是原NVIDIA输入/输出调查，未形成已验证修复，不能改首层舍入掩盖差异。
- d3d-hip-stall、outside-net为同步计时调查，包含无效时间戳批次的撤销记录；不导入探针或性能结论。
- current-main部署与package-041脚本用于交叉检查38模块、最终默认和LLVM/COMGR配方；
  作者机器安装、回滚、锁、压缩包镜像和REFramework/RE9包装不作为产品构建依赖。
- 旧experiments组（bitexact-pm、c128-c64-inchain、compiler-sweep、config-layers、default-c/swap、
  fast-numeric、free-res、ideas-yi、input-slim、llvm23-vit、multi-pass/skip、net-timing/fix、
  next-candidate、night、pdl-1080、pending-review、rebuild-baseline、rtz1080、swin-body-gap）
  的脚本修改没有改变最终hip配方以外的产品消费者；其既有结论沿用0.40/0.39，不执行作者安装脚本。
- CHANGELOG/README/使用说明/DevHistory/WorkingPlan和package说明是文档，不把“全优化”措辞当实测。

旧开关的profile行号与共享源blob随上述更改而变，逐项关联本页功能与历史结论。
除列出的新默认、数值twin、布局/路由和bridge改动外，Options赋值、模块宏和旧排除决定保持不变。
不单独刷新指纹来放行未知来源；本地补丁由独立0.41 raw fixtures顺序重放验证。

## 本地验证

阶段验证（2026-10-05，RX9070XT）：

- MSVC runtime构建通过；38/76模块包契约通过，全部ELF e_flags分别为gfx1200=0x48、gfx1201=0x4e。
  LLVM预编来源、实际宏、RowOpts和COMGR混合输出由构建包装器核对。
- `tests/host/test_config_priority.py` 6项通过，包括独立静态CRT间所有已注册键的更新；
  `tests/sync/test_upstream_sync.py SourcePatchTests` 2项通过，全部维护补丁重放后与vendor一致。
- `lmxxf_nr_gpu --041-controls` 11次设置变化、每种重复2帧通过：非有限host强度被拒绝；
  每次改变仅重建一次；单遍预测/肤色和双遍预测不改变输出；回到旧配置精确恢复。
  1920×1080、全71块、FAST1的hash：1层806dd30da2c516da，2层f28e5509d485544e，
  预测3层f43d48ca8298522a，真实3层a62022af1497da72，预测3+肤色3fd001cae7b22c7f，
  真实3+肤色985d0940fc10f903。首版测试将对照循环放在旧输出读回之前，重建后误读过期测试指针；
  调整测试顺序后通过，未把该测试缺陷当成产品GPU失败或删除断言。
- 对同一DLL和模块逐项关闭新接口：预测/真实3层+肤色的RGBA/RGB出口hash相同；
  2560×1440 FAST0=b35be417637a9466、FAST1=b372262be18f3ed1，关闭ViT字节接口或C256新尺寸门
  均保持相同输出。未用固定图hash代替上游完整域证明或游戏观感。
- `lmxxf_recording_runtime_gpu` 冷启动3层+预测+肤色通过，覆盖录制、保留/重放、队列和完成凭证，
  baseline=49cc30048d3410ca，8个有效网络时间样本；未出现producer等待中的首次同步分配停顿。

最终审查修复FAST0的RTZ选择退步后，未重建未变的76模块，重新编译MSVC Runtime。
最终DLL完整`tests/lmxxf/run.cmd gpu`通过：格式/曝光、有效subrect、任意分辨率、
新旧控制、recording/adaptive/clear、2层及冷3层预测+肤色全部通过。
EXACT=`fe40c904da05472e`、AE和AE×16=`79233836b6257864`、R10=`8ba14ef2db0dddfe`
四项黄金输出通过。Runtime SHA256为
`1eec34fc2345c4e0983d9bc3cac7660a35db98288738c4350b2f15c0bc026270`。
旧候选的完整CI中止，不沿用其凭证。最终DLL的`tests/run-all.cmd --tier ci,device`
全部通过，自动生成的runtime-ci凭证与上面的GPU测试DLL哈希一致；宿主Release构建通过。
完整CI暴露同步测试夹具重复参数头，修正为使用已有的真实上游参数头后，26项模块专项及
最终完整CI通过（同步工具68项全跑，未用缓存跳过本次修改）。未为此重编Runtime或模块。
最终包沿用这份DLL与凭证，README、菜单/INI备注和Actions的38/76模块契约同步核对。
游戏观感/性能、双HIP设备和gfx1200实卡未测。

菜单复核：公共Processing order位于三页之前；输入尺寸/曝光放Input，多层/预测/肤色放Model，
共享强度/稳定放Output。修正Native input resolution提示中遗留的“总在超分前”描述，
明确两种顺序分别使用渲染尺寸和SR输出尺寸。本次仅修改帮助字符串；配置、运行时与模块不变，
沿用对应完整CI/device/GPU证据，重新构建宿主并核验新包。


### 游戏验收后的菜单调整

lmxxf 版本标识修正为 0.41。Model 页在风格之后连续显示两行跳块输入，
使用普通分隔标题而非折叠节点；基础跳块从 Advanced Kernels 移入此处。
`DLSS5_SKIP_BLOCKS` 是所有实际网络 pass 的基础列表，
`DLSS5_MULTI_PASS_SKIP_BLOCKS` 是第 2/3 次实际网络执行追加的列表，
与 `hip_reference_network.h::MultiPassRest` 合并集合的实现一致。
两个默认值均为 `none`。帮助说明提供旧版 `42,43,46` 示例和 `none` 还原方式，
明确预测的第三层不再执行一遍网络；生成 INI 中两项相邻且说明一致。

Model 页 reset 现在同时重置两项并同步环境；Advanced Kernels reset 不再越界重置它。
基础输入复用后续层已有的模块限制校验，避免提交会被 Runtime 拒绝的 4/69 或
与 MH byte stream 冲突的块；合法输入仍通过 Config 和 CfgKey 写入，按 Enter 重建并清历史。
键名、默认值、持久化读取和 Runtime/模块字节保持原样，不覆盖用户已有配置。
最终 diff 人工核对布局、输入校验、reset 归属和 INI 文案；六项配置优先级专项
（含独立 CRT 环境更新）通过，宿主 Release 重编。仅复用未变 Runtime/模块对应的
完整 CI/device/GPU 证据，普通打包仍检查凭证、产物新鲜度和实际 ZIP。
这项布局修改的最终游戏显示留待用户验收。

作者取消默认跳块始于 0.40（2026-10-03），0.41 沿用；并非本项目删掉功能。
固定 pin 的 `CHANGELOG.zh-CN.md` 第 297 行、`README.zh-CN.md` 第 21/53 行和
`src/LmxxfProductionOptions.h` 第 20–22 行互相印证。示例只恢复三个块的跳过配置，
不承诺复现旧版整体输出；旧版算术、风格等设置也会影响结果。
