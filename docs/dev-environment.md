# 开发环境与工具

本页收集反复踩过的工具与环境坑，以及当前 `tools/`、`tests/` 的索引。发版流程见 [release.md](release.md)，无卡回归见 [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)，lmxxf sync 见 [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)。

## 工具与测试索引

菜单中文字体重建：`python tools/build/build-menu-font.py --source <NotoSansSC原字体>`，需要 fonttools；
普通构建使用已提交字体，不联网下载。来源、许可及更新规则见 [菜单本地化](architecture/menu-localization.md)。

所有脚本都在仓库根目录运行。每类东西放哪，见 [workspace.md](workspace.md)。

| 工具 | 用途 |
|---|---|
| `tools/build/build-release-local.cmd [--fast]` | 与 CI 对齐的发行构建，输出 `exports/release-local/`；默认跑 ci+device 层测试，`--fast` 只编 `OptiScaler.dll` |
| `tools/build/build-lmxxf-runtime.cmd [输出目录]` | 编 `LmxxfNrRuntime.dll`：有 `cl.exe` 用 MSVC，否则 MinGW（`LMXXF_GXX`）。旧路径 `tools/build-lmxxf-runtime.cmd` 暂留一个转发脚本，等安装/同步脚本迁移后删除 |
| `tools/build/build-rtgi-shaders.cmd` | 由 `assets/experimental_lighting/Lighting.hlsl` 重建两个 `.cso`（逐字节一致） |
| `tools/build/write-amd-graphics-source-id.ps1` | 生成日志里的宿主源码指纹（MSBuild 自动调用） |
| `tools/release/PACKAGE_RELEASE.ps1`、`tools/release/check-release-freshness.ps1` | 出 zip；新鲜度门禁 |
| `tools/install-amd-presr.ps1`、`tools/uninstall-amd-presr.ps1`、`tools/lmxxf-module-package.ps1`、`tools/stage-lmxxf-beside-optiscaler.*` | 安装、卸载、模块包、开发部署。计划移入 `tools/install/`，等上游集成审阅收尾后再动 |
| `tools/sync-lmxxf-upstream.ps1`、`tools/audit-lmxxf-enablements.py` + `tools/lmxxf-sync/` | 上游同步与审阅门禁。计划整体移入 `tools/lmxxf-sync/`，时机同上 |
| `tools/diag/set-lmxxf-diagnostic.ps1` | 改游戏 ini 的 `LmxxfDiagnostic`（`-GameDir` 必填） |
| `tools/diag/retirement_stats.py` | 汇总 `AMD_RETIRE_DIAGNOSTICS` 构建输出的 retirement 采集 |
| `tools/dev/scan-mojibake.py` | 扫描乱码（UTF-8 标点被按 GB18030 解码后写回，典型：U+9225 + `?`） |

测试按领域分目录，每个领域一个 `run.cmd`，统一入口：

```
tests\run-all.cmd --tier ci|device|gpu|all [--out 目录] [--skip-sync] [--keep-going]
```

| 层 | 需要 | 内容 |
|---|---|---|
| ci | 无 GPU（CI 跑这一层） | `tests/host`、`tests/shader`、`tests/lmxxf` 的 abi 与 WARP 用例、`tests/install`、`tests/sync` |
| device | 硬件 D3D12 | `tests/host` 的 D3D 场景、`tests/lmxxf` 的 list split / List1 / create-execute / evaluate-cut |
| gpu | AMD GPU + `LMXXF_ASSETS`（权重目录） | `tests/lmxxf/lmxxf_nr_gpu`（各输出哈希模式）、`lmxxf_bridge_zero_gpu` |

用例清单与哈希基线见 [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)。`lmxxf_nr_gpu` 必须在仓库根目录运行（runtime 有一个相对 cwd 的 `third_party\lmxxf\shaders` 开发兜底路径）。

## Windows 构建环境

- 本机 MSVC 是 **VS Build Tools**（构建脚本写死了 VS 18 Build Tools 的默认安装路径，并用 `vcvarsall.bat x64 -vcvars_ver=14.44`）。`cl.exe` 只在 Build Tools 的开发者命令行里可用。
- lmxxf sync 每次都用 `cl.exe` 从 `rtc_compile.cpp` 重编 `rtc_compile.exe`（编译失败就停，不用旧 EXE），所以 sync 要在开发者命令行里跑。
- `.githooks` 需每个 clone 启用一次：`git config core.hooksPath .githooks`（本地配置，不随 clone 走）。

## PowerShell 的坑

