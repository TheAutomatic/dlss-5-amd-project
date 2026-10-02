# 发版

本页是发版流程的唯一入口（2026-10-01 整理），统一维护构建、测试、试包、远端预验证、发布与故障处理。此前单独的构建一致性说明已合并到本页。

日常开发先审最终 diff；文案和简单菜单修改不要求全测，ABI/GPU 同步/资源生命周期等风险只做相关专项验证。发版前对最终产物完整跑一次；已有且仍适用的结果不因进入另一个流程入口而重复执行。宿主与 runtime 整包配套安装，不维护旧 ABI 降级。

无卡回归测试（安装、卸载、模块包、runtime 校验、sync）只写在 [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)，这里不重复。本页是打包前后要核对的规则。`PACKAGE_RELEASE.ps1` 不跑那些测试，检查模块契约并做新鲜度门禁（`tools/release/check-release-freshness.ps1`：打包用的 DLL 或 `.hsaco` 比源码旧就中止）。

## 版本号

- 版本串来自仓库根的 `VERSION`；`PACKAGE_RELEASE.ps1` 未显式给 `-Version` 时读它，并把 `VERSION` 放进包里。
- tag 为 `v<版本>`。CI 在 tag 名含 `-alpha`、`-beta` 或 `-rc` 时发成 prerelease，否则是正式版（Latest）。只认这三个后缀。
- 1.8.x 时代的形式是 `1.8.x-0.3.y`（本 fork 版本 + 支持的 daniel runtime 版本）；1.9 起双后端，不再带 runtime 后缀。
- **1.9.0.x 已全部撤包。** 本地遗留的 annotated tag `v1.9.0` 和 `dist/OptiScaler-AMD-PreSR-1.9.0.3.zip` 不要复用，任何新版本都不要再用 `1.9.0.x` 这个号。手动触发留空时读取 `VERSION`，也可显式指定版本。

## 构建

| 项 | 规则 |
|---|---|
| 入口 | `tools\build\build-release-local.cmd`：先编 runtime，通过 LMXXF_TEST_RUNTIME 让 `tests\run-all.cmd --tier ci,device` 验证同一 DLL，最后 MSBuild。输出 `exports/release-local/OptiScaler.dll` 与 `LmxxfNrRuntime.dll`，成功打印 `BUILD_OK`。仅编译本体时可用 `tools\build\build-release-local.cmd --fast`（不替代测试） |
| 工具集 | 本地与 CI 均固定 `PlatformToolset=v145`、MSVC `14.44.35207`、Windows SDK `10.0.26100.0`。runtime、测试和宿主使用同一环境；Actions 检查实际环境，缺失就失败。升级时同时改本地构建入口、`tests/_lib/msvc-env.cmd` 和 workflow，再验证 |
| 宏 | 发行构建不定义诊断宏（`AMD_RETIRE_DIAGNOSTICS`、`AMD_TIMING_DIAGNOSTICS` 等）。诊断构建只用于取证，不能拿去打包或报数 |
| 记录 | 记录源码 SHA、子模块状态、host/runtime/模块清单及最终 zip 的 SHA256、测试命令与结果；引用帧率时附构建脚本与 host SHA（见 [measurement.md](measurement.md)） |
| modules | 流程要求使用**已提交**的 `third_party/lmxxf/modules`；打包器读取工作树，故本地必须先检查干净状态。CI 不重编 HIP 内核。发版前确认 modules 与当前 `hip/` 源码一致（sync 负责重编，见 [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)） |
| shader-cache | `shader-cache/*.dxbc` 不进 git、不进包。runtime 私有加载 System32 编译器；缓存身份包含编译器、目标、flags、源码及 include。冷/热编译和曝光等实际变体由 shader 回归验证，不从本机缓存复制预编译结果 |

## 本地发版清单（与 CI 对齐）

完整 CI 测试成功后，统一入口在输出目录生成 `runtime-ci.sha256`，同时检查
runtime 在测试期间没有变动；失败或 `--skip-sync` 不生成此凭证。
本地完整构建和 Actions 将已测试 DLL 与凭证一起复制到 `exports/lmxxf-runtime/`。
打包在替换 staging 之前检查两者匹配，并复核 staging DLL 与所选 host/runtime 字节一致；
实际 zip 的逐文件校验继续执行。手动组合构建时也须复制同一次成功 CI 的 DLL 和凭证，
不能在测试后重新编译 runtime 再沿用旧凭证。
此哈希只绑定测试产物，不能替代源码审阅、GPU 或游戏验证。

