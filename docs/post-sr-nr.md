# SR 后运行 NR（实验）

Ins → Neural Rendering → **Processing order** 可选择：

- **NR → SR（默认）**：NR 处理渲染分辨率输入，SR 随后放大并积累结果。
- **SR → NR（实验）**：SR 先完成超分，NR 处理超分输出，结果写回原输出纹理。

Daniel、lmxxf、Mochizuki 使用同一顺序开关，不必分别安装三个适配版本。
保存设置后写入 `OptiScaler.ini` 的 `[DlssNr] RunBeforeSR`：`true` 为默认顺序，
`false` 为实验顺序。安装器的整包覆盖升级仍保留用户已保存的选项。
切换顺序会清空 NR 历史；若本次启动未启用所需 command-list hooks，状态会提示保存后重启。

## 分辨率与开销

模型输入尺寸相对于所选阶段：例如游戏以 1280×720 渲染、SR 输出 2560×1440，
100% 模型尺寸在默认顺序使用 720p，在实验顺序使用 1440p。后者可能明显增加时间和显存。
Daniel/Mochizuki 可降低 Model resolution；lmxxf 可关闭 Native input resolution，
改选已有网络档位。切换顺序不会擅自修改这些设置。

SR 后仍可使用每个后端原有的强度、passes 和输出控制。整体强度为 0 时保留原始 SR 图像，
但网络仍会运行。SR 后的效果不再经过 SR 的时间积累；清晰度、稳定性和性能须在游戏中比较。
这不会给游戏新增 SR、光追或 Ray Reconstruction，也不保证 NR 处理的位置一定在 HUD 之前。

## 多层 pass

三个后端在两种处理顺序下均保留 **1～3 层，默认 1 层**。选择 SR → NR 后，
三层的顺序是 `SR → NR 第 1 层 → NR 第 2 层 → NR 第 3 层 → 整体输出控制 → 写回`。
lmxxf 0.41 默认用局部预测生成第三层，因此实际执行两次网络；关闭 Predict third pass (lossy)
后才执行三次真实网络。Daniel、Mochizuki 的三层仍执行三次网络。
SR 只执行一次，后续 NR 层使用上一层的颜色结果；切换顺序不会重置已保存的层数。

| 后端 | Ins 的层数选项 | `[DlssNr]` 中的 INI 键 |
|---|---|---|
| Daniel | AMD neural passes | `Passes=1/2/3` |
| lmxxf | Network passes | `DLSS5_MULTI_PASS=1/2/3` |
| Mochizuki | Model passes → Passes | `MochizukiPasses=1/2/3` |

层数由各后端分别保存。Mochizuki 的 `MochizukiMaxPasses` 是预建容量，不是实际层数；
自动容量在降层时保留，以便再次升层。升层构建期间可暂用已有层数，以状态中的实际层数为准。
lmxxf 多层会关闭 Adaptive ViT reuse；第二、三层跳块仍由原有选项控制。
显示分辨率配合多层会进一步增加计算和显存开销，可以先测试两层，再决定是否使用三层。

## 范围和失败提示

支持现有 DX12 超分入口，以及 DX11/Vulkan → DX12 桥接入口。原生 Ray Reconstruction
和原生 Vulkan 入口未扩展。只在成功的 SR 调用后执行，帧生成不再额外执行 NR。

颜色取 SR Output；深度和运动矢量仍来自同一次 SR 的输入。适配层按有效区域采样辅助纹理，
用渲染像素单位的 jitter 对齐到输出网格，并换算运动矢量尺度。带 jitter 的运动矢量缺少可靠的
历史换算契约时使用空间 NR，避免沿用错误的时间历史。不是重新生成了显示分辨率的真实深度。

首版支持 FP16 RGBA、FP32 RGBA、R11G11B10_FLOAT、R10G10B10A2_UNORM、RGBA8_UNORM
输出（包括对应 typeless 分配），保留原 alpha 和有效区域以外的像素。输出本身无需 UAV 标志。
guide 要求可采样、单样本的 2D 纹理；非零子区域原点、其他输出格式、不可采样的 depth 等
暂不接入。此时保留 SR 图像，Ins 状态显示原因，不偷偷改回 NR → SR。

中间纹理按录制代数持有，直到录制丢弃且所有提交完成；关闭的列表重放也保留依赖，跨队列
重放先等待其上次执行。适配层最多保留 16 组、768 MiB 纹理；占满时跳过本帧并提示。
这是适配层额外预算，不包括后端本身的网络/模型显存。

