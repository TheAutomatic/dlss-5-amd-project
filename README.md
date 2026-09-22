**中文** | [English](README.en.md)

# OptiScaler AMD pre-SR — 1.9.0

在 **OptiScaler** 上接入 **AMD 神经渲染**（DLSS5 on AMD），让 **纯 DLSS / XeSS 游戏**在 AMD 显卡上跑神经降噪；超分仍由 **FFX/FSR** 完成。

本项目 fork 自 **Matheus** 及其上游，并在其基础上接手维护演进。

**项目主页：[github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## 目录
1. [双后端架构概览 (Dual-Backend Overview)](#1-双后端架构概览)
2. [安装与准备 (Installation & Setup)](#2-安装与准备)
3. [游戏内设置与控制 (In-Game Ins Menu Controls)](#3-游戏内设置与控制)
4. [常见问题、排错与卸载 (FAQ & Troubleshooting)](#4-常见问题排错与卸载)
5. [署名与许可 (Attribution & Licenses)](#5-署名与许可)

---

## 1. 双后端架构概览

本项目是 AMD 神经渲染的**调度与桥接层**。桥接层自身的执行开销极低（实测约 **0.01～0.03 ms**），几乎无额外性能损耗。
在 1.9.0 版本中，我们正式提供了对两大主流 AMD 神经渲染后端的完整支持：

> **路径**：游戏 DLSS/XeSS 输入 → 本项目 Pre-SR 调度与桥接 → 神经降噪核心（`lmxxf` / `danielblnc`）→ FFX/FSR 超分输出。

### 后端特性对比

两者均基于 ViT (Vision Transformer) 架构，均适配 AMD RX 7000 / 9000 系列显卡（RDNA3 / RDNA4）：

| 特性维度 | `lmxxf` 后端 | `danielblnc` 后端 |
|---|---|---|
| **核心来源** | [lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)（开源 HIP 算力核心） | [danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)（0.3.1 / 0.3.0 运行时） |
| **显卡支持** | AMD RX 7000 / 9000 系列 (RDNA3 / RDNA4) | AMD RX 7000 / 9000 系列 (RDNA3 / RDNA4) |
| **模型架构** | ViT 神经降噪网络 | ViT 神经降噪网络 |
| **运行时文件** | `LmxxfNrRuntime.dll` + `lmxxf-modules/` + `shaders/` | `dlssnr_amd_pass1.dll` ~ `pass3.dll` |
| **模型权重** | `native-game-tiled-assets/`（平铺权重文件夹） | `dlssnr_on_amd_weights.bin`（单二进制权重） |
| **调度机制** | 细粒度三阶段同帧微调度（输入录制 → HIP 推理 → 屏障同步） | 多槽（Multi-Slot，默认 3 槽）流水线调度与空等消除 |
| **动态画质调参** | 支持在 Ins 菜单实时调节 `Detail strength`、`Colour strength` 与 `Debug view` | 支持调节 Pass Preset / Style / WhitePoint / AmdSlots |
| **分辨率建议** | 当前模型切片推荐超分前渲染分辨率 **≤ 1080p**（如 4K 性能档、2K 质量档） | 支持常规渲染分辨率 |

### 双后端共存机制

两个后端的文件命名完全独立，**没有任何同名文件冲突**：
- `lmxxf` 文件集：`LmxxfNrRuntime.dll`、`lmxxf-modules\`、`shaders\`、`native-game-tiled-assets\`
- `danielblnc` 文件集：`dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`、`dlssnr_on_amd_weights.bin`

两个后端可以**同时安装在同一游戏目录中**。当前生效的后端由 `OptiScaler.ini` 中的配置决定：
```ini
[DlssNr]
Enabled = true
RunBeforeSR = true
NrBackend = lmxxf   ; 可选: lmxxf 或 daniel
```
如果需要切换后端，只需修改该行配置，或者重新运行 `Setup.bat` 选择对应后端即可。

---

## 2. 安装与准备

### 本压缩包文件说明

| 文件 / 目录 | 作用 |
|---|---|
| `OptiScaler.dll` | 本项目主体（安装时会改成你选择的代理名，如 `dxgi.dll`） |
| `OptiScaler.ini` | 配置文件模板；`[DlssNr]` 段包含后端与各项参数设置 |
| `OptiScaler\` | FFX / XeSS / Agility 等依赖库目录 |
| `Setup.bat` / `Setup.ps1` | 交互式安装脚本（**双击 `Setup.bat` 运行**） |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | 专用卸载脚本；Setup 会自动复制到游戏目录 |
| `LmxxfNrRuntime.dll` | lmxxf 神经渲染运行时核心库 |
| `third_party\lmxxf\modules\` 或 `lmxxf-modules\` | lmxxf 算力模块 (.hsaco) |
| `third_party\lmxxf\shaders\` 或 `shaders\` | lmxxf 图像处理着色器 |
| `native-game-tiled-assets\` | （完整包包含）lmxxf 模型权重文件夹 |
| `Licenses\` | 各上游开源项目许可协议 |

---

### 第一步：准备文件

根据你想使用的后端准备相应文件，放在解压后与 `Setup.bat` 同一目录下：

#### 方案 A：使用 `lmxxf` 后端
- 如果下载的是包含模型权重的**完整整合包**，所有 lmxxf 文件已内置，无需额外准备。
- 如果下载的是轻量包，请确保将 `native-game-tiled-assets` 权重文件夹放入安装包目录或目标游戏目录中。

#### 方案 B：使用 `danielblnc` 后端
需自行准备原作者文件（本包不随带闭源权重与原作者程序）：
- 将 `dlssnr_on_amd_setup.exe`（[原项目 Releases](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) 0.3.1 或 0.3.0）与 `nvngx_dlssnr.dll` 放在 `Setup.bat` 同目录下；
- 或者直接放入之前生成好的 `version.dll` 与 `dlssnr_on_amd_weights.bin`。

#### 方案 C：双后端共存
同时准备好上述两组文件，安装器会自动识别并允许一并安装。

---

### 第二步：运行安装器（推荐）

1. **确认游戏已完全退出**。
2. **双击 `Setup.bat`**。
3. 弹出文件夹选择框，选中**游戏主程序 (.exe) 所在的目录**。
4. 选择注入代理名称（默认推荐 `dxgi.dll`，也可选 `winmm.dll`、`d3d12.dll` 等；**不支持 `dinput8.dll`**）。
5. **后端检测与模式选择**：
   - 若检测到两种后端的文件均齐备，安装器会提示你选择：
     1. 安装 `lmxxf` 后端
     2. 安装 `danielblnc` 后端
     3. **同时安装两个后端（文件共存，随时在 ini 中切换）**
   - 若只检测到其中一种后端的可用文件，安装器会自动安装该后端并给出相应提示。
6. 若选择安装 `danielblnc` 且尚未生成权重，安装器会自动唤起原作者 setup 引导生成。
7. 安装器自动配置 `OptiScaler.ini` 并完成部署。

> **覆盖更新**：若之前已安装过老版本，直接重新运行 `Setup.bat` 即可覆盖更新，无需手动卸载。选同后端会刷新文件，选不同后端或双后端会自动补齐。

---

### 第三步：手动安装（进阶用户）

若不使用 `Setup.bat`，可手动将文件拷入游戏主程序目录：
1. 将 `OptiScaler.dll` 重命名为你选择的代理名称（如 `dxgi.dll`）放入游戏目录。
2. 将 `OptiScaler.ini` 和 `OptiScaler\` 依赖文件夹复制到游戏目录。
3. **部署后端文件**：
   - 若使用 `lmxxf`：将 `LmxxfNrRuntime.dll`、`lmxxf-modules\`、`shaders\`、`native-game-tiled-assets\` 放入游戏目录。
   - 若使用 `danielblnc`：将原作者 `version.dll` 复制三份，分别命名为 `dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`，并将 `dlssnr_on_amd_weights.bin` 放入游戏目录。注意**切勿在游戏目录保留名为 `version.dll` 的原作者文件**，以免与代理冲突。
4. 打开游戏目录中的 `OptiScaler.ini`，确认 `[DlssNr]` 段中的 `NrBackend` 设置为你需要的后端（`lmxxf` 或 `daniel`），并将 `Enabled = true`。

---

## 3. 游戏内设置与控制

1. 启动游戏。
2. 进入游戏画面后，按键盘上的 **Insert (Ins)** 键打开 OptiScaler 悬浮控制菜单。
3. 找到 **DLSS Neural Rendering** 设置区块，勾选 **Enable NR**。
   - 状态栏将显示当前活跃的运行时：`AMD NR runtime: lmxxf` 或 `0.3.x`。
4. 画面即时生效：**DLSS 输入缓冲 → 神经降噪核心 → FFX/FSR 超分输出**。

### `lmxxf` 后端专属调节项
- **Detail strength（细节与亮度强度）**：无级滑条，微调高频细节与亮度增益。
- **Colour strength（色彩校正）**：微调色彩饱和度与色调平衡。
- **Debug view（调试视图）**：实时切换原图、处理图及差分通道，方便观测降噪效果。
- **分辨率建议**：当前模型切片建议超分前渲染分辨率 **≤ 1080p**：
  - **4K 输出**：推荐搭配 **FSR 性能档**（渲染分辨率 1080p）或超级性能档（720p）；
  - **2K (1440p) 输出**：推荐搭配 **FSR 质量档 / 平衡档 / 性能档**；
  - **1080p 输出**：支持原生 1080p 或各超分档位。

### `danielblnc` 后端专属调节项
- **NR slots（多槽调度）**：默认 **3 槽**（可在 2–5 之间调节）。多槽调度可消除单槽空等上一帧的 GPU 等待延迟，避免满槽丢帧。
- **Pass Preset & Style**：调节各 pass 的降噪风格与预设。
- **Every-frame**：是否强制每帧执行降噪。

---

## 4. 常见问题、排错与卸载

### 卸载说明
1. 进入**游戏主程序目录**。
2. 双击运行 **`Uninstall_OptiScaler_NR.bat`**。
3. 卸载器会交互式询问是否保留之前的备份文件夹，列出所有计划清理的项目，输入 `Y` 确认后安全移除。
4. **安全保护机制**：卸载脚本**绝对不会**删除用户的模型权重文件（`native-game-tiled-assets/` 与 `dlssnr_on_amd_weights.bin`）以及 `nvngx_dlssnr.dll`，方便日后随时重装。

### 日志定位与排错
如遇菜单打不开或降噪未生效，请先查看游戏目录（或 XBOX PC 的 `_storage_` 目录）生成的日志文件：
- `OptiScaler.log`：OptiScaler 核心主日志。
- `amd_bridge.log`：AMD 神经渲染桥接层日志。
- `amd_presr.log`：Pre-SR 调度日志。
- `dlssnr_on_amd.log`：Daniel 后端专用运行日志。

**常见问题自检**：
1. **Ins 菜单未显示或打不开**：
   - 检查代理文件名是否与游戏冲突，尝试换用 `winmm.dll` 或 `d3d12.dll`。
   - 检查游戏目录是否存在多余的旧版本 `version.dll`。
2. **提示 weights 缺失或 EnqueueHip 返回 UNAVAILABLE**：
   - 若使用 `lmxxf`：检查游戏目录是否存在 `native-game-tiled-assets` 文件夹。
   - 若使用 `daniel`：检查游戏目录是否存在 `dlssnr_on_amd_weights.bin`。

---

## 5. 署名与许可

代码链与技术传承：
[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → **本仓库**。

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) (GPL-3.0) — 通用超分辨率与神经渲染代理框架。
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) (GPL-3.0) — 初始将 DLSS-NR 接进 OptiScaler。
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — Pre-SR 超分前执行与 Multi-Pass 架构。
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — AMD Pre-SR 桥接方案。
- [**danielblnc / DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) — AMD 神经渲染 0.3.1 / 0.3.0 运行时核心。
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — 开源 HIP 神经渲染运行时与算力核心。
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) (MIT) — `dlssnr.hlsl` 色彩通道合成算法。

本项目不含 NVIDIA 专有二进制文件、原作者闭源安装工具或未授权分发资产。使用时请遵循各上游项目许可协议。
