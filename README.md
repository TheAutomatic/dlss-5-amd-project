**中文** | [English](README.en.md)

# OptiScaler AMD pre-SR — 1.9.0

在 **OptiScaler** 上接入 **AMD 神经渲染**（DLSS5 on AMD），让 **纯 DLSS / XeSS 游戏**在 AMD 显卡上跑神经降噪；超分仍由 **FFX/FSR** 完成。

本项目 fork 自 **Matheus** 及其上游，并在其基础上接手维护演进。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## 📢 1.9.0 更新日志 (Changelog)（当前仅更新源码，Release 版将在细节填充及用户安装器升级后尽快发布）

本次 1.9.0 是一次**重大的架构级里程碑升级**。我们正式引入了开源的 [**`lmxxf` HIP 神经渲染后端**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)。

### 🚀 核心更新点

1. **全新引入 `lmxxf` 神经渲染后端**
   - **拥抱开源算力核心**：在兼容 danielblnc 版基础上，全新接入开源 HIP 神经渲染后端。
   - **同帧执行（Same-Frame Execution Contract）**：将输入录制、HIP 异步推理、输出屏障无缝嵌入在游戏主命令队列内超分辨率（Pre-SR）之前完成。相较 lmxxf 原版，理论上支持在现代虚幻引擎及《燕云十六声》等 FSR 后依然有复杂 GPU 活动进行帧渲染的游戏中实现真正的同帧神经渲染（DLSS5）。
   - **支持 DLSS / XeSS 游戏输入**：充分发挥 OptiScaler 的通用代理接入优势，无需游戏原生支持 FSR，直接拦截游戏原本发给 DLSS / XeSS 的输入缓冲（Color / Motion Vectors / Depth）送入 lmxxf 神经降噪，再转接 FFX/FSR 完成超分辨率重建，让仅支持 DLSS 的游戏也能在 AMD 显卡上享受 DLSS5 体验。
   - **双后端无缝兼容**：保持完整向后兼容，如需使用 Daniel 后端，仍可在 `OptiScaler.ini` 中通过 `NrBackend=daniel` 自由切换，后续将支持 Ins 菜单内切换。
   - **内存与稳定性优化**：优化 `fast_prefix` 加速模式，跳过无用的 201MB 噪声 Buffer 分配，显著降低主机内存占用与初始化耗时；强化伪装 NVIDIA（Fake NVAPI）时的 GPU LUID 智能匹配，避免多显卡或驱动欺骗时跨卡崩溃。
   - **⚠️ 分辨率支持限制与推荐档位**：注意当前 `lmxxf` **仅支持超分前渲染分辨率 ≤ 1080p** 的画面进行神经渲染。对应典型档位参考：
     - **4K 显示输出**：推荐使用 **FSR 性能档**（渲染分辨率 1080p）或超级性能档（720p）；若设为 4K 质量档（1440p 渲染）会超出当前模型切片架构上限。
     - **2K (1440p) 显示输出**：可使用 **FSR 质量档 / 平衡档 / 性能档**（渲染分辨率均在 1080p 及以下）。
     - **1080p 显示输出**：可使用 **1080p 原生** 或各类超分档位。

2. **安装器升级（即将更新）**
   - 兼容以 lmxxf 为后端的安装流程。若手动安装，只需将 `LmxxfNrRuntime.dll` 与 `native-game-tiled-assets` 权重文件夹直接放入游戏主程序目录即可，程序已内置自动识别。

3. **菜单（Ins Menu）全面净化与画质原生动态调参**
   - **智能菜单过滤**：在 `lmxxf` 模式下自动隐藏 Daniel 专属的无效选项（如 passes、slots、new wait、实验性 RTGI 等），避免设置混淆。
   - **排版与间距修复**：修复了 `Enable NR` 与 `AMD processing` 挤在同一行的布局 Bug，恢复清晰合理的垂直层级与间距。
   - **原生动态调参滑条**：在 Ins 菜单新增 `Detail strength`（细节/亮度强度）、`Colour strength`（色彩饱和校正）无级滑条，并支持 `Debug view` 实时可视化调试图，改动即时生效。

---

## 历史背景与架构说明

本项目是 AMD 神经渲染的**桥接层**。多轮实机诊断下来，桥接部分自身的开销大约在 **0.01～0.03 ms** 量级，可以认为几乎无额外性能损耗。

