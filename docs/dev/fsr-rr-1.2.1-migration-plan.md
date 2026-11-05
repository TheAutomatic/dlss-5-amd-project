# FSR Ray Regeneration 1.2.1 迁移与后续验证计划

## 目标与执行顺序

**首要任务是接入 RR 1.2.1。** 运动、反射、游戏和性能对照在迁移验收后统一基于新版开展，不再为 1.2.0 补一轮完整质量测试。升级与画质优化分开提交、分开归因。

本文是待执行计划，不表示各项已经完成。测试复用入口见 [tests/fsr-rr/README.md](../../tests/fsr-rr/README.md)，测量约束见 [measurement.md](../measurement.md)，打包以 [release.md](../release.md) 为准。既有性能调查见 [fsr-rr-offline-performance-plan.md](fsr-rr-offline-performance-plan.md)；本文优先控制升级顺序，不宣称该旧计划全部完成。

## 已知起点

- 当前接入 RR 1.2.0，本地兼容头 `OptiScaler/include/fsr-rr/ffx_denoiser.h` 声明 patch=0；`FfxApi_Proxy.h` 的实际 provider 门禁精确接受 `feature_version { 1, 2, 0 }`（上游参考 `burak113/OptiScaler` 同样写死精确匹配 `{ 1, 2, 0 }`，直接放入 1.2.1 DLL 会被判为 `Unsupported` 拒绝加载）。实施时重新确认最新代码，不能依据旧行号修改。
- 已有有限静态漫反射原生输入／转换双路径；合成输入并非真实低采样 RT。原生输入是独立生成的 demodulated Lambertian 信号，不是转换器输出改名。
- 已接入 OpenRR（`OpenNrRrFeature_Dx12`／`OpenNrRrFeature_Vk` + `OpenNrRrRuntime.dll` + `nvngx_dlssd.dll`），直接消费游戏 NGX DLSS-D 输入表（`OnrRrFrame`）在 AMD GPU 上运行 DLSS-D 降噪与超分网络（支持 `D3D12` 同列表录制与 `Vulkan` 跨设备后端）。
- 已有配置／相机／信号契约、真实宿主 NGX smoke、跨列表历史与 replay 生命周期回归。复用这些检查，不重建第二套框架。
- 用户反馈“移动时噪点多，停下恢复”为真实观察；尚未定位是原生时域收敛还是桥接额外损失历史。
- 已知 Floor 暗部增亮／局部细节取舍、重建边界无新输出、轻微历史暗化等，不因升级自动关闭或宣告修复。

## 官方变更与待核实事项

