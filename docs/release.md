# 发版

无卡回归测试（安装、卸载、模块包、runtime 校验、sync）只写在 [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)，这里不重复。本页是打包前后要核对的规则。`PACKAGE_RELEASE.ps1` 不跑那些测试，检查模块契约并做新鲜度门禁（`tools/release/check-release-freshness.ps1`：打包用的 DLL 或 `.hsaco` 比源码旧就中止）。

## 版本号

- 版本串来自仓库根的 `VERSION`；`PACKAGE_RELEASE.ps1` 未显式给 `-Version` 时读它，并把 `VERSION` 放进包里。
- tag 为 `v<版本>`。CI 在 tag 名含 `-alpha`、`-beta` 或 `-rc` 时发成 prerelease，否则是正式版（Latest）。只认这三个后缀。
- 1.8.x 时代的形式是 `1.8.x-0.3.y`（本 fork 版本 + 支持的 daniel runtime 版本）；1.9 起双后端，不再带 runtime 后缀。
- **1.9.0.x 已全部撤包。** 本地遗留的 annotated tag `v1.9.0` 和 `dist/OptiScaler-AMD-PreSR-1.9.0.3.zip` 不要复用，任何新版本都不要再用 `1.9.0.x` 这个号。手动触发留空时读取 `VERSION`，也可显式指定版本。

## 构建

| 项 | 规则 |
|---|---|
| 入口 | `tools\build\build-release-local.cmd`：先跑 `tests\run-all.cmd --tier ci,device`，再编 runtime，最后 MSBuild，输出 `exports/release-local/OptiScaler.dll` 与 `LmxxfNrRuntime.dll`，成功打印 `BUILD_OK`。仅编译本体时可用 `tools\build\build-release-local.cmd --fast`（不替代测试） |
| 工具集 | 与 CI 一致：`PlatformToolset=v145`，MSVC 14.44（本地脚本钉 `VCToolsVersion=14.44.35207`；CI 取镜像里最新的 14.44.x，缺失时 preflight 报错） |
| 宏 | 发行构建不定义诊断宏（`AMD_RETIRE_DIAGNOSTICS`、`AMD_TIMING_DIAGNOSTICS` 等）。诊断构建只用于取证，不能拿去打包或报数 |
| 记录 | 打包前记下 `OptiScaler.dll` 的 SHA256；引用帧率时附构建脚本与这个 SHA（见 [measurement.md](measurement.md)） |
| modules | CI 与 `PACKAGE_RELEASE` 都只拷贝**已提交**的 `third_party/lmxxf/modules`，CI 不重编 HIP 内核。发版前确认 modules 与当前 `hip/` 源码一致（sync 负责重编，见 [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)） |
| shader-cache | `shader-cache/*.dxbc` 不进 git、不进包。runtime 首次运行时编译 4 条（编码、解码、RGB 输入、RGB 贴图），在开发机上约 30–45 ms；目录可写就缓存 |

## 本地发版清单（与 CI 对齐）

`PACKAGE_RELEASE.ps1` **不会**跑安装/ABI/sync 全套，只做新鲜度 + 模块契约。要尽量贴线上，发版前按顺序：

1. `tests\run-all.cmd --tier ci`（至少；有卡再加 `device`）
2. `tools\build\build-release-local.cmd`（完整：测试 + runtime + OptiScaler；不要用 `--fast` 充当发版构建）
3. `powershell -File tools\release\check-module-contract.ps1`
4. `powershell -File tools\release\PACKAGE_RELEASE.ps1`
5. 打 `v<VERSION>` tag 推远程，**以 CI 上传的 zip 为准**（不要用本机 `dist/` 冒充）

### 模块数量契约（追 lmxxf / 增删 hsaco 时最容易漏）

**当前约定：每架构 31 / 双架构 62。**  
唯一权威数字在 `tools/release/check-module-contract.ps1` 的 `$PerArch` / `$Dual`。追上游若 `hip/build-modules.ps1` 增删了 `name = '...'` 行，必须**同一次改动里**改完契约点，再跑该脚本；否则本地只重编 modules 会过，线上 install/ABI 仍按旧数断言。

| 必须同步的位置 | 内容 |
|---|---|
| `tools/release/check-module-contract.ps1` | `$PerArch`、`$Dual` |
| `third_party/lmxxf/hip/build-modules.ps1` | recipe 行数 = `$PerArch` |
| `third_party/lmxxf/modules/gfx1200`、`gfx1201` | 各 `$PerArch` 个 `.hsaco` + `SHA256SUMS` + `modules.json`（**提交进 git**） |
| `LmxxfNrRuntime.cpp` | `kKnownModuleNames[$PerArch]` 与名字列表 |
| `tools/release/check-release-freshness.ps1` | `$hs.Count -ne $PerArch` |
| `tools/lmxxf-module-package.ps1` | 期望计数与 manifest `module_count` / `module_count_per_arch` |
| `tests/_lib/lmxxf_fixtures.py` | `MODULE_NAMES` 个数 |
| `tests/lmxxf/lmxxf_nr_abi.cpp` | `modules_ok=$Dual` / `modules_ok=$PerArch` |
| `tests/lmxxf/test_runtime_validation.py` | 同上 |
| `tests/install/test_module_packages.py`、`test_installer_exit.py` | 安装后每架构 / zip 合计断言 |

