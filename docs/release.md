# 发版

本页是发版流程的唯一入口（2026-10-09 优化更新），统一维护构建、契约核验、本地试包、远端预验证、GitHub Actions 权威发布与故障处理。

发版采用**“本地轻量自测与契约核验 + 远端 GitHub Actions 权威全量 CI”**的高效分工模式：
- **本地端（核心自测）**：仅执行秒级模块契约核查、产物新鲜度检查与核心宿主/ABI 测试（几秒至十几秒）；若需实机试玩，直接使用一键本地出包脚本。无需在本地开发机耗时数分钟模拟跑完 70+ 项系统安装器与全套卸载回归。
- **远端 GitHub Actions（权威发包）**：在干净的云端 runner 环境中执行完整发版 CI（涵盖完整 ABI、Shader 矩阵、70+ 项真实安装/升级/卸载/多进程模块包回归、代码签名与强绑定哈希生成），作为正式发布的唯一权威门禁。

无卡回归测试（安装、卸载、模块包、runtime 校验、sync）细节见 [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)。本页是打包前后要核对的规则。`PACKAGE_RELEASE.ps1` 检查模块契约并做新鲜度门禁（`tools/release/check-release-freshness.ps1`：打包用的 DLL 或 `.hsaco` 比源码旧就中止）。

## 版本号

- 版本串来自仓库根的 `VERSION`；`PACKAGE_RELEASE.ps1` 未显式给 `-Version` 时读它，并把 `VERSION` 放进包里。
- 日常发版只更新三语 README 的标题版本号，保留其 Release 更新日志入口，不再向 README 添加逐版更新摘要。中英文更新日志统一维护在 `.github/workflows/release.yml` 的发布正文中；安装说明等长期文档仅在功能用法确有变化时更新。
- 当前产品名为 **OptScaler(NR)**。品牌仅用于安装/卸载窗口、README、包名等产品展示：窗口显示 `OptScaler(NR) <版本>`，正式 ZIP 为 `OptScaler-NR-<版本>.zip`，Actions artifact 名为 `OptScaler(NR)`。ZIP 使用连字符，避免 GitHub 上传附件时将括号改为点号、导致按文件名核验失败。同步修改名称时检查打包、上传和下载路径。`OptiScaler.dll` / INI、旧日志名、源码目录及注册表位置保持兼容，旧安装记录仍可识别，新记录写入 `project=OptScaler(NR)`。
- **tag 与 GitHub Release 标题固定为 `v<版本>`**（例如 `v1.10.2`），不添加品牌或其他描述；workflow 的发布标题直接使用 `github.ref_name`。CI 在 tag 名含 `-alpha`、`-beta` 或 `-rc` 时发成 prerelease，否则是正式版（Latest）。只认这三个后缀。
- 1.8.x 时代的形式是 `1.8.x-0.3.y`（本 fork 版本 + 支持的 daniel runtime 版本）；1.9 起双后端，不再带 runtime 后缀。
- **1.9.0.x 已全部撤包。** 本地遗留的 annotated tag `v1.9.0` 和 `dist/OptiScaler-AMD-PreSR-1.9.0.3.zip` 不要复用，任何新版本都不要再用 `1.9.0.x` 这个号。手动触发留空时读取 `VERSION`，也可显式指定版本。

## 一键重编本地测试包

双击 `tools/release/BUILD_LOCAL_PACKAGE.cmd`，选择源码工作树目录，再输入如
`1.9.10.3` 的版本号。也可用参数运行 `BUILD_LOCAL_PACKAGE.ps1 -Root <源码目录>
-Version 1.9.10.3`；加 `-PlanOnly` 只检查和显示计划，不修改或构建。

脚本使用所选工作树（包括未提交修改），不切分支、不提交、不发布。重新编译 lmxxf
runtime；分支包含 Mochizuki 时也编译该 runtime 和着色器；宿主使用 MSBuild Rebuild。
HIP `.hsaco` 使用该分支已提交模块，不另行追更或重编 HIP 实验。Daniel 闭源 DLL 和
模型仍由安装器原有流程获取，不属于本仓库可编译目标。

