**中文** | [English](README.en.md)

# OptiScaler AMD pre-SR — 1.8.0-0.3.0

在 **OptiScaler** 上接入 **AMD 神经渲染（DLSS-NR on AMD）**，让 **纯 DLSS 游戏** 在 AMD 显卡上跑神经降噪；超分由 **FFX/FSR** 完成。

`1.8.0` = 本仓库版本；`0.3.0` = 必需的上游 NR 运行时版本。

> 不是 NR 核的重实现，也不是 ReShade 滤镜。  
> 路径：**游戏 DLSS 输入 → 本仓库 → NR（0.3.0）→ FFX/FSR 超分**。

---

## 相比前人

| 上游 | 他们做了什么 | 本仓库额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分代理（DLSS / FFX / XeSS） | 仍作为安装与运行主体 |
| **[dlss-5-amd（Matheus）](https://github.com/MatheusGViana/dlss-5-amd-project)** | AMD pre-SR：DLSS 输入 → AMD NR → FFX | **每帧双槽**：忙则跳 → GPU 不再空转；**修好 Xbox PC 安装路径**（原脚本按 exe 旁写入，商店版易落到 `WindowsApps` 导致权限失败；本安装器指向游戏 **exe 所在目录** 并做写探测） |
| **[DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | AMD NR 运行时本体 | **不改核**，按 0.3.0 调用 |

### 每帧双槽（相对「忙就跳帧」）

| 配置（鬼武者 720p，每帧 NR） | 中位帧周期 | 约 fps | GPU Wait |
|---|---:|---:|---:|
| 单槽 / 忙则跳 | ~29.7 ms | ~33.5 | ~8.7 ms |
| **本仓库双槽每帧** | ~22.4 ms | **~44.6** | **~0** |
| 原生 0.3（对照） | ~22.2 ms | ~45.0 | 0 |

提升来自 **不再为等上一帧 NR 退休而卡住录制线程**，GPU 保持忙碌；不是把 NR 核算快。场景更轻时中位可到约 49 fps。偶发 2s+ 长卡可能来自 NR 运行时日志里的 `SPIKE` job。

---

## 安装

### 你需要自己准备的（本包不附带）

1. **只支持** [DLSS-NR on AMD **0.3.0** Release](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)  
   （其它版本未适配本包布局，请勿混用。）  
2. 按上游说明生成本机 **weights**（若你已有 `dlssnr_on_amd_weights.bin` 可直接用）。  
3. NVIDIA 相关文件只来自 **你自己的游戏**，不要从第三方下载分发。

### 脚本安装

1. 解压本 Release。  
2. 在解压目录建 `vendor\`，放入 **0.3.0** 的 `version.dll`，以及 weights 或上游 setup + 你游戏里的 `nvngx_dlss.dll`。  
3. 关闭游戏后执行（参数是 **游戏 exe 所在目录**，不是 exe 文件本身）：

```bat
Setup.bat "游戏目录"
```

**游戏目录** = 放着游戏主程序 exe 的那一层，也就是你平时放 `dxgi.dll` 的位置（与 OptiScaler 安装说明相同）。

- 鬼武者（Xbox PC）示例：`C:\XboxGames\Onimusha- Way of the Sword\Content`  
  （本机上 `OnimushaWotS.exe` 在 `Content\` 下。）  
- 许多游戏在 `...\Win64\` 或 `...\WinGDK\` 等子目录 —— **以 exe 实际所在目录为准**，不要猜固定文件夹名。

安装器会把 `OptiScaler.dll` 写成你选的代理名（默认 `dxgi.dll`），并把 `vendor\version.dll` 复制为 `dlssnr_amd_pass1.dll`（及 pass2/3）。

### 手动安装

1. 先按 [DLSS-NR on AMD 0.3.0](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) 的说明装进游戏目录。  
2. 在游戏目录把该 **`version.dll` 复制** 为：  
   - `dlssnr_amd_pass1.dll`（**至少 1 份**）  
   - 需要多层神经渲染时再复制 `dlssnr_amd_pass2.dll` / `pass3.dll`（多份同内容，供多层独立加载）  
3. 把本 Release **全部内容** 解压到同一游戏目录。  
4. 把 `OptiScaler.dll` **改名** 为你要注入的代理，例如 `dxgi.dll`（也可用 `winmm.dll` 等，按该游戏/其它模组情况选择）。  
5. 需要 NR 时，在游戏内 OptiScaler 菜单打开 **DlssNr**。

其它 OptiScaler 用法（**帧生成**、菜单快捷键、兼容性与各游戏注意点等）请参照官方文档：  
[**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki)。

---

## 署名与许可

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler)（GPL-3.0）  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**danielblnc/DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0**（不随本包分发）  
- 本仓库：每帧双槽、安装器、打包  

本包不含 NVIDIA 二进制、NR weights、上游闭源 pass。请遵守各上游许可。