同步模块后还要：

- **重编 runtime**（`build-lmxxf-runtime.cmd`），否则 freshness 会报 `STALE LmxxfNrRuntime.dll`
- 若改了 `hip/*.hip` / `*.inc` 却没换 hsaco，freshness 会报 `STALE modules`
- 跑 `check-module-contract.ps1`，必须输出 `Module contract OK`
- 玩家包里是 **gfx1200+gfx1201 合计**，与 `runtime-manifest.json` 一致

漏改任一处 = 本地打包能过、GitHub Actions 在 install/ABI/契约处红，或线上模块数与 runtime 名表不一致。

## 包内容

`tools\release\PACKAGE_RELEASE.ps1` 输出 `dist/OptiScaler-AMD-PreSR-<版本>.zip`。`dist/` 只放发版产物，被 git 忽略。

包根目录：

| 项 | 说明 |
|---|---|
| `OptiScaler.dll`、`OptiScaler.ini` | ini 的 `[DlssNr]` 段由打包脚本生成 |
| `LmxxfNrRuntime.dll`、`lmxxf-modules/`（gfx1200 + gfx1201，数量见模块契约）、`shaders/`（只含顶层 `.hlsl`） | lmxxf 后端 |
| `Setup.bat`、`Setup.ps1`、`lmxxf-module-package.ps1` | 安装器，见 [architecture/installer.md](architecture/installer.md) |
| `Uninstall_OptiScaler_NR.bat`、`Uninstall_OptiScaler_NR.ps1` | 卸载器 |
| `README.md`、`README.en.md`、`README.es.md` | 直接拷贝仓库根的 README，没有第二份副本 |
| `VERSION`、`SHA256SUMS.txt` | |
| `OptiScaler/`、`Licenses/` | FFX / XeSS / Agility 依赖与第三方许可证 |
| `experimental_lighting/` | 可选 |

**禁止入包**（打包脚本与 CI 各有一道绊线，检查的是实际 zip）：`nvngx*.dll`、`dlssnr_amd_pass*.dll`、`dlssnr_on_amd_weights.bin`、`version.dll`、`dlssnr_on_amd_setup.exe`；打包脚本另外拒绝 `*.generated.hip`、`*.hsaco.s`。lmxxf 权重 `native-game-tiled-assets/` 也不进 GitHub 包（体积约 200 MB+）；给玩家的整合包可以在 zip 外另加。

## 编码规则

| 文件 | 要求 | 原因 |
|---|---|---|
| `SHA256SUMS.txt` | UTF-8 **无 BOM**、**LF**、正斜杠路径、`<hash> *<path>` | BOM 粘在第一个哈希前会让 `sha256sum -c` 报格式错误；CRLF 会让每个文件名带 `\r` 全部找不到 |
| 含中文的 `.ps1` | UTF-8 **带 BOM** + CRLF | Windows PowerShell 5.1 按 GBK 读无 BOM 的脚本，中文字面量乱码甚至解析失败；`.githooks/pre-commit` 会拦 |
| 生成的 `OptiScaler.ini` | UTF-8 无 BOM | |

## 实机烟测（双后端都要测）

| 后端 | 通过条件 |
|---|---|
| daniel | Ins 菜单 Status 显示识别到的 runtime 版本（来自布局表，不是写死的字符串）；`amd_presr.log` 与作者的 `dlssnr_on_amd.log` 正常；无设备移除 |
| lmxxf | Status 显示 lmxxf；Detail / Colour strength 与 Debug view 可调；`OptiScaler.log` 的 `lmxxf:` 行无错误 |
| 切换 | ini 里改 `NrBackend` 后重启游戏，两边都能跑；卸载不误删两类权重 |
| 退出 | 正常退出无崩溃、无 `DXGI_ERROR_DEVICE_REMOVED` |

GPU 输出哈希、9060 实机等不在无卡清单里，见 RELEASE-TESTS.md 的 D 节。

## CI 与 GitHub Release

- `.github/workflows/release.yml`：推 `v*` tag 或手动触发。依次跑安装/卸载回归、宿主契约回归、编 runtime、ABI 与 C 冒烟、编 OptiScaler、打包、专有文件绊线、上传。
- 核对线上资产用 **CI 产出的 zip**，不要用本机 `dist/` 冒充。
- Release 的 tag、CI 构建的 commit（`head_sha`）、正文里的源码链接必须指向同一个提交。
- 查 GitHub 状态用 REST（`gh api repos/TheAutomatic/dlss-5-amd-project/releases/tags/<tag>`），不要凭一次 GraphQL 失败下结论；`gh` 一律带 `-R TheAutomatic/dlss-5-amd-project`（见 [dev-environment.md](dev-environment.md)）。
- 保留已发布的 tag；改动以新版本发布。附件先暂存上传，核验大小和 SHA-256 后改成正式名称；网络失败会重试。同名内容一致可重跑，不同内容会拒绝覆盖。
- Release 正文写清用户需自备的文件：daniel 需要作者的 setup 与 `nvngx_dlssnr.dll`（或现成 `version.dll` + weights）；lmxxf 需要权重目录。

## 发版当天不要顺手修

- daniel 多槽的 abandon / 多 list 记账（已定案不做，见 [decisions.md](decisions.md)）。
- 测量专用开关与诊断宏。
- 改写 git 历史或强推已发布的 tag。