依赖从所选工作树或主工作区 `dist/exports` 的完整解压包自动寻找；缺失时先解压一份
已有完整包再运行。脚本必须在已配置编译工具与子模块的开发机运行。MSBuild 参数与
`build-release-local.cmd` 使用相同的固定工具链，升级工具链时须同时更新这两个入口。

输出为所选工作树 `dist/OptScaler-NR-<版本>-local-<时间>.zip`，避免覆盖旧包；
日志在 `exports/local-build-<时间>/`。成功后 VERSION 保留输入版本；失败恢复原 VERSION，
停止后续打包。使用 LocalTest，不自动跑完整发版测试；已有产物新鲜度、依赖和 ZIP 校验
照常执行。不要与同一工作树中的其他构建同时运行。

## 构建

| 项 | 规则 |
|---|---|
| 入口 | `tools\build\build-release-local.cmd`：先构建或按内容校验复用 Mochizuki，再编 lmxxf runtime，通过 LMXXF_TEST_RUNTIME 让 `tests\run-all.cmd --tier ci,device` 验证同一 DLL，最后 MSBuild。输出 `exports/release-local/OptiScaler.dll` 与 `LmxxfNrRuntime.dll`，成功打印 `BUILD_OK`。仅编译本体时可用 `tools\build\build-release-local.cmd --fast`（不替代测试） |
| 工具集 | 本地与 CI 均固定 `PlatformToolset=v145`、MSVC `14.44.35207`、Windows SDK `10.0.26100.0`。runtime、测试和宿主使用同一环境；Actions 检查实际环境，缺失就失败。升级时同时改本地构建入口、`tests/_lib/msvc-env.cmd` 和 workflow，再验证 |
| 宏 | 发行构建不定义诊断宏（`AMD_RETIRE_DIAGNOSTICS`、`AMD_TIMING_DIAGNOSTICS` 等）。诊断构建只用于取证，不能拿去打包或报数 |
| 记录 | 记录源码 SHA、子模块状态、host/runtime/模块清单及最终 zip 的 SHA256、测试命令与结果；引用帧率时附构建脚本与 host SHA（见 [measurement.md](measurement.md)） |
| modules | 流程要求使用**已提交**的 `third_party/lmxxf/modules`；打包器读取工作树，故本地必须先检查干净状态。CI 不重编 HIP 内核。发版前确认 modules 与当前 `hip/` 源码一致（sync 负责重编，见 [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)） |
| shader-cache | `shader-cache/*.dxbc` 不进 git、不进包。runtime 私有加载 System32 编译器；缓存身份包含编译器、目标、flags、源码及 include。冷/热编译和曝光等实际变体由 shader 回归验证，不从本机缓存复制预编译结果 |

## 减少重复验证（2026-10-05）

日常小修运行受影响领域的专项，不把默认完整构建入口当作每次编辑后的检查。
发布前仍运行 `tests\run-all.cmd --tier ci`：当前 DLL 的 ABI、host/WARP、shader、
安装升级/卸载、包内容和凭证检查每次执行；GPU/device 按实际改动补充，不被缓存替代。

同步工具完整回归与本地试包启动器回归由 `tests\sync\run.cmd` 统一管理。
只有测试输入内容、Python/PowerShell/Git/Windows/编译环境和当前 UTC 周全部匹配，
才可复用 `exports/test-cache/sync.json` 中的成功记录。输入包含测试与夹具、
同步/审计/构建/打包工具、lmxxf源码和模块、配置键及Git属性；新增、删除或修改均失效。
缺失、损坏、环境变化或跨周自动全跑；失败先移除旧记录，测试中途输入变化不发新记录。
用 `tests\sync\run.cmd --force` 强制完整运行。周一 UTC 后首次使用会重新验证，
不是后台定时任务。单跑该工具套件不生成 runtime CI 凭证。

