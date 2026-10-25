# 上游 (OptiScaler) 继承问题台账

记录随上游集成带入、已在下游修复或仍待处理的缺陷,供向上游提 issue 时取证据。每条含:上游位置(9df512d 快照)、下游修复提交或状态、复现证据。

## U1 FSR4 驱动签名 DLL 无条件补丁 → 内核反作弊击杀（提供显式 opt-out）

- 上游:`fsr4/FSR4DriverProvider.cpp:144-145` 加载 amdxcffx64.dll 后无条件 `FSR4Presets::Hook`(代码页 Detours)+ `FSR4Models::Hook`(provider 虚表改写)。
- 危害:崩坏:星穹铁道(HoYoKProtect 内核反作弊)超分首帧交接后 ~85ms 进程被外部终止(异常码 0x00000000、无栈、无 dump);1.13.1-alpha 同 DLL 不补丁存活 9.5 分钟。
- 当前修复（2026-10-25）：`[FSR] Fsr4DisableDriverHooks` 默认 false，驱动/SDK 的 preset/model 钩子照常安装，包括 Auto。true 才跳过两类钩子，菜单/日志告知自动模型选择支持检查、预设修正/跟踪及显式覆盖不可用；显式选择不绕过 opt-out。
- 旧 `e9767600`/`85df65da` 的 has_value 门会破坏 Auto 的 DetectedModel/CorrectedPreset 行为，现已替换。只读 RTTI 枚举及 `e4f05d79` 模块扫描缓存保留；false 时保留菜单延迟补挂。建议上游采用同一 opt-out，而非默认零补丁；该键不保证全面 AC 兼容。

## U2 XeLL 配对缺校验 → 旧 XeSS 栈游戏 XeFG 启动崩(已修;**需对上游复验,暂缓提交**)

- 上游:`XeFG_Dx12::CreateSwapchainContext` 把选中的 libxell 上下文直接喂给 XeFG,无版本校验,
  违反其自身注释 "XeFG and XeLL contexts must come from the same pair of libraries"。
- **需复验(2026-10-25)**:上游 8f0b7d875 已把 XeLL 选择改为优先 OptiScaler 捆绑的 libxell——
  旧描述"全进程引流到最先加载的 libxell(first one becomes the real XeLL)"的前提可能已不存在。
  须先在当前上游 master 复验"游戏目录旧 libxell 仍可胜出"是否成立,再决定是否提 issue;当前不提交。
- 危害(旧前提下的现场):巫师 3 + MFG unlocker(游戏目录旧 libxell 1.2.1)× Opti libxess_fg 1.3.1
  → XeFG 首次 present 崩溃;鸣潮(libxell 1.3.2)正常。
- 下游修复:`3182b248`——真实 libxell 不得旧于捆绑副本(`XeLLProxy::BundledVersion()` 读文件版本
  资源)才挂;不配对则 XeFG 无延迟加速运行。注:Intel 按 SDK 整套配对 fg/xell、版本号独立演化
  (官方 3.0.2 即 fg 1.3.4 + xell 1.3.2),下游校验以捆绑副本为基线,勿照搬"XeLL minor ≥ XeFG minor"。

## U3 Win10 Agility 自动升级误伤非 Agility 游戏(已修)

- 上游:`dllmain.cpp:1160-1168`——`CheckAgilitySDK` 对无 D3D12SDKVersion 导出的游戏(全部 DX11 游戏、未集成 Agility 的 DX12 游戏)返回 0,`0 < 615` 为真 → Win10 上自动开启 `FsrAgilitySDKUpgrade`。
- 危害:`RunAgilityUpgrade` 对 kernelbase `IsDeveloperModeEnabled` 做 Detours + `SetSDKVersion(615)` 强切 SDK:内核反作弊游戏直接杀进程;DX11 游戏被强拖 d3d12/Agility 初始化。仅 Win10(build<22000),Win11 不受影响。
- 下游修复:本提交——`gameAgilityVersion > 0 &&` 下限门禁。
- 建议:上游同款一行修复。

## U4 CustomOptional 带默认值时显式 false 存不住 → auto 回写(部分修复,同类键风险待上游处理)