### 执行顺序

1. **固定待发布源码。** 更新 `VERSION`、各语言 README 和 release workflow 的发布正文，确认描述与实际验证一致。修复和版本变更提交后记录完整 SHA；检查 `git status --short` 和 `git submodule status --recursive`。发布验证用干净检出及完整子模块，不继承旧 `exports`。修改源码后重新构建受影响产物，不能继续沿用旧验证结果。
2. **完成上游接入再发版。** 涉及 lmxxf 同步时先按 [同步流程](../tools/lmxxf-sync/README.md) 审阅、补丁重放、模块来源与契约检查，确认 `sync-state.json` 为 reviewed 且审计针对当前接入内容有效。不能只看历史 reviewed 字样；pending、非零退出或仅 report-only 都不放行。发布 job 只使用提交的模块，不临时追移动的上游分支。
3. **构建并测同一 runtime。** 有 D3D12 设备的本机运行下方完整构建命令，已包含 `ci,device`，不用在前面再重复跑一次 CI。无设备时在非 tag Actions 上跑完整 CI/构建，并如实记录 device/GPU SKIP。根据改动补充 GPU、双后端烟测；已有验证仅在源码、配置和产物身份适用时沿用。
4. **本地试包与正式安装包统一放 dist。** 使用下方显式 host 路径和输出路径。打包器自动校验 runtime-ci 凭证、源码新鲜度、模块契约、包内容及实际 zip 哈希。`--fast`、`--skip-sync`、`-AllowMissingDeps`、`-WarnOnly`、`-AllowStaleModules` 不能作为发版通过依据。构建后不要再同步模块或重编 runtime 然后沿用旧测试凭证。
5. **先验证远端非 tag ref。** 推送待验证分支后，在该分支触发 release workflow 的 `workflow_dispatch`，核对 run 的 `head_sha`。这条路径构建并上传 Actions artifact，不创建 GitHub Release。检查完整 job 结果，下载该 run 的 artifact，核对 zip 与包内 SHA256SUMS；本地通过不能代替远端通过。若无法执行，明确记为待验证，不宣称两端一致。
6. **确认发布后再创建 tag。** `v<VERSION>` 指向已验证的同一提交，推送后 tag workflow 会重新构建并发布。若又修改了版本或正文，应先把新的提交验证好。tag run 是新的构建，核对它的 commit、测试结果和产物，不假定 zip 字节与预验证 run 相同。
7. **核验实际线上附件。** 以成功 tag run 的 zip 为准；核对 Release tag、run `head_sha`、版本与正文源码链接，并把下载附件的 SHA256 与该 run 产物对照。上传失败按已有暂存/哈希重试机制处理；不同内容的同名附件禁止覆盖，不强推旧 tag。记录最终 run URL、zip SHA 和跳过的实测项。

在仓库根目录执行（PowerShell）：

```powershell
cmd /d /c tools\build\build-release-local.cmd
if ($LASTEXITCODE -ne 0) { throw 'Release build or regression failed' }

powershell -NoProfile -ExecutionPolicy Bypass -File tools/release/PACKAGE_RELEASE.ps1 `
    -OptiDll exports/release-local/OptiScaler.dll -OutDir dist