完整CI中的 `PASS sync` 可能对应当次运行或日志明确标为 `REUSED` 的匹配成功记录。
`--skip-sync` 仍只用于开发排查，不能生成发版凭证；不能手工编造缓存记录。
不跨输入变化复用整个CI，不缓存GPU测试结果，不因同一源码重新编出了不同时间戳
就机械重复所有历史GPU场景；按实际源码/工具链/模块变化和产物身份判断证据范围。

Mochizuki 使用 `build-mochizuki-runtime.cmd --reuse`：源码、生成器、构建脚本、
Vulkan导入库、MSVC/SDK/Python身份、DLL及全部shader哈希匹配才复用。
旧清单或任何不匹配均重建；不传 `--reuse` 可强制完整构建。
本地完整构建入口已启用此选项，一键交互式“重编试包”仍按其承诺强制重编。

Actions按精确键缓存上述Mochizuki产物及工具测试记录，不使用模糊restore key，
不缓存用户模型、驱动pipeline cache或ABI通过记录。恢复后仍校验实际文件并重跑ABI。
`tools/build/build-ci.ps1` 在同一runner并行执行宿主构建和CI，分别输出
`exports/release-ci/host.log`、`tests.log`及对应stderr日志；两者完成且成功后才打包。
失败不会进入后续上传/发布。CI仍使用同一次测试的lmxxf DLL和自动生成的哈希凭证。

最终包模块契约、Mochizuki构建清单、源码新鲜度、DLL配套及ZIP逐文件哈希均保留。
缓存仅减少重复工作，不能代替上游接入审阅、当前产物检查、游戏验收或远端验证。

## 发版清单（本地核心核对 + 远端权威 CI）

### 执行顺序

1. **固定待发布源码与文案。** 更新 `VERSION`、各语言 README 标题和 `.github/workflows/release.yml` 的发布正文，确认版本一致且描述准确。检查 `git status --short` 确认工作区无未跟踪/未提交的脏改动。
2. **本地核心契约与新鲜度核验（~5 秒）。**
   在本地运行模块契约与产物新鲜度检查，确保 40/80 模块契约与当前源码匹配：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools/release/check-module-contract.ps1
   powershell -NoProfile -ExecutionPolicy Bypass -File tools/release/check-release-freshness.ps1
   ```
3. **本地核心宿主与 ABI 快速自测（~15 秒）。**
   运行轻量宿主 CI 单元测试（校验 ABI 兼容、配置优先级、字体字形覆盖与设备判定）：
   ```powershell
   cmd /c "tests\host\run.cmd ci"
   ```
4. **（可选）本地快速试玩打包（~1 分钟）。**
   若开发者或测试人员需要在本地实机试玩验证，直接调用本地出包工具（自带 LocalTest，1 分钟内重编 runtime 与宿主并生成带签名测试 zip，跳过耗时的安装器场景模拟）：
   ```powershell
   pwsh -File tools/release/BUILD_LOCAL_PACKAGE.ps1 -Root . -Version <VERSION>
   ```
   产物输出至 `dist/OptScaler-NR-<VERSION>-local-<时间>.zip`。
5. **推送分支并触发 GitHub Actions 远端全量预验证。**
   推送待验证分支到 GitHub。在 GitHub 仓库的 **Actions** 页面，找到 `Release` 工作流，点击 **Run workflow**（`workflow_dispatch` 模式，不创建 tag/Release）。
   - GitHub Actions 在独立的纯净 Windows 环境中自动跑完包含全部 70+ 项安装回归、模块契约、真实自签名与包校验的完整 CI 流程；
   - 验证通过后（绿勾），下载该 Run 生成的 Release Artifact，核对 zip 与 `SHA256SUMS.txt`。
6. **确认发布并创建 tag。**
   远端 Actions 预验证全绿后，在已验证的提交上创建并推送 tag：
   ```bash
   git tag v<VERSION>
   git push origin v<VERSION>
   ```
   tag workflow 会自动触发全量构建、复核并发布 GitHub Release，正式挂载 release zip 附件。
7. **核验线上正式 Release 附件。**
   以成功 tag run 生成的 zip 为准，核对 GitHub Release 页面上的附件、SHA256 哈希、Release Notes 与版本号一致性。

> **注：离线/全本地完整构建后门（旧流程备用）**
> 若遇断网或特殊离线环境必须完全在本地生成 `runtime-ci.sha256` 凭证并正式打包，可仍按旧流程执行（耗时约 5~8 分钟）：
> ```powershell
> cmd /d /c tools\build\build-release-local.cmd
> powershell -NoProfile -ExecutionPolicy Bypass -File tools/release/PACKAGE_RELEASE.ps1 `
>     -OptiDll exports/release-local/OptiScaler.dll -OutDir dist
> ```
> 日常开发与正式发包均推荐采用上方 GitHub Actions 远端权威流程，避免在本地开发机重复跑 70+ 项耗时测试。

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