官方 [SDK 2.4.0 更新说明](https://gpuopen.com/manuals/fsr_sdk/whats-new/)列出 RR 1.2.1：独立 AO 与 specular occlusion 降噪、显存查询修正，以及 `NON_ENCODED_ALBEDO` 作为 `NON_GAMMA_ALBEDO` 的别名，旧名弃用。页面可能继续更新，研究时记录实际发布 tag、commit、资产和获取时间。

这些说明没有承诺修复本项目的运动噪点。开源材料便于核查，但必须确认 **1.2.1 实际公开源码范围**、头文件与签名二进制对应关系；不能将公开接口／sample 与全部后端实现等同。源码未公开部分按二进制契约验证，不进行受许可禁止的逆向。

## 范围边界

本次迁移包含：官方来源核验、必要源码／头文件、版本识别、创建／dispatch／配置／查询接口、依赖与打包、文档和最小验证。

不包含：同时升级 SR／FG／radiance cache、开启独立 AO／specular occlusion 产品功能、改 Floor／recovery 默认、重做菜单、重构通用提交系统、修 Godot DX12、搭建新的采集平台。若新版必须配套更新 loader 或其他组件，先给出依赖证据和受影响范围；不能静默升级整套 SDK。

## 开发对照策略：三路三角定位（纯转接优先 + OpenRR 同源参考）

开发阶段在原有 **官方原生 FSR-RR 输入路径 vs 测试专用纯转接 FSR-RR 路径** 的基础上，引入 **OpenRR（DLSS-D）路径** 构成三路三角定位（Triangulation），不借用 Fast 产品预设，不将 Balanced／Quality 混入基础验收。三档产品预设及用户当前配置不变；Balanced／Quality 的画质、调参和成本评估集中到后续独立任务，不作为本轮迁移或基础对照的前置。

| 对照路径 | 输入契约 | 核心管线 | 诊断定位作用 |
|---|---|---|---|
| **1. FSR-RR 原生路径** | AMD 原生拆分信号（独立生成的解调 diffuse／specular、原生线性深度与材质） | `FFX Denoiser (1.2.1) -> 线性重调制 -> FFX SR (FSR 4)` | **AMD 算法上限参考**：排除转接拆分误差后，观察 AMD RR + FSR 4 自身的时域收敛与反射质量 |
| **2. FSR-RR 纯转接路径** | NGX DLSS-D 输入（合成 noisy `Color` + albedo／法线／粗糙度／深度／MV／hit distance） | `InputConv (无 Floor) -> FFX Denoiser (1.2.1) -> OutputComp (无 Recovery) -> FFX SR (FSR 4)` | **桥接损耗评估对象**：与路径 1 对比量化“DLSS-D 混合 `Color` 解调拆分”引入的误差；与路径 3 对比同输入下的方案差异 |
| **3. OpenRR（DLSS-D）路径** | **与路径 2 完全相同的 NGX DLSS-D 输入**（`OnrRrFrame`） | `OpenNrRrRuntime.dll`（`nvngx_dlssd.dll` 权重，端到端降噪+超分） | **同源 DLSS-D 契约参考**：验证同一份 NGX 输入在原生目标模型（DLSS-D 网络）下的上限，隔离游戏输入缺陷与转接缺陷 |

### 三角归因准则

1. **隔离桥接损耗（路径 1 vs 路径 2）**：若路径 1（FSR-RR 原生）正常而路径 2（FSR-RR 纯转接）退化，问题在 DLSS-D $\to$ FSR-RR 的信号拆分、albedo 解调／重调制或深度／法线转换。
2. **隔离游戏输入缺陷 vs 算法代差（路径 2 vs 路径 3）**：
   - 若路径 3（OpenRR）与路径 2（FSR-RR 纯转接）**同时退化**（如错位、鬼影、重置失效），优先排查共享的 NGX 输入契约（`hwDepth` 判定、`roughnessPacked`、MV 缩放与 jitter、相机矩阵、subrect 偏移），不盲目修改 FSR-RR shader。
   - 若路径 3（OpenRR）显著优于路径 1 与路径 2，说明差异来自 **DLSS-D 端到端时域网络与 AMD FSR-RR + FSR 4 分离式架构的算法边界**，而非转接 bug。
3. **纯转接纯净性约束**：
   - 纯转接仅保留满足官方契约必需的深度、MV、法线／roughness 格式与坐标转换、必要 albedo 解调／重调制，以及资源状态、同步和生命周期管理。
   - 明确关闭 Floor 预滤波、flat/diffuse/specular recovery、非必要 albedo 修复、启发式光照拆分、NR、额外锐化／调色；不能只设置 `Profile=Fast` 就假定这些都已关闭。测试启动校验并记录实际生效项。
   - 路径 1 与路径 2 固定相同 1.2.1 provider、实际查询的官方默认参数、输入采样、曝光、相机、reset 规则与独立历史。官方默认值以目标 DLL 查询结果为准，不把 1.2.0 的数值硬写成 1.2.1 标准。
4. **OpenRR 对照约束**：
   - **排除构建期直通帧**：OpenRR 在 `ONR_RR_STATE_BUILDING`（首次提取 `nvngx_dlssd.dll` 权重或编译管线）期间会直接输出未降噪的放大画面（`scaled colour`）。所有画质与性能采样必须校验 `OnrRrInfo.state == ONR_RR_STATE_READY` 并完成独立历史预热，严禁把构建期直通帧计入指标。
   - **比较阶段对齐**：OpenRR 是端到端降噪+超分网络（直接生成 `output_width x output_height`），而 FSR-RR 是“渲染分辨率降噪合成（`presr`）+ FSR 4 超分（`sr`）”。三路主指标统一在 **最终输出分辨率（`native_sr` vs `bridged_sr` vs `openrr_out`）** 对齐比较；OpenRR ABI 2/4 导出的渲染分辨率 `OnrRrDenoisedImage` 仅作内部特征诊断参考，不直接当作与 `presr` 同量纲的物理线性 radiance 排名。
   - **可选 Guide 对齐**：离线基础对照（E1/E2）中，OpenRR 先限制在与 FSR-RR 纯转接完全一致的公共输入集（关闭 `disocclusion_mask`、`sss_guide`、`responsivity_mask`、`color_before_transparency`、`depth_of_field_guide`、`specular_motion` 等额外开关），确保严格同输入；随后再单独开启额外 guide 评估 OpenRR 的完整能力。

## 阶段 A：固定来源、差分与迁移清单

1. 使用 AMD 官方发布仓库／release 资产，记录旧与新 tag、commit、子模块 pin、文件 SHA256、版本、许可及再分发要求。不要覆盖历史包和第三方输入。
2. 清点实际使用闭包：本地兼容头、SDK denoiser 头、共享 API／DX12 backend 描述符、sample、loader、signed denoiser DLL、构建与打包路径、provider 选择和所有版本提示／测试。
3. 对比结构大小、对齐、字段顺序、effect/type ID、enum/flag 数值、版本协商、可用 query/configure keys、返回码和信号／guide语义。旧 flag 别名是否同值必须以头文件证实。
4. 逐项分类：必要兼容改动、行为改变、新功能暂不启用、无关更新。输出精简差分清单，标明源码证据和验证方式。

**完成条件：**来源可追溯，必要依赖集合明确，无未解释 ABI 差异。若 loader 配套关系或接口信息不明，继续查官方材料，不凭 patch 版本推断兼容。

## 阶段 B：实现最小接入

1. 更新实际构建使用的头文件／vendor pin，避免本地 shim 与 SDK 两套声明漂移。通过已有 ABI 检查核对布局。
2. 将生产与离线链调用更新至新官方 flag 名称；遍历全部相关调用，确认不是仅改显示版本。
3. 更新 provider 识别、支持门禁、查询与 context 创建版本。明确允许的版本集合，不以 `major==1` 接受所有未知未来版本，也不伪装 1.2.0 为 1.2.1。
4. 当前完整包以 1.2.1 为目标，不新增旧 ABI 兼容 shim／静默回退；如官方 DLL 实际同时提供多个 provider，明确选择与日志，不只取列表首项猜测。
5. 按官方差分处理显存查询接口，保持不支持／失败与真实零值可区分；不为新增功能编造输入。
6. 更新打包器的 DLL 来源、loader 必需配套、版本提示、许可和哈希检查；用户必须更新完整包，不能建议仅替换某一个 DLL。
7. 更新测试中的显式 provider pin 和版本期望，保留旧 manifest 原件，不改标签冒充新版结果。

**完成条件：**生产宿主和测试工具实际使用一致的 1.2.1 契约；未改动画质默认、用户 INI 优先级或真实信号选择。

## 阶段 C：最小迁移验收（本次升级门禁）

由一个 owner 串行调度 GPU；前后检查游戏，碰到游戏只停止自己的任务。不得以升级为由展开完整场景矩阵。

|检查|执行内容|接受条件|
|---|---|---|
|契约与构建|现有相关 ABI／相机／信号契约；shader mirror 与受影响 shader；Release host|实际结构与新接口一致，必要变体同步，构建成功|
|provider 身份|核对实际加载路径、API/provider版本与SHA256|确为目标1.2.1及所核准配套，无旧路径混用|
|生产 NGX smoke|沿用 absent→arrival→dropout→return，检查创建、dispatch、新鲜输出、释放；同时验证 OpenRR ↔ FSR-RR 1.2.1 后端切换与回退（`OpenNrRrFeatureDx12::FallBack`）|稳定阶段输出有效，切换与回退不卡死、无新增错误/设备移除；已知重建边界单列，不能用PASS掩盖|
|有限静态对照|复用 bounded flat/detail，明确使用官方原生与测试专用纯转接策略、同一采样、独立历史|实际guides与尺寸合法，无miss/越界/far bypass；附加处理关闭且有生效记录；输出有限；检查参考图、能量与历史趋势，无未解释重大差异|
|查询与包完整性|验证受影响显存query、依赖新鲜度及本地完整包|结果/失败如实上报，包内组件及许可齐全|

1.2.0 历史数据用于追溯，不作为与 1.2.1 同 provider 的身份相同对照。不为了通过改变阈值、反复 reset 历史或只挑稳定帧。若输出异常先验证输入契约与环境，不立刻调整产品算法。

**门禁失败：**保留失败结果和原因，不提交伪装支持或发布该包。修正迁移／测试缺陷后只重跑受影响项。若是产品既有问题，明确区分新增与存量，不能自动归咎新SDK。

## 阶段 D：固定新版基线

保存源码commit（含未提交状态说明）、DLL路径/版本/哈希、驱动/GPU/LUID、构建工具链、manifest、场景版本、随机种子、配置、输入与实际guide绑定身份、reset和历史策略。涉及 OpenRR 对照的记录须同时固化 `OpenNrRrRuntime.dll`（版本、ABI、SHA256）、权重来源 `nvngx_dlssd.dll`（路径、版本、SHA256）、实际执行后端（`D3D12` vs `Vulkan`）及 `ONR_RR_STATE_READY` 预热帧数。迁移与后续算法改动不得混为一个A/B变量。

记录哪些检查实际执行、哪些未执行，以及已知存量问题。普通旧／新compare与原生双路径使用各自正确格式；不得补写metadata让历史CSV符合新口径。

**完成条件：**可以从受git管理的测试代码与明确依赖重跑1.2.1基线，无对被删除工作树或work临时脚本的运行依赖。至此迁移任务结束，下面项目分开实施。

## 阶段 E：新版上的后续质量任务

### E1：运动与停止收敛（最高后续优先级：三路对照）

- 固定有限场景及匹配信号，静止预热→受控相机平移→停止；先不混入反射或动态物体。
- **三路并行对照**：
  1. **FSR-RR 原生**（拆分信号 + 1.2.1 官方默认参数 + FSR 4）；
  2. **FSR-RR 纯转接**（NGX DLSS-D 输入 + 1.2.1 官方默认参数 + FSR 4）；
  3. **OpenRR（DLSS-D）**（与纯转接完全相同的 NGX DLSS-D 输入 + `ONR_RR_STATE_READY` 稳态推理）。
  三路使用相同噪声样本、曝光与相机，各自独立历史；记录实际 MV XYZ、深度、jitter、reset、历史使用和提交序列，并校验纯转接未混入产品预设、OpenRR 未混入额外可选 guide。
- 区分持续可见区域与新显露区域；对有效对应做运动补偿残差，单独报告显露恢复和停止收敛，不能将图像空间 MAD 叫作表面闪烁。
- 输出短序列与逐帧图，接受用户运动噪点观察，不要求重跑来确认观察。按三角归因判断：
  - 若 **FSR-RR 原生 ≈ FSR-RR 纯转接**，但两者运动噪点均高于 **OpenRR**：根因在于 AMD FSR-RR 1.2.1 时域累积策略本身与 DLSS-D 网络的差异；
  - 若 **FSR-RR 原生** 与 **OpenRR** 均稳定，唯独 **FSR-RR 纯转接** 运动噪点退化：根因在于桥接转换（解调／重调制或深度／MV 转换），此时再做桥接消融；
  - 宿主 Fork 调参、`FloorRecovery` 及 Balanced／Quality 统一留待后续附加策略评估，不加入本阶段基础矩阵。

### E2：反射与粗糙度（三路对照）

- 单独扩展语义真实的 specular 信号、反射率与物理 `specular_hit_distance`，从粗糙到近镜面、明亮反射目标及反射边界逐步覆盖。
- 若解析 renderer 不能提供匹配的原生／DLSS 形态输入，先实现并验证该契约，不用最终 RGB 猜出原生信号，不伪造距离。
- 在 **FSR-RR 原生 / FSR-RR 纯转接 / OpenRR（DLSS-D）** 三路间比较噪声、偏差、细节及边缘／历史稳定性，不把更平滑直接判为更准确。注意披露输入契约差异：OpenRR 直接消费 `specular_hit_distance`（及可选 `specular_motion`），不读取组合的 `specular_ray_direction_hit_distance`。

### E3：短游戏端到端对照（DLSS-D 同源双路 + 游戏原生三路）

- **第一层（任意支持 DLSS-D 的游戏，严格同源输入）**：固定分辨率、质量、光追设置、曝光、站位及短路线，在同一套游戏 NGX DLSS-D 输入下直接对比 **FSR-RR 转接路径 vs OpenRR（DLSS-D）路径**。
  - 利用 `FSRDFeature_Dx12` 与 `OpenNrRrFeature_Dx12` 的 NGX 输入探针日志，核对两路看到的纹理／矩阵／标志位完全一致；
  - 分别记录 OpenRR 在仅启用基础 guide 与启用 DLSS-D 扩展 guide（`disocclusion_mask`、`sss_guide`、`responsivity_mask`、`color_before_transparency`、`depth_of_field_guide`、`specular_motion`、`transparency_layer`）时的表现差异，明确哪些画质优势来自扩展 guide、哪些来自模型本身。
- **第二层（同时具备游戏原生 FSR-RR 路径的游戏）**：加入 **游戏原生 FSR-RR** 构成游戏内三方对照。保存实际配置／模块身份和完整轮转日志。游戏原生输入差异需披露，不能由端到端差异直接推导某个 shader 有 bug。真实 RT 连续原始输入若可取得，保存完整 guides／camera／history 来源；取得前不宣称有可用数据集。

### E4：同场景与游戏性能归因（含 OpenRR）

- 在前述场景稳定后，同条件对比 **FSR-RR（转换 + RR 1.2.1 + 合成 + SR 深度编码 + FSR 4 SR）** 与 **OpenRR（DLSS-D）** 的分段及端到端开销：
  - **OpenRR `D3D12` 后端（ABI 4 `Record`）**：测量同队列 D3D12 录制与 GPU 执行时间；
  - **OpenRR `Vulkan` 后端（ABI 1–3 `RecordInputs` + `Enqueue` + `RecordOutput`）**：区分 D3D12 输入准备、CommandList split + 跨队列共享 fence 同步等待、Vulkan 网络推理与后处理（`GetHopTiming` 的 `net`／`post` 及 `gpu_ms_last`）与 D3D12 输出写回开销。
- 正式计时关闭重诊断，确认 OpenRR 已处于 `ONR_RR_STATE_READY` 且在途模式真的在途、upload/readback 生命周期正确；预热、A/A 噪声带、重复轮次、GPU 提交完成归属按既有性能计划执行。小分辨率串行 debug 时间戳不得转写成游戏 FPS 收益。

## 交付、记录与停止条件

- 可复用代码和流程进git；新鲜构建输出放exports，本地完整测试包放dist，调查与用户日志按workspace规则放work。历史输入/结果不覆盖，不将私有采集或权重入包。
- 迁移提交、迁移验收记录、后续质量结论分开。未经用户明确授权不push、不tag、不发布、不替换游戏文件。
- 下版先完成A–D；E1–E4按独立任务推进，不要求一次全部完成，不以Godot修复为前置。
- “迁移完成”只指目标版本契约与最小链路验收；“画质改善”“原生接近”“发布就绪”分别需要对应证据。

## 执行清单

- [x] A：固定官方1.2.1来源与实际公开范围，完成闭包差分清单（见 `third_party/fsr-rr-1.2.1/README.md` 及 `work/notes/2026-11-06-fsr-rr-1.2.1-migration-and-3way-comparison.md`）。
- [x] B：更新ABI/头文件/flag/provider门禁及必要打包依赖。
- [x] C：完成最小迁移验收，记录异常与存量边界。
- [x] D：固定可复用1.2.1基线，迁移独立提交（`747a3598a`）。
- [x] 固化测试专用纯转接策略与 OpenRR 同源对照约束（`ONR_RR_STATE_READY` 预热、公共 guide 对齐、最终输出分辨率对齐），不依赖 Fast 预设，不改变产品默认。
- [x] E1：新版运动／停止收敛三路对照（FSR-RR 原生 vs FSR-RR 纯转接 vs OpenRR）。
- [x] E2：新版反射／粗糙度三路对照（FSR-RR 原生 vs FSR-RR 纯转接 vs OpenRR）。
- [ ] E3：短游戏端到端对照（DLSS-D 同源下 FSR-RR 转接 vs OpenRR + 可选游戏原生 FSR-RR）及真实输入来源记录。
- [x] E4：同场景受控性能归因（FSR-RR 全链 vs OpenRR `D3D12`／`Vulkan` 后端）。
- [ ] 后续独立任务：在纯转接基线稳定后统一评估Balanced／Quality与Fork调参，不混入基础对照。
