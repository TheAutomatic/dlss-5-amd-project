# XeFG 多帧生成（MFG）宿主兼容与性能说明

本文记录外部 XeFG 多帧生成插件（如 `MFGUnlocker.asi`）在 OptiScaler(NR) 宿主下的兼容机制、动态多帧生成（Dynamic MFG）、`HighResMV` 自动检测行为，以及 AMD 显卡在 4K 输出下的性能开销构成。

## 1. 宿主兼容契约（三处关键点）

外部 `MFGUnlocker.asi`（通过 `OptiScaler\plugins\` 加载）识别到 OptiScaler 宿主后，会在首次创建 XeFG 上下文（`xefgSwapChainD3D12CreateContext`）时解锁已加载的 `libxess_fg.dll` / `libxell.dll` 帧数上限（使 `GetProperties().maxSupportedInterpolations` 返回 `5`，即最高 6X），随后将实时倍率控制权交回宿主（`xefgSwapChainSetNumInterpolatedFrames`）。

宿主侧需保持以下三项兼容守卫（见 [Xell_Hooks.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/Xell_Hooks.cpp)、[XeFG_Dx12.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp)、[Kernel_Hooks.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/Kernel_Hooks.cpp)）：

1. **不对 `libxell.dll` 导出表做纯日志 Detour（`#ifndef LOW_LATENCY_INPUTS`）**
   - 当未启用 `LOW_LATENCY_INPUTS` 时，`XellHooks::Hook()` 直接返回，不再将 `libxell.dll` 的 `xellD3D12CreateContext` / `xellDestroyContext` 等 8 个导出 Detour 到 `OptiScaler.dll`。
   - 否则插件扫描 `libxell.dll` 导出时会因入口跳入宿主模块而置位 `hostLatency = true`，在 `ApplyHostUnlock` 中触发 `host_xell_redirected` 熔断并跳过帧数解锁。
2. **`CreateSwapchainContext` 前触发公开 `GetProcAddress` 同步解析**
   - 宿主启动时 `CheckMemoryForProxies()`（内含 `XeFGProxy::InitXeFG()`）早于 `LoadAsiPlugins()` 执行，且使用 `KernelBaseProxy::GetProcAddress_()` 缓存函数指针，绕过了插件挂在 `kernel32!GetProcAddress` 上的 `FirstCreate` 同步初始化钩子。
   - 在 `XeFG_Dx12::CreateSwapchainContext` 调用 `D3D12CreateContext` 前显式调用一次 `GetProcAddress(XeFGProxy::Module(), "xefgSwapChainD3D12CreateContext")`，确保插件在当前线程同步完成模块发现与钩子安装，避免与后台 `PatchWorker` 线程竞态。
3. **`hk_K32_GetModuleHandleExA/W` 过滤 `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`**
   - 当 `dwFlags` 含 `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` 时，`lpModuleName` 是内存地址而非模块名字符串，直接透传给原函数早退。

## 2. 动态多帧生成（Dynamic MFG）

在 [XeFG_DynamicCount.h](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_DynamicCount.h) 与 [XeFG_Dx12.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp) 中，宿主实现了基于目标帧率的动态多帧生成控制器：

使用宿主的动态倍率控制时保持 `[FrameGen] External=false`；解锁插件负责提高倍率上限，
OptiScaler 负责每帧选择倍率。菜单按 `xefgSwapChainSetNumInterpolatedFrames` 接口是否存在
显示 Dynamic MFG，多帧上限大于 1 时可选。不要用 XeSS 产品版本判断 XeFG API 主版本；
本地包的 XeFG 1.3.1 已提供该接口。仅安装插件不会自动开启 `DynamicMFG`。

- **配置项（`[XeFG]`）**：
  - `DynamicMFG`（菜单：`Dynamic MFG`）：启用动态多帧生成（默认 `false`）。
  - `FramerateTargetDMFG`（菜单：`DMFG FPS Target`）：目标显示帧率（菜单 0～360 FPS，默认 `0`；0 跟随 FPS 限制，无限制时取略低于屏幕刷新率的目标）。
  - `RoundingDMFG`（菜单：`DMFG Rounding`）：倍率取整策略（`0` = Floor 向下取整，`1` = Nearest 就近取整（默认），`2` = Ceiling 向上取整）。
  - `DynamicAllow1X`（菜单：`Allow 1X`）：允许控制器按目标容差降为 1X（调用 XeFG SetEnabled 关闭插帧；默认允许，取消后下限为 2X）。