- 上游:`Config.h` `value_for_config()`(WithDefault 分支)在"当前值==默认值"时返回 nullopt,SaveIni 把 nullopt 写成 `auto`;`FsrAgilitySDKUpgrade { false }`(上游 Config.h:484)。
- 危害:与 U3 组合成死循环——用户显式 `false` 保存后被回写 `auto`,下次启动 Win10 又强制 volatile true(volatile 值同样坍缩 nullopt,连 true 都存不下来),崩溃循环无法自救。
- 下游修复:本提交——`FsrAgilitySDKUpgrade` 改 NoDefault。
- 同类键审计结论(2026-10-22 复核,初版判断有误):自动调整类键均已使用 `SoftDefault`
  ("Change always gets saved",Config.h:15),显式值可持久化、quirk 翻转带 `!has_value()` 门且
  `set_volatile_value` 保留用户旧显式值——**无坑**:`DontUseNTShared { true }`、`DxgiSpoofing { true }`
  (FSR4-Int8 分支无视显式值是上游故意的运行时覆盖)、`CreateD3D12DeviceForLuma { false }`。
  `FsrNonLinearColorSpace` 是除 Agility 外唯一 WithDefault,但其自动置 true 是"PQ/SRGB 显式设置→推导",
  运行时本就覆盖显式值(设计如此),无用户可感知陷阱。
- 建议:上游为会被运行时自动调整的键一律使用 SoftDefault/NoDefault;`FsrAgilitySDKUpgrade` 是唯一的
  WithDefault 漏网(下游已改 NoDefault)。

## U5 XeFG passthrough 输入的 XeLL 配对(未修;**与 U2 同前提取决于上游复验,暂缓**)

- 上游:LL 构建下 `XeFGInputs::Hook` 把游戏旧 libxess_fg 的导出重定向到 Opti 副本,而游戏的
  XeLL 上下文按旧设计落在最先加载的旧 libxell;非 LL 构建是整对交换(正确),LL 构建不是。
- **需复验(2026-10-25)**:与 U2 共享同一过时前提——上游 8f0b7d875 已改为优先捆绑 libxell,
  "游戏 XeLL 落在旧 libxell" 可能不再成立。随 U2 一并复验,当前不提交。
- 状态:无现场报告(FGInput=XeFG+FGOutput=XeFG 组合);正确修法需 XeLL 输入在 passthrough
  重定向时改用 Opti 配对,涉及引流设计,留待与上游讨论。


## U6 DxgiSpoofing 无提示强制覆盖(仅剩 UX 问题;持久化上游已修)

**勘误(2026-10-25)**:上游 cf6da0010 已把 `DxgiSpoofing` 改为 `SoftDefault`——显式值持久化
问题(下游旧记录"用户显式值存不住"的担忧)上游已修复,不再作为 issue 内容。**剩余问题只有一处**:
FSR4-Int8 GPU 分支(dllmain ~1510)**无门强制开启**且**零提示**地覆盖用户显式 false——
Int8 跑 FFX 必须伪装是上游故意,但覆盖时没有任何日志,用户观感即"设置被吞"。

下游已在该处加日志("forces DxgiSpoofing on ... your explicit ini value is kept")。
建议上游:无门强制处补同款提示(纯 UX,低风险)。判别口径(供支持):ini 值为
auto=从未显式设置(门控自动调整,显式设一次即固定);ini 显式值仍在而行为相反=Int8
强制(看日志);两者皆非=新问题,收日志。

原 7 处改写点核验表(2026-10-22,门控结论仍有效,其余 6 处均为 `!has_value()` 门控的设计行为):
OptiPatcher 成功(dllmain:301/361,关)、Luma/Sekiro TSR(1349/1378,关)、游戏不用 DLSS
且伪装无服务对象(1783,关)、nvngx 替换缺失且无 nvngx.dll(1858,关,外层整块 `!has_value()`
+ 已有日志)、FSR4-Int8(1510,强制开,无门)——后两类合并为上述一处剩余问题。

## 上游同步审计(2026-10-23,9df512d → 990ff98,19 commits)

