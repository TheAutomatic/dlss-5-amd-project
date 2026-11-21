**中文** | [English](README.en.md) | [Español](README.es.md)

# OptiScaler(NR)

> [!IMPORTANT]
> **源码公开 · 完全免费 · 严禁商业倒卖及收费代安装**：本项目启动器与脚本基于 **PolyForm Noncommercial License 1.0.0** 开源，仅限个人非商业使用。如付费购买请立即申请退款并举报卖家！详见[第 6 节：免费声明与非商业许可](#6-免费声明署名与非商业许可)。
>
> **🎉 2.0 架构全面升级公告**：本项目已升级为 **“官方原版 OptiScaler + 独立双 `.asi` 插件（`dlssnr-amd.asi` + `openrr-amd.asi`）+ 图形化一键启动器”** 架构！支持 **OptiScaler / 独立直通 (Standalone) / ReShade 兼容 / RTX Remix for Minecraft (Java 版)** 多模式自动适配。
> 若需查阅 `1.x` 旧版历史说明文档，请点击查看 **[旧版中文说明 (Legacy README)](docs/legacy-readme/README.md)**。

**特别感谢**：各位用户的测试与反馈意见。

本项目提供图形化启动器（**`OptiScalerNRLauncher.exe`**），支持自动扫描 Steam / Epic / GOG / XBOX 游戏库、结合 PE 导入表与 OptiScaler Wiki 智能推荐注入代理、一键离线提取/合成模型权重，按需勾选并部署 **官方原版 OptiScaler** 与两大 AMD 神经网络插件——**DLSS 神经渲染 (`dlssnr-amd.asi`)** 和 **DLSS 光线重建 (`openrr-amd.asi`)**，同时内置 INI 配置预设与 `MZ5` 画风分享码管理，让 **AMD 显卡** 在各类 DX11 / DX12 / Vulkan 游戏中原生运行 DLSS 神经渲染与 DLSS 光线重建（Ray Reconstruction）。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## 目录
- [1. 核心架构与上游致谢](#1-核心架构与上游致谢)
- [2. ⚡ 极速安装指南 (Windows & Linux)](#2--极速安装指南-windows--linux)
  - ├─► [Windows 图形化启动器安装（推荐）](#windows-图形化启动器安装推荐)
  - └─► [Linux / SteamOS (Proton) 脚本安装](#linux--steamos-proton-脚本安装)
- [3. 双 `.asi` 插件核心能力说明](#3-双-asi-插件核心能力说明)
- [4. 游戏内快捷键与菜单控制](#4-游戏内快捷键与菜单控制)
- [5. 兼容修复、一键日志打包与卸载](#5-兼容修复一键日志打包与卸载)
- [6. 免费声明、署名与非商业许可](#6-免费声明署名与非商业许可)

---

## 1. 核心架构与上游致谢

`2.0` 版本不再维护定制修改版 `OptiScaler.dll`，全面转向**官方原版 OptiScaler 桥接层 + 独立 `.asi` 插件按需挂载**的解耦设计：

| 核心组件 | 上游与来源 | 作用与特性 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>（官方原版桥接框架） | 上游官方 Nightly 构建（校验 SHA-256，支持自行替换其他第三方构建） | 负责通用超分辨率转换（DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS）与帧生成（DLSSG → FSR3 FG / XeFG），并通过内置 ASI Loader 加载 `OptiScaler\plugins\*.asi` |
| **`dlssnr-amd.asi`**<br>（DLSS 神经渲染插件） | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | 在 AMD **RX 9000 (RDNA4)** 与 **RX 7000 / 700M / 800M / 8000S (RDNA3)** 上运行 DLSS 神经渲染；支持 **D3D12 Direct-ELF 原生直录** 与 **Vulkan ACO 机器码加速**，内置 RenoDX 与 wilsjo2 双风格管线 |
| **`openrr-amd.asi`**<br>（DLSS 光线重建插件） | **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | 在 AMD **RDNA4（已实测）** 与 **RDNA3 / RDNA3.5（实验性）** 上原生运行 NVIDIA **DLSS 光线重建（Ray Reconstruction / DLSS-RR）** 网络；支持 **D3D12 Direct-ELF 原生直录** 与 **Vulkan**，可与 `dlssnr-amd.asi` 无缝共存 |
| **[rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)**<br>（RTX Remix for Minecraft 兼容补丁） | **[NVIDIAGameWorks / rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)**（上游 Apache-2.0）<br>**[TheAutomatic / rtx-remix-minecraft-amd-patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** | 自动扫描 PrismLauncher / `.minecraft\remix-sdk` 实例或手动添加实例；原名部署 `OptiScaler.dll`、自动备份并应用 AMD 兼容版 `remixbridge.dll`（完整卸载可无损还原原版），同时动态启用 `[Spoofing] VulkanExtensionSpoofing = true` |

---

## 2. ⚡ 极速安装指南 (Windows & Linux)

### Windows 图形化启动器安装（推荐）

1. **下载解压**：从 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases) 下载最新压缩包并完整解压。
2. **启动安装器**：双击运行解压目录下的 **`OptiScalerNRLauncher.exe`**，自动打开 **`OptiScaler NR` 图形化启动器**。
3. **选择游戏与注入代理**：
   - 在左侧游戏库中选择目标游戏（也可点击右上角 **`+`** 手动选择游戏的实际渲染 `.exe`，注意虚幻引擎游戏通常位于 `<Game>\Binaries\Win64\*-Shipping.exe`）；
   - 启动器会结合 PE 导入表与 OptiScaler Wiki 自动预选推荐的代理 DLL（通常为 `dxgi.dll` 或 `winmm.dll`）。
4. **按需勾选要安装的 `.asi` 插件**：
   - **[☑] `dlssnr-amd.asi`（DLSS 神经渲染 · Mochizuki）**：
     - 首次使用时，若提示缺少模型源，点击 **“导入模型源文件…”** 选择自备的 `nvngx_dlssnr.dll`（支持 `310.9.16`［含新旧双模型］或 `310.8.0`［含原版模型］）。启动器会将其保存至启动器目录，并在安装时自动提取与合成模型权重，后续给其他游戏安装直接复用。
   - **[☑] `openrr-amd.asi`（OpenRR 光线重建）**：
     - 仅支持原生包含 NVIDIA 光线重建的游戏。若本机游戏目录中已含 `310.9.1.0` 版 `nvngx_dlssd.dll` 会自动识别复用；若提示缺少模型源，点击卡片右侧 **“导入 nvngx_dlssd.dll…”** 选择一次 **`310.9.1.0`** 版 `nvngx_dlssd.dll` 即可全局复用。
   - **[☐] 安装 OptiPatcher（可选）**：按 Wiki 与内置支持表自动预选，用于解锁 270+ 款游戏的原生 DLSS 选项。
5. **一键安装**：点击右上角的 **“安装 OptiScaler NR”**（或 **“更新 OptiScaler NR”**）按钮即可完成部署。

---

### Linux / SteamOS (Proton) 图形启动器（新）

从 [Release 页](https://github.com/TheAutomatic/dlss-5-amd-project/releases) 下载 Linux 包（基于 Mochizuki 上游 [DLSSNR-AMD Linux 版](https://gitlab.com/mochizuki0323/DLSSNR-AMD/-/releases)，与上游一样分为两个版本）：

| 包 | 适用 | 安装路线 |
| --- | --- | --- |
| `OptiScaler-NR-Linux-x86_64-<版本>.tar.gz` | 64 位游戏 | OptiScaler（含 Mochizuki NR + OpenRR 0.1.7plus + OptiPatcher）+ 三条 ReShade Feeder 路线 |
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
  - **AMD Radeon RX 7000 系列 & Radeon 700M / 800M / 8000S (RDNA3)**：同样支持全部 4 套网络（需通过预编译 RDNA3 ACO 机器码运行）。
- **双渲染后端**：支持 **D3D12 Direct-ELF**（直接通过 AMD 驱动在游戏原生 D3D12 命令列表内执行，零跨设备拷贝）与 **Vulkan 桥接/原生路径**。
- **双后处理风格**：支持 **RenoDX 风格**（默认，还原 RenoDX 色调映射）与 **wilsjo2 风格**（原版 OptiScaler-NR 风格），支持在启动器中一键导入/导出 **`MZ5` 画风分享码**。

### 二、`openrr-amd.asi`（OpenRR · DLSS 光线重建）
- **支持显卡**：**AMD Radeon RX 9000 系列（RDNA4，已实测）** 与 **RX 7000 / 700M / 800M / 8000S 系列（RDNA3 / RDNA3.5，实验性）**，需搭配 `nvngx_dlssd.dll`（`310.9.1.0`）。
- **核心功能**：接管游戏的 `NVSDK_NGX_Feature_RayReconstruction`（DLSS 光线重建）调用，在原生支持路径追踪 / 光线重建的游戏（如《赛博朋克 2077》《心灵杀手 2》《夺宝奇兵：古老之圈》等）中运行神经网络光线重建降噪。
- **双后端与无缝共存**：支持 **D3D12 Direct-ELF** 与 **Vulkan**；检测到同目录加载 `dlssnr-amd.asi` 时自动协同命令列表与快捷键，互不抢占焦点。
- **显卡伪装（Spoofing）策略**：默认按游戏启用全局伪装（`full`，将 `OptiScaler.ini` 中 `[Spoofing]` 的 `Dxgi`、`Vulkan`、`VulkanExtensionSpoofing` 设为 `true`）；若部分游戏在开启全局伪装后出现兼容问题，可在 OpenRR 卡片勾选 **“减少全局显卡伪装”**（命令行对应 Windows `-OpenRrSpoof full|auto` / Linux `--openrr-spoof full|auto`），将该三项改为 `auto`（关闭 OpenRR 时同样恢复为 `auto`）。选择保留现有 `OptiScaler.ini` 时也仅更新这三项，不改动其余自定义配置；命令行未显式指定时自动沿用上次记录的策略。

---

## 4. 游戏内快捷键与菜单控制

进入游戏并在画面设置中开启 **DLSS**（或 FSR / XeSS）及 **光线重建（Ray Reconstruction，若游戏支持）** 后：

| 快捷键 | 呼出的菜单 / 功能 | 说明 |
|---|---|---|
| **`Insert (Ins)`** | **OptiScaler 主菜单** + **OpenRR 侧边栏** | 调节超分辨率（FSR / XeSS）与帧生成；若安装了 `openrr-amd.asi`，**OpenRR 设置面板会跟随 `Insert` 键同步在右侧弹出/收起** |
| **`End`** | **NR Control（Mochizuki 神经渲染控制面板）** | 调节 `dlssnr-amd.asi` 的全部参数：实时切换 4 套神经网络、切换 RenoDX / wilsjo2 风格、配置多渲染层（NR 链）与 Pass 次数、开启 HUD 修复与显存/性能诊断 |
| **`Ctrl + F11`** | 快捷循环切换 NR 网络 | 在已安装的神经渲染模型之间一键切换 |
| **`F5`** | NR 前后对比截图 | 同时保存同一帧开启/关闭 NR 的对比截图至游戏目录 `NR Screenshots\` |

---

## 5. 兼容修复、一键日志打包与卸载

1. **配套运行库兼容修复（XeSS / FidelityFX）**：
   - 若游戏自带的旧版 `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` 导致超分或帧生成异常，可在启动器中选中游戏后点击 **“兼容修复”**，一键备份原文件并替换为包内新版；随时可点 **“恢复原文件”** 无损还原。
2. **一键打包日志（Bug 反馈）**：
   - 遇到任何问题时，点击启动器右下角的 **“打包日志”** 按钮，启动器会自动收集游戏目录下的 `OptiScaler.log`、`dlssnr-amd.log`、`openrr-amd.log`、配置文件、系统硬件/驱动信息与最近两天的驱动崩溃事件，打包成 `.zip` 生成到桌面。
3. **干净卸载**：
   - 在启动器中选中游戏点击 **“卸载”**（或双击游戏目录中的 `Uninstall_OptiScaler_NR.bat`），即可完整移除所有注入代理、`.asi` 插件、着色器、日志与历史残留文件，恢复游戏原始状态（启动器目录中已提取的模型仍保留，方便下次给任意游戏秒装）。

---

## 6. 免费声明、署名与非商业许可

### 免费与禁止倒卖声明
本项目**永久免费分发**。启动器与安装脚本中的“**本软件免费，如付费购买请立即退款！**”是保留的防倒卖声明；**严禁任何人在闲鱼、淘宝、QQ 群、付费网盘、收费整合包中售卖本工具，或以“付费远程代安装 / 付费调试”名义向他人收费提供。如果您是付费买到的，请立即申请退款并举报卖家！**

### 署名与许可协议 (Attributions & Licenses)
- **[本项目 (TheAutomatic / dlss-5-amd-project)](https://github.com/TheAutomatic/dlss-5-amd-project)** — **[PolyForm Noncommercial License 1.0.0](LICENSE)**（见 [NOTICE](NOTICE)）：图形化启动器、PE 导入表探测、多模式部署与安装/卸载/打包脚本（**仅限个人、科研、非商业用途，禁止任何商业倒卖与收费代安装**）；
- **[RTX Remix for Minecraft AMD Patch](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch)** (`remixbridge.dll`) — **PolyForm Noncommercial License 1.0.0**：衍生自 [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)（**Apache License 2.0**）；
- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**：官方原版超分辨率与帧生成桥接框架（DLL 本体未做修改，支持自行替换其他第三方构建）；
- **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** & **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **MIT License**（© mochizuki0323）：AMD RDNA4 / RDNA3 神经网络渲染核心、D3D12 Direct-ELF 原生路径、ACO 机器码记录与 NR Control 插件；
- **[Storm (Matheus) / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** & **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **MIT License**：AMD RDNA4 / RDNA3 开源 DLSS 光线重建（Ray Reconstruction）运行时与独立 ASI 插件；
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**：RenoDX 风格色调映射与色彩处理算法；
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**：原生 DLSS 选项解锁 ASI 插件。

> 本仓库与发布包**不包含**任何 NVIDIA 专有二进制文件（`nvngx_dlssnr.dll` / `nvngx_dlssd.dll`）或从其中提取的模型权重文件（`*.bin`）。所有模型权重均由用户在本地通过自身合法持有的驱动/游戏文件离线提取生成。