- **与 `Interpolation Count` 协同**：用户设置的 `Interpolation Count`（或 `[DlssNr] XeFGInterpolationCount` 覆盖项）作为动态调节的**最高倍率上限（ceiling）**，控制器在 `[minCount, maxTargetCount]` 范围内平滑增减插帧数量，并在动态切换时跳过 `WAR_xefgRequestFGToggle` 重建闪烁。

## 3. `HighResMV` 的自动检测、落盘与首次启动重启生效机制

`OptiScaler.ini` 中的 `[XeFG] HighResMV` 默认是 `auto`（`false`），但宿主会在进入游戏场景后根据当前选用的输入源自动检测并调用 `Config::SaveXeFG()` 写回 INI：

- **`FGInput = dlssg`（Streamline 输入）**：在 [Streamline_Inputs.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/inputs/FG/Streamline_Inputs.cpp) 中，若游戏向 Streamline 提交的 `data.motionVectorsDilated == sl::Boolean::eTrue`（即引擎已将运动矢量膨胀到显示分辨率），宿主会自动将 `FGXeFGHighResMV` 设为 `true` 并保存到 `OptiScaler.ini`。
- **`FGInput = fsrfg` / `upscaler`**：分别在 [FSR3_Dx12_FG.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp)、[FfxApi_Dx12_FG.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp) 或 [IFeature.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/IFeature.cpp) 中根据 `FG_Flags::DisplayResolutionMVs` / `!_initFlags.LowResMV` 自动同步并调用 `SaveXeFG()`。
- **为什么落盘到 INI 且首次开启后必须重启游戏**：
  - `XeFG_Dx12::CreateSwapchain` 在游戏启动早期创建交换链时就必须确定 `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV`（见 [XeFG_Dx12.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp) 第 399–400 行），早于进入场景首次初始化超分/帧生成常量的时机。
  - 若启动时 `OptiScaler.ini` 中尚未写入 `HighResMV=true`（首次运行默认 `false`），交换链会以低分辨率运动矢量模式创建；进入场景后宿主虽自动检出并写入 `HighResMV=true`，但该标志必须在**重启游戏重建交换链**后才会真正生效（对应菜单提示 [menu_common.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/menu_common.cpp) `"Restart the game to apply correct XeFG settings!"`）。

## 4. 4K 多帧生成实测对比与首次未重启掉帧归因（纠正历史错误结论）

在《燕云十六声》（`yysls`，RX 9070 XT，渲染 `1920x1080` → 输出 `3840x2160`，pre-SR Mochizuki NR `passes=1`）下的同场景受控对比实测表明：

### 4.1 `XeFGUnlock.asi`（38 KB）与 `MFGUnlocker-3.asi`（6.9 MB）性能完全一致

在 OptiScaler 宿主模式（`optiScalerRoute: "plugin"`）下，无论加载社区版 `XeFGUnlock.asi`（38 KB）还是 dashdogy `MFGUnlocker-3.asi`（`v1.5.1-beta.3`，6.9 MB），插件均只在首次创建上下文时将 `libxess_fg.dll` / `libxell.dll` 的最大插帧数解锁为 `5`（`Max supported interpolations: 5`），随后的每帧插帧与 Present 均交由 OptiScaler 宿主直接调用原生 `libxess_fg.dll`。在 `XeFG.HighResMV: true` 已生效的同场景测试中，两者的帧率与单次插帧开销几乎完全相同：

| 状态档位 | `XeFGUnlock.asi`（38 KB） | `MFGUnlocker-3.asi`（6.9 MB） | 实测结论 |
| :--- | :--- | :--- | :--- |
| **FG 关闭 (1X)** | **61.5 FPS** (`16.26 ms/帧`) | **62.5 FPS** (`16.00 ms/帧`) | 基准帧率一致 |
| **XeFG 2X (`InterpolationCount=1`)** | **51.9 FPS** (`19.27 ms/帧`，`+3.01 ms`) | **52.1 FPS** (`19.20 ms/帧`，`+3.20 ms`) | 单帧插帧开销约 `3.1 ms` |
| **XeFG 4X (`InterpolationCount=3`)** | **41.5 FPS** (`24.06 ms/帧`，`+7.80 ms`) | **43.1 FPS** (`23.18 ms/帧`，`+7.18 ms`) | **3 次插帧总开销 `7.2 ~ 7.8 ms`（平均每生成 1 帧仅耗 `2.4 ~ 2.6 ms`）**，显示输出 **166 ~ 172 FPS** |
| **`NrFrameDiag` Present 阻塞 (`gw_present_wall_ms`)** | 中位数 `1.18 ms`（P90 `1.56 ms`） | 中位数 `0.79 ms`（P90 `1.00 ms`） | 4X 下 `Δpresent_serial / Δnr_frame = 4.0`，主线程阻塞约 `1 ms` |

