**中文** | [English](README.en.md) | [Español](README.es.md)

# OptiScaler(NR)

> [!IMPORTANT]
> **源码公开 · 完全免费 · 严禁倒卖与抹除署名**：本项目启动器与脚本基于 **[PolyForm Noncommercial License 1.0.0](LICENSE)** 开源，仅限个人非商业使用，严禁有偿售卖、收费整合包/网盘/群分发及收费代安装。**二次修改/衍生分发须严格遵守 [NOTICE](NOTICE)**：允许署二改作者名，但必须完整保留原始作者（`TheAutomatic`）与全部上游作者署名、官方仓库链接及 `Required Notice:` 声明，严禁移除署名或篡改二进制信息/版本号冒充原创。如付费购买请立即申请退款并举报卖家！详见[第 6 节：免费声明、署名与非商业许可](#6-免费声明署名与非商业许可)。
>
> **🎉 v2.1 架构与功能概览**：采用 **“官方原版 OptiScaler + 独立双 `.asi` 插件（`dlssnr-amd.asi` + `openrr-amd.asi`）+ 图形化一键启动器”** 解耦架构，全面支持 **OptiScaler / 独立直通 (Standalone) / ReShade 兼容（D3D9–D3D12 & Vulkan，含 32/64 位游戏）/ RTX Remix for Minecraft (Java 版)** 四大部署模式，并实验性提供 **Linux / SteamOS (Proton)** 独立发行包与图形启动器。
> 若需查阅 `1.x` 旧版历史说明文档，请点击查看 **[旧版中文说明 (Legacy README)](docs/legacy-readme/README.md)**。

**特别感谢**：各位用户的测试与反馈意见，以及 Bilibili 数十位热心网友共同参与设计水獭品牌图标！

本项目提供 **Fluent UI** 图形化启动器（Windows **`OptiScalerNRLauncher.exe`** / Linux **`OptiScalerNR-Launcher.sh`**），支持自动扫描 Steam / Epic / GOG / XBOX / Heroic / Lutris 游戏库、结合 PE 导入表与 OptiScaler Wiki 智能推荐部署模式与注入代理、一键离线提取/合成模型权重，按需勾选并部署 **官方原版 OptiScaler**、两大 AMD 神经网络插件——**DLSS 神经渲染 (`dlssnr-amd.asi`)** 和 **DLSS 光线重建 (`openrr-amd.asi`)** 以及 **RTX Remix for Minecraft AMD 补丁**，同时内置 INI 配置预设、Mochizuki 快捷键自定义与 `MZ5` 画风分享码管理，让 **AMD 显卡** 在各类 DX9 / DX10 / DX11 / DX12 / Vulkan 游戏中原生运行 DLSS 神经渲染与 DLSS 光线重建（Ray Reconstruction）。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## 目录
- [1. 核心架构与上游致谢](#1-核心架构与上游致谢)
- [2. ⚡ 极速安装指南 (Windows & Linux)](#2--极速安装指南-windows--linux)
  - ├─► [Windows 图形化启动器安装（推荐）](#windows-图形化启动器安装推荐)
  - └─► [Linux / SteamOS (Proton) 图形启动器（实验性）](#linux--steamos-proton-图形启动器实验性)
- [3. 双 `.asi` 插件核心能力说明](#3-双-asi-插件核心能力说明)
- [4. 游戏内快捷键与菜单控制](#4-游戏内快捷键与菜单控制)
- [5. 兼容修复、反馈问题日志与卸载](#5-兼容修复反馈问题日志与卸载)
- [6. 免费声明、署名与非商业许可](#6-免费声明署名与非商业许可)

---

## 1. 核心架构与上游致谢

`2.x` 系列不再维护定制修改版 `OptiScaler.dll`，全面转向**官方原版 OptiScaler 桥接层 + 独立 `.asi` 插件按需挂载 + 多兼容路线**的解耦设计：

| 核心组件 | 上游与来源 | 作用与特性 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>（官方原版桥接框架） | 上游官方 Nightly 构建（校验 SHA-256，支持自行替换其他第三方构建） | 负责通用超分辨率转换（DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS）与帧生成（DLSSG → FSR3 FG / XeFG），并通过内置 ASI Loader 加载 `OptiScaler\plugins\*.asi` |
| **`dlssnr-amd.asi`**<br>（DLSS 神经渲染插件） | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | 在 AMD **RX 9000 (RDNA4)** 与 **RX 7000 / 700M / 800M / 8000S (RDNA3)** 上运行 DLSS 神经渲染；支持 **D3D12 Direct-ELF**、**D3D11→D3D12 桥接** 与 **Vulkan ACO**，提供 OptiScaler / Standalone / 32 & 64 位 ReShade 路线及 RenoDX / wilsjo2 双风格 |
| **`openrr-amd.asi`**<br>（DLSS 光线重建插件） | **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | 在 AMD **RDNA4（已实测）** 与 **RDNA3 / RDNA3.5（实验性）** 上原生运行 NVIDIA **DLSS 光线重建（Ray Reconstruction / DLSS-RR）** 网络；支持 **D3D12 Direct-ELF** 与 **Vulkan**，可与 `dlssnr-amd.asi` 及 ReShade / RenoDX 共存 |
| **[rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)**<br>（RTX Remix for Minecraft 兼容补丁） | **[NVIDIAGameWorks / rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)**（上游 Apache-2.0）<br>**[TheAutomatic / rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** | 自动扫描 PrismLauncher / `.minecraft\remix-sdk` 实例；原名部署 `OptiScaler.dll`、自动备份并应用解锁 AMD 硬件 **Wave32 稀疏光追（~2.5× 路径光追提速）** 的 `remixbridge.dll`（卸载可无损还原），并自动启用 `VulkanExtensionSpoofing = true` |
| **[ReShade](https://github.com/crosire/reshade) / [DXVK](https://github.com/doitsujin/dxvk) / [Vulkan Loader](https://github.com/KhronosGroup/Vulkan-Loader)**<br>（兼容模式运行库） | 随 `DLSSNR-AMD` 聚合分发（保留各自开源协议，见 `Licenses/`） | 为没有原生 DLSS / FSR / XeSS 的 D3D9 / D3D10 / D3D11 / D3D12 / Vulkan（含 32 位与 64 位）老游戏提供 ReShade Feeder 运动矢量喂入与兼容层支持 |

---

## 2. ⚡ 极速安装指南 (Windows & Linux)

### Windows 图形化启动器安装（推荐）

1. **下载解压**：从 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases) 下载 `OptiScaler-NR-Win-<版本>.zip` 并完整解压。
2. **启动安装器**：双击运行解压目录下的 **`OptiScalerNRLauncher.exe`**，自动打开 **`OptiScaler NR` 图形化启动器**。
3. **选择游戏与部署模式**：
   - 在左侧游戏库中选择目标游戏（也可点击右上角 **`+`** 手动添加游戏渲染 `.exe` 或 RTX Remix MC 实例；注意虚幻引擎游戏通常位于 `<Game>\Binaries\Win64\*-Shipping.exe`）；
   - 启动器会自动结合 PE 位数/导入表与 OptiScaler Wiki 推荐最合适的部署模式（**OptiScaler** / **独立直通 Standalone** / **ReShade 兼容［支持 32 位与 64 位］** / **RTX Remix for Minecraft**）与注入代理 DLL（通常为 `dxgi.dll` 或 `winmm.dll`）。
4. **按需勾选要安装的 `.asi` 插件**：
   - **[☑] `dlssnr-amd.asi`（DLSS 神经渲染 · Mochizuki）**：
     - 首次使用时，若提示缺少模型源，点击 **“导入模型源文件…”** 选择自备的 `nvngx_dlssnr.dll`（支持 `310.9.16`［含新旧双模型］或 `310.8.0`［含原版模型］）。启动器会将其保存至启动器目录，并在安装时自动提取与合成模型权重，后续给其他游戏安装直接复用。
   - **[☑] `openrr-amd.asi`（OpenRR 光线重建）**：
     - 仅支持原生包含 NVIDIA 光线重建的游戏。若本机游戏目录中已含 `310.9.1.0` 版 `nvngx_dlssd.dll` 会自动识别复用；若提示缺少模型源，点击卡片右侧 **“导入 nvngx_dlssd.dll…”** 选择一次 **`310.9.1.0`** 版 `nvngx_dlssd.dll` 即可全局复用。
   - **[☐] 安装 OptiPatcher（可选）**：按 Wiki 与内置支持表自动预选，用于解锁 270+ 款游戏的原生 DLSS 选项。
5. **一键安装**：点击右上角的 **“安装 OptiScaler NR”**（或 **“更新 OptiScaler NR”**）按钮即可完成部署。

---

### Linux / SteamOS (Proton) 图形启动器（实验性）

从 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases) 下载 Linux 包（基于 Mochizuki 上游 [DLSSNR-AMD Linux 版](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases)，与上游一样分为两个架构版本）：

| 包 | 适用 | 安装路线 |
| --- | --- | --- |
| `OptiScaler-NR-Linux-x86_64-<版本>.tar.gz` | 64 位游戏 | OptiScaler（含 Mochizuki NR + OpenRR 0.1.7plus6 + OptiPatcher）+ 三条 ReShade Feeder 路线 |
| `OptiScaler-NR-Linux-i686-<版本>.tar.gz` | 32 位游戏 | 仅 ReShade Feeder 路线（OptiScaler 与 OpenRR 只有 64 位） |

```bash
tar -xzf OptiScaler-NR-Linux-x86_64-<版本>.tar.gz && cd OptiScaler-NR-Linux-x86_64-<版本>
./OptiScalerNR-Launcher.sh                 # 图形启动器（需要 python3 ≥ 3.8；自动扫描 Steam / Heroic / Lutris）
./OptiScalerNR-Launcher.sh --install-desktop   # 添加到应用菜单
bash uninstall.sh "/path/to/game/folder"   # 卸载（--all 卸载启动器记录的全部游戏）
```
上游 `install.sh` 原样保留，也可不经启动器直接使用（见包内 `README-Linux.md` / `README.txt`）。安装完成后，把启动器显示的**启动选项**复制到 Steam › 游戏属性 › 通用 › 启动选项，例如：
```text
WINEDLLOVERRIDES="dxgi=n,b" %command%
```

---

## 3. 双 `.asi` 插件核心能力说明

### 一、`dlssnr-amd.asi`（Mochizuki · DLSS 神经渲染）
- **支持显卡**：
  - **AMD Radeon RX 9000 系列 (RDNA4)**：支持全部 4 套网络（`CC_SMALL_FP8`、`CC_SMALL_FP8_EXPERIMENTAL`、`CC_SILVER_AARDWOLD`、`CC_SILVER_AARDWOLD_OPTIMIZED` 含 INT4 加速层）。
  - **AMD Radeon RX 7000 系列 & Radeon 700M / 800M / 8000S (RDNA3 / RDNA3.5)**：同样支持全部 4 套网络（需通过预编译 RDNA3 ACO 机器码运行）。
- **多渲染后端与兼容路线**：支持 **D3D12 Direct-ELF**（零跨设备拷贝原生命令列表执行）、**D3D11 → D3D12 桥接** 与 **Vulkan 桥接/原生路径**，覆盖 OptiScaler / Standalone / 32 & 64 位 ReShade Feeder 路线。
- **双后处理风格**：支持 **RenoDX 风格**（默认，还原 RenoDX 色调映射）与 **wilsjo2 风格**（原版 OptiScaler-NR 风格），支持在启动器中一键导入/导出 **`MZ5` 画风分享码**。

### 二、`openrr-amd.asi`（OpenRR · DLSS 光线重建）
- **支持显卡**：**AMD Radeon RX 9000 系列（RDNA4，已实测）** 与 **RX 7000 / 700M / 800M / 8000S 系列（RDNA3 / RDNA3.5，实验性）**，需搭配 `nvngx_dlssd.dll`（`310.9.1.0`）。
- **核心功能**：接管游戏的 `NVSDK_NGX_Feature_RayReconstruction`（DLSS 光线重建）调用，在原生支持路径追踪 / 光线重建的游戏（如《赛博朋克 2077》《心灵杀手 2》《夺宝奇兵：古老之圈》《Minecraft RTX Remix》等）中运行神经网络光线重建降噪。
- **双后端与无缝共存**：支持 **D3D12 Direct-ELF** 与 **Vulkan**；与 `dlssnr-amd.asi`、ReShade / RenoDX 良好共存，并兼容 Linux / Proton（`vkd3d-proton`，需 Mesa RADV ≥ 26.3）。
- **显卡伪装（Spoofing）策略**：默认按游戏启用全局伪装（`full`，将 `OptiScaler.ini` 中 `[Spoofing]` 的 `Dxgi`、`Vulkan`、`VulkanExtensionSpoofing` 设为 `true`）；若部分游戏在开启全局伪装后出现兼容问题，可在 OpenRR 卡片勾选 **“减少全局显卡伪装”**（命令行对应 Windows `-OpenRrSpoof full|auto` / Linux `--openrr-spoof full|auto`），将该三项改为 `auto`（关闭 OpenRR 时同样恢复为 `auto`）。选择保留现有 `OptiScaler.ini` 时也仅更新这三项，不改动其余自定义配置；命令行未显式指定时自动沿用上次记录的策略。

---

## 4. 游戏内快捷键与菜单控制

进入游戏并在画面设置中开启 **DLSS**（或 FSR / XeSS）及 **光线重建（Ray Reconstruction，若游戏支持）** 后，默认快捷键如下（Windows 启动器已内置 **Mochizuki 快捷键自定义菜单**，且会在主界面插件行内联提示当前快捷键）：

| 快捷键 | 呼出的菜单 / 功能 | 说明 |
|---|---|---|
| **`Insert (Ins)`** | **OptiScaler 主菜单** + **OpenRR 侧边栏** | 调节超分辨率（FSR / XeSS）与帧生成；若安装了 `openrr-amd.asi`，**OpenRR 设置面板会跟随 `Insert` 键同步在右侧弹出/收起** |
| **`End`** | **NR Control（Mochizuki 神经渲染控制面板）** | 调节 `dlssnr-amd.asi` 的全部参数：实时切换 4 套神经网络、切换 RenoDX / wilsjo2 风格、配置多渲染层（NR 链）与 Pass 次数、开启 HUD 修复与显存/性能诊断 |
| **`Ctrl + F11`** | 快捷循环切换 NR 网络 | 在已安装的神经渲染模型之间一键切换 |
| **`F5`** | NR 前后对比截图 | 同时保存同一帧开启/关闭 NR 的对比截图至游戏目录 `NR Screenshots\` |

---

## 5. 兼容修复、反馈问题日志与卸载

1. **配套运行库兼容修复（XeSS / FidelityFX）**：
   - 若游戏自带的旧版 `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` 导致超分或帧生成异常，可在启动器中选中游戏后点击 **“兼容修复”**，一键备份原文件并替换为包内新版；随时可点 **“恢复原文件”** 无损还原。
2. **一键反馈问题日志**：
   - 遇到任何问题时，点击主界面顶部的 **“反馈问题日志”** 按钮，启动器会自动收集游戏目录下的 `OptiScaler.log`、`dlssnr-amd.log`、`openrr-amd.log`、配置文件、系统硬件/驱动信息与最近两天的驱动崩溃事件，打包成 `.zip` 生成到桌面。
3. **干净卸载**：
   - 在启动器中选中游戏点击 **“卸载”**（或双击游戏目录中的 `Uninstall_OptiScaler_NR.bat`），即可完整移除所有注入代理、`.asi` 插件、着色器、日志与历史残留文件（RTX Remix MC 模式会自动还原原始 `remixbridge.dll`），恢复游戏原始状态（启动器目录中已提取的模型仍保留，方便下次给任意游戏秒装）。

---

## 6. 免费声明、署名与非商业许可

### 免费、禁止倒卖与二次开发署名要求
本项目**永久免费分发**。启动器与安装脚本中的“**本软件免费，如付费购买请立即退款！**”是保留的防倒卖声明；**严禁任何人在闲鱼、淘宝、QQ 群、付费网盘、收费整合包中售卖本工具，或以“付费远程代安装 / 付费调试 / 付费优化包”名义向他人收费提供。如果您是付费买到的，请立即申请退款并举报卖家！**

基于本项目的**非商业二次修改、重打包与衍生作品必须遵守 [NOTICE](NOTICE)**：二改作者可以署上自己的名字，但必须完整保留原始作者（`TheAutomatic`）署名、所有上游开源项目作者的版权声明、官方仓库链接以及全部 `Required Notice:` 行，**严禁抹除署名或仅修改二进制信息/版本号冒充原创**。

### 署名与许可协议 (Attributions & Licenses)
发布包内已随包附带全部上游与第三方组件的完整许可文本（见包内 `Licenses/` 目录与根目录 [NOTICE](NOTICE)）：

#### 核心项目与插件
- **[本项目 (TheAutomatic / dlss-5-amd-project)](https://github.com/TheAutomatic/dlss-5-amd-project)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)**（见 [NOTICE](NOTICE)）：图形化启动器、PE 导入表探测、多模式部署与安装/卸载/打包脚本（**仅限个人、科研、非商业用途，禁止任何商业倒卖与收费代安装**）；
- **[RTX Remix for Minecraft AMD Patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** (`remixbridge.dll`) — **PolyForm Noncommercial License 1.0.0**：衍生自 [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（**Apache License 2.0**；含 NVIDIA Remix C API 头文件 **MIT License**）；
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**：官方原版超分辨率与帧生成桥接框架（作为独立聚合分发，DLL 本体未做修改，不施加额外限制；对应源码与版本说明见 `Licenses/OptiScaler_SOURCE.txt`，支持自行替换其他第三方构建）；
- **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **PolyForm Noncommercial License 1.0.0**（本分支修改与发行构建），衍生自 **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**（**MIT License**，© mochizuki0323）：AMD RDNA4 / RDNA3 神经网络渲染核心、D3D12 Direct-ELF / D3D11 桥接路径、ACO 机器码记录与 NR Control 插件；
- **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **PolyForm Noncommercial License 1.0.0**（本分支插件修改与发行构建），衍生自 **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**（**MIT License**，© 2026 OpenNR）：AMD RDNA4 / RDNA3 开源 DLSS 光线重建（Ray Reconstruction）运行时与独立 ASI 插件；
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**：RenoDX 风格色调映射与色彩处理算法（`Licenses/RenoDX_MIT.txt`）；
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**：原生 DLSS 选项解锁 ASI 插件（作为独立聚合未经修改分发，对应源码说明见 `Licenses/OptiPatcher_SOURCE.txt`）。

#### 兼容模式与随包第三方开源组件（作为独立聚合按各自原协议分发）
- **[ReShade](https://github.com/crosire/reshade)** (`ReShade64.dll` / `ReShade32.dll` 及标准着色器头文件) — **BSD-3-Clause License**（`Licenses/ReShade-LICENSE.md`）；
- **[DXVK](https://github.com/doitsujin/dxvk)**（GE-Proton 构建，用于 DX9 兼容路线）— **zlib License**（`Licenses/DXVK-LICENSE.txt`）；
- **[Vulkan Loader](https://github.com/KhronosGroup/Vulkan-Loader)**（`vulkan-1.dll`，含补丁说明 `vulkan/Vulkan-Loader-PATCHES.diff`）— **Apache License 2.0**（`Licenses/Vulkan-Loader-LICENSE.txt`）；
- **[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)**（`DLSS5_Feed.fx` 与 ReShade Add-on 参考）— **MIT License**（`Licenses/DLSS5_Feed-LICENSE.txt`）；
- **[vort_Shaders](https://github.com/vortigern11/vort_Shaders)**（ReShade 路线运动矢量着色器）— **MIT License**（`Licenses/vort-LICENSE.txt`）；
- **[Dear ImGui](https://github.com/ocornut/imgui)**（游戏内 NR Control 菜单界面）— **MIT License**（`Licenses/DearImGui-LICENSE.txt`）；
- **[Inter Font](https://github.com/rsms/inter)**（菜单字体）— **SIL Open Font License 1.1**（`Licenses/Inter-LICENSE.txt`）；
- **[AMD FidelityFX SDK / FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)**、**[Intel XeSS](https://github.com/intel/xess)** 与 **DirectX Agility SDK** — 随 OptiScaler / DLSSNR-AMD 保留原许可文本（见 `Licenses/FidelityFX_*`、`Licenses/XeSS_LICENSE.txt`、`Licenses/DirectX_LICENSE.txt`）。

> 本仓库与发布包**不包含**任何 NVIDIA 专有二进制文件（`nvngx_dlssnr.dll` / `nvngx_dlssd.dll`）或从其中提取的模型权重文件（`*.bin`）。所有模型权重均由用户在本地通过自身合法持有的驱动/游戏文件离线提取生成。
