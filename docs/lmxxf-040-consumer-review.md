# lmxxf 0.40 接入审阅

范围：从已完成的 `82ce821f0ea1a12925d04c353cb2d1b9ee006c11` 到 tag 0.40
`c81a88bc8534f7193df08ec3cae21d06d10d285d`。只针对该快照，不跟随移动 main。
本页记录功能结论；逐项覆盖及当前指纹在 `third_party/lmxxf/upstream-review.json`。
上游实验的数字不是本产品实测，也不代表每个候选都进入本产品。

## 配置、默认值与叠层

来源：`scripts/hip-{game,magpie,re9}-flags.txt`、`scripts/CONFIGURATION.md`、
`Development/deployments/{default-c,fast-numeric,multi-pass,multi-pass-skip,config-layers}-20261003/`。
部署先后有不同默认，最终以 0.40 模板全 71 块、FAST_NUMERIC=1、MULTI_PASS=1 为准。
产品另按用户要求默认 FREE_RES=1；上游模板仍是 0。

产品链路为 ConfigKeys → Config/INI → PutEnvAlias/PutEnvString → runtime 的
ApplyFlagsFileFallback → LmxxfProductionOptions/Network → 模块选择及派发。
新增键是产品键，不使用菜单标签作为 INI 标识。菜单/INI 优先，环境和三层文件仅补未设置键。
`native_config_layers.h` 合并 default→custom→native；重复取末行、空值覆盖下层。
宿主用 `none` 表示不跳块，避免 CRT 空值删除环境变量的歧义。
上游 F9、直接写 custom/native 文件和 add-on 热重载不接入：产品由 Ins 与 Save Settings 管理。

`Network::MultiPassRest` 将上一遍最终 RGB 转成 RGBA 输入，所有遍共用本帧历史、种子与风格。
2/3 遍会禁用 adaptive reuse，计时包围全部遍数。产品改选项时重建网络，保持录制租约语义。
本地补丁修正 N=2 时错误轮换到第二 feed buffer 的行为；仅 N>2 使用两个缓冲。
后续跳块叠加在首遍跳块集合上；禁用 C32 末端 4/69，MH byte stream 开启时禁用 5–22/48–65。
菜单拒绝不支持的组合，INI/外部无效组合由 runtime 明确回退为无额外跳块。
上游 `multi-pass-extrap` 只在解码后的图像上外推，结果不能代替实际叠层，不进入产品。

## 自由几何

来源：`native_network_geometry.h`、`native_input_geometry.h`、codec/RGB 输入 shader、
`hip_reference_network.h`；上游 `Development/results/free-res-20261002/README.md`。
每轴补到 64，至少 320；两轴均为 256 倍数时宽加 64，1088 行改 1152。
输入左上角放置、补边镜像，ViT 网格 pad4；熟悉的固定档位保持原网格。
超过单轴 8192 或 3840×2176 处理像素预算时回退档位；自由模式优先于档位/紧凑1080设置。
不是将任意输入强制压到1080，也不是提升游戏输出分辨率。

本地 PDL preflight 增加 free geometry 的计数器容量检查，超过 4.19M 处理像素不启用 PDL。
Swin/C256 的既有尺寸门控保留。上游 history/temporal shader 的 NATIVE_WIDE_ROW 必须与
对应 2D dispatch 配对；本产品主路径使用直接输入，不因此声称启用了 add-on 的时序历史链。
FREE_PAD/FREE_EXTRA 是几何对照诊断项，不新增玩家菜单控制。
上游 720 档与 NVIDIA 几何一致性仍未定论，保留旧固定720档；自由720按新规则计算。

## 数值模块与编译器

0.40 每架构 34 模块，共 68。Fast numeric 选择 c32-wave1-fast/c64-wave2-fast，
对应 CW_FAST_NUM=3/W2_FAST_NUM=3；修改的是舍入及倒数路径，有损，不等于逐位算术。
关闭 fast 时，1920×1152/1088 才选 c32-wave1-rtz（HIP_C32_RTZ_ISA=2）；其他几何用普通 C32。
fast 优先于 rtz，不需要内容重复的 rtz-fast 文件。缺模块不冒充成功，完整包仍检查全部受控模块。