### 4.2 首次测试 4X 掉至 27 FPS（`36.6 ms/帧`）的真实根因

此前单次日志（`14:37` 会话）中出现 4X 基础帧降至 `27.3 FPS`（`36.6 ms/帧`，较 FG 关闭 `+20.6 ms`，平均每生成 1 帧耗 `6.87 ms`），**并非**两个解锁插件存在性能差距，也**并非** RX 9070 XT 在 4K 下运行 XeFG 4X 的固有开销需要 5～6 ms/帧（特此废止此前将掉帧归因于插件差异或 4K DP4a 固有成本的错误推断）：

1. `14:36:58` 首次启动时，`OptiScaler.ini` 中尚无 `XeFG.HighResMV=true`（默认为 `false`）。
2. `14:37:16` `XeFG_Dx12::CreateSwapchain` 在**未携带 `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV`** 的状态下创建了 XeFG 交换链。
3. `14:39:11` 进入场景后，`IFeature::SetInitParameters` 检测到高分辨率运动矢量并通过 `Config::SaveXeFG()` 将 `HighResMV=true` 保存进 `OptiScaler.ini`，但当次会话未重启游戏，导致 XeFG 全程在 `HighResMV=false` 的错配状态下运行，单次插帧耗时从正常的 `2.4 ~ 2.6 ms` 暴涨至 `6.87 ms`。
4. 随后重启游戏的所有测试会话（`18:54`、`19:25`、`19:30`）在启动时均已读取 `[I] XeFG.HighResMV: true`，交换链以正确的 `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV` 初始化，两款插件在 4X 下均稳定达到 **41.5 ~ 43.1 FPS** 基础帧（**166 ~ 172 FPS** 显示帧）。

### 4.3 关于 `RTXMFG-Universal.status.json` 退出状态的说明

使用 `MFGUnlocker.asi` 时，游戏退出后生成的 `RTXMFG-Universal.status.json` 可能显示 `xessInitialized: false`、`xessFramesPresented: 0`。原因有二：
1. 在 `optiScalerRoute: "plugin"` 模式下，插件仅在 `CreateContext` 时对 `libxess_fg.dll` / `libxell.dll` 执行一次性解锁，后续每帧调用直接透传给宿主和原生 DLL，插件自身不接管每帧 Present 计数；
2. 部分游戏（如《燕云十六声》）在进程退出瞬间会触发一次收尾设备初始化并重写该状态文件。

因此判断解锁与多帧生成是否生效，应以 `OptiScaler.log` 中的 `XeFG_Dx12::CreateSwapchain Max supported interpolations: 5`、`UpdateInterpolationCount` 以及 `NrFrameDiag.log` 中 `present_serial` 相对 `nr_frame` 的倍率步进为准。

## 安装文件归属与动态切换边界

- 包内插件说明使用专名 `OptScaler-NR-plugins-README.txt`，仅安装到项目私有依赖目录；旧的通用 `README.txt` 不作为自定义文件分发，卸载也不按此通用名称认领用户文件。
- 用户提供的 ASI 可更新覆盖；同名 INI/JSON/TXT 已存在时保留，缺失时才写入初始配置。依赖树里的自定义插件也统一走此路径，避免默认插件目录先被覆盖。
- 复制 ASI 不覆盖显式的 `LoadAsiPlugins=true/false`；缺失、空值或 `auto` 才自动开启。用户主动选择安装 OptiPatcher 的原有交互流程保持不变。
- 动态倍率和静态倍率共享 API 失败状态。参数拒绝保留原已应用倍率；其他错误锁定为需重建并立即停用、结束当前 dispatch，新交换链初始化前不再调用改倍率或激活。
- 倍率上下限改变或当前实际倍率越界时取消旧试验；所有控制器返回值都受当前边界约束。关闭 Allow 1X 后，旧试升失败不能把倍率回退到 1X。