## 验收

1. 安装本分支测试包，先保持 NR → SR 检查原有路径，然后改为 SR → NR 并保存。
2. 分别选择已装好模型的三个后端；确认状态和模型输出有变化，比较 NR 关闭、强度 0/1。
3. 检查运动边缘、细线、透明物和镜头切换；比较 100% 与较低模型分辨率的时间/显存。
4. 切换 SR 质量、分辨率和 NR 顺序，检查是否能恢复；保存后重启确认选择持久化。
5. 按游戏已有支持打开 FG，检查没有重复 NR、异常拖影；正常退出。
6. 每个后端在 SR → NR 下依次切换 1→2→3→1 层，检查实际层数、效果、耗时及恢复情况；
   再比较默认 NR → SR。Mochizuki 等待升层构建完成，lmxxf 等待网络重建。

自动化使用 `tests/shader/nr_post_sr.cpp` 验证辅助纹理映射、输出转换/alpha/区域保护，以及
丢弃、重放、跨队列、未完成时 Reset 和失败凭证。它接在 `tests/shader/run.cmd`，
默认 WARP；显式 `NR_EFFECTS_HARDWARE=1` 可在 AMD 设备执行。模拟网络结果验证的是
宿主适配层，不等同三个后端的真实游戏验收。
同一测试还用逐层翻倍的 GPU 颜色变换检查 1→2→3→1 层的最后结果、原 alpha/区域保护，
覆盖直接录制、producer/continuation 拆分和关闭列表重放。

## 本地接入审阅（2026-10-05）

仅修改产品消费链，lmxxf 0.40 pin、C ABI v2、LLVM23/RowOpts 的 68 个模块、生产开关和
网络代码没有变化。`AmdBridge::Evaluate` 选择 Color 或 Output，SR 后由 `PostSr::Prepare`
映射辅助纹理，再进入原有三个 `Host::Record`；`PostSr::Finish` 在同一次录制内写回 Output。
`SrPlacement` 固定一次 SR 调用前后的选择，原有调用方只在 SR 成功且非 FG 时进入后置路径。

审查涵盖格式/有效区域、运动矢量单位、FSR jitter 坐标约定、显式 INI 优先级、失败保留原图、
阶段/辅助尺寸变化的历史清空，以及拆分列表、重放、跨队列和未完成时 Reset 的所有权。
适配层在 WARP 和 RX 9070 XT 上通过，含真实 proxy 的 producer/continuation 拆分。
记录来自测试的模拟 NR 输出；未将此结果写成三后端完整游戏通过，画质/性能仍待游戏验收。

多层复核：Daniel 在 `AmdPreSr::Record` 内逐层修改私有颜色，各层各有时间历史；
lmxxf 的 `MultiPassRest` 串联网络并复用同一帧的历史/噪声，层数改变会重建网络；
Mochizuki 的 `Runtime::record_all` 保存各层历史，并在层数变化或旧录制重放时使历史失效。
共享适配层位于整个后端调用之外，只准备一次辅助纹理和写回一次最终结果。

2026-10-05 的补充验证：适配层 1～3 层测试在 WARP、RX 9070 XT 上通过；
Mochizuki 的 `tests/mochizuki/run.cmd pass-switch normal` 真实模型用例通过
1～3 层切换、容量复用、独立冷启动结果比较及旧录制重放。
lmxxf 的 `lmxxf_nr_gpu --040-controls` 在 RX 9070 XT、1920×1080 上通过了
1→2→3→1 层及跳块切换：各层输出不同，相同设置重复输出一致，恢复一层后回到基线。
这些运行时用例与适配层测试分开执行；Daniel 的闭源网络和三个后端的完整游戏链仍需本地验收。

## lmxxf 0.41 补充

同一处理位置仍包含整个叠层链。请求 3 层时，默认 `DLSS5_MULTI_PASS_PREDICT=true` 为两次真实网络加局部预测；设为 false 才是真实三遍。肤色保护默认关闭，可在两层或三层时保留第一遍的肤色；采用颜色启发式，不是人物分割。准备输入、最终输出效果和写回 SR 结果各执行一次。上面的 0.40 验证为历史基线；0.41 结果见 [接入审阅](lmxxf-041-consumer-review.md)。
