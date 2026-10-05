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

lmxxf 版本标识修正为 0.41。两行跳块输入放在原有 `Advanced Kernels` 分类中，
紧跟 High resolution，相邻显示；不在 Model 页显示，也不新增 Block skipping 分类
或第 2/3 层子组。基础跳块恢复原位置，后续 pass 跳块一同归入此处。
`DLSS5_SKIP_BLOCKS` 是所有实际网络 pass 的基础列表，
`DLSS5_MULTI_PASS_SKIP_BLOCKS` 是第 2/3 次实际网络执行追加的列表，
与 `hip_reference_network.h::MultiPassRest` 合并集合的实现一致。
两个默认值均为 `none`。帮助说明提供旧版 `42,43,46` 示例和 `none` 还原方式，
明确预测的第三层不再执行一遍网络；生成 INI 中两项相邻且说明一致。

Advanced Kernels 的组 reset 同时重置两项并同步环境；Model 页 reset 不修改跳块配置。
基础输入复用后续层已有的模块限制校验，避免提交会被 Runtime 拒绝的 4/69 或
与 MH byte stream 冲突的块；合法输入仍通过 Config 和 CfgKey 写入，按 Enter 重建并清历史。
键名、默认值、持久化读取和 Runtime/模块字节保持原样，不覆盖用户已有配置。
此前六项配置优先级专项（含独立 CRT 环境更新）已通过。本次仅移动跳块控件和
重置归属：与原代码逐字比较确认两行控件、校验、帮助和提交逻辑未变；移除一对
TreeNode/TreePop，两项均在原 Advanced Kernels 内，Model 的 reset 不再包含跳块。
最终 diff 复核和宿主 Release（--fast）重编通过，不重复未受影响的完整 CI/device/GPU。
本地试包使用 LocalTest，继续检查产物新鲜度、模块契约和实际 ZIP 哈希；完整发布验证
和最终游戏菜单显示待验收。

`ViT / image reuse` 保留外层分组，已移除 `Reuse tuning` 子组。四个滑块
（period/global/local/image）直接跟在 `ViT adaptive reuse` 之后，同帧 checkbox 状态
控制 `BeginDisabled(!adapt)`，关闭时保留数值并全部置灰禁用。帮助及 reset 位于禁用
范围之外，原 Config/CfgKey 绑定和 ViT byte stream 互斥逻辑不变；本次未再修改复用控件。

作者取消默认跳块始于 0.40（2026-10-03），0.41 沿用；并非本项目删掉功能。
固定 pin 的 `CHANGELOG.zh-CN.md` 第 297 行、`README.zh-CN.md` 第 21/53 行和
`src/LmxxfProductionOptions.h` 第 20–22 行互相印证。示例只恢复三个块的跳过配置，
不承诺复现旧版整体输出；旧版算术、风格等设置也会影响结果。


### 覆盖安装的模块切换收尾

本地 `lmxxf-module-package.ps1` 的接入指纹随覆盖策略调整更新；上游 pin、
模块配方、ABI、Runtime 和模块字节不变。覆盖时先验证候选完整模块集，
旧目录只临时改名，发布成功后清理；发布失败或用户模块复制失败则恢复原目录。
受控旧模块、元数据及旧构建附件可直接替换；普通用户文件留在新树，无法放进
合法模块包的用户自加 hsaco / 其它架构文件另存，只保留这些用户内容。
开发者 staging / 同步的严格模块契约保持不变。

实际验证：Windows PowerShell 5.1 安装交互与覆盖专项通过；连续覆盖无永久旧包
或临时目录残留、旧布局迁移只保留自加模块、旧目录/源模块/自加模块锁定失败
保持原安装完整；普通及 Upgrade 发布阶段故障注入均恢复原目录且清理失败副本。
卸载专项覆盖两种日志轮转、后端日志/INI/dump、受限旧日志记录、备份内模型保留、
用户文件与 junction 保护。最终 diff 已复核复制/切换/回滚顺序和删除范围。
这次未重编 Runtime/模块、未重复完整 CI/device/GPU、未打包；用户选择只修改脚本，
游戏目录未进行清理或权重搬移。

### Mochizuki 尺寸诊断对共享入口的影响