首次接入使用全 COMGR、未启用 RowOpts；2026-10-04 增量接入改为上游 RowOpts 配方。
每架构 c32-wave1、c32-wave1-rtz、c32-wave1-fast、c64-wave2、c64-wave2-fast 使用
LLVM23.1.2，其余29行使用驱动 COMGR；c512-m32-deep 使用 max-ilp。
保留两个 multihead 模块的产品 HIP_FFN_LINE_STORES=1。
各模块实际 defines/source/hash、LLVM 版本和完整命令记录在 modules.json。
源码宏、配方与消费者的其余不变部分沿用 [0.39 审阅](lmxxf-039-consumer-review.md)，
本轮完整模块重建及旧配置黄金输出再次验证，不能仅因源码在模块中出现就宣称该核被派发。

原暂缓原因是官方 Windows 包不含 AMDGPU；这不是硬件或内核限制。
现使用 WSL2 Ubuntu 中官方 `LLVM-23.1.2-Linux-X64.tar.zst`，归档 SHA256
`6382de1c1a210ce5a5cc49d18bc8444d137742e7cbf9b19f4ae602bb1ab52534`，
Clang/LLD 23.1.2，LLVM commit `85ac560262434c9ccfc0c183ec22d4138ed647fb`。
该包包含 AMDGPU；LLD 所需 ICU70 从 Ubuntu 签名仓库取得并私有解包，未降级系统库。
无需构建 LLVM 源码或安装 ROCm SDK；上游 build-llvm23.sh 作为可选源码构建路径排除。

固定 pin 的源码经 git archive 提取；未修改上游 compile-modules.py：
`--compiler-rows llvm23 --row-opts --target-feature=-real-true16 --jobs 4`，同时生成 gfx1200/gfx1201。
Linux 只离线编译这10个 GPU 模块；其余模块仍用原版 Windows build-modules.ps1 的
`-RowOpts -PrebuiltDir`，宿主和 Runtime 继续 MSVC，不迁移 MSYS2 或产品桥接补丁。
C64 两行实际含 l23defines 的 HIP_BARRIER_FENCE=1，两个编译阶段均禁用 real-true16。
普通 C64 产物与上游 next-candidate 接受版本完全相同：gfx1201
`a0cad8cb4b53db0756cde6c4abec9c1d7b3f4c0eaa82cd59fa281c8958dc898d`，gfx1200
`b90443ee1589815c00f9e009cbffedaef2f079497b83e82369af25f139a6fdd1`。