`1.9.0` = 本仓库当前版本；全新支持 `lmxxf` 开源后端（Daniel `0.3.1` / `0.3.0` 仍可通过配置兼容）。

> 不是神经核的重实现，也不是 ReShade 滤镜。  
> 路径：**游戏 DLSS/XeSS 输入 → 本仓库（Pre-SR 调度）→ DLSSNR（lmxxf / Daniel 0.3.1）→ FFX/FSR 超分**。

---

## 巨人的肩膀

| 上游 | 他们做了什么 | 本项目额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分代理（DLSS / FFX / XeSS） | 仍作为安装与运行主体 |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | 先把 DLSS 神经渲染接进 OptiScaler，再做成超分前多 pass | 继承其 OptiScaler 代码基底与 pre-SR 架构 |
| **[Matheus / dlss-5-amd](https://github.com/MatheusGViana/dlss-5-amd-project)** | 把 pre-SR 接到 AMD 运行时：DLSS 输入 → AMD NR → FFX | 在其基础上：默认 **3 槽**调度，尽量每帧 NR；相对原 repo **1.7.3** 版单槽旧基线约 **+33%**（33.5→44.5），去掉约 **8.7 ms**/帧 GPU 空转；对接 0.3.1 / 0.3.0；为新等待补状态冻结/恢复；安装器更兼容 XBOX PC。桥接开销实测约 **0.01～0.03 ms** 量级 |
| **[原项目 / 原作者 danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD 神经渲染运行时本体 | **不改核**，按原作者 0.3.1 / 0.3.0 调用；并为 0.3.1 **新等待**补上 D3D12 状态冻结/恢复（含空状态「空→空」还原），以便在 DLSS/XeSS 游戏上安全启用 |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | 开源 HIP 神经渲染运行时本体 | 将其深度集成至 OptiScaler 的 Pre-SR 同帧管线；将 D3D12 桥接拆解为细粒度三阶段微调度，支持在严苛渲染管线中同帧执行；跳过 201MB 冗余噪声分配；补齐 Fake NVAPI 下的 LUID 匹配；实现本地权重自发现与 Ins 菜单实时画质动态调参（Detail / Colour / Debug View） |

### 多槽：每帧都要 NR

降噪（DLSS5）插在画面路径上：拿到槽的那一帧，要等自己的降噪算完才能出图。本模组给每个还没算完的降噪任务留一块独立缓冲（**槽**）。**槽不够时，那一帧会整帧跳过降噪**——画面更快出来，但可能糊、闪。

Matheus 那条线更偏向少槽/跳帧换吞吐：NR 跟不上时，部分帧完全不做降噪。本项目改成默认多槽：尽量**每帧都挂上 NR**，并去掉单槽在提交后空等上一份 GPU 工作的开销（PresentMon 里约 **MsGPUWait 8.7 ms/帧**）。

| 配置（鬼武者类，4K FSR 超级性能（＝720p 渲染；锁 60 帧对照）） | 帧周期中位 | 大约 fps | MsGPUWait | 每帧 NR |
|---|---:|---:|---:|---|
| 单槽·每帧 NR（旧基线） | 29.82 ms | **33.5** | **8.69 ms** | 被上一帧卡住，吞吐上不去 |
| **本项目默认多槽** | 22.45 ms | **44.5**（约 **+33%**） | **≈ 0** | **尽量每帧都有 NR** |
| 原作者 0.3 原生（对照） | 22.35 ms | 44.8 | 0 | 原生路径本身不靠跳帧 |

- 在尽量**每帧 NR** 的前提下，相对单槽旧基线实测约 **+33%**（33.5→44.5），与原作者原生 0.3 同档；不是靠跳帧把数字做高。
- 变快靠的是调度：不再空等上一帧，也不再因满槽整帧丢掉降噪。神经核本身没有变快（`network` 仍约 12～13 ms @720p）。
- 鬼武者后续不限帧/不同场景的多槽观测大约在 **44～51 fps**；上表对照用的是同一时期 33.5 vs 44.5。

**游戏里槽位调几？** 默认 **3**。`DLSS Neural Rendering` → `NR slots` 可调（2–5，改完即生效，不用重启）。当前多轮测试，槽位本身不增加延迟，直接选 5 理论上不会有性能损耗。

| 实测（4K FSR 超级性能） | 2 槽 | 3 槽 |
|---|---:|---:|
| 鬼武者 | 19.50 ms，**0 跳过** | 19.49 ms，**0 跳过** |
| 燕云十六声（下称燕云） | 19.05–19.25 ms，**大量跳过 NR 帧**（呈现更快，但是无降噪帧） | 21.78–21.89 ms，**0 跳过** |

- 在鬼武者上，2/3 槽的帧周期与显示延迟落在重复测量波动内，**没有测出差异**；燕云极致画质下必须 **≥3** 才稳
- 燕云 A/B 会话里，2 槽阶段的日志计数器分别增加约 **1200 / 1440**，3 槽阶段为 0；段长与 PresentMon 的 45 秒窗口不同，不能据此计算跳过率  
  另一次独立的 1→5 槽会话里，2 槽的 60 秒阶段计数 **1800**，3/4/5 槽都是 0；两次会话的计数不作横向比较
- 同一 A/B 会话里，2 槽显示延迟为 47.6–47.9 ms，3 槽为 62.9–63.2 ms——前者伴随大量降噪跳过，不是同等工作的免费收益
- **4–5 槽已经在上述扫描中测过**，在该场景没有比 3 槽更快；尚未测到真正需要 4 或 5 槽的更重场景
- 每槽是**渲染分辨率**（DLSS 输入）的一张 FP16 纹理——4K 输出配超分质量档（1440p 渲染）约 29 MB，原生 4K 渲染才 66 MB——且**只按所选数量分配**
- ini 里 `AmdSlots` 也接受 `1`（只允许一帧同时降噪，接近旧单槽行为），菜单不提供
- `AmdEveryFrame` 默认 `true`，Ins 菜单为 **Every-frame**（与 Enable NR 同排），ini 的 `[DlssNr] AmdEveryFrame` 仍可改（多槽下平时不再靠它阻塞等待）

> **+33%** 指的是相对**原 repo 1.7.3 版**单槽硬等旧基线的调度收益；神经渲染本身没有因此变快。原作者 danielblnc 的运行时本身**没有跳帧问题**，主要面向**已支持 FSR 的游戏**。本项目相对 Matheus 的改动是**多槽调度**（以及安装/XBOX 兼容、0.3.1 适配与新等待状态恢复等），用来在 **DLSS / XeSS 游戏**上尽量做到每帧 NR，并消掉单槽空等——不只是适配 daniel 的 0.3.1。

---

## 安装

### 本压缩包里有什么

| 文件/目录 | 作用 |
|---|---|
| `OptiScaler.dll` | 本项目主体（安装时会改成你选的代理名） |
| `OptiScaler.ini` | 配置模板；`[DlssNr]` 段的选项（含 `AmdSlots`）都在这里 |
| `OptiScaler\` | FFX / XeSS / Agility 等依赖 |
| `Setup.bat` / `Setup.ps1` | 安装器（**双击 `Setup.bat`**） |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | 卸载器；Setup 会拷进**游戏目录**。在游戏目录里双击：先问是否保留老备份，再列出将删除的文件/文件夹，Y/N 确认 |
| `LmxxfNrRuntime.dll` | lmxxf HIP 神经渲染运行时核心库（使用 lmxxf 后端需放入游戏目录） |
| `native-game-tiled-assets\` | lmxxf 模型权重资源目录（完整包包含，或手动放入游戏目录） |
| `Licenses\` | 第三方许可 |
| `SHA256SUMS.txt` | 校验和 |
| `README.md` / `README.en.md` | 本文件 |

**不含**：NVIDIA 二进制、Daniel 闭源权重及原作者安装程序（若使用 Daniel 后端请见下节说明）。

### 第一步：你自己准备文件（本包不附带）

本压缩包 **只含** OptiScaler 这一层（`OptiScaler.dll`、依赖、安装器）。  
所需文件有两种准备方式：推荐放入 `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`，或者放入已经生成好的 `version.dll` + `dlssnr_on_amd_weights.bin`；**不用四个都准备**。都放在解压后和 `Setup.bat` 同一目录：

| 文件名 | 是什么 | 从哪来 |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | 原作者的 **0.3.1 / 0.3.0** 安装程序 | [原项目 Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 神经渲染运行库 | 部分最新游戏自带；也可自行从网上获取 |
| （可选）现成的 `version.dll` / `dlssnr_on_amd_weights.bin` | 已经生成过就可直接放 | 跑过一次原作者 setup 后会得到 |

**推荐做法：放 `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`。**

双击本包的 `Setup.bat` 时，若还没有 `version.dll` 或 `weights.bin`，会 **自动启动原作者 setup** 帮你生成（在选完游戏目录之后），然后再继续安装本项目。  
若 `nvngx_dlssnr.dll` 只在本包目录、游戏目录没有，安装器会在你选完游戏文件夹后 **自动复制一份进去**（原作者 0.3.0 会在游戏目录找它）。

**只认 0.3.0 / 0.3.1。** 其它版本装了也跑不了，安装器会直接拒绝。

### 第二步：运行安装器（推荐）

1. 解压本 Release 到任意目录。  
2. 把 `dlssnr_on_amd_setup.exe` 和 `nvngx_dlssnr.dll` 放进这个目录（和 `Setup.bat` 并排）。  
   已有现成的 `version.dll` / `dlssnr_on_amd_weights.bin` 也可以一并放上。  
3. **确认已关闭游戏**。  
4. **双击 `Setup.bat`** → 弹出 **文件夹选择框** → 选中 **游戏 exe 所在的文件夹** → 确定。  
5. 按提示选择 **注入用的代理 DLL**（默认 `dxgi.dll`；也可选 `winmm.dll`、`d3d12.dll`、`winhttp.dll`、`wininet.dll`、`dbghelp.dll`。**不支持 `dinput8.dll`**）。  
6. 若这时还缺 `version.dll` 或 `weights.bin`，安装器会 **自动启动原作者 setup** 生成；完成后继续装本项目。

**有时会让你选两次游戏目录，这是正常的，不是 Bug。**

第二次是**原作者 danielblnc 的安装工具**在问——本项目需要调用它生成 `version.dll` / 权重，所以会再要一次路径。两次都选同一个游戏文件夹即可。

**游戏文件夹**是放着游戏主程序的那个目录（安装 OptiScaler 用的同款路径）：

- 很多游戏在 `...\Win64\` 或 `...\Binaries\Win64\`  
- XBOX PC 商店版若选到只读的系统安装目录，安装器会拒绝并提示换可写目录  

安装器会做这些事：

| 来源 | 装到游戏目录后变成 |
|---|---|
| 本包 `OptiScaler.dll` | 你选的代理名（默认 `dxgi.dll`） |
| `dlssnr_on_amd_setup.exe` 生成的 `version.dll`（0.3.1 或 0.3.0） | `dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll` |
| 你的 `dlssnr_on_amd_weights.bin` | 原样复制 |

自动安装 **不会**把 `version.dll` 留在游戏目录里（那会和代理冲突）。若你想用 `version.dll` 这个名字注入，可走下面的手动安装。

旧电脑若不能弹窗，也可以在命令行里写路径：

```bat
Setup.bat "D:\Games\SomeGame\Binaries\Win64"
```

若游戏目录里已有旧的 OptiScaler / 其它注入 DLL，安装器会先列出并让你选择：取消 / **备份后移走再装** / 忽略（仅当不是你要覆盖的代理名）。默认不会静默覆盖。

### 第三步：游戏里打开 DLSSNR（自动 / 手动安装都一样）

1. 启动游戏。  
2. 按 **Insert（Ins）** 打开 OptiScaler 菜单。  
3. 找到 **DLSS Neural Rendering**，勾选 **Enable NR**（AMD 神经渲染）。同一行应显示运行时版本，如 `0.3.1`、`0.3.0` 或 `lmxxf`。  
4. 之后画面上走的就是 **DLSS5 神经降噪 + FFX/FSR 超分**。

其它 OptiScaler 用法（菜单快捷键、兼容性、更多 FG 选项）见： [**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki)。

> **可选（与 DLSSNR 无关）：** 下面折叠里是 **3 倍及以上多帧生成** 的两条外置方案，文件都不随本包分发。

<details>
<summary><strong>可选：3倍及以上多帧生成</strong>（Arturs / XeFG，点开）</summary>

两条方案都要自己下文件，本项目 **都不带**。若游戏在运行中，改完 ini 必须 **保存文件并重启游戏**。保持 `[FrameGen] External=false`（`true` 会关掉 Opti 的 FG）。**不要**两条一起开。

---

#### 1. Arturs（DLSS Enabler）

1. 从原作者获取 `dlss-enabler-headless.dll`（不要用第三方整合包）：  
   [artur-graniszewski/DLSS-Enabler](https://github.com/artur-graniszewski/DLSS-Enabler/releases) 或 [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)  
2. 文件必须叫这个名字，放到代理 / `OptiScaler.ini` 旁边的 **`OptiScaler\`** 子目录。  
3. 游戏**已有 DLSSG** 时：

```ini
[FrameGen]
External=false
Enabled=true
FGInput=nvngxfg
FGOutput=auto
FGNvngxReplacement=Arturs
```

   只有超分、没有 DLSSG 时，用 Wiki 的 `FGInput=upscaler` + `FGOutput=dlssg`。  
4. 日志出现 `Artur's initialized` 才算加载成功。

细节以 [OptiScaler Wiki · Frame Generation](https://github.com/optiscaler/OptiScaler/wiki) 和 Enabler 原作者说明为准。本项目不代发该 DLL。

---

#### 2. XeFG（XeMFG DP4A Unlocker）

`XeFGUnlock.asi` 和同名 `XeFGUnlock.ini` 来自 **OptiScaler 官方群「XeMFG DP4A Unlocker」帖**。本项目未内置文件，请自行获取。

1. 把这两个文件放到游戏目录 `OptiScaler\plugins\`（和 `libxess_fg.dll` 同一棵树）。不要加 `-loadlate`。  
2. 改的是游戏根目录的 **`OptiScaler.ini`**，不是 plugins 里那份插件 ini。游戏**已有 Streamline DLSS-FG** 时：

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

   `Path` 可保持 `auto`（默认就是 `OptiScaler\plugins`）。没有 DLSS-FG 时把 `FGInput` 改成 `upscaler`。  
   `InterpolationCount`：`1` = 2x，`2` = 3x，以此类推。本包不再把上限写死成 3（4x）。实际能到几倍，还受 Intel XeFG 能力和多帧生成插件限制，见下一步。  
3. 插件 ini（`XeFGUnlock.ini`）只写解锁开关，例如 `UnlockMFG=true`、`MaxInterpolatedFrames=3`。插件里的数字是解锁上限；游戏目录 `OptiScaler.ini` 里的 `InterpolationCount` 才是实际倍率。两边都要够，XeFG 自己也有能力上限。首轮可把 `DisableLogging=false`，旁边会出 `XeFGUnlock.log`。  
4. 建议先 2x 跑通 XeFG，再把倍率调高。可通过 Page Up 打开帧数显示后，按 Page Down 切换显示详情，确认多帧生成已生效。

</details>

---

### 手动安装（不用 Setup.bat）

适合已经熟悉「往游戏目录丢 DLL」的人。`version.dll` 来自 `dlssnr_on_amd_setup.exe`（原作者 **0.3.1 或 0.3.0**）。  
原项目详细步骤：[danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. 跑 `dlssnr_on_amd_setup.exe`，拿到 `version.dll` 和 `dlssnr_on_amd_weights.bin`（需要 `nvngx_dlssnr.dll` 才能生成 weights）。  
2. 把 `version.dll` **改名** 为 `dlssnr_amd_pass1.dll`，再复制两份为 `dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`（内容相同；至少要有 pass1）。  
3. 把 `dlssnr_on_amd_weights.bin` 放进同一游戏目录。  
4. 把本 Release **全部** 解压进同一游戏目录。  
5. 把 `OptiScaler.dll` **改名** 为要注入的名字：  
   - 常用：`dxgi.dll`（或 `winmm.dll` 等，**不要用 `dinput8.dll`**）  
   - 也可以直接改成 **`version.dll`**（和原作者同一代理名时尤其方便）  
6. **倒数第二步**：确认游戏目录里没有多余的、和注入名冲突的旧 `version.dll` / 旧代理 DLL（若你在第 5 步已把 OptiScaler 改成 `version.dll`，则不要再留一份原作者的 `version.dll`）。  
7. 进游戏，**Ins** 打开菜单，在 **DLSS Neural Rendering** 下勾选 **Enable NR**。

### 卸载

在**游戏目录**里双击 **`Uninstall_OptiScaler_NR.bat`**（Setup 会把它拷过去）。不要在安装包目录里选文件夹。若有 `backup-amd-presr-*` 老备份，先问是否保留（Y 保留 / N 一并删除）；然后列出计划删除的文件和文件夹，再输入 **Y 或 N** 确认（不区分大小写）。也检查 `_storage_`。

**默认保留**：`nvngx_dlssnr.dll`、`dlssnr_on_amd_weights.bin`、原作者 setup 与日志、非 OptiScaler 的同名代理，以及 `OptiScaler` 中额外添加的插件和文件。`backup-amd-presr-*` 按上面的选择处理。只移除已经清空的依赖目录，不会整目录删除 `OptiScaler`；因此卸载后该目录可能仍然存在。

---

## 排错 / 反馈问题

装完进游戏若菜单打不开、没有 DLSSNR、或画面异常，先按下面收集信息。**反馈时请一并附上这些内容**，否则很难判断是安装问题还是运行问题。

### 1. 先确认日志在哪

日志和代理 DLL（`dxgi.dll` / `winmm.dll` 等）在**同一目录**。常见文件：

| 文件名 | 谁写的 |
|---|---|
| `OptiScaler.log` | 本项目主日志 |
| `amd_bridge.log` | AMD 桥接层 |
| `amd_presr.log` | AMD pre-SR / NR 调度 |
| `dlssnr_on_amd.log` | 原作者运行时（0.3.1 / 0.3.0） |

**XBOX PC / 部分微软商店版游戏**可能因为文件系统映射，在游戏 exe 旁边另建一个名字类似 **`_storage_`** 的文件夹。  
若你安装时选的目录里找不到上述 `.log`，请到：

```text
<游戏 exe 所在目录>\_storage_\
```

再找一遍。安装文件（代理、`dlssnr_amd_pass1/2/3.dll`、weights）有时也会出现在那里——以**实际能写出日志的那个目录**为准。

### 2. 确认代理旁应有的文件

以你安装时选的代理名为准（例如 `dxgi.dll` 或 `winmm.dll`），同一目录里应有：

| 文件 | 说明 |
|---|---|
| 你选的代理（`dxgi.dll` / `winmm.dll` / …） | 本项目 OptiScaler |
| `dlssnr_amd_pass1.dll` | **必须**；原作者 0.3.1 或 0.3.0 |
| `dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll` | 多 pass 用；内容与 pass1 相同 |
| `dlssnr_on_amd_weights.bin` | **必须** |
| `nvngx_dlssnr.dll` | 常见；部分游戏自带 |

**不要**再留一份原作者的 `version.dll` 与代理并存（会双注入）。自动安装会把它挪走。

### 3. 游戏内自检

1. 启动游戏，按 **Ins** 打开 OptiScaler 菜单。  
2. 看 NR 状态是否显示：**`AMD NR runtime: 0.3.x`**（0.3.1 或 0.3.0）或 **`lmxxf`**。  
3. 若显示 waiting / 未识别 runtime / 没有该行，多半是 pass/runtime 或 weights 路径不对，回到上一节核对文件。

### 4. 反馈时请写清

请在 Issue / 反馈里写明：

1. **代理名**：`dxgi.dll`、`winmm.dll`，还是其它？  
2. **代理旁文件是否齐全**：pass1/2/3（或 `LmxxfNrRuntime.dll`）、weights（或 `native-game-tiled-assets` 文件夹）、（可选）`nvngx_dlssnr.dll`；有没有多余的 `version.dll`？  
3. **Ins 菜单**：NR 是否显示 `AMD NR runtime: 0.3.x` 或 `lmxxf`？  
4. **日志**：`OptiScaler.log`、`amd_bridge.log`、`amd_presr.log`、`dlssnr_on_amd.log`（若在 `_storage_` 请说明完整路径）。  
5. 游戏名、显卡、驱动版本，以及问题现象（打不开菜单 / 无降噪 / 卡顿 / 崩溃）。

---

## 署名与许可

代码链（由上到下）：[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → **本仓库**。

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler)（GPL-3.0）  
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR)（GPL-3.0）—— 本项目的 OptiScaler 代码基于它（`v0.2.0-dlssnr` / commit `97376162`）  
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) —— 在超分前跑神经渲染、多 pass 的架构来源  
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) —— AMD pre-SR 桥接  
- [**原项目 / 原作者 danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.1 / 0.3.0**（不随本包分发）  
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) —— 开源 HIP 神经渲染运行时与算力核心  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx)（MIT）—— `dlssnr.hlsl` 的色彩合成取自其 DLSS 5 神经渲染 addon，全文见 `Licenses/RenoDX_ATTRIBUTION.txt`  
- 本项目：NR 槽位、0.3.1 适配、新等待状态冻结/恢复、lmxxf HIP 运行时深度集成与 Pre-SR 同帧管线重构、安装器与打包  

本包不含 NVIDIA 二进制、原作者 setup、NR 权重、上游闭源 pass。请遵守各上游许可。
