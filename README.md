**中文** | [English](README.en.md) | [Español](README.es.md)

第三后端 mochizuki（Windows / RDNA4）已接入此开发分支，安装、模型来源、独立菜单和验证范围见 [mochizuki 说明](docs/mochizuki.md)。不附带 NVIDIA DLL 或模型；游戏表现待本地验收。


# OptiScaler AMD pre-SR — 1.10.0

在 **OptiScaler** 上接入 **AMD 神经网络渲染**（DLSS5 on AMD），让 **纯 DLSS / XeSS 游戏** 在 AMD 显卡上跑神经网络降噪；超分辨率仍然由 **FFX/FSR** 完成。

本项目 fork 自 **Matheus** 及上游社区。在上游成熟方案的基础上持续深度研发与维护。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

## 更新日志

详情请看 release 1.10.0 更新日志。

### 1.10.0 配置与安装

lmxxf GPU 模块采用上游 LLVM23 / RowOpts 混合编译配方：每架构5个模块由 LLVM23.1.2 编译，其余29个由 COMGR 编译。两个架构共68个模块已随包提供，玩家无需安装 WSL 或编译器。RX9070XT 数值与运行时回归已通过；本版实际游戏性能仍待验收，不承诺固定提速。

lmxxf 更新至 0.40。默认全 71 块（`DLSS5_SKIP_BLOCKS=none`）、快速数值开启（`DLSS5_FAST_NUMERIC=true`）、叠层 1 遍（`DLSS5_MULTI_PASS=1`）、自由分辨率开启（`DLSS5_NETWORK_FREE_RES=true`）。自由分辨率按实际输入计算，超过处理预算时回退网络档位；1440p/4K 输入会显著增加耗时和显存。2/3 遍叠层加强风格，计算量约为对应倍数，并关闭自适应 ViT 复用。`DLSS5_MULTI_PASS_SKIP_BLOCKS=none` 建议保持默认。改变这些选项会在下一帧重建网络，可能短暂卡顿。

Ins 流程图按 **Input → Model → Output** 导航：输入页管理分辨率和曝光；模型页管理风格、叠层和 ViT；输出页管理合成强度、稳定器与外观效果。游戏输入和后续超分作为流程说明。兼容、内核和诊断保留独立高级分组。点击 Save Settings 保存到 INI；每页 Reset 只重置该页。

Setup 在选后端前显示检测到的权重。lmxxf 需要含 `block0-ffn.f16/.f32` 的权重目录；Daniel 需要提取后的权重；Mochizuki 需要 `dlssnr-amd/dlssnr.bin`。若检测到受支持的 `nvngx_dlssnr.dll` 310.8.0，Setup 可使用 Python 3.10+ 提取 Mochizuki 模型。缺少源 DLL 时会说明所需文件；无效模型会保留并要求先移走再重建，不显示为可用。Ins 在 Enable NR 附近提示所选后端缺失的依赖。

更新使用安装器整包覆盖；选择保留 INI 会保留旧的显式设置。菜单/INI 始终优先；上游的 default/custom/native 三层文件与外部环境只补充宿主没有设置的键。下面 1.9.0 部分为历史记录，其旧分辨率限制不适用于本版。

### lmxxf 配置速查（ini / 菜单）

跨层键与上游同名（`DLSS5_*`）。**Ins 菜单文案不会写入 ini**；优先级：菜单/ini > `native-game-flags.txt` / 环境变量 > 默认值。`DLSS5_STRENGTH` 与 Detail/Colour 为同一组强度，本侧已传入时以菜单为准，无需第二套滑条。

| 菜单位置 | 键 | 说明 |
|---|---|---|
| Output | `TransferStrength` / `ColourStrength` | 网络细节 / 色彩合成（Colour 0–1 保原色，>1 网络色） |
| Compatibility & Scheduling | `DLSS5_FIT_LARGE` | High resolution；大 Color 拟合进网络（含 >1080p） |
| Input | `DLSS5_NETWORK_HEIGHT` | **Network tier** auto（默认）或 720 / 900 / 1080 |
| Input | `LmxxfAutoExposure` 等 | 无曝光纹理时自动测光；关掉后用 Exposure scale |
| Advanced Kernels | `DLSS5_HIP_WAVE_OWNED` 等 | 内核 / 显存池 / 字节流 |
| ViT / image reuse → Reuse tuning | `DLSS5_VIT_ADAPTIVE` 等 | 静止帧 ViT 复用（可调，非逐位） |
| Compatibility & Scheduling / Diagnostics | `DLSS5_HIP_PDL`、增强屏障、early wrap / Debug view | 兼容设置与诊断画面分别归组 |

### 1.10.0 新增控制

以下选项位于 `OptiScaler.ini` 的 `[DlssNr]`，在 Ins 菜单调节后点 **Save Settings** 保存。发布状态以 GitHub Releases 为准。

