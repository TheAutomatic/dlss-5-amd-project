# 架构总览

## 管线

```
游戏（DLSS / NVNGX 调用，D3D12）
  -> OptiScaler 代理（dxgi.dll / winmm.dll / ...）
       拦截超分调用，取 Color / Depth / MV / 曝光等输入
  -> pre-SR 神经渲染（[DlssNr] RunBeforeSR=true）
       后端二选一：[DlssNr] NrBackend = daniel | lmxxf
  -> 超分（FSR / XeSS，由 OptiScaler 原有路径完成）
  -> Present
```

NR 在超分**之前**作用于低分辨率 Color。`[DlssNr] Enabled=false` 关掉整个 pass；`NrBackend` 只选后端，不作开关（旧值 `off`/`none` 在首次加载时迁移成 `Enabled=false`）。

## 两个后端

| | daniel | lmxxf |
|---|---|---|
| 运行时 | danielblnc 的闭源 `version.dll`，安装器复制成 `dlssnr_amd_pass1-3.dll` | `LmxxfNrRuntime.dll`（本仓库的 C-ABI runtime，包着上游 MIT 代码）+ `lmxxf-modules/` + `shaders/` |
| 权重 | 用户自备 `nvngx_dlssnr.dll` 由作者 setup 生成 `dlssnr_on_amd_weights.bin`，或现成的 bin | `native-game-tiled-assets/`（不随 GitHub 包分发） |
| 驱动方式 | 按 SHA256 与尺寸识别版本，只驱动 `AmdLayout.h` 里钉过的版本（0.3.0–0.5.0）；加载与调用方式见 `AmdLayout.h`、`RuntimeHostLoad.h`。未知版本 fail closed | `LoadLibrary` + 唯一导出 `LmxxfNrGetApi`，契约见 [lmxxf-c-abi.md](lmxxf-c-abi.md) |
| 同帧 | 作者 runtime 自己的 inline 等待（compute 或 graphics/SpinDraw） | 宿主把游戏的 command list 拆成代理/逻辑列表，在同一帧内插入 NR；无法建模的列表（query、predication、未跟踪的屏障等）fail closed，本帧不做 NR |

后端选择（`dlssnr/backend/Selector.cpp`、`Kind.h` 的 `ResolveInstalled`）：探测 `OptiScaler.dll` 同目录下的 `dlssnr_amd_pass1.dll` 与 `LmxxfNrRuntime.dll`。显式请求的后端文件在就用它，不在就用另一个已安装的；缺 `NrBackend` 键时按 daniel 请求处理。探测结果缓存约 1 s。

lmxxf 后端细节见 [backends/lmxxf.md](../backends/lmxxf.md)。

## 代码归属

仓库是独立库（不是 GitHub fork，也没做历史嫁接），但保留了 Matheus 快照时期的提交。

| 部分 | 来源 |
|---|---|
| `OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/` 除 `dlssnr/amd/` 外 | optiscaler/OptiScaler → Dagherbou/OptiScaler_DLSSNR → wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass（直接上游）这条线，GPL-3.0 |
| `OptiScaler/dlssnr/amd/` | 由 Matheus（MatheusGViana/dlss-5-amd-project）建立，Dagherbou 与 wilsjo2 都没有这个目录；之后主要由本仓库重写 |
| `dlssnr/backend/`（含 `lmxxf_runtime/`）、`dlssnr/submission/`、多槽调度、安装器、打包、CI | 本仓库 |
| `third_party/lmxxf/` | lmxxf/dlss5-on-amd-9070xt-porting 的 vendored 源码闭包，MIT，见 [UPSTREAM.md](../../third_party/lmxxf/UPSTREAM.md) |
| `dlssnr.hlsl` 色彩合成 | RenoDX（clshortfuse），MIT |

`dlssnr/amd/` 的提交作者计数（`git log`，截至 d788963）：

| 文件 | 本仓库 | Matheus |
|---|---:|---:|
| `AmdPreSr.cpp` | 53 | 4 |
| `AmdBridge.cpp` | 33 | 4 |
| `AmdLayout.h` | 16 | 0 |

**许可证：GPL-3.0。** `MatheusGViana/dlss-5-amd-project` 跟随 OptiScaler，属 GPL-3.0 谱系（维护者 2026-09-28 确认）；该仓库根目录没有单独的 LICENSE 文件，GitHub API 因此返回 `license: null`，以谱系为准。`README.md` / `README.en.md` 把它标成 GPL-3.0 是对的。danielblnc 的 runtime 是另一回事：保留所有权利，本项目只做外部检测对接、不随包分发。

## 日志位置

| 文件 | 位置 | 内容 |
|---|---|---|
| `OptiScaler.log` | 代理 DLL 同目录 | OptiScaler 版本串、超分挡位；lmxxf 后端的 `lmxxf:` 行 |
| `amd_presr.log` | 同上 | daniel 路径：横幅、runtime 版本、Record / Completed / 边界、退休统计 |
| `amd_bridge.log` | 同上 | 桥接层：窗口与退出钩子、分辨率稳定等 |
| `dlssnr_on_amd.log` | 作者 runtime 写，商店版游戏在游戏目录的 `_storage_\` 子目录，不在代理同目录 | `network job`、`SPIKE` 等 |
| `DLSS5-AMD/native-game-flags.txt` | 游戏目录 | 不是日志；lmxxf 的可选 `DLSS5_*` 配置，只补菜单 / ini 未设置的键 |

商店（XGP）版游戏装在可写的 `Content\` 一类目录，不装进 `WindowsApps`（见 [installer.md](installer.md)）。

## 查看 wilsjo2 上游而不 fork

```sh
git remote add wilsjo2 https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass.git
git fetch wilsjo2
git log --oneline wilsjo2/main -- OptiScaler/dlssnr/   # 只看 NR 部分
git cherry-pick <sha>                                  # 逐个甄别后挑选
```

本仓库与上游没有共同祖先（根提交 `350cabf` 只有 README，下一个提交一次放入约 6751 个文件的快照），直接 `git merge` 会全面冲突。若将来确实要建立共同祖先，嫁接点选 wilsjo2 的 `d2b65cda`（2026-09-09，645/682 文件相同；`dlssnr/design/pre-sr-multipass.md` 只存在于 wilsjo2，不在 Dagherbou）。核对谱系时比较 blob SHA，不比较文件大小。当前决定是不嫁接，见 [decisions.md](../decisions.md)。