**当前约定：每架构 40 / 双架构 80。**
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

### 自动 Authenticode 自签名

本地与 Actions 共用 `tools/release/package-signing.ps1`。先完成原始 host/runtime
的测试凭证、源码新鲜度、复制哈希和禁止内容检查，再签署 staging 内所有 DLL、EXE、
ASI、OCX，最后生成 `SHA256SUMS.txt` 和 ZIP。`exports/` 中的已测试产物及其凭证不变；
`SIGNATURES.json` 记录每个二进制的原始/签后 SHA256、证书指纹和验证结果。签名后只允许
PE checksum、安全目录和末尾证书变化，其余原始字节必须一致。ZIP 校验使用签后字节。

有效的已有嵌入签名保留原字节，不用开发证书覆盖第三方发布者。只有本机目录信任的
catalog 签名不随文件分发，所以按未嵌入签名处理。坏摘要、非法 PE、签名失败均中止打包。
`WinVerifyTrust` 离线校验不下载吊销信息；允许缺少可信根/证书链，并将结果写入清单。

本地默认复用 CurrentUser/My 中 `CN=OptiScaler Dev SelfSign` 的有效代码签名证书；
缺失则生成 RSA3072/SHA256、私钥不可导出的五年证书。可用
`PACKAGE_RELEASE.ps1 -SigningThumbprint <指纹>` 显式选择有效证书。不导入 Root 或
TrustedPublisher，也不需管理员权限。自签名符合 Authenticode 格式，但**不等于 Windows
信任的发布者**，不保证所有 Loader/杀软放行。没有时间戳服务，证书到期后需更新包。

Actions 每次创建独立证书，打包步骤传入其指纹，随后的 `always()` 步骤删除证书及私钥；
不上传私钥，不把证书写入源码。临时的是私钥持有时间，证书有效期仍是五年。失败或强制
终止的 runner 随 GitHub 托管环境销毁。专项入口 `tests/install/package-signing.ps1`
已接入安装领域测试，检查真实签名、别名、保留签名、重复调用、篡改拒绝和 manifest 哈希。

本地 `BUILD_LOCAL_PACKAGE.ps1` 在构建成功后取得证书，再调用同一打包器；支持
`1.10.5-alpha` 等版本。`-PlanOnly` 不创建证书。LocalTest 与完整发版验证的区别不变。

`PACKAGE_RELEASE.ps1 -OutDir dist` 在 `dist/` 生成 `OptScaler-NR-<版本>/` 打包目录和同名 `.zip`，本地测试与正式发布统一使用这一位置。Actions 在独立 runner 的 `dist/` 生成附件；线上发布仍须使用通过远端验证的产物。

包根目录：

| 项 | 说明 |
|---|---|
| `OptiScaler.dll`、`OptiScaler.ini` | ini 的 `[DlssNr]` 段由打包脚本生成 |
| `LmxxfNrRuntime.dll`、`lmxxf-modules/`（gfx1200 + gfx1201，数量见模块契约）、`shaders/`（只含顶层 `.hlsl`） | lmxxf 后端 |
| `MochizukiNrRuntime.dll`、`dlssnr-amd/shaders/`、`Mochizuki-Model.bat`、`mochizuki-python.ps1`、`mochizuki-model.py`、`model-tools/` | mochizuki 后端及用户模型提取工具；共用 Python 探测与商店安装指引，不含 `dlssnr.bin` |
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