上游快照落后 19 个提交,已全量比对。当时的 5 个问题上游全部未修，另有 3 个新修复。
2026-10-25 勘误：U1/U1b 的 has_value 门与 Auto 策略确有冲突，已替换为统一 opt-out；
其余修复不变。下表保留提交溯源并标明当前方案。

### A. 我们已修、上游仍未修(提 issue 的候选,附下游证据)

| # | 问题 | 上游位置(990ff98) | 下游修复 |
|---|---|---|---|
| U1 | 驱动 DLL 无条件钩子 → AC 击杀 | fsr4/FSR4DriverProvider.cpp:181-182 | e9767600 的 has_value 门已替换为 Fsr4DisableDriverHooks opt-out |
| U1b | SDK DLL 无条件钩子(同 AC 面 + ZZZ 卡顿源) | proxies/FfxApi_Proxy.h:285-286,433-434 | 85df65da 的 has_value 门已替换为同一 opt-out，两处 SDK 加载路径均生效 |
| U2 | XeLL 配对无版本校验 → 旧栈崩 | framegen/xefg/XeFG_Dx12.cpp:110-112 | 3182b248(BundledVersion 比对) |
| U3 | Win10 Agility 0<615 陷阱 | dllmain.cpp:1160-1167(无 >0 下限) | 2138e127(gameAgilityVersion > 0 门) |
| U4 | FsrAgilitySDKUpgrade WithDefault{false} 显式值存不住 | Config.h:485 | 2138e127(改 NoDefault) |

注:上游 d8aebe4 "Improve auto model selection" 给 hkIsSupported 加了 DetectedModel() 回退,
**不是** ZZZ 卡顿修复——它仍每次调用 o_IsSupported、无缓存、钩子仍无条件安装;只改了
Auto 时的模型选择策略(INT8 检测到就当已选)。旧 has_value 门使该 Auto 分支不可达；现在默认安装钩子以保留它，只有显式 opt-out 才关闭。

### B. 上游新修复、我们缺(移植候选)

| # | 上游提交 | 内容 | 移植判断 |
|---|---|---|---|
| P1 | d06f31e 菜单 >8 缓冲区崩溃 | 数组 8→16(DXGI 上限),blur 描述符 20→36,拒绝超大链,越界帧跳过 | **已移植**(a7537612):NUM_BACK_BUFFERS 8→16 + MaxBackBuffers 8→16;下游越界守卫原已存在 |
| P2 | fa44a4a Vulkan RDR2 Reflex/FSRFG 崩溃 | KHR/EXT 别名解析设备函数 + vkSignalSemaphore[KHR] 时间线信号 + vkAcquireNextImage2KHR FG 路由 | **已移植**(a7537612):5 文件全量;SignalSemaphore 按设备解析 core/KHR,GetDeviceProcAddr 带 KHR/EXT 回退 |
| P3 | 33e434e DX12 FSR4 交换链升级 | FrameGeneration 选项链加 FrameGenerationSwapchain next 项 | **已移植**(a7537612) |
| P4 | d8aebe4 FSR4 Auto 模型选择改进 | DetectedModel() 回退 + FP8 抑制 | **策略层已移植**(a7537612,见下);IdentifyGpu.cpp 检测精化跳过(与 65ee5d6 重叠,待同步窗口) |


### P4 移植说明（2026-10-23；2026-10-25 更新 opt-out 语义）

d8aebe4 三部分,移植两部分、跳过一部分:

1. **FSR4Models.cpp `DetectedModel()` + `hkIsSupported` 策略**——已移植。INT8/INT8_2/INT8Lite 检测值
   映射为模型；selected 未设置时回退检测值（默认安装钩子时可达）；FP8 抑制分支（显式
   选择跑不动时不让 FFX 落到 FP8）；device==nullptr 跳过 lastReportedSupport。
2. **FSR4DriverProviderDx12.cpp FP8 仿真门**——已移植。仿真 FP8 wave matrix 只对 FG 模块和显式 FP8
   超分开放(INT8 GPU 上 FFX 默认选 FP8 质量差/慢)。
3. **IdentifyGpu.cpp 140 行**——跳过。与 65ee5d6 重叠,无 GPU 实测不整体搬;现有检测喂同一枚举,
   策略层已完整生效。