`AmdBridge.cpp::Evaluate` 保存原有 render-subrect Get 的返回码与回退前尺寸，
在 Mochizuki 输入尺寸/模型比例变化时限量记录原始值、最终尺寸及颜色/motion 分配。
Get 次数、参数、零值回退、SR 前后尺寸选择和配置优先级保持不变；lmxxf 不进入
新增日志分支。本次没有修改 lmxxf Runtime、ABI、模块、内核或上游 pin。

对应 Mochizuki 修复在其私有 `Session::EnsureNetwork` 中实现：启用 DRS 但仍按
精确尺寸运行时，新尺寸稳定 300 ms 才替换现有网络；真正的 bucket 增长仍即时处理。
完整行为及边界见 [Mochizuki 说明](mochizuki.md)。最终 diff 已复核，宿主 Release
构建及 `tests/mochizuki/run.cmd gpu` 通过，涵盖三次短暂变尺寸后的立即恢复、
持续变尺寸、bucket 即时增长，以及已有回放、双队列、历史、格式与销毁检查。
测试以同步发布的 Info.building 判断已启动构建，避免与后台进度首次发布竞争。
沿用未变的 lmxxf 证据；未重复完整 CI/lmxxf GPU 或远端 Actions，游戏验收另行记录。

### Unity 启动列表的共享接管

伊莫启动时由 UnityPlayer.dll 创建、在交换链建立后仍反复使用的 DIRECT 列表，
无法被原来的 EXE 调用方过滤捕获；交换链后开启代理也不能改变已有原生对象的身份。
`SubmissionHooks.h::ShouldWrapCreate` 增加实际返回地址所属模块的判断，
`D3D12_Hooks.cpp::HookToDevice` 默认仅启用 Aniimo.exe 与已加载 UnityPlayer.dll
的组合。保留 Unreal/Forza EXE 规则，明确 true 时也接受其他 Unity 游戏的该模块，
false 同时关闭两条早期路径。探针 DIRECT 队列确认提交钩子就绪之后才发布标志；
Disarm 清除新标志。没有导入逐帧创建来源诊断、网络实验或其它分支的执行路径。

两个后端在原 Compatibility & Scheduling 中复用 Early command-list wrap 控件。
继续绑定已有 `LmxxfEarlyExeWrap`，Auto 清除显式值，保存后重启；菜单和源模板/
生成 INI 的说明一致。普通交换链后的代理、DIRECT 限制、内部创建抑制与开列表
诊断开关不变；不改变 lmxxf Runtime、ABI、模块、内核、上游 pin 或环境优先级。

最终 diff 复审、`tools/build/build-release-local.cmd --fast` 及
`tests/lmxxf/run.cmd device` 通过。新增回归用两个真实 DLL 调用方验证自动策略、
显式启停、EXE/其它模块排除、DIRECT/COMPUTE/COPY 以及两种 Create 接口；
保留列表跨代理开关后两轮拆分/执行/Reset，GPU 读回与原始数据一致，COM 身份、
viewport 和录制失效通知正确。新回归只接入 device 一次，并提供 early-unity
专项入口。未重复未变的完整 CI/lmxxf 网络 GPU；尚未在游戏里验证这次新宿主，
后续本地安装需确认早期 Unity 接管记录与首次角色管理/返回场景，不能将先前
Mochizuki 瞬态尺寸修复的游戏结果当成本次验证。按用户要求本轮不打包。

### HIP 往返诊断（2026-10-05）

`[DlssNr] LmxxfDiagnostic=hip-passthrough` 用于拆分/codec对照正常但正常推理花屏时，
进一步定位 RGB 布局与 HIP 桥接。默认仍为 off，仅 INI 设置，完全重启后生效。
源模板、生成 INI、诊断脚本的允许值和宿主状态同步说明；不是菜单热切开关。
两个 passthrough 模式互斥，非法组合在 PrepareFrame 发布 job 之前拒绝。
ABI2表和结构体尺寸不变，新增显式帧标志；旧runtime拒绝未知标志时提示整包更新，
不以原图回退伪装诊断通过。

`LmxxfBackend::RecordDiagnostic` 沿用正常颜色/曝光契约与 FinishRecord 租约；
Runtime 的 RecordInputs/RecordOutputs、实际 Begin/EndRecordingExecution 不变。
固定 bridge 新增 PrepareHipPassthrough，在录制前分配私有 RGB 缓冲；Enqueue
沿用原 producer signal → HIP wait → HIP工作 → HIP signal → consumer wait。
仅把 Network::Enqueue 替换为分块异步 float4→float3 复制，再使用正常的
device-to-shared输出复制。分块不超过2^19行，防止Windows HIP的2^20行截断。
临时缓冲跟随bridge，录制失效且已提交工作完成后才释放；提交回调没有新增分配、
释放或CPU同步。正常模式不分配该缓冲。网络初始化/预热仍保留，之后每帧跳过
全部网络层；本模式不测量推理耗时，也不证明网络内部张量/布局正确。

