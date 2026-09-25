# 发版前无卡测试清单

适用：`release/1.9.0`（及同构产品分支）在 **打包 / 合并后** 的回归。  
**不含** GPU/游戏实测（那是 P-E-local / 9060 实机）。

工作目录：仓库根（`dlssnr_on_amd_setup`）。

---

## 何时跑

| 时机 | 必跑 |
|---|---|
| 合并功能/修复到 `release/1.9.0` 后 | 全套 A+B |
| 改 `tools/PACKAGE_RELEASE` / 安装 / 卸载 / 模块校验 | 全套 A+B |
| 只改文档 / 注释 | 可跳过 |
| 准备打 zip 发给玩家 | 全套 A+B，再 `PACKAGE_RELEASE` |

`PACKAGE_RELEASE.ps1` **不会**自动跑这些测试；它只做 **新鲜度门禁**（`check-release-freshness.ps1`）。

---

## A. 必跑（快）

按顺序；任一失败则 **不要发包**。

```powershell
# 1) Runtime 清单/路径/ABI 无卡
# 若改过 LmxxfNrRuntime.cpp 或 runtime 头，先：
#   tools\build-lmxxf-runtime.cmd
$env:PYTHONPATH = "tests"
python -m unittest tests.test_runtime_validation

# 2) 打包/模块助手
python -m unittest tests.lmxxf_module_packages

# 3) 安装/卸载退出码
python tests\amd_installer_exit.py
```

预期：三项全 `OK`（当前约 19 + 16 + 40 用例）。

---

## B. 同步/上游工具（改动 sync 或第三方时）

```powershell
$env:PYTHONPATH = "tests"
python -m unittest tests.lmxxf_upstream_sync
```

较慢（约 1.5～3 分钟）。改 `tools/lmxxf-sync/*`、`third_party` 布局时必跑。

---

## C. 发包（产品产物）

```powershell
# 若改了宿主或 runtime 源码，先编：
# tools\build-release-local.cmd   (OptiScaler.dll)
# tools\build-lmxxf-runtime.cmd   (LmxxfNrRuntime.dll)

powershell -NoProfile -ExecutionPolicy Bypass -File tools\PACKAGE_RELEASE.ps1 -AllowMissingDeps
```

- 版本号读根目录 `VERSION`（当前如 `1.9.3-alpha`），或显式 `-Version`。  
- 新鲜度检查失败会 **中止打包**（DLL/hsaco 比源码旧）。  
- **不**替代 A/B 测试。

---

## D. 不在本清单（需实机）

- 9070：输出哈希金标、PDL 开/关、游戏抽测  
- 9060：用户实机  
- 画质/闪屏/性能专项游戏验证  

---

## 速查表

| 改动类型 | A | B | C | 实机 |
|---|---|---|---|---|
| lmxxf Runtime / 宿主逻辑 | ✓ | | ✓ | 建议 |
| 安装/打包脚本 | ✓ | | ✓ | |
| sync / 第三方 | ✓ | ✓ | ✓ | |
| 菜单/文案 only | ✓ 或抽测 | | | |
| 发 zip | ✓ | | ✓ | 按需 |

---

## 依据（测试从哪来）

| 文件 | 来源 |
|---|---|
| `tests/amd_installer_exit.py` | 安装器退出码与卸载安全（`586b911` 及后续） |
| `tests/test_runtime_validation.py` | 双架构 Runtime 校验（P-B，`95d030a` 等） |
| `tests/lmxxf_module_packages.py` | 模块打包/安装助手（双架构修复轮） |
| `tests/lmxxf_upstream_sync.py` | 上游同步与审阅门禁（`ed745d1` 等） |

计划里的「无卡测试」指本清单；GPU 回归另见 P-E 与 `exports/` 实机记录。