回归包含真实 PowerShell 安装/卸载夹具（默认、相对自定义、游戏目录外绝对路径，重复安装、用户配置和 README 保留），以及动态倍率失败锁定/恢复和试验中修改上下限的 CPU 测试。它们不代替 XeFG 插件游戏实测。

## XeLL 低延迟输入与 MFG 解锁插件的互斥（R4 Choice B）

开启 `LOW_LATENCY_INPUTS`（宿主接管 XeLL 输入）后，`XeLLProxy::RedirectToInput`
会将真实 `libxell.dll` 的导出重定向到宿主的 XeLL 输入导出。MFG Unlocker
（`MFGUnlocker-3.asi` / `XeFGUnlock.asi`）扫描到导出跳入宿主模块时会按其设计策略
跳过帧数解锁（`host_xell_redirected` 熔断）。因此 **XeLL 输入与高于 2X 的解锁倍率不可兼得**，
需按游戏二选一：要么关闭 XeLL 输入以保留解锁插件的全倍率，要么保留 XeLL 输入并接受
2X 上限。宿主不会静默牺牲任何一方：检测到重定向时 `RedirectToInput` 会明确记录
"MFG Unlocker 检测到宿主管理的低延迟，将保持帧数锁定"；`[FrameGen] External=true`
（外部帧生成）时所有 LL 输入拦截整体跳过，解锁插件按原生方式工作。
NR 倍率上限与就绪判定（`_nrMultiplier` / `ResolveInterpolation`）不依赖 XeLL 输入路径，
不受此互斥影响。分析记录见
`wt-opti-low-latency/work/notes/2026-10-21-xefg-mfg-unlocker-xell-input-blocker.md`。

## MFG-Unlocker 1.7.0（DLSS 5）兼容指引

研究基线：插件仓库 v1.7.0（`b97deee`）与 `main`（+`7523faa`）、dashdogy/OptiScaler
`mfgunlocker` 分支（=上游 `83e18f1a` + 5 提交，tag `mfgunlocker-v1.7.0`）。本仓库实机
验证锚点为插件 **1.6.1**；1.6.2 内部测试线（alpha/beta 20+ 里程碑）被压成单提交发布，
无中间源码可查。

**DLSS 5 是 N 卡专属，无需任何兼容动作。** 插件通过 D3DKMT 内核 PCI VendorId
（`0x10DE`）硬门控（`dlss5.cpp:1078-1085`），该判定在 OptiScaler 的 DXGI 伪装之下，
AMD 宿主上无法开启也无法被伪装骗开。其本体是插件自带的 DLSS-NR 注入器
（驱动 `nvngx_dlssnr.dll`，RenoDX 式特性集），与本项目 `dlssnr/` 互为独立实现；
在 N 卡宿主上两者会同时驱动同一 NGX feature-18 并同钩 Evaluate/Present，
属 N 卡用户需自行二选一的问题，本项目不为其做兼容。

**AMD 宿主上 >1.6.1 的两处高风险行为与规避：**

1. UI 默认改为 Shared：插件在我们的宿主下也会包裹 DXGI factory/swapchain/输入
   （1.6.1 检测到宿主即不包）。规避：`RTXMFG-Universal.json` 中
   `"optiScalerUi": "optiscaler"`（等于 1.6.2 行为：无插件菜单、无包裹、无 DLSS 5）。
2. 翻转队列节奏默认注册到宿主创建的 XeFG 上下文（RX 7000/9000 + Adrenalin
   ≥26.10.1），经 `AmdExtD3DCreateInterface` 驱动 AMD 翻转队列，与 XeLL sleep
   存在已知死锁形态。规避：`OptiScaler.ini` `[XeFG] XeFGFlipQueue=off`。

未规避而运行 1.7.0 Shared 模式属于未验证组合；如需支持需按上文 XeFG 实测锚点
重新验证。插件新增读取我们 INI 的键（`XeFGAmdFlipMeter`、`XeFGOutputRing`、
`XeFGHdr10Flag`、`XeFGFlipQueue*`、`XeFGNativeBuffers`）与本项目键零冲突。
宿主已采用上游 `d06f31e9` 的菜单容量标记行（`menu_overlay_dx.cpp`：
"Swapchain has {} buffers, menu supports {}"），插件据此识别本菜单支持 16 缓冲，
不再默认按 8 缓冲封顶 `XeFGNativeBuffers`（`91df0082`）。

XeLL 互锁（`hostLatency` 熔断）、kernel32 FirstCreate 同步、GetModuleHandleEx 用法
经逐字比对与 1.6.1 一致；XeMFG standalone 在检测到 OptiScaler 时硬关闭。

