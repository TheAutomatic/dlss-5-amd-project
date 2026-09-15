**中文** | [English](README.en.md)

# OptiScaler AMD pre-SR — 1.8.1-0.3.0

在 **OptiScaler** 上接入 **AMD 神经渲染**，让 **纯 DLSS 游戏** 在 AMD 显卡上跑神经降噪；超分由 **FFX/FSR** 完成。

`1.8.0` = 本仓库版本；`0.3.0` = 必需的上游运行时版本。

> 不是神经核的重实现，也不是 ReShade 滤镜。  
> 路径：**游戏 DLSS 输入 → 本仓库 → DLSSNR（0.3.0）→ FFX/FSR 超分**。

---

## 相比前人

| 上游 | 他们做了什么 | 本项目额外做了什么 |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | 通用超分代理（DLSS / FFX / XeSS） | 仍作为安装与运行主体 |
| **[dlss-5-amd（Matheus）](https://github.com/MatheusGViana/dlss-5-amd-project)** | AMD pre-SR：DLSS 输入 → AMD NR → FFX | **每帧双槽**：忙则跳 → GPU 不再空转；安装器改为通用目录选择 |
| **[DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)**（以下简称原项目，danielblnc 为原作者） | AMD 神经渲染运行时本体 | **不改核**，按原作者 0.3.0 调用 |

### 每帧双槽（相对「忙就跳帧」）

| 配置（4K 超级性能档，相当于原生 720p 渲染；每帧 NR） | 中位帧周期 | 约 fps | GPU Wait |
|---|---:|---:|---:|
| 单槽 / 忙则跳 | ~29.7 ms | ~33.5 | ~8.7 ms |
| **本项目双槽每帧** | ~22.4 ms | **~44.6** | **~0** |
| 原生 0.3（对照） | ~22.2 ms | ~45.0 | 0 |

提升来自 **不再为等上一帧 NR 退休而卡住录制线程**，GPU 保持忙碌；不是把神经核算快。场景更轻时中位可到约 49 fps。

---

## 安装

### 第一步：你自己准备文件（本包不附带）

本压缩包 **只含** OptiScaler 这一层（`OptiScaler.dll`、依赖、安装器）。  
下面这些要 **你自己弄好**，放在 **解压后和 `Setup.bat` 同一目录** 里，**否则无法开启 DLSS5**：

| 文件名 | 是什么 | 从哪来 |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | 原作者的 **0.3.0** 安装程序 | [原项目 0.3.0 Release](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 神经渲染运行库 | 部分游戏（如某些 NBA 2K）自带；也可自行从网上获取 |
| （可选）现成的 `version.dll` / `dlssnr_on_amd_weights.bin` | 已经生成过就可直接放 | 跑过一次原作者 setup 后会得到 |

**推荐做法：只放 `dlssnr_on_amd_setup.exe` + `nvngx_dlssnr.dll`。**  
双击本包的 `Setup.bat` 时，若还没有 `version.dll` 或 `weights.bin`，会 **自动启动原作者 setup** 帮你生成（在选完游戏目录之后），然后再继续安装本项目。  
若 `nvngx_dlssnr.dll` 只在本包目录、游戏目录没有，安装器会在你选完游戏文件夹后 **自动复制一份进去**（原作者 0.3.0 会在游戏目录找它）。

**只认 0.3.0。** 其它版本装了也跑不了，安装器会直接拒绝。

### 第二步：运行安装器（推荐）

1. 解压本 Release 到任意目录。  
2. 把 `dlssnr_on_amd_setup.exe` 和 `nvngx_dlssnr.dll` 放进这个目录（和 `Setup.bat` 并排）。  
   已有现成的 `version.dll` / `dlssnr_on_amd_weights.bin` 也可以一并放上。  
3. **关闭游戏**。  
4. **双击 `Setup.bat`** → 弹出 **文件夹选择框** → 选中 **游戏 exe 所在的文件夹** → 确定。  
5. 按提示选择 **注入用的代理 DLL**（默认 `dxgi.dll`；也可选 `winmm.dll`、`d3d12.dll`、`winhttp.dll`、`wininet.dll`、`dbghelp.dll`。**不支持 `dinput8.dll`**）。  
6. 若这时还缺 `version.dll` 或 `weights.bin`，安装器会 **自动启动原作者 setup** 生成；完成后继续装本项目。

**游戏文件夹**是放着游戏主程序的那个目录（安装 OptiScaler 用的同款路径）：

- 很多游戏在 `...\Win64\` 或 `...\Binaries\Win64\`  
- 商店版若选到只读的系统安装目录，安装器会拒绝并提示换可写目录  

安装器会做这些事：

| 来源 | 装到游戏目录后变成 |
|---|---|
| 本包 `OptiScaler.dll` | 你选的代理名（默认 `dxgi.dll`） |
| `dlssnr_on_amd_setup.exe` 生成的 `version.dll`（0.3.0） | `dlssnr_amd_pass1.dll`、`dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll` |
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
3. 找到并勾选 **DLSSNR**（AMD 神经渲染）。  
4. 之后画面上走的就是 **DLSS5 神经降噪 + FFX/FSR 超分**。

其它 OptiScaler 用法（帧生成、菜单快捷键、兼容性）见：  
[**OptiScaler Wiki**](https://github.com/optiscaler/OptiScaler/wiki)。

---

### 手动安装（不用 Setup.bat）

适合已经熟悉「往游戏目录丢 DLL」的人。`version.dll` 来自 `dlssnr_on_amd_setup.exe`（原作者 0.3.0）。  
原项目详细步骤：[danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. 跑 `dlssnr_on_amd_setup.exe`，拿到 0.3.0 的 `version.dll` 和 `dlssnr_on_amd_weights.bin`（需要 `nvngx_dlssnr.dll` 才能生成 weights）。  
2. 把 `version.dll` **改名** 为 `dlssnr_amd_pass1.dll`，再复制两份为 `dlssnr_amd_pass2.dll`、`dlssnr_amd_pass3.dll`（内容相同；至少要有 pass1）。  
3. 把 `dlssnr_on_amd_weights.bin` 放进同一游戏目录。  
4. 把本 Release **全部** 解压进同一游戏目录。  
5. 把 `OptiScaler.dll` **改名** 为要注入的名字：  
   - 常用：`dxgi.dll`（或 `winmm.dll` 等，**不要用 `dinput8.dll`**）  
   - 也可以直接改成 **`version.dll`**（和原作者同一代理名时尤其方便）  
6. **倒数第二步**：确认游戏目录里没有多余的、和注入名冲突的旧 `version.dll` / 旧代理 DLL（若你在第 5 步已把 OptiScaler 改成 `version.dll`，则不要再留一份原作者的 `version.dll`）。  
7. 进游戏，**Ins** 打开菜单，勾选 **DLSSNR**。

---

## 署名与许可

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler)（GPL-3.0）  
- [**MatheusGViana/dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project)  
- [**原项目 / 原作者 danielblnc**](https://github.com/danielblnc/DLSS-NR-on-AMD) **0.3.0**（不随本包分发）  
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx)（MIT）—— `dlssnr.hlsl` 的色彩合成取自其 DLSS 5 神经渲染 addon，全文见 `Licenses/RenoDX_ATTRIBUTION.txt`  
- 本项目：每帧双槽、安装器、打包  

本包不含 NVIDIA 二进制、NR weights、上游闭源 pass。请遵守各上游许可。
