# 决策记录

## 09-30 · host Record 是唯一网络 pass，PreUpscale 强制关闭

- **决定**：daniel 路径上由宿主驱动 Record；会话内将 `PreUpscale` 内存置 0（不写回用户 ini）。NR 开关只动 `enabled` 或 shutdown→init，不改变「每帧一次网络」。
- **原因**：`PreUpscale=1` 时 daniel 的 FSR 挂钩会再拼 Packet 调一次 Record，与 host Record 叠成双跑；`enabled=0` 只是 Record 早退，不释放资源，也不能当热切释放显存。同一帧仍由 `HasUnsubmitted` 限制只有一个未提交 job。
- **落在**：`AmdPreSr.cpp`（init/Record 内存清 `preUpscale`）；菜单 NR 关闭说明。实现细节不在公开文档展开。

## 09-30 · 热切后偶发 GPU 卡死列为已知问题，不阻塞发版

- **决定**：lmxxf ↔ daniel 热切换路径本身可用（同代码路径连续 1900+ job 正常）；切换后偶发 inline wait 超时 / capture 未落地 / 崩溃，列为已知问题，不作为本次修复项，不阻塞发版。
- **原因**：同机同包两次对照：一次切换后 1900+ job 正常收尾，一次在 job 9 卡死；崩溃侧先出现 GPU wait 超时，capture 未落地是果不是因。predication 日志两局都有，非触发器。根因未明，样本量不足以支撑定向修复。
- **落在**：已知问题记录；游戏实测若多次复现再立项。对照日志仅作内部取证，不进 git。

## 09-30 · daniel 模型缓存只能随进程释放

- **决定**：daniel 关 NR 时释放 host 缓冲和 native staging（约 64 MiB + slot 缓冲），但引擎/模型缓存（约 1.4 GB 量级）保留在进程内，不承诺显存归零。同一局内可关闭后重新启用 NR；**要释放模型缓存只能退出游戏进程**，且退出后本局接不回来。
- **原因**：已由独立调查确认，daniel 的关闭路径只释放 staging，模型分配不在释放路径内；运行库生命周期与进程一致，会话内无法安全卸载；重新初始化也不是幂等的。没有安全的模型释放入口。
- **落在**：`AmdPreSr.cpp`（日志 `model cache retained`）；菜单 NR 关闭提示。lmxxf 侧不受此限（会话可完整销毁）。取代 09-29 前「关 NR 释放显存」中未区分模型缓存的表述。

## 09-28 · 已修复问题的旧测量不作为当前限制