卡顿规避现在需显式设置 `Fsr4DisableDriverHooks=true`，Auto 本身不再隐式禁钩。该 opt-out 同时关闭预设修正和显式覆盖，菜单会提示；未新增游戏性能/AC 实测结论。
### C. 剩余提交处理记录(二批 + 三批)

勘误(2026-10-24):初版把 b686ef4/824a7b7/62887bf(Vulkan FSR4 支持、FG 隐藏、VK 管线 workaround)
误列为待同步——经 `git merge-base --is-ancestor` 核实,它们是 9df512d 的祖先,早已随快照集成。
策略修正:上游已修 = 上游用户报过真实 bug,同 bug 在我们树里;不再"等我们的用户再报一遍"。

**已二批移植(P5-P8,小冲突直进)**:

| # | 提交 | 内容 |
|---|---|---|
| P5 | d1a146a | AL2→AL2 转换 × FSRFG:UsingDifferentInput 容忍 + mark_present_start 的 AL2+FSRFG 帧标记 |
| P6 | 0778a10 | FSRFG Vulkan vsync:vkWaitForPresent[KHR/2KHR] 挂钩,FG 持有交换链时立即返回成功(FG 自己节奏控制) |
| P7 | 223cf6b | Enshrouded MV:新 velocity_convert 计算着色器(R16G16_SNORM/UNORM → R16G16_SFLOAT)+ FFXFeature_Vk 接线 + shaderStorageImageExtendedFormats 特性启用 |
| P8 | 14434bf | HZD 重制版启动崩溃:版本枚举 outputCount 空指针防护 + 非零容量时才写数组 |
| (P4 内含) | 42d6162 | device==nullptr 跳过 lastReportedSupport(已随 P4 移植) |

**E1-E6 已全部移植(2026-10-23 第三批,wt-upstream-sync 单 worktree 按上游拓扑序串行)**:

审阅(work/notes/REVIEW-upstream-e1-e6.md)修正三处误判后全量执行:E4 全量移植
(三项"拒收"是 diff 上下文,上游 creatingD3DDevice 本就 thread_local);E6 不暂缓
(导出 nvapi_QueryInterface 作主 nvapi 时被 NvApiHooks detour,不绕过;被删
signal→sleep 在 LL 构建不可达);九提交一次合入,无实机门槛。19/19 全部消化。

| # | 提交 | 我们的提交 | 适配要点 |
|---|---|---|---|
| E3d | a8d0f0a | 8b566b20 | 旧单例直接可套;[FSRFG] DisableFlipMetering 不进 kKnown(防与 [NvApi] 同名混淆),直读 |
| E3b | c471428 | 540b326f | FpsOverlayShowRatio 进 kKnown([Menu],host allowlist 已加) |
| E3c | c2529ed | 5b8b5c42 | OverlayLine 栈数组;接受上游语义(Just FPS 不再显示比例) |
| E3a | 03e6ef0 | 4c4528a2 | SetFlagEnvVar 删变量语义;表格 hunk 适配我们 MenuUi |
| E1 | e0a4859 | 7baef8c3 | 修真 bug:Release 归零后 intrinsics 空指针;上游残留照搬,见 U7 |
| E4 | 65ee5d6 | ce1a8939 | LUID 绑定/线程局部保留;修我们的 InitFg"先置 initTried 后检查"永久拒绝 bug |
| E5 | a79c2f3 | 12b180f7 | 47 处 ifdef 扁平化 + Xell_Hooks 删除 + 旧 fakenvapi FG/计时路径删除 |
| E6 | 5dfedd6 | fb8f6da4 | 加导出侧防御(延迟 ID 直接导出→InputReflex),闭合上游同样存在的 detour 失败缺口 |
| E2 | c220ad9 | edec0643 | 计时器进底栏状态组;信息卡用 MenuCard;标题保留 (Q)/(OP) |

host ci PASS。**验收缺口(源码完成、待实机)**:AMD FSR-FG flip meter、GPU 检测矩阵
(RDNA2/3/4 + Intel/NV + 混合适配器)、Linux/Proton Vulkan 变更、XeLL/XeFG 全组合。