固定头变更同步维护 `tools/lmxxf-sync/patches/bridge.patch`，从独立原始0.41
夹具重放，FOLLOW头、HIP模块、算法默认值及上游pin未变。
实际验证：MSVC runtime及宿主构建；ABI/C宿主和23项runtime检查；原始补丁重放2项；
WARP颜色探针及新增解析/代理接管断言；RX9070XT `tests/lmxxf/run.cmd hip-passthrough`
和同一可执行文件的三层配置通过。TYPELESS/FLOAT、preExposure=2，720p/1080p/
1707×961的整张读回哈希与codec-passthrough一致，包含大于百万像素、旧录制、
跨队列、改尺寸和正常网络交替；实际HIP复制计数与互斥标志拒绝均已检查。
正常录制生命周期回归通过；1080p正常推理输出哈希仍为806dd30da2c516da，
与改动前相同。诊断日志沿用采样，新增一条仅在hip-passthrough中输出，默认模式
无新增逐帧日志。复核成功状态只在FinishRecord设置，避免每帧两种状态来回刷日志。

本次为本地调查包，未重复完整CI/全GPU矩阵/Actions，gfx1200实卡与游戏中新增
诊断尚待验证；本机D3D12 debug layer不可用，GPU证据为完成栅栏后的实际读回。
实机须同时确认hip_passthrough_recorded、hip_copy_queued递增和画面表现；
计数表示成功排入队列，不单独作为GPU完成或像素正确性证明。

### 残留着色器遮蔽整包更新（2026-10-05）

伊莫正常lmxxf及有效HIP往返均出现彩色噪点/条块/错位，codec往返正常。
将RGB改成私有缓冲再复制仍复现，已撤回该候选。实际问题路径是
`FindShaderDir` 原先优先选择 `assets_directory/shaders`，游戏残留的
`lmxxf-modules/shaders` 因而遮蔽本次整包自带的 `shaders`。只核对DLL、
76个HIP模块和权重无法覆盖此问题，必须核对真正被选中的HLSL。

旧 `native_game_rgb_input.hlsl` 不认识 `NATIVE_RGB_NO_TILES`，仍向u0写tile
排列、向u1写raster排列；当前 NativeGameRgbInput 在关闭无消费者的tile输出时，
将两者绑定到同一输出缓冲。旧shader继续两路写入，造成不同排列互相覆盖；
即使绕过逐帧网络或增加一次输入复制也保留此冲突。
相关RGB输入/输出HLSL及包装头在上游0.40 pin c81a88bc到0.41 pin b687e13a
没有变化；当前证据指向本地资源选择的混装问题，不能归因0.41新网络算法。
这不排除其它独立问题，仍需最终游戏画面验收。

修复只将runtime同目录的整包shaders提到assets/shaders之前；独立工具没有
runtime旁shaders时，原assets与开发目录查找仍可用。没有增加配置、CPU/GPU
同步或每帧复制，保留DirectInput；ABI/上游pin/模块/算法/INI默认值不变。
无需更新菜单或INI注释。包本来就将当前HLSL放在runtime同目录shaders中。

GPU复现：旧runtime配合游戏实际modules/shaders时，既有HIP全图对照失败；
修复版使用相同游戏modules（包括旧shader），旁边放经哈希核对的13份游戏整包
shader后，720p/1080p/1707×961全图对照、跨队列/保留录制/改尺寸/正常网络
交替全部通过，1080p正常推理黄金哈希恢复806dd30da2c516da。MSVC构建与
ABI/C宿主及23项runtime检查通过。新增 `test_shader_precedence.py` 将明确
不可编译的旧shader放到modules/shaders，确认整包HLSL优先；它包装原来一次
HIP专项调用，接入gpu与hip-passthrough入口，不重复GPU矩阵。

本次只需增量runtime；关闭HIP诊断后继续真实推理游戏验收。未重复全CI/全部
GPU/Actions，gfx1200实卡及不可用的D3D12 debug layer未验证，不视为发布认证。