- **决定**：帕鲁的秒级卡顿已由 `a129c5f` 修复；保留 `[DlssNr] DLSS5_FIT_LARGE=true` 默认值，不再引用修复前耗时建议降分辨率或关闭 FitLarge。维护者确认目前未发现 FitLarge 仍有问题。
- **原因**：旧测量发生在 allocation 与子矩形比较错误、每帧重建链路的时期，不能代表修复后的性能。公开兼容说明应分开写当前状态与已修复案例。
- **落在**：三语 README、[Palworld](games/palworld.md)、[lmxxf 后端](backends/lmxxf.md)。当前 ini 键名为 `DLSS5_FIT_LARGE` / `DLSS5_HIP_PDL`，旧名仅用于迁移；模块契约以 [发布测试清单](../tests/RELEASE-TESTS.md#数字契约改模块列表时必须同步) 的 30/60 为准。

## 09-28 · Matheus 仓库按 GPL-3.0 处理

- **决定**：`MatheusGViana/dlss-5-amd-project` 跟随 OptiScaler，按 GPL-3.0 处理；README 的 GPL-3.0 标注正确。
- **原因**：仓库根没有单独 LICENSE 文件（GitHub API 返回 `license: null`），但它是 GPL-3.0 谱系；维护者确认以谱系为准。danielblnc 的 runtime 仍是「保留所有权利」，本项目只做外部检测对接。
- **落在**：`docs/architecture/overview.md` 的许可证段；`README.md` / `README.en.md` / `README.es.md` 的署名表。

## 09-27 · TYPELESS 的解释由宿主按帧选择

- **决定**：`R16G16B16A16_TYPELESS` 不再固定按 UNORM16 解释；宿主每帧设定，view、codec、测光共用同一种解释（《浪人崛起》LDR 用 UNORM，HDR 游戏用 FLOAT）。
- **原因**：同一个 `fmt=9` 被 lmxxf 读成 UNORM、被 daniel/FFX 读成 FLOAT，数值差了约 4 倍，直接把高光推爆（卧龙 2）。
- **落在**：`native_lab_paths.h`（`NativeTypelessRgba16AsFloat()`）、`LmxxfNrRuntime.cpp` 每帧设置；随包附 `typeless-float16.patch` 与 `codec-hue-safe-preexp.patch`（`fa308b3`）。

一条一个持久决定，新的在前。格式：决定 / 原因 / 落在哪里（代码、脚本或流程）。被推翻的决定保留，并注明被哪条取代。日期是做出决定的日期（2026 年）；「约」表示原始记录没有精确日期。

提交号若在 2026-09-15 仓库重建之前，已无法在当前历史中解析，只作历史引用。

---

## 09-27 · FitLarge 默认改回开启，跟随上游 PR #9

- **决定**：`[DlssNr] LmxxfFitLarge` 缺失或 `auto` 时视为 true；安装器补缺失键时写 `true`。`native_rgb_reflect.h` 与 `native_input_geometry.h` 不再钉住，跟随上游；只剩 `hip_d3d12_bridge.h` 钉住。
- **原因**：上游 PR #9 已合并，上游包的 flags 默认开启 FIT_LARGE；产品与上游一致。取代 09-24 的「默认 false」。
- **落在**：d788963；`Config.h`（`LmxxfFitLarge { true }`）、`Config.cpp`、`tools/install-amd-presr.ps1`、`third_party/lmxxf/UPSTREAM.md` OURS 表。

## 09-26 · 无游戏曝光时由 runtime 自测曝光

- **决定**：游戏不给可用曝光纹理时，runtime 每帧测 Color 的平均亮度并在对数域平滑，作为 codec 曝光；ini `LmxxfAutoExposure=true` 为默认，false 时用固定 `LmxxfAutoExposureScale`（默认 8）。codec shader 的 auto-white 补丁保留但宿主不再置位，避免两者叠加。
- **原因**：此前「auto exposure」只是固定 scale 8，不是真正的自动估计；卧龙 2 等无曝光 HDR 游戏高光过曝。
- **落在**：e6ecf77、c6075d3；`LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE`；`UPSTREAM.md` 的 `auto-white.patch` 说明。

## 09-25 · 上游 sync 必须有审阅记录才算完成

- **决定**：sync 拷贝和编译成功不等于完成。必须有一份绑定到上游提交的审阅记录，逐项写明已接入、推迟（附下一步）或排除（附证据），sync 才推进 `UPSTREAM.md` 的完成 pin；否则 `sync-state.json` 保持 `pending`。
- **原因**：防止「只跟代码、忘了看作者开了什么」，也防止批量填模板冒充审阅。
- **落在**：`tools/lmxxf-sync/`、`AGENTS.md`、`third_party/lmxxf/sync-state.json`（d788963 时仍为 pending，346 项审阅未填）。

## 09-25 · 旧安装升级：推荐先卸载，覆盖也必须安全

- **决定**：检测到旧 OptiScaler 时推荐 Y（自动用新包卸载器卸载后安装）；N 走覆盖，旧的扁平模块目录经「验证候选目录 → 备份旧目录 → 整体切换」升级，不留混合布局。`-NonInteractive` 默认覆盖，`-UninstallExisting` 才卸载。模块包必须是完整的 gfx1200 + gfx1201 双架构（当时各 24 个，现行契约见 09-28 记录）；gfx1200 在 9060 实机验证前视为实验性。
- **原因**：早先的自动替换会留下新旧混合的模块目录；坏包曾在校验前就覆盖了安装。
- **落在**：`tools/install-amd-presr.ps1`、`tools/lmxxf-module-package.ps1`，见 [architecture/installer.md](architecture/installer.md)。

## 09-24 · 一个包通吃所有游戏，不按游戏名判断

- **决定**：只发一个构建。所有行为差异用输入格式、后端、文件是否存在等通用判据决定，不查游戏名，不设按游戏的颜色默认值。
- **原因**：按游戏分支无法维护，也会掩盖通用缺陷（RE9 的 RGB9E5、帕鲁的 alloc/valid 比较都是通用问题）。
- **落在**：lmxxf 路径按 `DXGI_FORMAT` 切 RGB9E5 私有输出；`RecordInputs` 不调用上游的 `LegacyParameters()`，所以不会对某个 exe 强制颜色力度（见 `sync-state.json` 的 encoder 说明）。

## 09-24 · 只做桥接，性能用上游口径

- **决定**：我们只测桥接开销；后端网络的性能数字引用作者自己的口径，不起后端基准。选项是否改画质用输出哈希 A/B 判断（同哈希即逐位等价）。
- **原因**：B 系列性能排查已表明桥接开销很小（GPU 侧约 0.007 ms、hook 约 0.03 ms 量级），瓶颈在后端网络本身；自测后端基准既费时又与作者口径不可比。
- **落在**：[measurement.md](measurement.md)；`tests/lmxxf/lmxxf_nr_gpu.cpp --output-hash`。

## 09-24 · F 门关闭，没有唯一发行默认后端

- **决定**：不再用「G3∧G4∧G5 通过后把发行默认切到 lmxxf」这道门。安装时只检测到一边就装那一边，两边都在就让用户选，选中的写入 `NrBackend`；`-NonInteractive` 两套都装并写 lmxxf。个别游戏的问题走小版本。
- **原因**：两个后端各有适用场景，一道全局门拦不住个别游戏的问题，反而拖住发版。
- **落在**：`tools/install-amd-presr.ps1`；`Kind.h` 的 `ResolveInstalled`（请求的后端缺文件时回退到另一个已安装的）。

## 09-24 · RGBA32F 曝光不适配

- **决定**：不做 RGBA32F 曝光纹理的适配。
- **原因**：该情况会安全降级为无曝光，NR 照常运行，没有人报告画质问题。
- **落在**：runtime 只接受 1×1 `R16_FLOAT` / `R32_FLOAT` 曝光（`LmxxfNrApi.h` 注释）。

## 09-24 · 输入契约违反不毒化 session

- **决定**：runtime 的输入契约检查提前做，违反时返回可重试的 `INVALID_ARGUMENT` 并报出具体属性，不 throw；宿主对 `INVALID_ARGUMENT` 不重建 session。
- **原因**：RE9 的 RGB9E5 输入触发异常后 session 被永久标为失败，且日志无法定位。
- **落在**：`LmxxfNrRuntime.cpp`（`GuardSession`）、`LmxxfBackend.cpp`；规则见 [architecture/lmxxf-c-abi.md](architecture/lmxxf-c-abi.md)。

## 09-24 · 准入：像素预算加宽 ≤2560、高 ≤1080

- **决定**：不开 FitLarge 时，lmxxf 按 1920×1080 像素预算准入，同时限制宽 ≤2560、高 ≤1080，最多降采样 25%；21:9 与 32:9 超宽仍在范围内。
- **原因**：只看预算会放进任意形状并大幅降采样（例如 3840×540 横向被砍一半），且没有开关。
- **落在**：615cdbf；上游在 PR #9 后采用了同一规则（`native_input_geometry.h` 已改为 FOLLOW）。

## 09-24 · shader-cache 不跟踪、不进包

- **决定**：`third_party/lmxxf/shaders/shader-cache/*.dxbc` 不进 git、不进安装包，runtime 首次运行时编译并在可写时缓存。
- **原因**：磁盘上 18 个缓存有 9 个已与当前源码不符；首次编译只需约 30–45 ms。
- **落在**：8e1b99e（取代同日 916e969「重新打包 shader-cache」）；`tools/release/PACKAGE_RELEASE.ps1` 只拷顶层 `.hlsl`。

## 09-24 · 上游关系：必须跟的为 0，PR 攒批、实机验证后再提

- **决定**：本批修复里没有必须推给上游的项；C ABI 接口文件（`LmxxfNrRuntime.cpp`、`LmxxfNrApi.h`、`LmxxfProductionOptions.h`）本来就是我们写的。给上游的改动攒成一批，实机验证后再提 PR，顺序为契约不毒化 → 曝光/RGB9E5 → 内核选项。不必每个修复都跟上游。
- **原因**：上游 codec 没被改动，唯一影响上游用户的是契约毒化；逐个提 PR 成本高且未经实机验证。
- **落在**：流程约定；这三个文件不在 sync 清单里（`UPSTREAM.md` OURS 表）。

## 09-24 · 安装时 ini 覆盖为第一选项

- **决定**：已有 `OptiScaler.ini` 时，第一项是「Overwrite OptiScaler.ini (Recommended: major version update!)」；无论覆盖与否都写回本次选择的 `NrBackend` 并强制 `Enabled` / `RunBeforeSR` / `LmxxfDiagnostic`，偏好键只在缺失时补。包内 ini 与代码默认对齐（`LocalTone=1`、`SkinStructure=-1`，`RunBeforeSR` 包内为 true）。
- **原因**：大版本升级时旧 ini 与新代码冲突；但用户的偏好不应被无故覆盖。
- **落在**：42194e7 起的 `tools/install-amd-presr.ps1`；`tools/release/PACKAGE_RELEASE.ps1` 生成的 `[DlssNr]` 段。

## 09-24（取代） · FitLarge 默认关闭

- **决定**：`LmxxfFitLarge` 默认 false，ini `auto` 视为 false。
- **原因**：帕鲁在同帧集成下，Color 2258 宽时约 2 s/帧，1920 宽可玩。后来查明主因是 alloc 尺寸与 valid 子矩形比较导致每帧重建 codec 链（已修）。
- **状态**：**已被 09-27 取代**（默认改回 true）。

## 09-23 · 放行已完成的别名屏障

- **决定**：已录完、`Flags` 为 none 的别名屏障留在录到它的那一段，不再否决同帧切分；未配对的过渡屏障仍否决。比上游 RE9 补丁宽的两处（屏障自带 `BEGIN_ONLY`/`END_ONLY`、夹在后来才配对的过渡中间）仍放行，不单独立项。
- **原因**：原先只要出现别名屏障就否决切分，拒掉了本可支持的游戏。
- **落在**：803c8ba。

## 09-22 · 不跨仓库 cherry-pick；modules 进 `third_party`

- **决定**：lmxxf 上游代码只通过 `tools/sync-lmxxf-upstream.ps1`（`git archive` + `-UpstreamRef`）进入本仓库，不跨仓库 cherry-pick。预编译 `.hsaco` 统一放在 `third_party/lmxxf/modules/` 并提交进 git，不再用带提交号的临时目录。hsaco 跟 hip 源码一起走：sync 在本机重编，CI 只打包已提交的 modules。
- **原因**：上游 git 不发布 hsaco；cherry-pick 无法记录 pin 和审阅状态。
- **落在**：`tools/sync-lmxxf-upstream.ps1`、`tools/lmxxf-sync/`、`third_party/lmxxf/UPSTREAM.md`。

## 09-21 · lmxxf 同帧执行；1 帧 staging 被否定

- **决定**：lmxxf 必须用当前帧的 Color 在同一帧内执行（切分游戏 command list，在中间插入 HIP 任务），不再用「上一帧 Color + 当前帧 MV/深度」的 staging 折中。
- **原因**：受控 A/B 中 `staging-previous`（约 2200 帧）整幅模糊，`staging-current`（约 1900 帧）正常；1 帧 Color 滞后是整幅糊的直接原因，而这个滞后是我们适配层自己引入的。
- **落在**：`dlssnr/submission/`、`LmxxfBackend.cpp`；诊断模式 `staging-current` / `staging-previous` 仍保留在 `LmxxfDiagnostic`。

## 09-20 · 路线甲：保留 OptiScaler 接入，修 lmxxf 契约

- **决定**：主攻「OptiScaler（B）游戏接入 + 修正 lmxxf 的数据与提交契约」；不主攻「作者包 + 游戏接入」（乙），也不单开「Evaluate + 作者 pipeline」（丙）。只改桥接层，不改上游 kernel。
- **原因**：三条路的共同瓶颈是同帧 producer → NR → SR 的提交边界，换发行包不会自动解决；作者包还多一个未打通的入口。
- **落在**：`dlssnr/backend/LmxxfBackend.cpp`、`dlssnr/submission/`。

## 09-19 · B 性能专项收口

- **决定**：不再做 OptiScaler 桥接层的常规性能排查。保持默认新等待、unsafe 关闭、Normal 曝光、现有提交/退休与退避策略。超时/SPIKE 作为未解决问题单列，不据此冻结预算、改 cap 或 TDR。
- **原因**：实测桥接 GPU 前后处理约 0.007 ms，hook 与状态恢复约 0.03 ms；CPU 热点在 daniel runtime 内部，宿主侧改动不能带来整帧提速。
- **落在**：流程约定；daniel 路径保持 Normal 曝光。

## 09-17 · graphics 等待默认开启；兼容摘修线停止

- **决定**：daniel 路径默认 `AmdGraphicsWait=1`、`AmdGraphicsUnsafe=0`；逐帧准入失败安全回退 compute，不绕过 freeze / pin / restore。graphics 等待不是治 SPIKE 的药方。上游兼容修复只按缺陷定点摘取（官方 OptiScaler 有价值的小修），不追版本号、不整树合并；首两批做完后这条线停止。
- **原因**：graphics 路径在燕云持续工作且无设备移除；兼容修复的收益是资源安全，与 FPS 无关，继续做没有触发依据。
- **落在**：`Config.h`（`AmdGraphicsWait { 1 }`、`AmdGraphicsUnsafe { 0 }`）；包内 ini 模板。

## 09-16 · XeFG 解锁补丁不进公开仓库（方案 C）

- **决定**：不把 Coldwood1026 的 XeFG 多帧生成 Unlock / Pacing 补丁代码移植进本仓库；多帧生成由用户另加社区的 `XeFGUnlock.asi`（README 有说明）。
- **原因**：本项目发行包本身分发 Intel 的 `libxess_fg.dll`（许可允许分发、禁止修改与逆向）；同一个包里再附上绕过它门禁的代码，与项目「不分发、不附带绕过」的版权取舍不一致。
- **落在**：`README.md` 第 179 行附近；包里只有未修改的 `libxess_fg.dll`。

## 2026-09-30 · NR 关闭与后端切换按提交生命周期释放

- **决定**：关闭 NR 或切走后端时停止接收新 Record，保留已录制任务的提交回调；等实际提交及 GPU 完成后释放会话资源。关闭期间的 Evaluate 继续非阻塞轮询，不能只等下一次 Record。
- **原因**：摘掉回调或取消 CPU job 不会撤销游戏已经录制的 GPU 命令。lmxxf 的 enqueue 回调与会话销毁使用同一生命周期锁；释放前另用队列 fence 确认完成。无法确认完成时保留资源。
- **Daniel 边界**：保留已验证的模块句柄，避免重新加载被进程固定的 DLL。关闭时释放 host 缓冲和 native staging；模型缓存仍驻留，不承诺显存归零。再开复用模块并重建 staging，避免把单纯清除 new wait 标志当成资源重建。
- **切换条件**：启用热切换且安装 lmxxf 时，Daniel 启动也预备 submission hooks；仅安装 Daniel 时不引入这条代理路径。graphics tracker 跟踪代理背后的 native list，continuation 有独立状态。启动时未安装 graphics hooks 的情况仍可能要求重启。
- **落在**：`AmdPreSr`、`LmxxfBackend`、`LmxxfEvaluateCut`、`Selector`、`D3D12_Hooks` 和 `DlssNr_Dx12`。不改变 QueuePriority、preUpscale 或常规逐帧等待策略。
- **验证范围**：Release 宿主编译与 `tests\\run-all.cmd --tier ci,device` 通过；新增回调/释放并发回归和 native list 身份断言。游戏内双向切换、长时间显存曲线与 low 帧仍需实机游戏验收，不能由这些测试推定。

## 约 09-16 以后 · 1.9.0.x 撤包

- **决定**：1.9.0.x 全部撤包；本地遗留的 `v1.9.0` tag 与 `1.9.0.3` zip 不复用，版本号不再使用 1.9.0.x。
- **原因**：原始记录未写明撤包原因（本页无法核实）。
- **落在**：流程约定，见 [release.md](release.md)。当前 `release.yml` 手动触发留空时读取 `VERSION`。

## 09-15 · 独立仓库，不 fork、不嫁接；1.8.0 / 1.8.1 不发布

- **决定**：`TheAutomatic/dlss-5-amd-project` 删库后同名重建为独立库，历史从 1.8.2 起；不设 GitHub fork 关系，不做 `git replace --graft`。1.8.0 / 1.8.1 不再发布。需要看 wilsjo2 上游时用 `git remote add` + `fetch` + 逐个 `cherry-pick`。
- **原因**：旧库历史里有不应公开的文件，force-push 后旧对象仍可按完整 SHA 取到，只有重建能清干净；fork 标记对合并能力没有影响；与上游没有共同祖先，嫁接的收益抵不上成本。
- **落在**：GitHub 仓库本身；操作方法见 [architecture/overview.md](architecture/overview.md)。

## 09-15 · 内部文档不进 git

- **决定**：交接文档、工作稿、逆向材料、本机路径只留本地，不进 git；tracked 文件不引用它们。仓库里只留能复现发布的东西。
- **原因**：其中有不应公开的材料、给审阅者的提问稿和本机路径；这条谱系的四个公开仓库都不公开此类内容。文档是否公开、公开多少另行决定（本 `docs/` 目录是经过筛选的公开部分）。
- **落在**：`.gitignore`；`.githooks/pre-commit` 拦 `git add -f`。

## 09-15 · 多槽的 abandon 与多 list 记账：什么都不做

- **决定**：daniel 多槽路径的 5 秒 abandon（无 GPU 完成信号时清 pending）与「多 list 只记账一个」这对已知风险，不修、不再测。
- **原因**：历史上只出现 2 次，都在同一会话、同一诊断构建；此后多次长会话（含 48000 帧）均为 0。两者耦合：单独删掉超时释放会把罕见的 UAF 换成持续的槽泄漏；依赖 `nativeDone` 的替代推理已被复核推翻。
- **落在**：`SubmissionState.h`、`AmdPreSr.cpp` 保持现状；发版时不作为修复或卖点宣传。

## 09-15 · 不以隔帧 NR 作为退路

- **决定**：不把隔帧或降级当作性能退路；槽数决定会不会丢降噪帧，不决定帧率。默认 3 槽，运行时可调 2–5。
- **原因**：长卡顿来自 daniel 自身偶发的长 job（SPIKE），不是宿主调度；隔帧只降画质不解决根因。
- **落在**：`Config.h`（`AmdSlots { 3 }`）、菜单。

## 09-14/15 · 发行包不含任何专有文件；`dist/` 是发版输出

- **决定**：发行包和仓库都不含 NVIDIA DLL、daniel 的 runtime / weights / setup；这些由用户自备。`dist/` 只放发版产物并被忽略；每次发版重新打包，不复用 `dist/` 里已有的 zip 或 staging。
- **原因**：保持仓库与发行物版权干净；作者二进制曾进过历史，已通过改写与重建清除。
- **落在**：`tools/release/PACKAGE_RELEASE.ps1` 与 `.github/workflows/release.yml` 两道禁入绊线；`.gitignore` 的 `dist/`。
