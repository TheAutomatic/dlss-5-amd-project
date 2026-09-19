**中文** | [English](README.en.md)

# OptiScaler AMD pre-SR — 1.8.6-0.3.1

在 **OptiScaler** 上接入 **AMD 神经渲染**（DLSS5 on AMD），让 **纯 DLSS / XeSS 游戏**在 AMD 显卡上跑神经降噪；超分仍由 **FFX/FSR** 完成。

本项目 fork 自 **Matheus** 及其上游。上游已定稿最终版，本项目在此基础上接手维护，主要做了三件事：

1. 增加**多槽**，尽量每帧都做 NR；实测约 **+33%** 帧率  
2. 更新对 **danielblnc** 项目 **0.3.1** 的适配  
3. 为 0.3.1 **新等待**补上 D3D12 状态冻结/恢复（含空状态「空→空」还原），增强对**鬼武者**等游戏的兼容  

（详见下文「相比前人」「多槽」等节。）

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

（若你从网盘等渠道拿到本包，请以上述仓库为准。）

---

本项目是 danielblnc 运行时的**桥接层**。多轮实机诊断下来，桥接部分自身的开销大约在 **0.01～0.03 ms** 量级，可以认为几乎无额外性能损耗。

`1.8.6` = 本仓库当前版本；`0.3.1` = 主推的上游运行时（**0.3.0 仍可用**）。

**相对 1.8.5：** 根据玩**鸣潮、异环**等网友的实机反馈，Ins 菜单**补回 Every-frame 勾选**（1.8.5 菜单无此按钮，仍可通过 ini 的 `AmdEveryFrame` 修改）。无实际性能提升，只是把开关交回菜单，并更新排版。

**画面等待模式：默认 0.3.1 新等待（`AmdGraphicsWait=1`）。** 新等待会请求 0.3.1 的 1 像素 draw 等待（仍在测试）。仅在本帧 D3D12 状态可冻结、恢复准备就绪时才请求新等待，否则回退原等待；空图形状态会按空还原，不脏改游戏的 command list。这不代表运行中发生卡死、崩溃或设备移除后能自动恢复。

游戏内 **Ins → New wait**：关闭即原等待模式（无需重启游戏）；重新打开时若 hooks 或某个 pass 尚未就绪，菜单会提示重启。若新等待出现异常，请手动关闭；无法进入菜单时，先关闭游戏，将 `OptiScaler.ini` 的 `[DlssNr]` 中 `AmdGraphicsWait=0`（原等待），再启动游戏。

> 不是神经核的重实现，也不是 ReShade 滤镜。  
> 路径：**游戏 DLSS 输入 → 本仓库 → DLSSNR（0.3.1 / 0.3.0）→ FFX/FSR 超分**。

---

## 相比前人

| 上游 | 他们做了什么 | 本项目额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分代理（DLSS / FFX / XeSS） | 仍作为安装与运行主体 |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | 先把 DLSS 神经渲染接进 OptiScaler，再做成超分前多 pass | 继承其 OptiScaler 代码基底与 pre-SR 架构 |
| **[Matheus / dlss-5-amd](https://github.com/MatheusGViana/dlss-5-amd-project)** | 把 pre-SR 接到 AMD 运行时：DLSS 输入 → AMD NR → FFX | 在其基础上：默认 **3 槽**调度，尽量每帧 NR；相对原 repo **1.7.3** 版单槽旧基线约 **+33%**（33.5→44.5），去掉约 **8.7 ms**/帧 GPU 空转；对接 0.3.1 / 0.3.0；为新等待补状态冻结/恢复；安装器更兼容 XBOX PC。桥接开销实测约 **0.01～0.03 ms** 量级 |
| **[原项目 / 原作者 danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD 神经渲染运行时本体 | **不改核**，按原作者 0.3.1 / 0.3.0 调用；并为 0.3.1 **新等待**补上 D3D12 状态冻结/恢复（含空状态「空→空」还原），以便在 DLSS/XeSS 游戏上安全启用 |

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
| 燕云十六声（Where Winds Meet，下称 WWM） | 19.05–19.25 ms，**大量跳过 NR 帧**（呈现更快，但是无降噪帧） | 21.78–21.89 ms，**0 跳过** |

- 在鬼武者上，2/3 槽的帧周期与显示延迟落在重复测量波动内，**没有测出差异**；WWM 极致画质下必须 **≥3** 才稳
- WWM A/B 会话里，2 槽阶段的日志计数器分别增加约 **1200 / 1440**，3 槽阶段为 0；段长与 PresentMon 的 45 秒窗口不同，不能据此计算跳过率  
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
| `Licenses\` | 第三方许可 |
| `SHA256SUMS.txt` | 校验和 |
| `README.md` / `README.en.md` | 本文件 |

**不含**：NVIDIA 的二进制、NR 权重、原作者安装程序与闭源 pass——见下一节。

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
3. 找到 **DLSS Neural Rendering**，勾选 **Enable NR**（AMD 神经渲染）。同一行应显示原项目版本，如 `0.3.1` 或 `0.3.0`。  
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

`XeFGUnlock.asi` 和同名 `XeFGUnlock.ini` 来自 **OptiScaler 官方群「XeMFG DP4A Unlocker」帖**。本版用已有的 ASI 加载器加载，**不把解锁补丁合进本仓库、也不随 zip 分发**。走 **XeFG** 输出（仍要本包的 `libxess_fg.dll` / `libxell.dll`）；**不是** NVIDIA DLSSG，也 **不是** 上面的 Arturs。

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
2. 看 NR 状态是否显示：**`AMD NR runtime: 0.3.x`**（0.3.1 或 0.3.0）。  
3. 若显示 waiting / 未识别 runtime / 没有该行，多半是 pass 或 weights 路径不对，回到上一节核对文件。

### 4. 反馈时请写清

请在 Issue / 反馈里写明：

1. **代理名**：`dxgi.dll`、`winmm.dll`，还是其它？  
2. **代理旁文件是否齐全**：pass1/2/3、weights、（可选）`nvngx_dlssnr.dll`；有没有多余的 `version.dll`？  
3. **Ins 菜单**：NR 是否显示 `AMD NR runtime: 0.3.x`？  
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
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx)（MIT）—— `dlssnr.hlsl` 的色彩合成取自其 DLSS 5 神经渲染 addon，全文见 `Licenses/RenoDX_ATTRIBUTION.txt`  
- 本项目：NR 槽位、0.3.1 适配、新等待状态冻结/恢复、安装器与打包  

本包不含 NVIDIA 二进制、原作者 setup、NR 权重、上游闭源 pass。请遵守各上游许可。