同步包装器核对预编 manifest 的实际 defines、源码 SHA256、产物 SHA256、目标和调度选项，
补齐上游预编拷贝清单遗漏的 l23defines 与生成源码；不改写 FOLLOW 的上游编译脚本。
缺少 manifest、源码/宏/产物不匹配时失败，不能静默退回全 COMGR。
可复现步骤见 [同步工作流](../tools/lmxxf-sync/README.md#llvm23-与-rowopts-构建)。

依据：`compiler-sweep-20261001`、`llvm23-vit-20261002`、`next-candidate-20261002` 的结果与生成脚本。
LLVM22/23 不再为 gfx12 裸拆分屏障自动补 LDS 等待；COMGR 本轮仍走既有 LLVM21 路径。
HIP_BARRIER_FENCE=1 对实际 LLVM C64 行是正确性前提，不能删掉。
不扩展 LLVM 到所有模块：上游已查明 ViT/MH 裸屏障问题，补栅栏后仍无稳定性能收益。

## Bridge、ABI、计时和输入

审查上游及 pinned header diff 后保留 bridge 基址 `54e14de503431cd4536f8a7151b022af232178a9`。
产品 ABI v2、录制租约、跨队列完成凭证、故障退出和 timing epoch 均保留。
上游 ABI1 函数表加尾字段/旧大小重试不能替换这些契约。
TimingEnd 后非阻塞 event query 已由产品先行修复，本轮保留；新增输出 signal 后
非阻塞 stream query，保留 release event 回收计数，两者不是 CPU 等待。
用户计时仍按需开启，异常/过期样本显示不可用，不将 D3D 段冒充 HIP 网络。

runtime 已采用共享直接输入；上游 DIRECT_IO bitmask 不作为第二套玩家设置。
上游 add-on/ReShade hook、REFramework 宿主、Magpie 部署、游戏进程探针与作者安装路径不接入。
其调用顺序文档用于对照，不能把上游串行 ABI1 的 Drain/Retire 行为套到本产品录制重放。

## 其余候选与沿用决策

- C512_FFN_PROJ_FUSE/HIP_C512_FFN_PROJ_FUSE：宿主导出探测与模块宏必须配对。
  上游 ideas-yami-ikaruga 三轮均变慢，默认0；本轮不启用。
- HIP_ADDR_SKEW 的 KIND/MOD/SEED/STRIDE：改变缓冲偏移的对照探针；上游真实链收益处于噪声内，默认0，不增加菜单。
- 1080 强制分体 PDL：pdl-1080 候选有更高耗时，保留融合路由；请求PDL不等于每种几何都派发PDL。
- compiler sweep 的其他调度、部分寄存器、VOPD、s_delay_alu、LLVM ViT/MH 等组合：没有进入最终配方，保留实验，不普遍启用。
- INPUT_POLL、IO_FUSE、投影FB8已决定不接入（excluded），依据和重开条件见
  [决策记录](decisions.md#2026-10-04--lmxxf-输入轮询io-融合与-c512-fb8-已决定不接入)，不再作为优化待办重复推荐。
  其余20项也已按用户决定排除，见下节；不再安排重跑实验或新增无生产消费者的 INI 键。
- experiments 的运行/备份/看门狗脚本、results 原始日志/图片为作者测量材料。
  它们用于解释最终生产决策，不作为本产品构建依赖，也不逐份重跑作者游戏测量。
- 三种部署模板的主体是既有开关的中英注释展开；核对实际赋值、生成器和最终消费者，
  不把注释中的旧发布数字当作当前本地默认。上游部分 README 仍残留 fast 默认关闭的旧句，以最终模板为准。

## 未采用路径与上游验证范围

2026-10-04 用户决定：沿用上游已采用配方，以下20项均为 `excluded`，不再作为优化待办。
核对固定0.40的 `src/native_hip_network.h`、`src/native_hip_env_options.h`、
`scripts/hip-game-flags.txt`、`Development/HIP/hip_reference_network.h` 及当前模块配方。
这些实验开关未被生产配方采用；不能把 Options 的初始值当作最终部署值。
尤其 `DLSS5_HIP_VIT_BYTE_STREAM=0` 与当前 `DLSS5_HIP_VIT_STREAM=3` 是不同接口。

下表除内核宏和最后一行外，名称均省略 `DLSS5_HIP_` 前缀。
“有测试脚本”只表示存在实验入口，不证明其数值通过、性能变慢或测遍了所有架构。
不接入的依据可以是旧路径已被替代、当前配方不兼容或上游未采用；不必编造性能结论。

| 项目 | 不接入依据 / 已知上游验证 |
|---|---|
| FFN_QKV_BN | 默认关闭，与现用 MH byte stream 冲突；有 test-ffn-qkv-bn.ps1，未据此声称整网收益。 |
| GATHER_FOLD | gap-fusion-20260930：19组逐位一致，900p三轮全慢，作者明确不收、宿主门控0。 |
| MH_ATTN_W16 | 旧非字节C64投影路径，当前字节管线不选；test-c64-w16.ps1不是已采用的C256 W2_FFN_W16。 |
| MH_FFN_FRAG | 旧C64/C128 fragment路径，现用wave-owned布局；有test-ffnfrag.ps1，不能从已采用FRAG256推断本项启用。 |
| MH_WINDOW_FUSED | 旧整窗融合需额外c64-window-fused模块及非字节路径；closed-v040报告提到旧融合未获收益，不能套到新的wave结构。 |
| POOL_PROJECT_FUSED | 旧Down替代派发，要求token数整除16；有test-pool-project/test-poolf脚本，最终配方未选。 |
| QKV_WAVE_C512 | 与现用c512_qkv_frag互斥；有test-c512-qkv.ps1，保留已采用fragment/M32路径。 |
| SPARSE_WEIGHTS | Options内明确DO NOT SHIP：多chunk VMM触发驱动挂起/失败，三组哈希改变。 |
| SPLIT_MIX_FUSED | 与现用split_mix_h16w及c512_proj_tiles冲突；gap-fusion报告称既有融合为null/负账。 |
| TILED_FFN_SMALL | 将C64/C128纳入旧tiled替代路径；生产门槛256，未采用降至64。 |
| VIT_BYTE_STREAM | 旧布尔接口关闭，不能与现用VIT_STREAM=3混同，也不因此禁用已采用流式优化。 |
| VIT_EXPAND_M2 | 旧fragment替代expand，和M4互斥；有test-vit-m2.ps1，当前配方未选。 |
| VIT_EXPAND_M4 | 旧替代expand需特定packed mask/layout；有test-vit-expand-m4.ps1，当前配方未选。 |
| VIT_FFN_FUSED | 与当前stream及多个输入/expand路径不兼容，默认关闭。 |
| VIT_HALF_STREAM | 依赖旧byte-stream布尔路径；现用VIT_STREAM bit1已有独立half契约。 |
| VIT_INPUT_TILED | 旧输入布局依赖fragment并排斥M2/M4/旧byte；不能等同现用packed输入。 |
| VIT_QKV_N4 | 依赖旧half/byte stream，当前mask3不使用。 |
| VIT_SPLIT_K | HIP README的ViT16x64节：窄版和16x64版均保持输出但更慢，故关闭；不是HLSL的DLSS5_VIT_SPLIT_K。 |
| HIP_VIT_GATHER_FOLD（内核宏） | 与GATHER_FOLD同一候选的模块侧记录；producer/consumer及host门控均0，沿用同一拒收报告。 |
| DLSS5_VIT_ADAPTIVE_IDLE_MS | 排除额外Config/菜单绑定。生产500 ms空闲重置及外部诊断覆盖已存在，继续保留。 |

数值差异检查原来写的是“将来若产品采用该候选，应进行的本地验证”，并不表示上游没检查。
已排除候选不再要求产品侧补测；有上游结论就引用，没有结论就记录未采用及具体依赖。
已采用的LLVM23/RowOpts、当前stream及产品桥接仍由本地已有回归证明其兼容性。
执行与后续沿用规则见 [决策记录](decisions.md#2026-10-04--lmxxf-剩余20项不再作为接入待办)。

## 已取得验证与边界

2026-10-04，RX9070XT/gfx1201：COMGR 68模块构建、runtime MSVC构建；旧配置
SKIP=42,43,46/FAST=0/FREE=0/MULTI=1 的完整既有GPU回归通过，黄金值
`fe40c904da05472e` / `79233836b6257864` / `8ba14ef2db0dddfe` 保持。
新增 `--040-controls` 覆盖 live fast、1/2/3遍、首遍/后续跳块、自由分辨率切换：
每次只重建一次、重复结果稳定、恢复默认逐位一致。默认1080哈希 `8ab18e93c98441d7`；
自由1440哈希 `0ee1753213d7b7f8`。默认配置两遍的录制/重放与计时专项通过。
RuntimeConfigTests 4项通过（层合并、BOM/空值/重复、宿主优先）；SourcePatchTests 2项通过。

扩展GPU完整回归已通过，含1280x720、1707x961、2560x1440、3440x1440及两遍录制/计时。
最终完整CI及打包状态单独记录在发布交接，不能由上述开发验证替代。
gfx1200真实硬件、游戏画面/帧率、完整安装后的用户验收未完成；由本地试包继续验证。

LLVM23/RowOpts 增量验证：10个 LLVM 模块与其余58个 COMGR 模块完整构建；
逐个比较 ELF `.text`：仅10个 LLVM 与2个 deep/max-ilp 模块的指令变化，
其余56个指令段与旧包一致；重建后的完整文件哈希包含生成源码换行等元数据差异。
LLVM 模块的上游屏障扫描启发式检查均为0命中（C32每个24核，C64每个116核），
该静态检查不替代 GPU 验证。RX9070XT 的完整 `tests/lmxxf/run.cmd gpu` 通过，
含上述旧黄金值、0.40 控制、四个自由尺寸、故障注入、录制重放、adaptive 重置及两遍计时。
COMGR/LLVM 默认1080、自由1440及控制切换哈希一致；未执行新的后端性能基准或游戏 ABBA，
不报告本地提速百分比。按 [测量纪律](measurement.md)，后端性能仅引用上游。
完整 CI 和最终 Runtime 身份记录在本次验证输出，不据此声称已发布或游戏验收。
最终 `tests/run-all.cmd --tier ci,device --out exports/llvm23-ci` 全部通过，
包含65项同步测试和新增来源/混合编译器专项；未跳过 sync。
MSVC Runtime SHA256（与完整CI凭证及最终GPU控制测试一致）：
`7738dfda5c1c9c2b8c503ae560169c376660a711a1112a6fb18fab960bb742b5`。
完整GPU套件验证的是同源码、同MSVC配置的前一构建；最终构建另验证全部0.40控制。
日志为 `exports/llvm23/{gpu,final-runtime-controls,ci,audit-final}.log`，
凭证为 `exports/llvm23-ci/runtime-ci.sha256`。本次未制作或发布安装包。

## 累计审查后的本地接入补审

2026-10-04 从鬼武者状态恢复修复起补审宿主与三后端的累计改动。
本次 dlssnr 接入指纹变化仅来自 Mochizuki Session 析构：已收集全部录制租约后，
移除对游戏整条队列的额外排空，保留 builder join、私有 Vulkan 完成与设备丢失隔离。
所有权依据及复现入口见 [Mochizuki 生命周期说明](mochizuki.md#session-destruction-and-queue-ownership)。
旧代码在队列阻塞复现中失败；修复后 destroy-tail 与完整 Mochizuki GPU 回归通过，
含14次重放、双队列、resize、延迟收集、构建取消、历史顺序、格式与DRS。
lmxxf Runtime、桥接、配置和68个模块未因该修正改变，沿用以上0.40功能证据；
当时其余逐项分类、32项暂缓及验收条件保持不变；后续 LLVM23/RowOpts 接入连同
C64 屏障宏关闭其中9项，当时余23项；随后用户确认INPUT_POLL、IO_FUSE、C512_PROJ_FB8
已决定不接入，再关闭3项，当时余20项；随后用户决定不采用其余实验/旧路径及
额外idle菜单绑定，将20项全部排除，当前暂缓0项。逐项分类与指纹保留，已关闭的验收待办清空。
最终CI与产物身份仍由验证记录。


## 2026-10-04：有效输入区域补修

《33号远征》的自由分辨率失败来自网络按有效区域建网，而 NativeGameCodec::Create 按 Color 资源分配尺寸生成输入几何。分配大于有效区域时，合法输入被报为 input exceeds the free network surface，随后会话进入失败状态。原游戏日志没有记录分配尺寸，因此不把回归用例的 3840×2160 当作该次捕获的实际分配。

产品 runtime 现在显式向 encode/decode 传递有效 Color 宽高；codec 保持输出分配尺寸及其行距，decode 只对有效区域做网络映射，区域外保留原图。创建时仍检查有效区域不超过分配，记录资源所有权和重绑定规则沿用原实现。补丁为 tools/lmxxf-sync/patches/codec-active-subrect.patch，已注册到 manifest 的顺序补丁链。

tests/lmxxf/run.cmd gpu 新增自由分辨率 subrect，以及 3840×2160 分配 / 2259×1271 有效区域回归；固定分辨率原 subrect 用例同样增加区域外逐字节比较。连续同区域帧必须不增加 recreates。编译与 GPU/完整 CI 的最终结果在本次产物记录中分别列出，不能用编译成功代替实机验收。上游 pin、68 个 HIP 模块和所有既有实验排除决定未变，不需要重编 WSL 模块。