| 坑 | 表现 | 对策 |
|---|---|---|
| PS 5.1 `Get-Content` 不带 `-Encoding UTF8` | 无 BOM 的 UTF-8 被按 GBK 读，写回后 em dash 变成 U+9225 + `?`。`third_party/lmxxf/UPSTREAM.md` 反复损坏的真正原因就是 sync 脚本这样读写（`deef416` 修复） | 5.1 下读文本一律带 `-Encoding UTF8`；写用 .NET 并明确是否要 BOM |
| `-Encoding UTF8` 写文件 | PS 5.1 会写 BOM | 需要无 BOM 时用 `[IO.File]::WriteAllText(..., [Text.UTF8Encoding]::new($false))` |
| 继承来的 `PSModulePath` | 从 Git Bash 或 pwsh 7 启动 `powershell.exe` 时，PS7 模块目录排在最前，5.1 加载了不兼容的 `Microsoft.PowerShell.Utility`，`Get-FileHash` 找不到 | 脚本里用 .NET SHA256 或 shim；排查时把 `PSModulePath` 限定为 Windows PowerShell 自己的目录 |
| 函数里的 `Write-Output` | 输出混进返回值，变量变成「输出行 + 路径」数组，后续参数全错位，报出看似无关的 `NamedParameterNotFound` | 函数里提示用 `Write-Host`，只 return 真正的值 |
| 含中文的 `.ps1` 丢 BOM | 5.1 按 GBK 解析，乱码或直接解析失败；曾因此发出过文件名乱码的包 | 保持 UTF-8 BOM + CRLF；`.githooks/pre-commit` 会拦 |

## Shell 与编码

- **Git Bash 的 `tar` 是 GNU tar**，把 `C:\...` 当成远程主机（`Cannot connect to C:`）。用相对路径，或用 `System32\tar.exe`。
- **bash heredoc 会损坏非 ASCII**，也会把 `\n` 之类转义变成真换行（曾把 C++ 字符串字面量拆成两行）。改含中文或反斜杠的文件用编辑工具，不用 heredoc。
- **控制台是 GBK。** 中文和符号会乱码或抛 `UnicodeEncodeError`；结论写文件再读，不信终端显示。
- 仓库 `core.autocrlf=true`；`.gitattributes` 把 `.githooks/*` 钉成 LF，否则 sh 脚本混入 `\r`。

## git 查证的坑

这些错误让「查过了」看起来和「没问题」一样。报「没问题」前先想：这个查法会不会恒返回空或恒返回命中。

| 坑 | 正确做法 |
|---|---|
| `git cat-file --batch-check` 输入是 `SHA PATH` 时漏了 `%(rest)`，每行都 missing | `--batch-check='%(objecttype) %(objectsize) %(rest)'` |
| `git check-ignore` 对已在 index 里的路径一律报「未忽略」 | 加 `--no-index`；两侧都加 `-c core.quotePath=false` |
| `git log --name-only` 也列出删除该文件的提交 | 判断可达性用 `git rev-list --objects <ref>` |
| `git show :file` 读的是 index 不是工作区 | 读工作区用普通文件读取 |
| `while read` 循环里跑会读 stdin 的命令，循环输入被吃掉 | 先写文件，再 `done < file` |
| 路径参数多套一层引号（`-- "\"$pat\""`） | 引号成了字符串的一部分，结果恒为 0 |
| annotated tag 的 `rev-parse` 给的是 tag 对象 | 用 `<tag>^{commit}`；不要用 `grep -v '\^{}'` 过滤，会把真正的提交行滤掉 |
| 用短 SHA 测远端对象是否可取 | 用完整 40 位 SHA，并选一个确定不可达的 SHA 作对照 |
| 凭记忆写 SHA | 引用前先 `git log` 核对 |
| grep 关键词太松得到假阳性 | 看上下文，不只看命中 |

## 协作约定

- **`gh` 默认解析到上游（Matheus 的库）。** 一律带 `-R TheAutomatic/dlss-5-amd-project`。
- **可能有并行写入者**（另一个 agent 或会话）在同一工作树或上游 clone 里改文件。动手写或做破坏性 git 操作前先 `git status` 全量看，并检查 mtime；它改过的措辞不要改回去。
- 没推送的提交也不要改写：别的工作树可能已经合并了旧 SHA。改写前先 `git branch -a --contains <sha>`。
- 提交时显式 `git add <路径>`，不用 `git add -A`：有文件按要求只留本地。提交后核对 `git show --stat`。
- `.githooks/pre-commit` 拦两类：被 `.gitignore` 排除却用 `git add -f` 加进来的新文件；丢了 BOM 且含非 ASCII 的 `.ps1/.psm1`。`.githooks/commit-msg` 删除提交信息里所有 `Co-Authored-By:` 与 `*-Session:` 尾注（项目约定：提交不带这类尾注）。
- 本地工作材料放在被忽略的目录（`work/`、`exports/` 等），tracked 文件不得引用它们。

## Mochizuki 工具

- `tools/build/build-mochizuki-runtime.cmd`：MSVC runtime 与固定版本 glslang shaders。
- `tools/build/mochizuki-manifest.py`：源码及可分发产物哈希验证。
- `tools/install/mochizuki-model.py`：仅从用户提供的指定版本 DLL 提取模型。
- `tools/install/mochizuki-python.ps1`：Setup 与独立模型工具共用的 Python 3.10+ 探测；缺失或不可用时提示到 Windows 商店搜索 Python 安装。
- `tests/mochizuki/run.cmd abi|gpu`：无 GPU ABI 与实际 D3D12/Vulkan 生命周期回归。

- `tools/build/build-ci.ps1`：Actions的宿主/CI并行入口，需已配置MSVC环境与pwsh。
- `tools/lmxxf-sync/test-cache.py`：同步工具回归的内容/环境/UTC周成功记录；通过tests/sync/run.cmd使用。