## U8 钩子安装竞态(上游已部分修复于 Amdxc64Hooks::Init;其余位点下游已修)

**勘误(2026-10-25)**:此前把上游 f903b2c64 的锁位置写成 HookFactory——实际该提交是在
**Amdxc64Hooks::Init** 内加了 mutex。HookFactory/HookFlipMeter 的"判空→装钩→提交"
无锁问题、FSR4Models flipMeter 安装、SDK 预热 set/call/clear 事务上游仍无锁。

下游修复(2026-10-23 审查 P1):各安装点使用**各自函数局部的 mutex**(并非上游式的单一
跨函数 mutex)——installMutex(HookFactory/HookFlipMeter 等安装点单一所有者)、
primingMutex(SDK 预热事务;DX12/VK 两个 provider 首载可并发,互相清标记会让旧游戏
SDK 赢得粘性版本决策)。同批另修:FG 水印开关需 fgChanged+scChanged 才真正重建原生
FSR FG 上下文(上游只设 fgChanged,在我们的 FSRFG 实现里不会销毁重建,驱动在创建时
读环境变量,故不生效)。

向上游提 issue 时只报**仍无锁的位点**(HookFactory/HookFlipMeter、flipMeter、SDK 预热),
不得把 Amdxc64Hooks::Init 误列为未修。

## 半程审计:97e99b4c→990ff98 残留真缺口(2026-10-23,五路并行审计)

背景:10-21 对 9df512d 是选择性文件级合并,产生函数级"半跟"。行级扫描对上游 990ff98
共 ~1200 缺失行,语义分类后 97% 为噪音(等价新版/有意 fork/格式),真缺口 16 项(G1-G16)。
证据:agent://AuditCore|AuditHooks|AuditMenu|AuditFG|AuditUpscale,材料 work/scratch/audit-vs-990ff98/。

**高危(G1-G7 已全部修复,49791359/c5f2e247/末批)**:
| # | 位置 | 问题 |
|---|---|---|
| G1 | FG_Hooks.cpp:769,1014 | DLSSG 的 ALLOW_TEARING 强制被错放进 FGSkipResizeBuffers 分支(默认关)→ DLSSG 激活时 resize 失败(上游 b48fcb0) |
| G2 | Vulkan_Hooks.cpp CreateFGFeature | 缺 FGOutput::Reprojection 分支 → Vulkan 重投影编译进工程/菜单可选但永不实例化,功能死 |
| G3 | D3D12_Hooks.cpp:3100 HookToDevice | 缺 !ScopedCreatingD3DDevice::Active() 守卫 → 探测用临时 D3D12 设备初始化 Streamline(上游 e3d24c7) |
| G4 | Gdi32_Hooks.h:66-82 | D3DKMTQueryAdapterInfo 伪装缺 INT8_2(143/40)/INT8Lite(150/1) → Linux/强制支持下 FFX SDK ASIC 校验拒绝 FSR4 |
| G5 | IdentifyGpu.cpp:587-609,843-867 | Linux 检测缺 preferDeviceInfo + 代数回退扩展(kGfx12→FP8、gfx11.5.1→INT8、gfx10.3→INT8_2、INT8Lite 族);getFsr4ModelForAsic 缺 Phoenix(148)/Krackan(150 rev40-50)/Navi10-12(143 rev1-20)→INT8Lite |
| G6 | menu_common.cpp:4501 | XeFG 安全守卫用 FGEnabled.reset() 而非 set_volatile_value(false) → follow-game 下守卫被绕过(坏 MV/独占全屏仍启用 XeFG) |
| G7 | menu_common.cpp:5755-5955 | RenderLowLatency/FramerateSettings 停在 E5/E6 前:Reflex/AL-Vk 输出被误禁、"None"静默变 Auto、缺 OptiScaler 输入显示/forced 锁/incompat 警告/实际限帧器文案 |

