# 安装器与卸载器

实现：`tools/install-amd-presr.ps1`（包内叫 `Setup.ps1`，由 `Setup.bat` 启动）、`tools/uninstall-amd-presr.ps1`（包内与游戏目录里叫 `Uninstall_OptiScaler_NR.ps1`）、模块包助手 `tools/lmxxf-module-package.ps1`。回归测试见 [tests/RELEASE-TESTS.md](../../tests/RELEASE-TESTS.md)；包的组成见 [release.md](../release.md)。

## 后端选择

安装器检测三类文件，交互时选择本次安装的后端（见 [decisions.md](../decisions.md)）：

| 可装 | 判据 |
|---|---|
| lmxxf | 找到 `LmxxfNrRuntime.dll` 和 `lmxxf-modules/`（权重 `native-game-tiled-assets/` 另外查找） |
| daniel | 找到 SHA256 在白名单内的作者 runtime（`version.dll` 或 `dlssnr_amd_pass1.dll`），或 `dlssnr_on_amd_setup.exe`，或 `dlssnr_on_amd_weights.bin` |
| mochizuki | 找到 `MochizukiNrRuntime.dll` 和 `dlssnr-amd/shaders/`；用户模型 `dlssnr-amd/dlssnr.bin` 缺失时明确提示另行提取 |

| 情况 | 行为 |
|---|---|
| 只有一边 | 装那一边，`NrBackend` 写成它 |
| 两边都在，交互 | 三选一：只装 lmxxf / 只装 daniel / 都装（再选默认后端） |
| 两边都在，`-NonInteractive` | 两套都装，`NrBackend=lmxxf`，之后可改 ini |
| 都没有 | 报错退出 |

存在 mochizuki 时可选全部可用后端或其中一个；全部安装后再选活动后端。
`-Backend daniel|lmxxf|mochizuki|all` 可显式指定安装范围，默认 `auto` 保留交互选择。
非交互 `auto/all` 安装全部可用后端，活动后端按 lmxxf、daniel、mochizuki 的顺序选取。
mochizuki 使用独立配置，模型提取和首次编译见 [mochizuki.md](../mochizuki.md)。

daniel runtime 白名单覆盖 0.3.0 / 0.3.1 / 0.3.2 / 0.3.3 / 0.4.0 / 0.4.1 / 0.4.2 / 0.4.3 / 0.5.0 的完整 SHA256（`$expectedAuthor`），与 `AmdLayout.h` 的 `kAmd*` 行对应；未知 SHA 一律拒绝（fail closed）。加新版本时两处一起改，并验证「同尺寸改 1 字节」的文件会被拒。

## 旧安装的升级

检测到已有 OptiScaler（代理 DLL 被识别为 OptiScaler，或存在 `amd-presr-install.txt` 安装记录）时，在选完目录后立即问一次：

| 选项 | 行为 |
|---|---|
| Y（推荐） | 新包校验通过、需要复用的来源文件先保存后，自动调用**新包**的卸载器，成功后继续安装。权重和已有备份保留，OptiScaler 设置重置。卸载失败则停止安装 |
| N | 覆盖安装。旧的扁平模块目录也能升级（见下） |
| `-NonInteractive` | 默认覆盖；显式加 `-UninstallExisting` 才先卸载 |

## `OptiScaler.ini`

- 游戏目录没有 ini：放入包内文件。
- 已有 ini：第一项是 `Overwrite OptiScaler.ini (Recommended: major version update!)`，第二项保留旧 ini。`-NonInteractive` 走覆盖。
- 不论覆盖还是保留，之后都改写 `[DlssNr]`：

| 类别 | 键 |
|---|---|
| 强制写入（本次安装的选择） | `Enabled=true`、`RunBeforeSR=true`、`NrBackend=<本次选择>`、`LmxxfDiagnostic=off` |
| 只在缺失时补（用户偏好） | `DLSS5_FIT_LARGE`、`DLSS5_HIP_PDL`：优先迁移对应旧键的值，否则补 `true`；其余补 `LmxxfPaperWhite=1`、`AmdModelScale=1`、`AmdEncoding=0`、`AmdEveryFrame=true` |