| 菜单 | 配置键与默认值 | 用法 |
|---|---|---|
| lmxxf style | `DLSS5_STYLE=1` | 0 / 1 / 2；与 Daniel 的 `Style` 独立。改变后下一帧重建网络，可能短暂卡顿 |
| Compact 1080 network | `DLSS5_NETWORK_1080_ROWS=1152` | 打开改为 1088，仅影响 1080 档；可能更快但会改变画面，尤其底边。改变后重建网络 |
| Additional colour formats | `DLSS5_FORMAT_FALLBACK=true` | 接受显卡支持的额外色彩格式，使用私有 FP16 输出；保存后重启游戏 |
| Overall Intensity | `NrOverallIntensity=1.0` | 三后端共用，0–2；0 显示原图，1 保留完整结果，>1 增强修正。0 仍运行 NR，省算力应关闭 NR |
| Residual Stabilizer | `NrStabilizerEnabled=false` | 三后端共用，利用运动与深度减少修正量闪烁；可能软化运动细节或拖影，增加 GPU 工作与显存 |
| History blend / Residual threshold | `NrStabilizerAlpha=0.8` / `NrStabilizerThreshold=4.0` | 范围分别 0–0.95 / 0–16；任一为 0 时旁路稳定器。先用默认值，拖影时降低 |
| Measure NR performance | `NrTimingEnabled=false` | 按需测量 HIP 网络、Encode、Decode 和输出效果；网络计时使用非阻塞 HIP 事件，不改变 PDL 设置 |
| Write timing summary to log | `NrTimingLog=false` | 同时启用计时和文件日志后，最多每 5 秒一条摘要，不逐帧刷屏 |

菜单按上面的 Input / Model / Output 三页组织；Network tier 表示分辨率档位，作为自由分辨率超出预算时的回退。

**Reset shared effects** 只恢复整体强度和稳定器；**Reset diagnostics** 恢复计时与调试显示。各分组 Reset 只作用于对应设置；ViT 分组统一管理执行路径、复用及互斥设置。底部 **Reset NR settings** 重置当前后端和共享 NR 控件，不改另一个后端、后端选择、热切换偏好或快捷键。Lighting / Appearance 的 Reset 不再修改模型次数、结构等分组外设置。

**Page Up** 打开帧率面板，**Page Down** 切换详情；Just FPS 不显示 NR 耗时，其他样式显示 NR GPU，详细样式补充编解码及有样本的输出效果耗时。编解码/效果数值是最近 120 个有效样本的统计；网络数值是最近 5 个样本的中位数，重建后跳过首样本、丢弃已观察到的不足 0.01 ms 异常事件区间，至少积累 3 个再显示；`N/A` 表示无可用样本，`stale` 表示过期。编解码与输出效果各自换行显示，各阶段不能相加当作整帧延迟。计时不新增等待 GPU 的操作，但不是绝对零开销，实际影响仍需游戏对照测试；Daniel 的内部网络耗时尚未接入此面板。

稳定器需要有效运动/深度，无法安全执行时旁路并在 Ins 显示状态。纯后端模式首次开启共享输出效果可能需要保存并重启。整体强度作用于最终输出，不等同于模型的 `Intensity`、Detail 或 Colour。

NR 快速开关默认 **End**，帧生成默认未绑定；可在快捷键菜单修改。移除 ViT 的 F8 开关，只通过 Ins 菜单调节；旧 `DLSS5_VIT_REUSE_HOTKEY` 不再生效。更新时使用安装器整包覆盖，宿主和 runtime 必须配套。

ViT 复用仍采用本项目的性能取向默认值 **16 / 1 / 50 / 1**（period/global/local/image），不是上游 **4 / 0.22 / 1 / 0.35**；更宽松的阈值可能留下旧细节或拖影。`DLSS5_HIP_INPUT_POLL` 与 `DLSS5_IO_FUSE` 尚未接入产品路径，不能通过添加 ini 键启用。

### Ins 窗口布局

可拖动窗口边缘调整大小；窄窗口改为单栏，内容滚动，底部操作独立保留。
底部 **Window** 可选择自由移动或四角停靠；拖动标题栏解除停靠。窗口随游戏显示区域变化保持在边界内。
**Menu Scale** 仍控制字体/控件大小，和窗口尺寸分开。**… → Reset window layout** 只恢复窗口尺寸、居中和自由模式，不改 NR 或缩放设置。
**Save Settings** 保存 `[Menu] WindowWidth/WindowHeight`（缩放前逻辑像素，`auto` 为自动）和 `WindowAnchor`（0=自由，1/2=左上/右上，3/4=左下/右下）。自由位置仅在当前会话保留。
Daniel/lmxxf 共用此布局；Page Up 浮层继续使用其独立位置设置。


---

