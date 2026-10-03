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

## 数值模块与编译器（暂缓项）

0.40 每架构 34 模块，共 68。Fast numeric 选择 c32-wave1-fast/c64-wave2-fast，
对应 CW_FAST_NUM=3/W2_FAST_NUM=3；修改的是舍入及倒数路径，有损，不等于逐位算术。
关闭 fast 时，1920×1152/1088 才选 c32-wave1-rtz（HIP_C32_RTZ_ISA=2）；其他几何用普通 C32。
fast 优先于 rtz，不需要内容重复的 rtz-fast 文件。缺模块不冒充成功，完整包仍检查全部受控模块。

本轮实际模块由 COMGR 构建，未启用 RowOpts。保留两个 multihead 模块的产品
HIP_FFN_LINE_STORES=1；各模块实际 defines/source/hash 记录在 modules.json。
源码宏、配方与消费者的其余不变部分沿用 [0.39 审阅](lmxxf-039-consumer-review.md)，
本轮完整模块重建及旧配置黄金输出再次验证，不能仅因源码在模块中出现就宣称该核被派发。

**明确暂缓 LLVM23 与 RowOpts 编译优化**。已下载并核对官方 Windows LLVM 23.1.2
归档 SHA256 `ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418`；
`clang --print-targets` 无 AMDGPU，真实 HIP 编译失败；本机未安装 WSL。
这不妨碍 COMGR 执行 0.40 功能，但不能宣称接入上游 LLVM23 的提速。

下一步：在包含 AMDGPU 的 LLVM23.1.2 构建上运行上游
`Development/tools/llvm-fork/compile-modules.py --compiler-rows llvm23 --row-opts --target-feature=-real-true16`，
为两个架构生成五个 LLVM 行；C64 两行必须加 l23defines 中的 HIP_BARRIER_FENCE=1。
其余 COMGR 行按 `build-modules.ps1 -RowOpts -PrebuiltDir` 构建，尤其 c512-m32-deep 的 max-ilp。
保存编译器版本、完整命令、源码和产物哈希；生成清单必须包含实际 l23defines，不能沿用遗漏这些宏的上游预编拷贝清单。
验收：模块契约、源补丁、旧输出黄金值、新默认与叠层/自由几何 GPU 回归，再做相同负载的 ABBA，才能声称提速。

依据：`compiler-sweep-20261001`、`llvm23-vit-20261002`、`next-candidate-20261002` 的结果与生成脚本。
LLVM22/23 不再为 gfx12 裸拆分屏障自动补 LDS 等待；COMGR 本轮仍走既有 LLVM21 路径。
HIP_BARRIER_FENCE=1 对未来 LLVM C64 行是正确性前提，不能因当前热核未使用就删掉。
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
- 原有 INPUT_POLL、IO_FUSE、gather-fold、投影FB8等暂缓条目继续引用0.39逐项理由与验收条件。
  本次改动未使其完成；不能通过添加无消费者 INI 键启用。
- experiments 的运行/备份/看门狗脚本、results 原始日志/图片为作者测量材料。
  它们用于解释最终生产决策，不作为本产品构建依赖，也不逐份重跑作者游戏测量。
- 三种部署模板的主体是既有开关的中英注释展开；核对实际赋值、生成器和最终消费者，
  不把注释中的旧发布数字当作当前本地默认。上游部分 README 仍残留 fast 默认关闭的旧句，以最终模板为准。

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