## 实机烟测（所有交付后端）

| 后端 | 通过条件 |
|---|---|
| daniel | Ins 菜单 Status 显示识别到的 runtime 版本（来自布局表，不是写死的字符串）；`amd_presr.log` 与作者的 `dlssnr_on_amd.log` 正常；无设备移除 |
| lmxxf | Status 显示 lmxxf；Detail / Colour strength 与 Debug view 可调；`OptiScaler.log` 的 `lmxxf:` 行无错误 |
| mochizuki | 首次编译完成后 Status 显示正在运行；画面控制、整体强度、分组 reset 可用，计时来自完成的 Vulkan query；切换分辨率和退出无设备移除 |
| 切换 | 开启热切换时验证菜单双向切换；`NrConvenience=0` 时分别重启验证所选后端；卸载不误删两类权重 |
| 退出 | 正常退出无崩溃、无 `DXGI_ERROR_DEVICE_REMOVED` |

GPU 输出哈希和实机覆盖范围见 [测试入口说明](../tests/RELEASE-TESTS.md)。未安装的游戏或没有的硬件明确记录 SKIP，不将本地无卡 PASS 写成游戏验证通过。

### 安装更新与历史残留验证

整包里的runtime、模块和HLSL是一套更新来源。Setup缺少lmxxf任一组件时，
在卸载/写入前提示重新解压完整包，不再从游戏目录补齐旧代码；游戏权重仍可复用。
核对运行资源时要检查实际shader目录，不能只验DLL、HSACO和权重：runtime旁的
整包`shaders`优先于旧`lmxxf-modules/shaders`，两处旧native_/preblock_ HLSL
及可识别的编译缓存都属于卸载范围。模块升级暂存也必须排除旧嵌套shader，
否则即使选Y先卸载，也会从卸载前准备的暂存树重新装回它们。

Y直接执行新包的`Uninstall_OptiScaler_NR.ps1`，沿用独立卸载的同一套规则；
不调用游戏里可能过时的副本，失败返回码会阻止后续安装。旧release子目录布局
优先选择与更新DLL同目录的卸载脚本，控制台打印调用路径，安装记录写入
`uninstallFirst=true/false`，方便核对是否真正执行过卸载。

这些回归已纳入`tests/install/test_installer_exit.py`与`amd_uninstall.ps1`，
覆盖Y先卸载、N直接覆盖、独立卸载、重复安装、缺组件前置拒绝，以及权重/
自定义文件/链接保护；按本页现有CI入口运行，不另开一轮重复安装测试。
GPU的shader优先级回归包装既有HIP专项调用，同样不重复矩阵。

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


## 本地快速试包

用户明确要求快速试包时，可只编译改动的宿主/runtime，使用 `PACKAGE_RELEASE.ps1 -LocalTest -OptiDll <绝对路径> -OutDir dist`。此模式跳过完整 runtime CI 凭证要求，保留源码新鲜度、模块契约、实际 ZIP 和哈希检查，并写入 `LOCAL-TEST.txt` 标记未做完整发版验证。普通打包和 Actions 不传此参数，仍须匹配完整 CI 凭证；本地模式不能作为正式发布通过依据。

## 第三后端构建与打包

mochizuki 的本地完整构建与 Actions CI 共用 `tools/build/build-mochizuki-runtime.cmd`。
`tests/run-all.cmd --tier ci` 在缺少构建清单时构建它，校验源码/产物清单后执行
`tests/mochizuki/run.cmd abi`。`--tier gpu` 另需本机自备的 `dlssnr-amd/dlssnr.bin`。
打包器核对 `exports/mochizuki-runtime/build-manifest.json` 与 ABI 产物哈希；没有模型也能
进行无 GPU 构建及打包。公开包仅带 runtime、shaders、模型提取工具和许可，不带模型或本机 cache。
安装、独立配置和首次编译说明见 [mochizuki.md](mochizuki.md)。