## Vulkan 游戏的 XeFG:VkwDx12SC DX12 互操作交换链(dashdogy 0ebe836e/50dd7859 吸收)

**它解决了什么。** Vulkan 游戏此前没有任何 DX12 FG 输出:XeFG 输出在菜单里被
置灰("Unsupported API")。VkwDx12SC(对应 DX11 侧的 Dx11wDx12SC)把游戏的
VkSwapchain 变成隐藏窗口上的真实交换链,呈现时把图像拷入 D3D12 共享纹理,经
`FGHooks::CreateSwapChain` 建立的 DX12 FG 交换链在游戏窗口上呈现;FG 输入
(深度/运动矢量,Streamline Vulkan tag 与 NVNGX EvaluateFeature 两条来路)经
同一拷贝桥写入共享纹理,时间线信号量与 D3D12 共享栅栏同步
(`State::swapchainInteropApi = VkwDx12`)。

**仅限 Windows。** 互操作依赖 `VK_KHR_win32_surface` + 隐藏 HWND + D3D12 共享
纹理,三者都不存在于 Wine/Proton(dxvk/vkd3d-proton 自持呈现)。因此
`VkwDx12SC::Wanted()` 在 `isRunningOnLinux`、Mochizuki Proton 增强作用域
(`ScopedGameD3D12Device`,即 vkd3d 收养路径)或 Vulkan 钩子跳过时一律返回
false;`hkvkQueuePresentKHR` 的 interop 呈现分支额外要求 `!dxgiBacked`,不与
DXGI 支撑的呈现路径争抢。Proton 下行为与吸收前完全一致。

**插件无关。** XeFG 2x 独立可用;MFG-Unlocker 插件只在其上叠加倍率解锁与
翻转队列节奏(XeFGFlipQueue 等),两者关系同 DX12 原生路径。

**诚实边界。** 上游验证过的组合是 XeFG-on-Vulkan;FSR-FG 理论上可走同一桥
(架构上只是另一个 DX12 FG 输出)但上游未测试,故 `Wanted()` 仍只对
`FGOutput::XeFG` 放行,Vulkan 上的 FSR-FG 维持原生 Vulkan 输出,菜单不再置灰
XeFG 并注明"Vulkan:经 DX12 互操作交换链(仅 Windows)"。

Streamline 输入经 `VkwDx12Inputs::SetResource` 进桥后仍向
`StreamlineFGActivationGate` 上报就绪信号(ReportDepth/MotionVectors 在路由
之后无条件执行)——这些就是喂给 FG 交换链的 DLSSG 输入本体,门控语义不变,
无需豁免。

**队列族所有权交接(2026-10-25 修复)。** 共享纹理均为 `VK_SHARING_MODE_EXCLUSIVE`,
时间线信号量只保证跨 API 的执行顺序,不替代队列族所有权操作(Vulkan 规范将 D3D12
的访问视作 `VK_QUEUE_FAMILY_EXTERNAL` 的访问)。每个跨边界纹理现在带成对的
release/acquire barrier(`vk_with_dx12_sc.h` 的 `VkwDx12ReleaseToD3D12` /
`VkwDx12AcquireFromD3D12`,契约测试 `tests/host/vkw_dx12_ownership.cpp`,host ci):
Vulkan→D3D12 在信号 `_vkDone` 的同一提交末尾以互操作队列族 release 到
EXTERNAL、布局落 GENERAL、`dstAccessMask=0`;复用时(下一帧 Vulkan 重写)在已等过
D3D12 完成的拷贝前从 EXTERNAL acquire 回同一队列族、`oldLayout=UNDEFINED`(内容
整体重写)。呈现图像的执行锚是该提交对 `_dxDone`@`copiedValue` 的时间线等待,
FG 输入的执行锚是 `SetResource` 里的 `WaitForInputSlot` CPU 等待。队列族用真实
索引不硬编码:呈现路径(`RecordCopy` 与 present 拷贝)用呈现队列族
(`Present` 经 `DeviceCreated` 建立的 queue→family 表解析);evaluate 命令缓冲里的
ValidNow 输入(首次呈现前)用 `VkwDx12SC::InteropQueueFamily()`——已呈现过即呈现
队列族,否则设备首个 compute 族(拷贝 shader 本就要求 compute)。不切
CONCURRENT,信号量/栅栏原样保留只提供执行顺序。