包内模板里的 `NrBackend=lmxxf` 只是占位，安装器会改掉。程序在键缺失时的默认值与包内值不同：`Enabled` 与 `RunBeforeSR` 缺失时为 false，`NrBackend` 缺失时为 `daniel`。FitLarge 和 PDL 默认均为 true，正式键名由 `ConfigKeys.h` 的 `CfgKey::FitLarge` / `CfgKey::Pdl` 定义。`LmxxfFitLarge` / `LmxxfPdl` 仅供读取旧配置；新键已有有效值时优先，保存设置写正式键名。

## `DLSS5-AMD/native-game-flags.txt`

安装 lmxxf 时，安装器保留已有 flags 文件；文件不存在时只创建带说明和示例注释的空模板，不把 ini 设置复制进去。卸载器兼容旧安装留下的 `DLSS5_FIT_LARGE=` 行：移除这一行，保留其它内容；没有其它内容时才删除文件。

配置优先级：Ins 菜单 / `OptiScaler.ini` 优先；flags 文件和外部 `DLSS5_*` 环境变量只补宿主未设置的键；最后才用编译默认值。宿主通过 `CfgKey::PutEnvAlias` 传递设置，runtime 的 `ApplyFlagsFileFallback` 不覆盖这些选择。此前安装器把 FitLarge 强制写回 flags 的行为已移除。

## lmxxf 模块包校验（双架构）

包内 `lmxxf-modules/` 必须是双架构布局，`tools/lmxxf-module-package.ps1` 在改动游戏目录**之前**完成全部校验：

- 恰好 `gfx1200/` 与 `gfx1201/` 两个架构目录，不允许其它 `gfx*`；根目录不允许旧的 `modules.json`。
- 每个架构包含契约规定的已知模块；数量以 [发版模块契约](../release.md#模块数量契约追-lmxxf--增删-hsaco-时最容易漏) 为准。根 `SHA256SUMS` 与各叶子 `SHA256SUMS` 一致，且与实际文件哈希一致；`modules.json` 与 `runtime-manifest.json`（schema、ABI、targets、计数）一致。
- 覆盖升级时先生成并验证候选目录，再把旧目录整体移入 `backup-amd-presr-*/lmxxf-modules`，最后切换；切换失败恢复旧目录。这只保证模块目录不留混合布局，不代表整套卸载加安装具备事务回滚。用户额外放的 `.hsaco` 留在备份里。
- runtime 自身在读清单和模块前也检查根目录与每级路径的 reparse 属性，拒绝 junction 和 symlink。

## 卸载器

- 对自己所在目录（游戏目录）执行，也可传 `-GameDir`。
- 先问是否保留 `backup-amd-presr-*`（`-RemoveBackups` 可在非交互时删除），再列出计划删除的文件，Y/N 确认。
- 只删明确依赖：识别为 OptiScaler 的代理、本项目的 pass/配置/日志、清单记录的依赖、受控名称的模块。不按扩展名清扫，不递归删整个 OptiScaler 目录。
- 保留：`nvngx_dlssnr.dll`、两类权重、作者的 setup 与日志、其它代理、用户插件和未知文件。`_storage_`（商店版游戏的作者日志位置）按同样规则处理。
- mochizuki 只移除 runtime、日志和已知 shader 文件；保留用户的 `dlssnr-amd/dlssnr.bin` 和本机 pipeline cache。

## 其它规则

- **不往 `WindowsApps` 写。** 路径含 `\WindowsApps\` 时安装器拒绝（ACL/TrustedInstaller）。商店版游戏装到可写的 `Content\` 之类目录。
- 本项目的代理与作者原生的 `version.dll` 注入互斥，不要在同一目录双注入；安装器不在游戏目录留下作者的 `version.dll`，而是复制成 `dlssnr_amd_pass1-3.dll`。
- 代理默认 `dxgi.dll`，交互时可选 `winmm.dll` / `d3d12.dll` / `winhttp.dll` / `wininet.dll` / `dbghelp.dll`。
- 含中文的 `.ps1` 必须是 UTF-8 BOM + CRLF，否则 Windows PowerShell 5.1 按 GBK 解析（见 [dev-environment.md](../dev-environment.md)）。
