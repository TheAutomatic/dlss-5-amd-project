# OptiScaler AMD pre-SR — 1.8.0-0.3.0 使用说明

在 **OptiScaler** 上接入 **AMD 神经渲染**：纯 DLSS 游戏在 AMD 显卡上做神经降噪，超分由 **FFX/FSR** 完成。

| 版本段 | 含义 |
|---|---|
| **1.8.0** | 本包（OptiScaler 宿主 / 安装器 / 每帧双槽） |
| **0.3.0** | 必需的上游运行时 |

上游为 [DLSS-NR on AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)（以下简称原项目，danielblnc 为原作者）。

不是神经核重实现，也不是 ReShade 滤镜。

---

## 这个压缩包里有什么

| 文件/目录 | 作用 |
|---|---|
| `OptiScaler.dll` | 本项目主体（安装时会改成你选的代理名） |
| `OptiScaler.ini` | 配置模板；**每帧 NR 为默认** |
| `OptiScaler\` | FFX / XeSS / Agility 等依赖 |
| `Setup.bat` / `Setup.ps1` | 安装器（**双击 Setup.bat**） |
| `Licenses\` | 第三方许可 |
| `SHA256SUMS.txt` | 校验和 |

**本包不包含、需要你自己准备**（和 `Setup.bat` 放同一目录），**否则无法开启 DLSS5**：

| 文件名 | 说明 | 来源 |
|---|---|---|
| `dlssnr_on_amd_setup.exe` | 原作者的 **0.3.0** 安装程序 | [原项目 0.3.0 Release](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) |
| `nvngx_dlssnr.dll` | DLSS 5 神经渲染运行库 | 部分游戏（如某些 NBA 2K）自带；也可自行获取 |
| （可选）现成 `version.dll` / `dlssnr_on_amd_weights.bin` | 已生成过可直接放 | 跑过原作者 setup 后会有 |

**推荐只放 setup.exe + nvngx_dlssnr.dll。**  
双击本包 `Setup.bat` 时，若还没有 `version.dll` 或 `weights.bin`，会 **自动启动原作者 setup** 生成（在选完游戏目录之后），再继续装本项目。

**只支持 0.3.0**，其它版本安装器会拒绝。

---

## 安装步骤

### 1. 准备文件

解压本包，把 `dlssnr_on_amd_setup.exe` 和 `nvngx_dlssnr.dll` 放进解压目录（和 `Setup.bat` 并排）。

### 2. 双击 Setup.bat

1. **关闭游戏**。  
2. 双击 `Setup.bat`。  
3. 弹出 **文件夹选择框**，选 **游戏 exe 所在目录**。  
4. 选择 **代理 DLL**（默认 `dxgi.dll`，也可选 `winmm` / `d3d12` / `winhttp` / `wininet` / `dbghelp`。**不支持 `dinput8`**）。  
5. 若这时还缺 `version.dll` 或 `weights.bin`，安装器会 **自动启动原作者 setup** 生成；完成后继续。

游戏目录 = 游戏 exe 所在那一层（安装 OptiScaler 用的同款路径）：

- Xbox PC：`C:\XboxGames\Onimusha- Way of the Sword\Content`  
- 不要选 `C:\Program Files\WindowsApps\...`

安装器会：

- 把本包 `OptiScaler.dll` 装成你选的代理名
- 把 `dlssnr_on_amd_setup.exe` 生成的 0.3.0 `version.dll` 复制为 `dlssnr_amd_pass1/2/3.dll`
- 复制 `dlssnr_on_amd_weights.bin`
- **不会**在游戏目录留下 `version.dll`（若要用 `version.dll` 这个名字注入，请走手动安装）

旧环境不能弹窗时，可在命令行传路径：

```bat
Setup.bat "C:\XboxGames\Onimusha- Way of the Sword\Content"
```

### 3. 游戏内启用（自动 / 手动安装都一样）

1. 启动游戏。  
2. 按 **Insert（Ins）** 打开 OptiScaler 菜单。  
3. 勾选 **DLSSNR**。  
4. 之后即为 **DLSS5 神经降噪 + FFX/FSR 超分**。

---

## 手动安装（不用 Setup.bat）

`version.dll` 来自 `dlssnr_on_amd_setup.exe`（原作者 0.3.0）。  
原项目详细步骤：[danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)

1. 跑 `dlssnr_on_amd_setup.exe`，拿到 0.3.0 的 `version.dll` 和 `dlssnr_on_amd_weights.bin`（需要 `nvngx_dlssnr.dll` 才能生成 weights）。  
2. `version.dll` 改名为 `dlssnr_amd_pass1.dll`，再复制出 `pass2`、`pass3`。  
3. 放入 `dlssnr_on_amd_weights.bin`。  
4. 本包全部解压进同一目录。  
5. `OptiScaler.dll` 改名为要注入的名字：常用 `dxgi.dll`（**不要用 `dinput8.dll`**），也可以直接改成 **`version.dll`**。  
6. **倒数第二步**：确认没有和注入名冲突的残留文件（若第 5 步已改成 `version.dll`，不要再留一份原作者的 `version.dll`）。  
7. 游戏内 **Ins** → 勾选 **DLSSNR**。

---

## 性能参考

**4K 超级性能档**（相当于原生 **720p** 渲染）、每帧 NR、RX 9070 XT：中位约 **45–49 fps**，`MsGPUWait ≈ 0`，与同场景原生 0.3 同档。偶发 2 秒以上长卡可能来自神经运行时日志中的 `SPIKE` job。

---

## 署名

- [OptiScaler](https://github.com/optiscaler/OptiScaler) — 超分代理主体（GPL-3.0，见 `Licenses\`）
- [MatheusGViana/dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project) — AMD pre-SR 桥基底
- [原项目 / 原作者 danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD) — AMD 神经运行时 **0.3.0**（不随本包分发）
- 每帧双槽宿主路径、安装器、打包：本项目

## 许可

OptiScaler 与附带的 FFX/XeSS/Agility：见 `Licenses\`。  
**OptiScaler 为 GPL-3.0**，本包按同一条款分发；对应源码见  
<https://github.com/TheAutomatic/dlss-5-amd-project>（release tag 与本包版本一致）。  
神经运行时与其它第三方文件按 **其各自** 许可使用。