**中危**(G7/G8 已修,G8 见下):
| # | 位置 | 问题 |
|---|---|---|
| ~~G8~~ | ~~menu_common.cpp:4782-4836~~ | ~~已修(49791359 后续批次)~~:补 RenderFGModeSlider 开关、Parallax Scale 0-10000 对数 + ParallaxScaleEstimate 只读显示、IsBypassed/ThirdPersonScore/OrbitScore 遥测行 |
| G9 | menu_common.cpp:4413,5225-5275 | Vulkan FSR-FG 帧节奏调整被 DX12 版本检查隐藏;Vulkan OptiFG 缺 Flip(Unity)/FlipOffset 勾选(后端已实现) |
| G10 | menu_common.cpp:1714 | FG 切换热键写持久值 → 保存后 FGEnabled=auto(跟随游戏)永久丢失(应 set_volatile_value) |
| G11 | menu_common.cpp:3876 | XeFG→XeFG passthrough 下 fgSettingsChanged 恒真 → 永久显示"保存并重启"红字 |
| G12 | FSRFG_Dx12.cpp:718-754 | _hudCopy/_hudlessCompareCompute 用缓存 _device 而非 cmdList->GetDevice 解析(上游 1ec11b8)→ 设备不匹配边角崩溃/花屏 |
| G13 | NvApiHooks.cpp:238 | hookReflex 在 Reflex ID 分支 return 之后才调 → 仅 Reflex 接口被查询时 ReflexHooks 未初始化 |

**低危(UI 小修)**:
| # | 位置 | 问题 |
|---|---|---|
| G14 | menu_common.cpp:642,4040,4912,5360,2865/2897,3981/4890 | DLSS_on12 未按 dlssCapable 禁;XeFG 输入的 Advanced FG 区隐藏;Vulkan DLSSG Reflex 标记勾选应禁;01195e9 已修的过时 Vulkan 警告文本残留;Vulkan usesDlssd 组合守卫+(DRS) 徽标;DMFG 目标上限 200(<高刷屏) |
| G15 | Streamline_Hooks.cpp:633 | cmdBuffer 残留 D3D12 强转(8e9e6f9 Vk SL 场景) |
| G16 | menu_common.cpp:2897 | Vulkan 分支 AddVulkanBackends 未按 usesDlssd 守卫(DX11/12 已守) |

**审计同时确认的"勿回搬"项**:dllmain Agility 判断(我们的 >0 门更对,U3)、
State::creatingD3DDevice/vulkanSkipHooks 布尔(我们的线程局部 RAII 更对)、
XeFG_DynamicCount(含超出上游的 Restart/clamp 安全改进)。

## U7 e0a4859 的 intrinsics 全局覆盖泄漏(上游同样存在,观察)

每次 IAmdExtD3DFactory::CreateInterface(IID_IAmdExtD3DShaderIntrinsics) 覆盖全局
o_amdExtD3DShaderIntrinsics,上一个 real 引用泄漏。上游 990ff98 同样存在;我们照搬,
不单独修(等上游;若多交换链场景出现泄漏证据再处理)。

### D. 行动项(2026-10-25 修订:三批提交策略)

**格式铁律**:任何将提交给上游的 issue/PR 正文**不得出现本台账的内部编号(U1、U8 等)**,
每条 issue 用自包含的标题与描述;编号只留本地追踪。

1. **第一批(立即)**:U1、U3+U4 合并 —— issue + PR
2. **第二批(完成上述勘误后)**:U8 剩余竞态位点(勿含 Amdxc64Hooks::Init)、U9 修正版
   (勿含已撤回的 UAV counter 指控)—— issue + PR
3. **第三批(可选)**:U6 剩余 UX(Int8 强制无提示)—— issue + 可选日志 PR
4. **U10**:三处缺口已闭合,升级为 **issue + PR**(见下;FSR 等待的阻塞风险必须随 PR 说明)
5. **暂缓**:U2/U5(先按 8f0b7d875 复验前提)、U7(缺证据)
6. ~~移植 P1-P3~~ 已完成(a7537612);~~评估 E1-E6~~ 已完成(见 C 节)

## U9 DX11 桥上下文恢复的残余缺口(上游 9df512d 现存,我们已超集修复)

