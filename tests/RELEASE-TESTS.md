# 发版前无卡测试清单

适用：`release/1.9.0`（及同构产品分支）在 **打包 / 合并后** 的回归。
**不含** GPU/游戏实测（那是 P-E-local / 9060 实机）。

工作目录：仓库根（`dlssnr_on_amd_setup`）。

---

## 何时跑

| 时机 | 必跑 |
|---|---|
| 合并功能/修复到 `release/1.9.0` 后 | 全套 A+B |
| 改 `tools/release/PACKAGE_RELEASE` / 安装 / 卸载 / 模块校验 | 全套 A+B |
| **增删 hsaco / 改模块列表** | **先跑 `check-module-contract.ps1`，再 A、C2** |
| 只改文档 / 注释 | 可跳过 |
| 准备打 zip 发给玩家 | 全套 A+B + **C2 ABI**，再 `PACKAGE_RELEASE` |

`PACKAGE_RELEASE.ps1` **不会**自动跑这些测试；它只做 **新鲜度门禁**（`check-release-freshness.ps1`）和模块契约（`check-module-contract.ps1`）。

---

## 统一入口

```cmd
tests\run-all.cmd --tier ci
```

包括配置、发布上传、安装、模块、ABI、WARP 与同步回归；不含硬件/GPU 层。失败时可用下面的分项入口定位。

## A. 必跑（快）

按顺序；任一失败则 **不要发包**。

```powershell
# 0) 模块数字契约（30/arch、60 dual 必须一致）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\check-module-contract.ps1

# 1) Runtime 清单/路径/ABI 无卡
# 若改过 LmxxfNrRuntime.cpp 或 runtime 头，先：
#   tools\build-lmxxf-runtime.cmd
$env:PYTHONPATH = "tests/_lib"
python tests/lmxxf/test_runtime_validation.py

# 2) 打包/模块助手
python tests/install/test_module_packages.py

# 3) 安装/卸载退出码
python tests\install\test_installer_exit.py
```

预期：契约 `OK` + 三项全 `OK`（当前约 22 + 16 + 44 用例）。

---

## B. 同步/上游工具（改动 sync 或第三方时）

```powershell
$env:PYTHONPATH = "tests/_lib"
python tests/sync/test_upstream_sync.py
```

较慢（约 1.5～3 分钟）。改 `tools/lmxxf-sync/*`、`third_party` 布局时必跑。

---

## C. 发包（产品产物）

```powershell
# 若改了 OptiScaler 本体或 runtime 源码，先编：
# tools\build\build-release-local.cmd   (OptiScaler.dll)
# tools\build-lmxxf-runtime.cmd   (LmxxfNrRuntime.dll)

powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\PACKAGE_RELEASE.ps1 -AllowMissingDeps
```

- 版本号读根目录 `VERSION`（例如 `1.9.6.1`），或显式 `-Version`。
- 新鲜度检查失败会 **中止打包**（DLL/hsaco 比源码旧）。
- **不**替代 A/B 测试。

### C2. 打包 / 打 tag 前（必跑，不要拖到 CI 才发现）

本地 A 不含 ABI。**与 Actions 相同的 ABI 必须在 PACKAGE_RELEASE 之前跑过**（29→30 那次就是只改了 python 侧）：

```powershell
# 需 VS/MSVC 开发者环境
cl /nologo /std:c++17 /O2 /EHsc /I "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime" `
  tests/lmxxf/lmxxf_nr_abi.cpp /Fe:exports/lmxxf-runtime/lmxxf_nr_abi.exe
& exports/lmxxf-runtime/lmxxf_nr_abi.exe exports/lmxxf-runtime/LmxxfNrRuntime.dll third_party/lmxxf/modules
cl /nologo /TC /W4 /I "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime" `
  tests/lmxxf/lmxxf_zero_fallback_abi.c /Fo:exports/lmxxf-runtime/ /Fe:exports/lmxxf-runtime/lmxxf_zero_fallback_abi.exe
& exports/lmxxf-runtime/lmxxf_zero_fallback_abi.exe
```

