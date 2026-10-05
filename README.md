**中文** | [English](README.en.md) | [Español](README.es.md)

可选实验性 **SR → NR**：Ins 的 **Processing order** 可切换超分与 NR 的顺序，
Daniel、lmxxf、Mochizuki 共用，默认仍为 NR → SR。SR 后按输出分辨率处理，可能增加显存和耗时；
使用方法、限制及验收见 [SR 后 NR](docs/post-sr-nr.md)。

Mochizuki 后端适用于 Windows / RDNA4，安装、模型来源、独立菜单和验证范围见 [mochizuki 说明](docs/mochizuki.md)。不附带 NVIDIA DLL 或模型；已有本地游戏测试，其他游戏与场景仍需验证。


# OptScaler(NR) 1.10.2
**特别感谢**：各位 Bilibili 用户的测试与反馈意见。

在 **OptiScaler** 上接入 **AMD 神经网络渲染**（DLSS5 on AMD），让 **纯 DLSS / XeSS 游戏** 在 AMD 显卡上跑神经网络降噪；超分辨率仍然由 **FFX/FSR** 完成。

本项目 fork 自 **Matheus** 及上游社区。在上游成熟方案的基础上持续深度研发与维护。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

## 更新日志

更新日志详见 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases)

---

## 目录
- [1. 巨人的肩膀](#1-巨人的肩膀)
- [2. 安装指南 (Installation Guide)](#2-安装指南-installation-guide)
  - ├─► [⚡ 极速简易安装教程](#quick-start)
  - ├─► [完整详细安装说明与高级选项](#完整详细安装说明与高级选项)
  - └─► [可选：3倍及以上多帧生成 (Frame Generation)](#可选功能3倍及以上多帧生成frame-generation)
- [3. 三后端架构与历史性能实测](#3-三后端架构与历史性能实测)
- [4. 游戏内设置与控制](#4-游戏内设置与控制)
- [5. 排错、日志定位与卸载](#5-排错日志定位与卸载)
- [6. 署名与许可 (Attributions & Licenses)](#6-署名与许可-attributions--licenses)

---

## 1. 巨人的肩膀

本项目并非凭空产生，而是建立在开源图形社区众多先驱者的卓越成果之上：

| 上游与先驱 | 他们做了什么 | 本项目额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分辨率代理框架（支持 DLSS / FFX / XeSS 输入输出） | 作为整体安装与运行主体，提供通用注入、Hook 与配置界面 |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | 首次把 DLSS 神经渲染接进 OptiScaler，并提出在超分前运行多 pass 的 Pre-SR 架构 | 继承其 OptiScaler 代码基底与 Pre-SR 调度管线 |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | 将 Pre-SR 接到 AMD 运行时：游戏 DLSS 输入 → AMD NR → FFX 超分；引入 [RenoDX](https://github.com/clshortfuse/renodx) 的 OkLab 与双分支色调映射改善高光偏色 | 在此基础上首创**多槽调度（Multi-slot）**，消除了单槽空等 **8.7 ms/帧** 的 GPU 挂起；适配 0.3.1；补全新等待 D3D12 状态冻结/恢复；增强 XBOX PC 兼容性。**桥接开销实测仅 0.01～0.03 ms** 量级 |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD 神经渲染运行时本体（0.3.0–0.6.0） | **不改动其核心**，按规范接口调用；并针对 0.3.1+ 的 1 像素 Draw 等待补齐状态保护，确保在 DLSS/XeSS 游戏上安全运行 |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | 逆向恢复 71 块网络并移植到 AMD HIP 的开源神经渲染算力核心 | **接入 OptiScaler 通用代理框架以兼容更多纯 DLSS / XeSS 游戏**；实现主队列同帧同步执行；开发标准版本化 C-ABI 独立运行时（`LmxxfNrRuntime` 并反哺合并至上游）；增加动态色彩/细节无级滑条等 |
| **[Mochizuki / DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)** | 纯开源 Vulkan / RDNA4 神经渲染网络核心与 SPIR-V 着色器体系 | 深度接入多后端架构并重构生命周期安全：解决跨 API 在无关游戏队列上的死等卡顿与显存泄漏；整合着色器取消机制与 26.9.2+ 驱动修复（由 [@MatheusFerreiraS](https://github.com/MatheusFerreiraS) 提供） |

使用 danielblnc 0.6.0 的 RX 6000（RDNA2）显卡需要安装 AMD HIP 7.2 runtime。

---

## 2. 安装指南 (Installation Guide)

### <span id="quick-start"></span>⚡ 极速简易安装教程（三后端通用）

1. **下载解压**：从 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases) 下载本项目最新 zip 压缩包，解压至任意非中文路径。
2. **收集必备外部文件**（复制到与 `Setup.bat` 同一文件夹下，即 **1 个文件夹 + 1 个 DLL + 1 个 EXE**）：
   - `nvngx_dlssnr.dll`（NVIDIA 原生降噪动态库，当前**仅支持 310.8.0** 版本）；
   - [`native-game-tiled-assets` 文件夹](https://gofile.io/d/RyvcrDxz)（lmxxf 权重目录，解压后包含权重文件）；
   - [`dlssnr_on_amd_setup.exe`](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)（Danielblnc 所需的安装/提取程序）。
   > 💡 **提示**：若计划使用 **Mochizuki** 后端，请提前在微软商店（Microsoft Store）搜索并安装 **Python 3.10+**。
3. **运行安装器并选择游戏**：
   - 双击运行 `Setup.bat`，在弹出的窗口中选择**游戏的实际主程序运行目录**（注意：必须是游戏 exe 实际执行的所在目录，例如虚幻引擎游戏通常为 `...\<GameName>\Binaries\Win64\`，而非平台 Launcher 或外层快捷方式目录）；
   - 按交互提示选择代理 DLL（通常可选 `dxgi.dll`，无法使用时可尝试 `winmm.dll` 或其他注入方式）与后端进行安装。若此前已安装过旧版本，升级推荐选择覆盖安装。
4. **注：升级安装时无需执行步骤 2**：
   - 如果你曾经在目标游戏中安装过本项目及对应后端权重，后续升级新版本时无需执行步骤 2，安装工具将自动识别并带回安装工具所在文件夹。

---

### 完整详细安装说明与高级选项

<details>
<summary><strong>📖 点击展开：完整详细安装说明与高级选项（文件清单、各后端独立准备、卸载机制与手动部署）</strong></summary>

<details>
<summary><strong>📦 点击展开：压缩包内文件清单</strong></summary>

| 文件/目录 | 作用 |
|---|---|
| `OptiScaler.dll` | 本项目主体（安装时会自动重命名为你选择的代理名称） |
| `OptiScaler.ini` | 核心配置文件（包含 `[DlssNr]` 三后端切换与参数选项） |
| `OptiScaler\` | 核心依赖库（FFX / XeSS / Agility SDK / 插件等） |
| `LmxxfNrRuntime.dll` | lmxxf 后端运行时（开源 HIP 神经渲染） |
| `MochizukiNrRuntime.dll` / `dlssnr-amd/shaders/` / `Mochizuki-Model.bat` | Mochizuki runtime、着色器及模型提取工具；模型另备 |
| `lmxxf-modules\` | lmxxf 双架构算子模块（`gfx1200` / `gfx1201` 各 38 个 `.hsaco`，附 `SHA256SUMS` 清单） |
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

#### 第一步：准备对应后端的文件（详细说明）

可以准备以下任意后端，或同时安装多个后端：

##### 选项 A：[准备 `lmxxf` 后端文件](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) 或[点击这里](https://gofile.io/d/RyvcrDxz)获取权重文件
- 使用本项目完整包内配套的 `LmxxfNrRuntime.dll`（不要混用上游 ABI1 runtime）；
- 算子模块目录 `lmxxf-modules\`（官方双架构两层目录结构，包含 `gfx1200` [9060 系列，实验性] 与 `gfx1201` [9070 系列，正式生产] 两个子目录，各含 38 个 `.hsaco` 算子模块、叶子清单与根 `SHA256SUMS` 清单，共 76 个模块；运行时由 D3D12/HIP 设备智能自动匹配，安装器校验完整双包并支持旧版覆盖升级）；
- 着色器目录 `shaders\`（包含 `native_codec_encode.hlsl` 等）；
- 模型权重目录 `native-game-tiled-assets\`（可[点击这里](https://gofile.io/d/RyvcrDxz)直接下载）；
- 将上述文件/文件夹放在与 `Setup.bat` 相同的解压目录下。

升级时直接运行新包的 `Setup.bat` 并选择游戏目录。检测到已有 OptiScaler 后，安装器会建议先卸载，以避免新版文件、模块布局和旧设置冲突：输入 **Y（推荐）**会自动调用新包卸载器，再继续安装；输入 **N** 则直接覆盖安装。卸载会重置 OptiScaler 设置，保留权重和已有备份。正常覆盖不再备份旧 DLL、INI 或整套模块；模块切换仅临时保留旧目录，成功后清理、失败时恢复。用户自加且不能混入新版模块集的 `.hsaco` 等内容单独保存在安装结束时显示的 `backup-amd-presr-*/lmxxf-modules`，其他兼容的用户文件继续保留。


##### 选项 B：[准备 `danielblnc` 后端文件](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- 准备 `dlssnr_on_amd_setup.exe` 与 `nvngx_dlssnr.dll`（推荐，可从 [danielblnc Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) 获取，安装器会自动调用生成 weights）；
- 或者放入已经生成好的 `version.dll` 与 `dlssnr_on_amd_weights.bin`；
- 同样放在与 `Setup.bat` 相同的解压目录下。

---

##### 选项 C：准备 Mochizuki（Windows / RDNA4）

完整包已包含 `MochizukiNrRuntime.dll` 和 `dlssnr-amd/shaders/`。另外准备自己的 `nvngx_dlssnr.dll` **310.8.0**（当前仅支持该版本，SHA256 为 `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`），放在 `Setup.bat` 旁；安装 Python **3.10+**，运行 Setup 并选择 Mochizuki，安装器会提取和验证模型。已有模型时可直接放入 `dlssnr-amd/dlssnr.bin`，无需重新提取。完整校验值和独立提取方法见 [Mochizuki 安装说明](docs/mochizuki.md)。

没有源 DLL 或模型时，Setup 会提示 `MODEL SETUP REQUIRED`；只有 runtime 不代表可以运行。其他版本的 DLL 不自动尝试提取。模型损坏时先把旧 `dlssnr.bin` 移走，再运行 Setup；安装器不会静默覆盖它。

首次进入游戏可能需要数分钟编译网络，右下角显示阶段和进度；期间显示原始画面。改变分辨率、模型比例或叠层容量可能再次编译。Mochizuki 有独立的输入预处理、三种风格、1–3 遍叠层和时序控制；默认叠层 1。Ins/INI 中 `Mochizuki*` 参数不影响另外两个后端。需要排错时先看 Ins 的缺失依赖/编译状态，再看 `OptiScaler.log`。

#### 第二步：运行安装器（详细流程）

1. 解压本 Release 包到任意临时目录；
2. 将准备好的后端文件与 `Setup.bat` 放在同一目录下；
3. **确认已完全退出游戏**；
4. **双击运行 `Setup.bat`**：
   - 弹出文件夹选择框，选中 **游戏主程序 exe 所在的目录**（例如 `...\Binaries\Win64\`）；
   - 若检测到已有 OptiScaler，输入 **Y** 自动卸载后安装（推荐），或输入 **N** 覆盖安装；
   - 按照提示选择你要注入的 **代理 DLL 名称**（默认为 `dxgi.dll`，推荐；也支持 `winmm.dll`、`d3d12.dll` 等，**不要选 `dinput8.dll`**；卡普空 RE 引擎游戏请提前自行安装相应补丁）；
   - 安装器显示三后端及权重检测结果；选择单个后端或安装全部可用后端，再选择当前启用的后端；
   - 安装器自动处理重命名、防双重注入清理、依赖部署，并配置 `OptiScaler.ini`。

---

#### 第三步：手动安装（高级玩家）

若你熟悉游戏模组手动放置，可直接将文件拷贝至游戏主程序目录：
1. 将 `OptiScaler.dll` 重命名为你选择的代理名称（如 `dxgi.dll`）放入游戏目录；
2. 将 `OptiScaler.ini` 和 `OptiScaler\` 依赖文件夹复制到游戏目录；
3. **部署后端**：
   - **若使用 `lmxxf`**：将 `LmxxfNrRuntime.dll`、`lmxxf-modules\`、`shaders\`、`native-game-tiled-assets\` 放入游戏目录；
   - **若使用 `danielblnc`**：将 danielblnc 的 `version.dll` 复制三份，分别命名为 `dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`；将 `dlssnr_on_amd_weights.bin` 放入游戏目录（**切勿保留名为 `version.dll` 的 danielblnc 文件**，以免冲突）；
   - **若使用 `mochizuki`**：复制 `MochizukiNrRuntime.dll`、`dlssnr-amd/shaders/` 及自己的 `dlssnr-amd/dlssnr.bin`；
4. 打开 `OptiScaler.ini`，在 `[DlssNr]` 中设置 `Enabled = true`，并通过 `NrBackend = lmxxf`、`NrBackend = daniel` 或 `NrBackend = mochizuki` 指定当前生效的后端。

</details>

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
| `NrConvenience` | `1` | `1`：装了 lmxxf 或 mochizuki 时预开提交代理，三后端（daniel ↔ lmxxf ↔ mochizuki）可会话内实时热切换；`0`：只启所选后端，换后端需重启游戏。改后重启生效。 |

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

> **注意：lmxxf 与 Mochizuki 后端的日志在哪？**
> 与 `danielblnc` 后端写入独立的 `dlssnr_on_amd.log` 不同，`lmxxf` 与 `mochizuki` 后端的日志已直接接入统一日志系统，其所有初始化、状态检测、网络编译与运行报错均**集中记录在 `OptiScaler.log`（以及 `amd_bridge.log`）中**，无需查找额外日志文件。

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

#### 3. `mochizuki` 后端专属排错
- **状态栏提示缺失依赖或无法启用**：
  - 打开 `OptiScaler.log`，搜索 `mochizuki` 关键字，或在 Ins 菜单中查看缺失依赖提示；
  - 检查游戏目录是否缺失 `MochizukiNrRuntime.dll`；
  - 检查 `dlssnr-amd/shaders/` 着色器目录是否完整，以及是否存在 `dlssnr-amd/dlssnr.bin` 模型文件；
  - 首次进入游戏可能需要数分钟编译网络，右下角会实时显示编译阶段与进度，编译完成前游戏显示原始画面。
- **环境依赖排查**：
  - 确保安装了支持 Vulkan 的 AMD 显卡驱动；初次提取模型需在微软商店安装 Python 3.10+。

#### 4. 微软商店版 / XBOX PC 特殊提示
由于系统文件虚拟化映射，部分微软商店或 XBOX PC 游戏会在游戏 exe 同级生成名为 **`_storage_`** 的文件夹，日志与生成文件可能会写入此处，请在此目录同步排查。

### 三、问题反馈格式
若遇到无法解决的崩溃或异常，提交 Issue 时请提供：
1. 注入代理名称（如 `dxgi.dll`）；
2. 所选后端（`lmxxf`、`daniel` 还是 `mochizuki`）；
3. 显卡型号、操作系统版本与 AMD 驱动版本；
4. 游戏名称与输出分辨率、超分档位；
5. 附带完整的上述 `.log` 日志文件。

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
- [**Mochizuki / DLSSNR-AMD**](https://github.com/mochizuki0323/DLSSNR-AMD) — **MIT License**：纯开源 Vulkan / RDNA4 神经渲染核心网络与 SPIR-V 着色器体系；
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **MIT License**：`dlssnr.hlsl` 色彩通道合成算法；
- [**本项目 (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **GPL-3.0 License**：多槽调度架构、主队列同帧同步执行、C-ABI 标准化运行时与 PR 反哺、0.3.1 状态冻结/恢复、双后端共存与智能安装器。

本项目不含 NVIDIA 专有二进制文件、danielblnc 安装工具或未授权分发资产。使用时请遵循各上游开源协议。
