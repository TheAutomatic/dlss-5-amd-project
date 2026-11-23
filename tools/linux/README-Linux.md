# OptiScaler NR — Linux

**本工具完全免费，如果你是付费购买的，请立即退款并举报。**
**This tool is free. If you paid for it, request a refund and report the seller.**

## 中文

本包 = Mochizuki 的 DLSSNR-AMD Linux 版（`install.sh` 原样保留，为权威安装程序）+ 中日韩语言包 + **OpenRR（`v0.1.7plus3`，光线重建，仅 x86_64）+ OptiPatcher（仅 x86_64）** + OptiScaler NR 图形启动器。
适用于 Linux + Proton，AMD RX 9000（RDNA4）显卡（OpenRR 实验性支持 RX 7000 / RDNA3），Mesa 26.2+（RADV；OpenRR 需 Mesa RADV ≥ 26.3）。

- **x86_64 包**：64 位游戏；支持 OptiScaler 路线（含 Mochizuki NR、OpenRR 光线重建与 OptiPatcher 插件）和三条 ReShade Feeder 路线。
- **i686 包**：32 位游戏；只有 ReShade Feeder 路线（OptiScaler 与 OpenRR 只有 64 位）。

**使用**

1. 解压到任意位置（例如 `~/Games/OptiScaler-NR`），运行 `./OptiScalerNR-Launcher.sh`。需要 `python3`（3.8+）。
   启动器会以应用窗口打开（优先 Chromium / Chrome / Edge / Brave，否则用默认浏览器），只监听 127.0.0.1，关闭窗口后自动退出。
2. 左侧选择游戏（自动扫描 Steam、Heroic、Lutris，也可点 `+` 手动添加），选择安装路线、ASI 插件（NR / OpenRR / OptiPatcher）、启动网络与处理风格。
3. 首次安装 NR 需要选择 NVIDIA 的 `nvngx_dlssnr.dll`（推荐 310.9.16，或 310.8.0）或包含它的 zip；模型会被提取并保存在本包中，之后无需再选。使用 OpenRR 光线重建时，可导入或自备 `nvngx_dlssd.dll`（310.9.1.0），同样会缓存在本包中。
4. 安装完成后，把显示的 **Steam 启动选项** 复制到：Steam › 游戏属性 › 通用 › 启动选项。

- 添加到应用菜单：`./OptiScalerNR-Launcher.sh --install-desktop`（或在启动器设置中添加）。
- 卸载：启动器中点“卸载”，或 `bash uninstall.sh "<游戏文件夹>"`；`--all` 卸载启动器记录的所有游戏，`--purge-settings` 删除启动器设置，`--desktop` 删除菜单项。
- 命令行安装（不使用启动器）：见 `README.txt`，例如 `bash install.sh "<游戏 exe 所在文件夹>" reshade-feeder --dll ~/Downloads/nvngx_dlssnr.dll`。

## English

This package = Mochizuki's DLSSNR-AMD for Linux (its `install.sh` shipped unmodified — the authoritative installer)
+ the CJK language pack + **OpenRR (`v0.1.7plus3`, Ray Reconstruction, x86_64 only) + OptiPatcher (x86_64 only)**
+ the OptiScaler NR graphical launcher. For Linux + Proton on AMD RX 9000 (RDNA4; OpenRR also has experimental RX 7000 / RDNA3 support), Mesa 26.2+ (RADV; OpenRR requires Mesa RADV >= 26.3).

- **x86_64 package**: 64-bit games; OptiScaler route (with Mochizuki NR, OpenRR Ray Reconstruction, and OptiPatcher) and the three ReShade Feeder routes.
- **i686 package**: 32-bit games; ReShade Feeder routes only (OptiScaler and OpenRR are 64-bit only).

**Usage**

1. Extract anywhere (e.g. `~/Games/OptiScaler-NR`) and run `./OptiScalerNR-Launcher.sh`. Needs `python3` (3.8+).
   It opens as an app window (Chromium / Chrome / Edge / Brave if present, else your default browser), listens on
   127.0.0.1 only and exits when the window is closed. `--no-browser` prints the address instead.
2. Pick a game on the left (Steam, Heroic and Lutris are scanned; `+` adds any folder), then choose the route, ASI plugins (NR / OpenRR / OptiPatcher), starting network and processing style.
3. The first NR install needs NVIDIA's `nvngx_dlssnr.dll` (310.9.16 recommended, or 310.8.0) or a zip containing it. The model is extracted and kept in this package, so later installs need nothing. For OpenRR Ray Reconstruction, import or supply `nvngx_dlssd.dll` (310.9.1.0), which is also cached in this package.
4. Copy the shown **Steam launch options** into Steam › game Properties › General › Launch options.

- App menu entry: `./OptiScalerNR-Launcher.sh --install-desktop` (or from the launcher's settings).
- Uninstall: the launcher's Uninstall button, or `bash uninstall.sh "<game folder>"`; `--all` removes it from every game the
  launcher installed into, `--purge-settings` deletes the launcher settings, `--desktop` removes the menu entry.
- Command line without the launcher: see `README.txt`, e.g.
  `bash install.sh "<folder with the game's exe>" reshade-feeder --dll ~/Downloads/nvngx_dlssnr.dll`.

Licences: `LICENSE`, `NOTICE`, `licenses/Mochizuki_MIT.txt`; third-party licences ship next to their components
(see `README.txt`). NVIDIA DLLs and model weights are not included and must never be redistributed.
