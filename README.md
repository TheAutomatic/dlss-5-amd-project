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
2. 本机 **weights**（`dlssnr_on_amd_weights.bin`；或上游 setup + 你游戏里的 `nvngx_dlss.dll` 生成）  
3. NVIDIA 相关文件只来自 **你自己的游戏**

### 脚本安装

1. 解压本 Release。  
2. 把下列文件放进**解压出来的同一目录**（和 `Setup.bat` 并排）：  
   - `version.dll`（0.3.0）  
   - `dlssnr_on_amd_weights.bin`（或 setup + `nvngx_dlss.dll`）  
3. 关闭游戏，执行（参数是 **游戏 exe 所在目录**）：

```bat
Setup.bat "游戏目录"
```

**游戏目录** = 放着游戏主程序 exe 的那一层（你平时放 `dxgi.dll` 的位置）。

- 鬼武者（Xbox PC）示例：`C:\XboxGames\Onimusha- Way of the Sword\Content`  
- 许多游戏在 `...\Win64\` 或 `...\WinGDK\` —— **以 exe 实际所在目录为准**。

安装器会：`OptiScaler.dll` → 你选的代理名（默认 `dxgi.dll`）；`version.dll` → `dlssnr_amd_pass1/2/3.dll`。  
**不会**把 `version.dll` 装进游戏目录。

安装前会核对 `version.dll` 的 SHA256 是否为 **0.3.0**（不一致会提示并询问是否继续）；`weights.bin` 会做体积 sanity 检查（哈希因机器而异，不写死）。

### 手动安装

1. 把 **0.3.0 的 `version.dll`** 放进游戏目录，然后：  
   - **改名** 为 `dlssnr_amd_pass1.dll`  
   - 再**复制两份**为 `dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`（内容相同；多层 NR 才需要 2/3，**至少要有 pass1**）  
2. 确认游戏目录里**没有**额外的 `version.dll`（若你当初把 0.3.0 装成了别的代理名，也一并检查不要和下面的 `dxgi.dll` 重复注入）。  
3. 把本 Release 全部内容解压到同一游戏目录。  
4. 把 `OptiScaler.dll` **改名** 为要注入的代理，例如 `dxgi.dll`（或 `winmm.dll` 等）。  
5. 游戏内菜单打开 **DlssNr**（需要 NR 时）。

其它 OptiScaler 用法（**帧生成**、菜单快捷键、兼容性等）见：  
[**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki)。

---

## 署名与许可

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler)（GPL-3.0）  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**danielblnc/DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0**（不随本包分发）  
- 本仓库：每帧双槽、安装器、打包  

本包不含 NVIDIA 二进制、NR weights、上游闭源 pass。请遵守各上游许可。