上游 9df512d 的 IFeature_Dx11wDx12::Evaluate 用 ComPtr 持有绑定引用,悬垂引用问题已被
上游 97e99b4c 的 ComPtr 修复闭合(**勘误 2026-10-25:撤回此前"UAV append counter 恢复时
清零(&raw, 0)"的指控**——经复核该行为不构成缺陷,不再列入 issue 内容)。

恢复仍不完整的剩余缺口:不恢复 **compute shader 本体与 class instances**(深度转换会改掉)、
**常量缓冲区区间被丢弃**、**UAV 槽数未按 feature level** 调整。我们的 ScopedContextState
(with_dx12/dx11_context_state.h)全部覆盖并有 WARP+哨兵 host 测试。可提上游:
"ComPtr restore in IFeature_Dx11wDx12 misses compute shader body, class instances and
CB ranges; UAV slot count not adjusted for feature level"。

另:99c9eb15b 集成 Present 计时读取器时漏带桥的计时器构造与 ScopedGpuTime_Dx11 两配套件
(纯本地遗漏,32291157 补齐),非上游问题。

## 非上游问题的说明

菜单虚基下转崩溃(bdd11179)与 `_ffxFGIndex` 越界是**我们集成时引入**(上游无此代码/上游本有上界保护),不属于上游问题,已在 decisions.md 记录。


## 上游状态核对(2026-10-25,c2876a76e)

上游 master 自 990ff98 新增 20 提交。与 U 台账相关的:
- 8f0b7d875 改 XeLL 选择为优先捆绑副本(动摇 U2/U5 前提,需复验;非版本校验)
- f903b2c64 在 **Amdxc64Hooks::Init** 加 mutex(部分缓解 U8;HookFactory/HookFlipMeter、
  FSR4Models flipMeter、SDK 预热仍无锁)
- cf6da0010 把 `DxgiSpoofing` 改 SoftDefault(U6 持久化已修;剩 Int8 强制无提示)
- 8f2a94358 修 Vk 侧 amdxcffx64 伪装(与 U6 无关)

**结论(2026-10-25 修订):U6 持久化、U8 Init 位点、U9 悬垂引用已部分修复,其余仍存在;
U2/U5 前提待复验后再定。**
审核材料: work/scratch/upstream-issues-review.md(纯 U1-U10 抽取版,含逐条上游代码比对)。

## U10 FG 输出缓存借用资源并在不持引用的情况下延长有效期(XeFG/DLSSG/FSR-FG)

**Title**: FG outputs cache borrowed resources and extend validity without holding references (XeFG/DLSSG/FSR-FG)

三个 Dx12 FG 输出共享同一从上游(FakeMichau 97e99b4c,2026-10-05)继承的模式:`SetResource`
初始缓存保持借用(符合输入生命周期契约),但 Present/Dispatch 路径把有效期延长到
`UntilPresentFromDispatch`(或以 `UntilPresent` 借用状态直接交给 FG SDK)而不 AddRef:

- **XeFG**(XeFG_Dx12.cpp):Dispatch 把 HudlessColor 重标记为 UntilPresentFromDispatch;
  Present 路径 `GetResource()->GetDesc()`(现场崩溃点 :1803,C0000005 命中已释放 vtable)
- **DLSSG**(DLSSG_Dx12.cpp):Dispatch 同时重标记 HudlessColor **和** Distortion
  (:267/:279);SetResource HudlessColor 分支同样 `GetResource()->GetDesc()`(:793 一带)
- **FSR-FG**(FSRFG_Dx12.cpp):无 Dispatch 重标记,但 SetResource 把借用资源以
  UntilPresent 有效期缓存(FGOnlyAcceptFirstHudless 检查 :1475 一带),随后 Dispatch 把
  `hudless->GetResource()` 直接交给 FFX API;SetResource HudlessColor 分支
  `GetResource()->GetDesc().Format`(:1546 一带)是同一崩溃形态

**现场证据**:yysls + MFG Unlocker 1.7.0,6 个 minidump,全部 C0000005,命中已释放资源的
vtable(XeFG_Dx12.cpp:1803 的 GetDesc)。

**我们的修复**:`PresentResourceReference`(IFGFeature_Dx12.h,三处共享)在帧槽
`_resourceMutex` 锁内 AddRef **GetResource() 实际选中的对象**(copy 优先),Release 同样持锁。
XeFG/DLSSG 在 Present 栈上持有,重标记使用锁内快照而非缓存裸指针;XeFG HudlessColor 与
Distortion 现均覆盖。FSR-FG 改为实例持有 `_presentResources`,跨越我们的 Present 栈退出、
真实 swapchain Present 内回调和异步 GPU 使用,仅在 SDK 完成等待后 Reset。初始 SetResource
缓存保持借用语义。FSR 自绘 UI 输入亦在同一保活窗口内持有。

**风险**:三个 FG 输出全部在列;任何第三方注入器或游戏侧提前释放都可能触发。
可提上游:"FG outputs retag borrowed HudlessColor/Distortion to UntilPresentFromDispatch
without AddRef — use-after-free on GetDesc/FFX handoff"。

**闭合结论(2026-10-25)**:以下三处缺口已完成源码级调查与修复,提交策略由仅 issue
升级为 **issue + PR**。这不是 GPU 实机复现/压力测试通过的声明。

### U10 缺口闭合证据

| 原缺口 | 闭合方式与证据 |
|---|---|
| XeFG Distortion 无锁重标记 | 复制 HudlessColor 模式:帧槽 unique_lock → Acquire(GetResource()) → retag → snapshot → 解锁后 SetResource。当前 XeFG SetResource 仍拒绝不支持的 Distortion,防护覆盖重标记入口而不改变功能支持。 |
| selected-copy 所有权 | XeFG/DLSSG 两种 retag、FSR acquire 均 pin GetResource(),不再误 pin 原始 resource。FSR 对 copy!=nullptr 也持有(包括 ValidNow copy)。三后端 `_resourceCopy` 的 CreateBufferResource 创建引用归表拥有,基类 ReleaseResourceCopies 在帧槽锁内 DeferredRelease;延迟释放不是本次保活依据。XeFG 的 depth-invert/flip Buffer、FSR HudlessFormatTransfer/UIFormatTransfer 的 Buffer 归 helper 拥有,selected reference 独立于 helper 退休。FSR UIFormatTransfer 调用当前注释停用;自绘 UI 的实际 selected 输入亦 pin。 |
| FFX 消费期 | **已证实由调用方保活,不是 FFX 自持引用**。vendored `FidelityFX-SDK/docs/techniques/super-resolution-interpolation.md:529-558` 明确要求 UI 延续到 real-frame composition 完成,HUDLess/distortion 延续到 interpolation 完成。`ffx_api_dx12.h:256` 只包装裸指针;`ffx_provider_framegeneration.cpp:324,353-360` 缓存裸资源;`FrameInterpolationSwapchainDX12.cpp:1395-1399` 在实际 Present 中回调后提交 GPU 并 signal interpolation fence。FSR 改为实例持引用,在下一次 Present、重配置/停用、销毁 context 和清理 copies 前调用 SDK WAIT_FOR_PRESENTS;成功后清除旧配置防止失败/跳过下一 dispatch 时继续使用旧指针,再 Release。等待失败保留引用并停止该边界操作。 |

**同步取舍**:SDK `ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12` 没有 timeout 字段;
provider 调用 `ffxWaitForPresents`,底层等待 game/interpolation/present fences
(`FrameInterpolationSwapchainDX12.cpp:1120-1125`)。本修复优先正确性,可能降低跨帧重叠、
被 vsync/呈现节奏拖慢,异常呈现管线可能无限阻塞;未伪造一个超时后不安全释放的退路。
源码契约回归覆盖两种 XeFG/DLSSG retag、selected-copy 和 FSR SDK-drain-before-release 边界。

**验证(2026-10-25)**:`test_xefg_present_lifetime.py` 5 项通过;
`tools\build\build-release-local.cmd --fast` 产出 `exports\release-local\OptiScaler.dll`;
`tests\host\run.cmd ci` PASS;`tests\host\run.cmd xefg` PASS(含生命周期源码契约、
NR multiplier、XeFG dynamic-count)。未运行真实 FFX/GPU 提呈压力测试。