if ($LASTEXITCODE -ne 0) { throw 'Release package validation failed' }
```

第二条命令默认读取 `VERSION`，输出目录与脚本默认值一致，均为 `dist/`。编译和测试仍在 `exports/`；交付用户测试或发布的打包目录与 zip 统一放 `dist/`。保护其他版本、第三方安装器和权重；重新打同版本包前核对目标及已保留的校验记录。已有同版本目录若含用户配置、权重或第三方 runtime，不得将其作为临时 staging 清空；在独立检出打包后只交付 zip。独立检出生成的包交付时也放到主工作区的 `dist/` 并复核哈希。目录不表示发布状态：本地试包须注明待游戏测试或待 Actions 验证。

### 失败时怎样继续

| 首个有效错误 | 处理规则 |
|---|---|
| 模块名/数量、arch、checksum、配方不一致 | 对照下方契约表、实际模块来源与同步审阅，修完消费者和产物再测；不只修改测试数字 |
| fixture 缺脚本/依赖 | 补完整真实夹具并测试原失败分支；不以总是成功的 stub 绕过门禁 |
| CI 与本地不同 | 先固定同一 SHA，核对子模块、MSVC/SDK、环境变量、构建顺序、缓存和选中的 DLL；日志保留首错、run URL、期待值与实际值 |
| runtime-ci 缺失/不匹配或产物陈旧 | 从完整构建/测试入口重新生成对应产物；不手写凭证、不改时间戳 |
| 上传断线 | 保留已构建产物，核对远端是否已接收，通过受控重试完成；不重新编一个不同 zip 覆盖同名附件 |

机器门禁目前覆盖编译器环境、测试退出码、runtime 身份、模块契约、新鲜度与 zip 内容。源码审阅质量、实际游戏表现及发版结论仍需按证据核对；文档不能替代这些验证。

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

`PACKAGE_RELEASE.ps1 -OutDir dist` 在 `dist/` 生成 `OptiScaler-AMD-PreSR-<版本>/` 打包目录和同名 `.zip`，本地测试与正式发布统一使用这一位置。Actions 在独立 runner 的 `dist/` 生成附件；线上发布仍须使用通过远端验证的产物。

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
| 切换 | 开启热切换时验证菜单双向切换；`NrConvenience=0` 时分别重启验证所选后端；卸载不误删两类权重 |
| 退出 | 正常退出无崩溃、无 `DXGI_ERROR_DEVICE_REMOVED` |

GPU 输出哈希和实机覆盖范围见 [测试入口说明](../tests/RELEASE-TESTS.md)。未安装的游戏或没有的硬件明确记录 SKIP，不将本地无卡 PASS 写成游戏验证通过。

## CI 与 GitHub Release

- `.github/workflows/release.yml`：推 `v*` tag 或手动触发。先检查工具链/依赖，再运行统一 CI（构建 runtime 并测试）、编宿主、打包、检查 zip、上传 artifact。创建 Release 和上传 Release 附件两步都有 tag 条件；手动触发时选择 tag 仍会发布，预验证必须选择非 tag 分支。
- 核对线上资产用 **CI 产出的 zip**，不要用本机 `dist/` 冒充。
- Release 的 tag、CI 构建的 commit（`head_sha`）、正文里的源码链接必须指向同一个提交。
- 查 GitHub 状态用 REST（`gh api repos/TheAutomatic/dlss-5-amd-project/releases/tags/<tag>`），不要凭一次 GraphQL 失败下结论；`gh` 一律带 `-R TheAutomatic/dlss-5-amd-project`（见 [dev-environment.md](dev-environment.md)）。
- 保留已发布的 tag；改动以新版本发布。附件先暂存上传，核验大小和 SHA-256 后改成正式名称；网络失败会重试。同名内容一致可重跑，不同内容会拒绝覆盖。
- Release 正文写清用户需自备的文件：daniel 需要作者的 setup 与 `nvngx_dlssnr.dll`（或现成 `version.dll` + weights）；lmxxf 需要权重目录。

## 已核实的 Actions 差异与历史失败

2026-10-01 核对后，不能把所有红灯都归因上游同步：

| run / 提交 | 首错与处理 |
|---|---|
| [36343700285](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36343700285) | 模块已增加，ABI 仍断言 `modules_ok=58`；`2b294be` 更新遗漏消费者 |
| [36388171199](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36388171199) | 测试夹具漏带 `check-module-contract.ps1`，4 项在到达目标断言前失败；`da21b03` 补齐真实依赖 |
| [36389642043](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36389642043) | 上传出现 `other side closed`；`4fef0c3` 增加暂存、哈希核验与重试 |
| [36748883931](https://github.com/TheAutomatic/dlss-5-amd-project/actions/runs/36748883931) / `9afa534` | 1.9.8.1 成功；只证明该提交，不能替代后续修复分支的远端验证 |

本轮另修正了两个实际流程缺口：`2263190` 将成功测试的 runtime 与打包 DLL 按哈希绑定；`8ed1b31` 将 Actions 的 runtime/测试编译器和 SDK 与宿主、本地入口统一。此前仅 MSBuild 选定编译器，setup-msvc-dev 仍可能采用 runner 默认值。

上述记录为 2026-10-01 的历史状态，不代表当前待发布版本已经通过验证。每次发布按具体提交和产物记录本地及远端结果；既有结果不能替代不同提交的验证。

## 发版当天不要顺手修

- daniel 多槽的 abandon / 多 list 记账（已定案不做，见 [decisions.md](decisions.md)）。
- 测量专用开关与诊断宏。
- 改写 git 历史或强推已发布的 tag。