## 目录
- [📢 1.9.0 更新日志 (Changelog)](#-190-更新日志-changelog)
- [1. 巨人的肩膀](#1-巨人的肩膀)
- [2. 安装指南 (Installation Guide)](#2-安装指南-installation-guide)
  - └─► [可选：3倍及以上多帧生成 (Frame Generation)](#可选功能3倍及以上多帧生成frame-generation)
- [3. 三后端架构与历史性能实测](#3-三后端架构与历史性能实测)
- [4. 游戏内设置与控制](#4-游戏内设置与控制)
- [5. 排错、日志定位与卸载](#5-排错日志定位与卸载)
- [6. 署名与许可 (Attributions & Licenses)](#6-署名与许可-attributions--licenses)

---

## 📢 1.9.0 更新日志 (Changelog)

本次 1.9.0 是一次**重大的架构级里程碑升级**。我们正式引入了开源的 [**`lmxxf` HIP 神经渲染后端**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)，并重点攻克了虚幻引擎 5（UE5）等复杂现代游戏中的多队列与切分兼容性难题。

### 🚀 核心更新点

1. **修复 `lmxxf` 后端对虚幻引擎 5（UE5，如《异环》、《幻兽帕鲁》等）游戏的兼容（1.9.0.3）**
   - **正确绑定渲染队列（解决《异环》Neverness to Everness 等崩溃问题）**：重构了后端队列生命周期，在首帧准确锁定执行 DLSS-NR 命令列表的真实渲染 Direct 队列，解决因视口渲染队列与 Swapchain 呈现队列分离导致的 `QueueContract: targetQueue != sessionQueue` 会话失效与崩溃问题。
   - **跨队列安全保护**：在命令切分执行回调中加入 COM 同一性检查，遇意外队列分发时主动跳过 HIP 调用并保持正常渲染。
   - **显存排干与安全迁移**：动态队列迁移前等待 GPU 排干完成，减少底层会话废弃（`AbandonSessionResources`）导致的显存泄漏风险。
   - **命令列表切分准入与日志优化（解决《幻兽帕鲁》Palworld 等卡顿问题）**：优化了命令列表切分准入判定；默认日志等级优化为 2 (Information)，移除导致部分游戏启动卡顿数十秒的静态哈希，并引入渐进式指数退避限流，避免磁盘暴增。

2. **引入 `lmxxf` 神经渲染后端**
   - **接入开源核心**：在保留原有 `danielblnc` 后端的基础上，全新接入开源 HIP 神经渲染后端。
   - **主队列同帧同步执行（Same-Frame Queue Execution）**：将输入录制、HIP 异步推理、输出屏障无缝嵌入在游戏主命令队列内超分辨率（Pre-SR）之前完成。
   - **lmxxf 支持 DLSS / XeSS 代理接入**：让 lmxxf 后端同样能拦截 DLSS / XeSS 输入并在送交 FSR 前完成神经降噪，使无原生 FSR 的游戏也能使用。
   - **双后端共存与切换**：支持在安装时自由选择后端，并可在 `OptiScaler.ini` 中通过 `NrBackend=lmxxf` 或 `NrBackend=daniel` 自由切换。
   - **内存与稳定性优化**：优化 `fast_prefix` 加速模式，跳过无用的 201MB 噪声 Buffer 分配，显著降低主机内存占用与初始化耗时；强化伪装 NVIDIA（Fake NVAPI）时的 GPU LUID 智能匹配，避免多显卡或驱动欺骗时跨卡崩溃。
   - **⚠️ 分辨率支持限制与推荐档位**：注意当前 `lmxxf` **仅支持超分前渲染分辨率 ≤ 1080p** 的画面进行神经渲染。对应典型档位参考：
     - **4K 显示输出**：推荐使用 **FSR 性能档**（渲染分辨率 1080p）或超级性能档（720p）；若设为 4K 质量档（1440p 渲染）会超出当前模型切片架构上限。
     - **2K (1440p) 显示输出**：可使用 **FSR 质量档 / 平衡档 / 性能档**（渲染分辨率均在 1080p 及以下）。
     - **1080p 显示输出**：可使用 **1080p 原生** 或各类超分档位。

3. **修复安装器交互逻辑**
   - 支持双后端选择与覆盖/共存安装，卸载脚本增加路径安全保护。

4. **菜单（Ins Menu）全面净化与画质原生动态调参**
   - **智能菜单过滤**：在 `lmxxf` 模式下自动隐藏 Daniel 专属的无效选项（如 passes、slots、new wait、实验性 RTGI 等），避免设置混淆。
   - **排版与间距修复**：修复了 `Enable NR` 与 `AMD processing` 挤在同一行的布局 Bug，恢复清晰合理的垂直层级与间距。
   - **原生动态调参滑条**：在 Ins 菜单新增 `Detail strength`（细节/亮度强度）、`Colour strength`（色彩饱和校正）无级滑条，并支持 `Debug view` 实时可视化调试图，改动即时生效。

---

## 1. 巨人的肩膀

本项目并非凭空产生，而是建立在开源图形社区众多先驱者的卓越成果之上：

| 上游与先驱 | 他们做了什么 | 本项目额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分辨率代理框架（支持 DLSS / FFX / XeSS 输入输出） | 作为整体安装与运行主体，提供通用注入、Hook 与配置界面 |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | 首次把 DLSS 神经渲染接进 OptiScaler，并提出在超分前运行多 pass 的 Pre-SR 架构 | 继承其 OptiScaler 代码基底与 Pre-SR 调度管线 |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | 将 Pre-SR 接到 AMD 运行时：游戏 DLSS 输入 → AMD NR → FFX 超分 | 在此基础上首创**多槽调度（Multi-slot）**，消除了单槽空等 **8.7 ms/帧** 的 GPU 挂起；适配 0.3.1；补全新等待 D3D12 状态冻结/恢复；增强 XBOX PC 兼容性。**桥接开销实测仅 0.01～0.03 ms** 量级 |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD 神经渲染运行时本体（0.3.0–0.6.0） | **不改动其核心**，按规范接口调用；并针对 0.3.1+ 的 1 像素 Draw 等待补齐状态保护，确保在 DLSS/XeSS 游戏上安全运行 |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | 逆向恢复 71 块网络并移植到 AMD HIP 的开源神经渲染算力核心 | **接入 OptiScaler 通用代理框架以兼容更多纯 DLSS / XeSS 游戏**；实现主队列同帧同步执行；开发标准版本化 C-ABI 独立运行时（`LmxxfNrRuntime` 并反哺合并至上游）；增加动态色彩/细节无级滑条等 |
| **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** | 开源 HDR / 色彩渲染 Addon | `dlssnr.hlsl` 色彩合成算法来源 |

使用 danielblnc 0.6.0 的 RX 6000（RDNA2）显卡需要安装 AMD HIP 7.2 runtime。

---

## 2. 安装指南 (Installation Guide)

<details>
<summary><strong>📦 点击展开：压缩包内文件清单</strong></summary>

| 文件/目录 | 作用 |
|---|---|
| `OptiScaler.dll` | 本项目主体（安装时会自动重命名为你选择的代理名称） |
| `OptiScaler.ini` | 核心配置文件（包含 `[DlssNr]` 三后端切换与参数选项） |
| `OptiScaler\` | 核心依赖库（FFX / XeSS / Agility SDK / 插件等） |
| `LmxxfNrRuntime.dll` | lmxxf 后端运行时（开源 HIP 神经渲染） |
| `MochizukiNrRuntime.dll` / `dlssnr-amd/shaders/` / `Mochizuki-Model.bat` | Mochizuki runtime、着色器及模型提取工具；模型另备 |
| `lmxxf-modules\` | lmxxf 双架构算子模块（`gfx1200` / `gfx1201` 各 34 个 `.hsaco`，附 `SHA256SUMS` 清单） |
| `shaders\` | lmxxf 编解码着色器（`native_codec_encode.hlsl` 等） |
| `experimental_lighting\` | 实验性光照 pass 的预编译着色器（`GatherCS.cso` / `ResolveCS.cso`） |
| `Setup.bat` / `Setup.ps1` | 交互式图形化安装器（**双击 `Setup.bat` 运行**） |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | 智能卸载器（安装时自动同步至游戏目录，安全防误删） |
| `lmxxf-module-package.ps1` | 安装器与卸载器共用的模块校验助手（须与 `Setup.ps1` 放在同一目录） |
| `Licenses\` | 第三方开源许可证文本 |
| `README.md` / `README.en.md` / `README.es.md` | 本使用文档（中英西三语） |
| `VERSION` | 本包版本号 |
| `SHA256SUMS.txt` | 包内全部文件的 SHA256 清单（可用 `sha256sum -c SHA256SUMS.txt` 校验） |

> **提示**：为遵守各开源协议与版权约束，本压缩包**不随包分发** NVIDIA 专有二进制文件、danielblnc 安装器或未授权模型权重。

</details>

---

### 第一步：准备对应后端的文件

可以准备以下任意后端，或同时安装多个后端：

#### 选项 A：[准备 `lmxxf` 后端文件](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) 或[点击这里](https://gofile.io/d/RyvcrDxz)获取权重文件
- 使用本项目完整包内配套的 `LmxxfNrRuntime.dll`（不要混用上游 ABI1 runtime）；
- 算子模块目录 `lmxxf-modules\`（官方双架构两层目录结构，包含 `gfx1200` [9060 系列，实验性] 与 `gfx1201` [9070 系列，正式生产] 两个子目录，各含 34 个 `.hsaco` 算子模块、叶子清单与根 `SHA256SUMS` 清单，共 68 个模块；运行时由 D3D12/HIP 设备智能自动匹配，安装器校验完整双包并支持旧版覆盖升级）；
- 着色器目录 `shaders\`（包含 `native_codec_encode.hlsl` 等）；
- 模型权重目录 `native-game-tiled-assets\`（可[点击这里](https://gofile.io/d/RyvcrDxz)直接下载）；
- 将上述文件/文件夹放在与 `Setup.bat` 相同的解压目录下。

升级时直接运行新包的 `Setup.bat` 并选择游戏目录。检测到已有 OptiScaler 后，安装器会建议先卸载，以避免新版文件、模块布局和旧设置冲突：输入 **Y（推荐）**会自动调用新包卸载器，再继续安装；输入 **N** 则直接覆盖安装。卸载会重置 OptiScaler 设置，保留权重和已有备份。覆盖升级会备份旧模块目录；额外 `.hsaco` 保存在安装结束时显示的 `backup-amd-presr-*/lmxxf-modules` 中，不混入新版模块目录，其他兼容的用户文件继续保留。


#### 选项 B：[准备 `danielblnc` 后端文件](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- 准备 `dlssnr_on_amd_setup.exe` 与 `nvngx_dlssnr.dll`（推荐，可从 [danielblnc Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) 获取，安装器会自动调用生成 weights）；
- 或者放入已经生成好的 `version.dll` 与 `dlssnr_on_amd_weights.bin`；
- 同样放在与 `Setup.bat` 相同的解压目录下。

---

#### 选项 C：准备 Mochizuki（Windows / RDNA4）

完整包已包含 `MochizukiNrRuntime.dll` 和 `dlssnr-amd/shaders/`。另外准备自己的 `nvngx_dlssnr.dll` **310.8.0**，放在 `Setup.bat` 旁；安装 Python **3.10+**，运行 Setup 并选择 Mochizuki，安装器会提取和验证模型。已有模型时可直接放入 `dlssnr-amd/dlssnr.bin`，无需重新提取。完整校验值和独立提取方法见 [Mochizuki 安装说明](docs/mochizuki.md)。

没有源 DLL 或模型时，Setup 会提示 `MODEL SETUP REQUIRED`；只有 runtime 不代表可以运行。其他版本的 DLL 不自动尝试提取。模型损坏时先把旧 `dlssnr.bin` 移走，再运行 Setup；安装器不会静默覆盖它。

首次进入游戏可能需要数分钟编译网络，右下角显示阶段和进度；期间显示原始画面。改变分辨率、模型比例或叠层容量可能再次编译。Mochizuki 有独立的输入预处理、三种风格、1–3 遍叠层和时序控制；默认叠层 1。Ins/INI 中 `Mochizuki*` 参数不影响另外两个后端。需要排错时先看 Ins 的缺失依赖/编译状态，再看 `OptiScaler.log`。

### 第二步：运行安装器（推荐，一键全自动）

1. 解压本 Release 包到任意临时目录；
2. 将准备好的后端文件与 `Setup.bat` 放在同一目录下；
3. **确认已完全退出游戏**；
4. **双击运行 `Setup.bat`**：
   - 弹出文件夹选择框，选中 **游戏主程序 exe 所在的目录**（例如 `...\Binaries\Win64\`）；
   - 若检测到已有 OptiScaler，输入 **Y** 自动卸载后安装（推荐），或输入 **N** 覆盖安装；
   - 按照提示选择你要注入的 **代理 DLL 名称**（默认为 `dxgi.dll`，推荐；也支持 `winmm.dll`、`d3d12.dll` 等，**不要选 `dinput8.dll`**）；
   - 安装器显示三后端及权重检测结果；选择单个后端或安装全部可用后端，再选择当前启用的后端；
   - 安装器自动处理重命名、防双重注入清理、依赖部署，并配置 `OptiScaler.ini`。

---

### 第三步：手动安装（高级玩家）

若你熟悉游戏模组手动放置，可直接将文件拷贝至游戏主程序目录：
1. 将 `OptiScaler.dll` 重命名为你选择的代理名称（如 `dxgi.dll`）放入游戏目录；
2. 将 `OptiScaler.ini` 和 `OptiScaler\` 依赖文件夹复制到游戏目录；
3. **部署后端**：
   - **若使用 `lmxxf`**：将 `LmxxfNrRuntime.dll`、`lmxxf-modules\`、`shaders\`、`native-game-tiled-assets\` 放入游戏目录；
   - **若使用 `danielblnc`**：将 danielblnc 的 `version.dll` 复制三份，分别命名为 `dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`；将 `dlssnr_on_amd_weights.bin` 放入游戏目录（**切勿保留名为 `version.dll` 的 danielblnc 文件**，以免冲突）；
   - **若使用 `mochizuki`**：复制 `MochizukiNrRuntime.dll`、`dlssnr-amd/shaders/` 及自己的 `dlssnr-amd/dlssnr.bin`；
4. 打开 `OptiScaler.ini`，在 `[DlssNr]` 中设置 `Enabled = true`，并通过 `NrBackend = lmxxf`、`NrBackend = daniel` 或 `NrBackend = mochizuki` 指定当前生效的后端。

---

### 可选功能：3倍及以上多帧生成（Frame Generation）

<details>
<summary><strong>👉 点击展开：3倍及以上多帧生成方案（Arturs DLSS Enabler / Intel XeFG）</strong></summary>

以下方案为外置可选增强（与 DLSSNR 相互独立），所需文件均不随本包分发，请自行获取。
**注意**：在游戏运行中修改 ini 必须保存并重启游戏生效；保持 `[FrameGen] External=false`；**请勿同时开启两条方案**。

---

#### 方案 1：Arturs（DLSS Enabler）
1. 从 DLSS Enabler 官方发布页获取 `dlss-enabler-headless.dll`（请勿使用第三方整合修改版）：
   [artur-graniszewski/DLSS-Enabler Releases](https://github.com/artur-graniszewski/DLSS-Enabler/releases) 或 [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)
2. 将该 DLL 重命名为 `dlss-enabler-headless.dll`，放入游戏目录中与 `OptiScaler.ini` 并列的 **`OptiScaler\`** 子目录内；
3. 游戏**已有 DLSSG** 时，在 `OptiScaler.ini` 中配置：
   ```ini
   [FrameGen]
   External=false
   Enabled=true
   FGInput=nvngxfg
   FGOutput=auto
   FGNvngxReplacement=Arturs
   ```
   若游戏只有超分没有 DLSSG，使用 `FGInput=upscaler` + `FGOutput=dlssg`；
4. 查看 `OptiScaler.log`，出现 `Artur's initialized` 即代表加载成功。

---

#### 方案 2：Intel XeFG（XeMFG DP4A Unlocker 多倍插帧）
`XeFGUnlock.asi` 与 `XeFGUnlock.ini` 来源于 OptiScaler 社区。
1. 将这两个文件放入游戏目录的 `OptiScaler\plugins\` 子目录中（与 `libxess_fg.dll` 同级），不要加 `-loadlate` 参数；
2. 修改游戏根目录下的 **`OptiScaler.ini`**（非 plugins 内部的 ini）：
   ```ini
   [Plugins]
   LoadAsiPlugins=true

   [FrameGen]
   External=false
   Enabled=true
   FGInput=dlssg
   FGOutput=xefg

   [XeFG]
   InterpolationCount=1
   ```
   - `InterpolationCount`：`1` 代表 2x 插帧，`2` 代表 3x 插帧，以此类推；
   - 实际倍率受硬件能力及插件解锁上限约束；
3. 建议先以 2x 模式跑通，确认无异常后再调高倍率；游戏中可通过 **Page Up** 呼出帧率面板，按 **Page Down** 切换详情观察插帧状态。

</details>

---

## 3. 三后端架构与历史性能实测

当前支持 lmxxf（HIP）、Mochizuki（Vulkan）与 Daniel 三个后端。下面的性能数据来自历史配置，不代表 1.10.0 或 Mochizuki 的性能：

```
                           ┌──► [lmxxf 后端]   ──► 开源 HIP 算子 / 主队列同帧同步 / 深度调优
游戏 DLSS/XeSS 输入 ──► OptiScaler ──┤
                           ├──► [Mochizuki] ──► Vulkan / D3D12 interop
                           └──► [daniel 后端] ──► 多槽调度 / 0.3.1 兼容 / 跨系列通用
                                       │
                                       ▼
                             FFX / FSR 超分辨率重建 ──► 游戏画面输出
```

### 一、`danielblnc` 后端：多槽调度（每帧都上 NR）与基准测试

降噪（DLSS5）插在画面渲染路径中：拿到缓冲的那一帧，要等降噪算完才能送去超分出图。原版单槽方案由于每一帧必须等待上一帧降噪完成，存在严重的 GPU 空转挂起（PresentMon 实测约 **MsGPUWait 8.7 ms/帧**）；当算力跟不上时，只能选择整帧跳过降噪，导致画面出现闪烁或间歇性模糊。

本项目在 `danielblnc` 后端上首创了**多槽调度（Multi-slot）**：为每个尚未完成的降噪任务分配独立的并行缓冲槽位，消除了空等上一帧的开销，**做到了尽量每帧都挂上 NR**。

#### 实测对比（鬼武者类，4K FSR 超级性能档 ＝ 720p 渲染；锁 60 帧对照）

| 配置方案 | 帧周期中位 | 大约 FPS | MsGPUWait（GPU空转） | 每帧 NR 状态 |
|---|---:|---:|---:|---|
| **单槽·每帧 NR（旧基线）** | 29.82 ms | **33.5** | **8.69 ms** | 被上一帧卡住，吞吐上不去 |
| **本项目默认多槽** | 22.45 ms | **44.5**（**约 +33%**） | **≈ 0 ms** | **尽量每帧都有 NR** |
| danielblnc 0.3 原生（对照） | 22.35 ms | 44.8 | 0 ms | 原生路径本身不靠跳帧 |

- **收益说明**：在尽量**每帧 NR** 的前提下，相对原版单槽旧基线实测提升约 **+33%**（33.5 → 44.5 FPS）；变快靠的是流水线调度优化，不再空等上一帧，神经网络本身运算耗时未变（`network` 在 720p 下仍约为 12～13 ms）。

#### 槽位选择指南（NR slots：2～5 可调，默认 3）

| 场景测试（4K FSR 超级性能，720p 渲染） | 2 槽 | 3 槽 |
|---|---:|---:|
| **鬼武者** | 19.50 ms，**0 跳过** | 19.49 ms，**0 跳过** |
| **燕云十六声** | 19.05～19.25 ms，**大量跳过 NR 帧**（虽快但无降噪） | 21.78～21.89 ms，**0 跳过** |

- **调参建议**：
  - 在绝大多数常规场景下，**3 槽** 是平衡显存与稳定性的最佳甜点；
  - 《燕云十六声》等高负载游戏极致画质下建议设置为 **≥ 3 槽**；
  - 显存开销极小：每槽仅为渲染分辨率（DLSS 输入）的一张 FP16 纹理（4K 输出配质量档 1440p 渲染仅约 29 MB，原生 4K 仅约 66 MB），按所选数量按需分配。

### 二、`lmxxf` 后端：开源 HIP 算力核心与同帧同步调度

- **双架构硬件支持与自动选择**：
  - **AMD Radeon RX 9070 / 9070 XT (`gfx1201`)**：标准正式生产架构，包含经过完整验证与调优的 24 模块集合；
  - **AMD Radeon RX 9060 (`gfx1200`)**：实验性支持，源码与离线 COMGR 3.0 编译验证完成，硬件实机冒烟与 PDL 表现待后续实机进一步验证；
  - **架构自适应与严格校验**：运行时基于 D3D12 渲染队列绑定与 HIP 设备 LUID 自动匹配对应架构子目录，严格执行 SHA-256 完整性校验与 PDL 孪生符号预检（Preflight）；
- **开源透明**：71 块 ViT 神经网络算子全部由 HIP 实现，针对现代 RDNA 架构进行汇编级优化，引入 LDS 局部作用域栅栏与 C32 CU 模式；
- **主队列同帧同步执行**：OptiScaler 在当前帧的命令列表提交前完成输入录制与外部 Fence 编排，使网络推理与主渲染管线在同一队列周期内紧密衔接，彻底消除外部多进程等待延迟；
- **原生参数支持**：无需重启游戏，可在 Ins 菜单内直接调整细节锐度与色彩校正滑条。

---

## 4. 游戏内设置与控制

1. 启动游戏，进入游戏 3D 渲染画面。
2. 按键盘上的 **Insert (Ins)** 键呼出 OptiScaler 控制菜单。
3. 找到 **DLSS Neural Rendering** 菜单区域，勾选 **Enable NR**。
   - 状态栏将显示当前正在运行的后端：
     - 若为 lmxxf：显示 `AMD NR runtime: lmxxf`；
     - 若为 danielblnc：显示 `AMD NR runtime: 0.3.x`。
4. 画面即时生效：**DLSS 输入拦截 → 神经降噪核心 → FFX/FSR 超分重建**。

### 后端专属调节项说明
- **`lmxxf` 专属**：
  - `Detail strength`：高频细节与亮度增益无级滑条（默认 1.0）；
  - `Colour strength`：色彩饱和与白平衡校正无级滑条（默认 1.0）；
  - `Debug view`：多通道调试可视化（原图、网络输出、差分视图等）。
- **`danielblnc` 专属**：
  - `NR slots`：多槽缓冲数量调节（2～5 槽，默认 3）；
  - `Every-frame`：强制每帧执行 NR 开关；
  - `New wait mode`：0.3.1 状态冻结/恢复新等待模式开关；
  - `Inline same-frame wait`：同帧等待 / async；
  - `Quality`：Reference（默认，高画质对齐 NVIDIA）/ Fast；
  - **Display**：`Tone curve` / `Tone lift`；
  - **Queue (experimental)**：`HIP high-priority queue`；
  - **Compatibility & Scheduling / Diagnostics**：`dlssnr_on_amd.ini` 额外键说明。

#### daniel 配置键（与 `dlssnr_on_amd.ini` `[DlssNrOnAmd]` 对应）

**优先级：Ins 会话 > `OptiScaler.ini` `[DlssNr]`（Save 后）> `dlssnr_on_amd.ini` / 环境 > 默认。**  
Ins 文案不进 ini；**Save Settings** 才把菜单值写入两侧 ini。

| Ins 菜单 | OptiScaler.ini | daniel 键 | 默认 |
|---|---|---|---|
| New wait mode | `AmdGraphicsWait` | `SpinDraw` | 开 |
| Inline same-frame wait | `AmdInline` | `Async`（0=inline） | 开 |
| NR slots | `AmdSlots` | — | 3 |
| Tone curve | `ToneCurve` | `ToneCurve` | reinhard |
| Tone lift (black) | `ToneLift` | `ToneLift` | 0 |
| Quality | `Quality` | `Quality` | Reference |
| HIP high-priority queue | `QueuePriority` | `QueuePriority` | 关 |
| Style（Pass 1） | `Style` | `Style` | 0 Default |

daniel 自有、未进 Ins 的键（含 **OverlayKey**、`PollSpacing`、`HipDevice` 等）见 `dlssnr_on_amd.ini`；`OverlayKey` 只绑 daniel 自家 overlay。  
高级进程环境变量（无 Ins 开关）：`DLSSNR_NO_REG`、`DLSSNR_CHAIN`、`DLSSNR_NOBLEND`、`DLSSNR_NO_REPACK`、`DLSSNR_WBLOG`。

**热切换**（菜单 **Allow backend hot switching**，或改 `OptiScaler.ini` `[DlssNr]`）：

| 键 | 默认 | 说明 |
|---|---|---|
| `NrConvenience` | `1` | `1`：装了 lmxxf 时预开提交代理，daniel ↔ lmxxf 可会话内热切换；`0`：只启所选后端，换后端需重启游戏。改后重启生效。 |

---

## 5. 排错、日志定位与卸载

### 一、卸载说明
1. 进入**游戏主程序目录**；
2. 双击运行 **`Uninstall_OptiScaler_NR.bat`**；
3. 卸载器会自动列出计划移除的文件与目录，并交互式询问是否保留备份文件夹；输入 `Y` 确认后执行安全清理；
4. **权重保留**：卸载脚本默认设计为保留权重文件夹（`native-game-tiled-assets/` 与 `dlssnr_on_amd_weights.bin`）以及 `nvngx_dlssnr.dll`，避免用户后续重装时需要重复下载大体积资产。

### 二、日志定位与排错

排查问题时，请查看游戏主程序目录（或 XBOX PC 的 `_storage_` 目录）生成的日志：
- `OptiScaler.log`：OptiScaler 核心主日志（检查注入、初始化与各后端创建状态）；
- `amd_bridge.log`：AMD 神经渲染桥接层日志；
- `amd_presr.log`：Pre-SR 调度管线日志；
- `dlssnr_on_amd.log`：Daniel 后端专用运行日志。

> **注意：lmxxf 后端的日志在哪？**  
> 与 `danielblnc` 后端写入独立的 `dlssnr_on_amd.log` 不同，`lmxxf` 后端与 C-ABI 运行时的日志已直接接入统一日志系统，其所有初始化、状态检测与运行报错均**集中记录在 `OptiScaler.log`（以及 `amd_bridge.log`）中**，无需查找额外日志文件。

#### 1. `lmxxf` 后端专属排错
- **状态栏显示 `waiting` 或无法启用**：
  - 打开 `OptiScaler.log`，搜索 `Lmxxf` 关键字；
  - 检查游戏目录是否缺失 `LmxxfNrRuntime.dll`；
  - 检查 `lmxxf-modules\` 目录是否完整存在，且内部包含 `SHA256SUMS` 和对应的 `.hsaco` 算子文件；
  - 检查 `shaders\` 目录是否存在且包含 `native_codec_encode.hlsl` 等着色器。
- **提示缺少权重或初始化失败**：
  - 确认游戏目录中是否存在 `native-game-tiled-assets\` 权重文件夹。
- **画面异常或未执行降噪**：
  - 检查当前渲染分辨率：lmxxf 当前仅支持超分前输入分辨率 **≤ 1080p**。若在 4K 下开启“质量档”（渲染分辨率为 1440p）会超出模型切片上限，请切换为“性能档”（1080p 渲染）或“超级性能档”（720p 渲染）。

#### 2. `danielblnc` 后端专属排错
- **状态栏未显示 `AMD NR runtime: 0.3.x`**：
  - 检查游戏目录是否存在 `dlssnr_amd_pass1.dll`（及 pass2/pass3）以及 `dlssnr_on_amd_weights.bin`；
  - 确认游戏目录中**没有多余的 danielblnc `version.dll`** 与代理文件冲突；
  - 查看 `dlssnr_on_amd.log` 排查底层报错。

#### 3. 微软商店版 / XBOX PC 特殊提示
由于系统文件虚拟化映射，部分微软商店或 XBOX PC 游戏会在游戏 exe 同级生成名为 **`_storage_`** 的文件夹，日志与生成文件可能会写入此处，请在此目录同步排查。

### 三、问题反馈格式
若遇到无法解决的崩溃或异常，提交 Issue 时请提供：
1. 注入代理名称（如 `dxgi.dll`）；
2. 所选后端（`lmxxf` 还是 `daniel`）；
3. 显卡型号、操作系统版本与 AMD 驱动版本；
4. 游戏名称与输出分辨率、超分档位；
5. 附带完整的上述 `.log` 日志文件。

### 四、已知问题 (Known Issues)
- **UE5（《幻兽帕鲁》《异环》等）**：旧构建把所有 Query 都判为不可切分，且首个交换链创建前的游戏命令列表未包装，导致 `lmxxf` 回退到原图输出。当前源码已放宽已完成 Query 的准入，并提前包装游戏主程序创建的列表；D3D12 测试通过，仍需在游戏中验证神经降噪实际执行和画面稳定性。

---

## 6. 署名与许可 (Attributions & Licenses)

代码链与开源传承（自上而下）：  
[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → [**本仓库 (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project)。

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) — **GPL-3.0 License**：通用超分辨率与神经渲染代理框架；
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) — **GPL-3.0 License**：初始接入 DLSS-NR；
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — **GPL-3.0 License**：Pre-SR 超分前执行与 Multi-Pass 架构；
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — **GPL-3.0 License**：AMD Pre-SR 桥接方案；
- [**danielblnc / DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) — **Custom Non-Commercial / All Rights Reserved**：danielblnc 保留所有权利，禁止未经授权重新分发，本项目不随包分发其二进制，采用外部检测安装方式对接；
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — **MIT License**：开源 HIP 神经渲染算力核心与 71 块网络还原；
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **MIT License**：`dlssnr.hlsl` 色彩通道合成算法；
- [**本项目 (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **GPL-3.0 License**：多槽调度架构、主队列同帧同步执行、C-ABI 标准化运行时与 PR 反哺、0.3.1 状态冻结/恢复、双后端共存与智能安装器。

本项目不含 NVIDIA 专有二进制文件、danielblnc 安装工具或未授权分发资产。使用时请遵循各上游开源协议。

## 已知问题（1.9.2-alpha）

- **`lmxxf` 大分辨率输入：** `[DlssNr] DLSS5_FIT_LARGE` 默认开启，更大的 Color 会拟合到网络尺寸。此前帕鲁的秒级卡顿是每帧重复重建链路的 Bug，已修复；截至 2026-09-28，维护者未发现修复后 FitLarge 仍有问题，不再根据旧测量建议降低分辨率或关闭它。显式设为 `false` 时，输入宽不超过 2560、高不超过 1080，并且总像素不超过 1920×1080（例如 2024×848）；2560×1080 不会放行。
- **《赛博朋克 2077》色彩：** 旧版网络色相混入导致霓虹发棕的路径已调整；当前 Colour strength 0–1 保留游戏原色，>1 才向网络色混合。无需沿用旧版强制设为 0 的建议；该规则适用于所有游戏。
- **PDL：** 链式启动默认打开。驱动里没有 `hipExtModuleLaunchKernel` 时，把 `DLSS5_HIP_PDL` 设为 false 后重启。