模块计数、导出表与包装契约改过时，这里最容易漏（例如 29→30 时只改了 python 侧）。

---

## D. 不在本清单（本地没有就不要当必测）

本机没有的游戏/硬件 **不列为发版门槛**，有设备再记实测：

- 卧龙 2 / 帕鲁 / 地平线 R10 / 鬼武者·RE9 / 9060：**无设备或未装则跳过**
- 帕鲁 FitLarge+UE5 重建卡顿、UE5 兼容：**代码已修**（见 README），不是待办 bug
- 可选：9070 金标 / PDL 开关（有卡再跑）

---

## 速查表

| 改动类型 | A | B | C | 实机 | CI（release.yml） |
|---|---|---|---|---|---|
| lmxxf Runtime / OptiScaler 本体逻辑 | ✓ | | ✓ | 建议 | runtime build + ABI |
| 安装/打包脚本 | ✓ | | ✓ | | installer/uninstall + package |
| sync / 第三方 | ✓ | ✓ | ✓ | | 模块新鲜度 + ABI |
| 菜单/配置键 | ✓ 或抽测 | | | | 不必加步 |
| 发 zip | ✓ | | ✓ | 按需 | 全量发版 job |
| 打 tag / GitHub Release | ✓ | | ✓ | 按需 | 全量发版 job（含 ABI） |

### CI 与本地分工（契约测试必须两边都有）

| 跑在哪 | 项 |
|---|---|
| **CI + 本地** | `check-module-contract`、`test_runtime_validation`、`lmxxf_module_packages`、`amd_installer_exit`、ABI/zero-fallback、PACKAGE_RELEASE 门禁 |
| **构建 / 发版** | 工具链 preflight、host-contract、LmxxfNrRuntime/OptiScaler 构建、发 Release |
| **统一 ci 层额外覆盖** | `tests/host/test_config_priority.py`（ini/菜单/txt 优先级与 `DLSS5_*` 键名契约；改配置时跑）、`amd_uninstall.ps1` 交互向用例 |
| **按需本地** | `lmxxf_upstream_sync`（动 vendor/sync 时）、GPU/金标/实机 |

### 数字契约（改模块列表时必须同步）

`30` = 每架构 hsaco 数；`60` = 双架构合计。**改列表时同时改下表全部位置**，再跑 `tools\release\check-module-contract.ps1`：

| 文件 | 断言 |
|---|---|
| `LmxxfNrRuntime.cpp` | `kKnownModuleNames[30]` 与实际名字个数 |
| `tests/_lib/lmxxf_fixtures.py` | `MODULE_NAMES` 30 个 |
| `third_party/lmxxf/hip/build-modules.ps1` | `name = '...'` 30 行（含注释里的总数） |
| `tools/release/check-release-freshness.ps1` | `$hs.Count -ne 30` |
| `tools/lmxxf-module-package.ps1` | 30/60 与 manifest 字段 |
| `tests/lmxxf/lmxxf_nr_abi.cpp` | `modules_ok=60` / `modules_ok=30` |
| `tests/lmxxf/test_runtime_validation.py` | 同上 |

漏改任一处 = 玩家包装了新模块但测试/CI 仍是旧数，或反过来。

---

## 依据（测试从哪来）

| 文件 | 来源 |
|---|---|
| `tests/install/test_installer_exit.py` | 安装器退出码与卸载安全（`586b911` 及后续） |
| `tests/lmxxf/test_runtime_validation.py` | 双架构 Runtime 校验（P-B，`95d030a` 等） |
| `tests/install/test_module_packages.py` | 模块打包/安装助手（双架构修复轮） |
| `tests/sync/test_upstream_sync.py` | 上游同步与审阅门禁（`ed745d1` 等） |
| `tests/host/test_config_priority.py` | 菜单/ini/txt 优先级与跨层键名（`818885e`）— 统一 ci 层覆盖 |

计划里的「无卡测试」指本清单；GPU 回归另见 P-E 与 `exports/` 实机记录。
