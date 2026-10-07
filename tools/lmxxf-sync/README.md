# lmxxf 上游同步与接入审阅

同步命令只完成可验证的步骤。新源码、新默认值、作者的部署选项是否适合本项目，需要人或 agent 按功能判断。逐项清单用于防漏，不要求逐 commit 或逐宏重复写长篇审批。测试开关既不能全部照搬，也不能仅凭 `TEST` 等名称认定不相关。

## 工作流程

1. 在独立 worktree 中工作，保留当前完成的 `third_party/lmxxf/UPSTREAM.md` pin。运行 `tools/sync-lmxxf-upstream.ps1`；可用 `-UpstreamPath` 指定本地作者仓库。脚本默认 fetch，然后只使用解析出的同一个 commit SHA。离线使用 `-SkipUpstreamFetch`，不要误把旧的 origin/main 当成刚拉到的代码。
2. 第一次运行会准备源码，然后生成 `exports/lmxxf-upstream/report.md`、`report.json`、`upstream.diff`、`pinned-headers.diff` 和 `review.template.json`。审阅尚未完成时退出非零，这是待接入状态。`sync-state.json` 记录当前尝试；`UPSTREAM.md` 的完成 pin 此时不前移。补丁冲突、必需文件消失会在任何 vendor 拷贝或删除前失败，保留完整旧快照。
3. 以上次完成接入的 pin 到目标 SHA 的最终 diff 为主，按功能归并审阅。清点所有变化路径，包括没有进入 vendor 的脚本、部署配置、测试和文档；先判断其与生产路径的关系，只对影响实际生产路径的实验深入追踪。报告为每个变化路径保留审阅项，防止新的配置位置、生成器或开关命名绕过文本扫描。无需逐条读中间 commit；只在最终 diff 无法解释意图、兼容行为或回归来源时追溯相关历史。检查 `hip_d3d12_bridge.h` 的 diff 及产品契约；PR #12 合入后它与其他闭包文件一样直接跟随上游，不再需要 `-UpdateBridge` 或重套补丁。
4. 追踪相关功能的完整调用链：上游 Options/环境变量 → 本地 `LmxxfProductionOptions.h` 和 Runtime/C ABI → 模块选择和资源/尺寸前提 → 每个模块的编译宏 → 内核实际执行路径。对照作者的发布配置和 deployments；实验若改变生产默认、ABI、布局、生成配方或可达消费者，才深入核查其代码和相关记录。无生产关联的实验保留具体排除理由与位置，不要求逐份阅读原始 benchmark 输出。特别检查默认从 0 变 1、从 1 变 0、数值变化、参数删除、只在生成器中启用的优化，以及绕过/消融/诊断分支。
5. 将模板复制到 `third_party/lmxxf/upstream-review.json`（或 `-ReviewFile` 指定的位置），按功能形成一次结论，再把覆盖项关联到该结论。现有 JSON 字段允许在 reason/evidence/validation/next_step 中简短引用受版本控制的功能审阅文档与章节；同组条目共用证据时不必复制正文，但分类、决策和指纹仍逐项保留，不能用同一结论覆盖不相关项。需要接入的功能按依赖分批修改、构建和验证；暂缓的功能保留明确的下一步和验收条件。不要用脚本把所有项批量填成已接入或不相关。已审阅且证据不变的决策会带入新模板；整体验证仍需重新填写。
6. 修改本地接入代码、模块宏或补丁后，重新运行同步，使用新模板重新检查变化项；仅本地接入变化时保留原上游范围，补审本地最终 diff 与受影响的功能组，沿用未变的来源证据，不重写整份历史审阅。审阅绑定上游范围和本地接入内容；旧记录不能直接放行新源码。不要把普通 FOLLOW 文件中的手工改动当作已保存的接入成果：重新运行会用上游版本覆盖这些文件；持久兼容修正应有明确补丁和验证。
7. 记录实际验证后，再运行相同命令。审计接受记录后，脚本才构建/更新模块、检查校验和并编译 Runtime。全部请求的步骤成功，才更新完成 pin 并打印完成。默认命令不会自动执行游戏实测；涉及性能、图像一致性、资源/队列语义的接入，需要记录对应测试结果，不能用“编译成功”代替。

审阅记录至少包括：`reviewer`、总体 `summary`、实际 `validation`，以及每项的 `classification`、`decision`、`reason`、`evidence`。

| 字段 | 可选值 / 要求 |
|---|---|
| classification | `production`、`experiment`、`out-of-scope`；按消费者和证据判断 |
| decision | `integrated`、`deferred`、`excluded` |
| reason / evidence | 具体理由与源码位置、部署记录或本地接入位置 |
| integrated | 必须填写该项实际 `validation`，包括命令/结果或已验证的不受影响依据 |
| deferred | 必须填写可执行的 `next_step`，说明依赖、接入步骤和验收条件 |
| 跳过验证 | 同样形成审阅项，必须填写后续 `next_step` |

有意暂缓可以通过审阅，完成信息会显示暂缓数量；这表示“同步已审阅，后续工作有记录”，不表示所有上游功能已启用。解析器只收集文本证据，不能证明运行时可达性、性能收益或判断正确性。程序能阻止遗漏、空白决策和过期记录，判断质量仍由审阅者负责。

## PR #12 合入后的零 pin / 零源补丁（2026-10-07）

作者已在 `b3d05ab34beaea97fa7062b20e19f76398c287fd` 完整合入 PR #12，
包括 1.10.4 的 final-pass 多层 History 接口。本次跟进其后续修正
`48a41fccb89300cd6636b16bc7b86010384c4cc1`；完成状态以
`third_party/lmxxf/sync-state.json` / `UPSTREAM.md` 为准。

`manifest.json` 的 `pinned` 与 `local_patches` 均为空。桥接、网络、codec 和
着色器直接使用正式上游源码。新增 `native_fast_history_policy.h` 仅补齐 wrapper
依赖；作者 addon 的 MP1 限制不影响产品直接调用 bridge 的多层 History。
见 [合并后接入审阅](../../docs/lmxxf-pr12-merged-review.md)。

上游提供可复用接口；History 算法、System32 编译器策略、热键/输入轮询权限、
额外跳块、模块选择及菜单默认值由本侧显式配置。没有启用作者 addon 的
`DLSS5_FAST_HISTORY` 或旧 reference History。产品配置优先级不变。

解除的是固定保留文件和源补丁；正式提交记录、模块源码/产物哈希及追更审阅
继续保留。后续上游接口、布局、默认值变化仍须按上述流程验证。
`patches/` 保留历史回归材料；`tests/sync/fixtures/lmxxf/patch-chain.json`
独立冻结旧清单与预期输出，不再把旧补丁当成当前产品依赖。

## 文件所有权

`manifest.json` 是唯一头文件清单，归档和拷贝不再分别维护同一批路径。

- 普通头文件：只维护 `src/*.h`、`Development/HIP/*.h` 的选定闭包。移出清单的旧头文件会删除。仍在必需清单中但上游消失的路径必须先修清单和调用者，不能留下混合快照后继续成功。
- `hip/*.hip`、`hip/*.inc`、`shaders/*.hlsl`：按顶层镜像同步；上游删除或改名后，旧文件删除。子目录、缓存和其他扩展名不属于这个镜像。
- HIP 的 `.hip` 和 `.inc` 都参与模块配方指纹、本地审阅指纹及发布新鲜度检查；片段变化同样需要重建模块和更新审阅。旧记录不含片段时，下一次同步会要求重新验证，不能直接改写已验证指纹放行。
- 当前没有固定保留头文件。`hip_d3d12_bridge.h` 的 PDL、录制租约和直写接口已在上游；与其他必需头文件一样镜像并检查缺失。
- 本地 Runtime、模块构建输出及元数据属于各自流程，不属于头文件镜像。上游新依赖头文件需要审阅后加入闭包。

产品 `[DlssNr] DLSS5_SKIP_BLOCKS` 与 Advanced Kernels 内的 Base skipped blocks (all passes) 共用一个键。基础列表用于所有实际网络 pass；相邻的 Extra skipped blocks in passes 2/3 仅为后续实际网络 pass 追加跳块。两项一起收在原 Advanced Kernels 折叠组内、紧跟 High resolution，不新增 Block skipping 分类或第 2/3 层子组。默认 `none`（全71块）；`none` 表示不跳块，`auto` 恢复编译默认。列表允许 `1..38`、`40..69`，会去重排序；非法值在 ini 读取时警告并回退默认，菜单拒绝提交。改动在下一次网络重建时生效。当前模块不能跳过 C32 链末尾的 `4` 或 `69`（Runtime 明确拒绝，避免 raw-chain 格式不匹配）；完整支持须有匹配模块。与上游一样，跳过 `5..22` 或 `48..65` 时须关闭 MH byte stream。Config 设置该键后优先于 flags / 外部环境；直接使用 Runtime 时未设置的键仍可由 flags / 环境补齐。

## 历史补丁回归

`patches/bridge.patch` 和 `patches/pr12-integration-interfaces.patch` 是合并前的
接口差异，其他旧补丁也仅作历史留存。当前同步不应用任何一个。
测试从 `snapshot.json` 指定的官方 297b032a 原始文件重放冻结清单，以合并前
产品提交的独立 SHA256 为期望值，覆盖严格应用、冲突及同步失败不改 vendor。
新的零补丁同步仍有独立回归；无需因上游正常演进而修改历史期望值。

优先在产品侧消费上游接口。确需修改上游闭包时应明确记录临时补丁并补充验证，
不能只改 vendor 后假定下一次同步仍保留；也不能把历史夹具重新接入当前清单。

`module-defines.json` 按模块维护本地明确启用的宏。目前保留两个 multihead-fast-padded-wave 模块的 `HIP_FFN_LINE_STORES 1`。单个模块启用了某宏，不能代表其他模块也启用。上游出现同名同值定义时不重复注入；值冲突会失败，要求审阅。

## 有意跳过与重试

- 审计脚本缺失、Python 不可用、上游证据读取失败、审计退出非零都不能完成同步。可用 `-PythonPath` 指定解释器。
- `-SkipEnablementAudit` 仅供无 Python 机器准备源码以便后续审阅：返回 0 表示准备完成，状态仍为 pending，不构建、不前移完成 pin，也不打印同步完成。后续必须不带该开关重新执行。
- `-SkipBuild`、`-SkipModules`、`-AllowStaleModules` 是明确的验证例外，必须在审阅中说明并安排后续验证。跳过构建绝不意味着产物已可发布。
- 首次使用此流程还没有模块验证基线，需构建模块，或明确使用 `-AllowStaleModules` 并记录后续验证；不能仅因当次源码没有变化就认定已有模块有效。
- `-ModulesPath` 只接受完整的 gfx1200 + gfx1201 构建树/模块包：两架构各 **$PerArch** 个受控模块（当前 **40**，以 `tools/release/check-module-contract.ps1` 为准）、根/叶子 `SHA256SUMS` 和两份 `modules.json` 必须一致。上游构建树可不含产品 `runtime-manifest.json`，同步时在候选目录补齐；安装和打包则必须已经包含它。缺少一个架构、清单不完整、哈希错配或链接路径均在目标改变前失败；`-AllowStaleModules` 不能绕过包完整性校验。旧扁平目标需先移出同步目录。增删模块时见 [docs/release.md](../../docs/release.md)「模块数量契约」。
- `-ModulesPath` 把提供模块的实际内容绑定到审阅记录，校验提供目录的摘要并刷新模块；摘要只证明字节一致，不能证明这些字节由当前源码生成，构建来源也需人工/AI审阅。
- 源码复制之后的失败会保留待审阅状态，方便分步接入。不要删除 `sync-state.json` 来清除失败；它保留失败前模块比较基线，防止第二次运行误把旧模块认作新源码的产物。审阅通过后也不会用一个允许旧模块的例外把这些模块标成已验证。
- `UPSTREAM.md` 的 pin 是最近完成的同步。pending 时用 `sync-state.json` 的 `to_commit` 查看正在接入哪个版本。提交接入变更时同时保留审阅记录和状态记录，避免其他 checkout 丢失上下文。

## 验证此工具

```powershell
python tests/sync/test_upstream_sync.py
$env:LMXXF_TEST_POWERSHELL = (Get-Command pwsh).Source
python tests/sync/test_upstream_sync.py
```

测试使用临时 Git 仓库和假模块，不访问网络、不执行真实 GPU 构建、不修改开发者的作者仓库。真实快照可以单独收集证据：

```powershell
python tools/audit-lmxxf-enablements.py <upstream-clone> <commit> --report-only
```

`--report-only` 的成功仅表示证据收集成功。审计默认返回 0 表示当前审阅记录有效，3 表示待审阅，1 表示读取/解析错误；同步脚本将任何非零审计结果转换为失败。

0.37 消费者复核、实际模块宏和验证范围见 [docs/lmxxf-037-consumer-review.md](../../docs/lmxxf-037-consumer-review.md)。不能只刷新 fingerprint 来接纳本地默认值变更。

0.39 完整上游审阅、实际生成配方和保留项见 [docs/lmxxf-039-consumer-review.md](../../docs/lmxxf-039-consumer-review.md)。

## 本地增量修正

上游 pin 不变、仅调整产品接入或本地补丁时，复核本地最终 diff、重放受影响补丁并重新执行审计即可；不必为此重拷 vendor 或重编未变的模块。功能结论引用同一文档，沿用未变证据。实际追更新 pin 仍走上面的 staged 流程。日常只做风险相关专项测试，完整测试集中在发版前；验证范围与未测项目如实记录。

0.40 功能、LLVM23/COMGR 混合模块、RowOpts 与验证边界见 [消费审阅](../../docs/lmxxf-040-consumer-review.md)。

## LLVM23 与 RowOpts 构建

生产同步启用原版配方的 RowOpts；每架构六个 LLVM23 行，其余34行 COMGR。
先在 WSL/Linux 提取固定 pin 的 `hip/`、`Development/HIP/swin_persistent_types.h` 与
`Development/tools/llvm-fork/`（使用 git archive，不能用作者克隆的不同工作树版本）。
使用官方含 AMDGPU 的 Linux LLVM23.1.2 包或同版本源码构建，先核对归档哈希和
clang --print-targets；官方 Windows 包不含 AMDGPU。Ubuntu26 的 LLD 缺 ICU70 时，
可从 Ubuntu 签名仓库下载并私有解包，以 LD_LIBRARY_PATH 指向其 lib 目录。
不替换系统库，不需要 ROCm SDK。

在 Linux 运行上游脚本（路径按本机设置）：

```sh
python3 <pinned-source>/Development/tools/llvm-fork/compile-modules.py \
  --bin <llvm23>/bin --out <linux-output> \
  --compiler-rows llvm23 --row-opts --target-feature=-real-true16 --jobs 4
```

把整个输出（含 manifest.json 和两个架构目录）复制到本工作树
`exports/lmxxf-llvm23/`，或通过 `-LlvmPrebuiltDir` 指定另一目录。同步例如：

```powershell
tools/sync-lmxxf-upstream.ps1 -UpstreamPath <clone> -UpstreamRef <full-pin> `
  -SkipUpstreamFetch -LlvmPrebuiltDir exports/lmxxf-llvm23
```

同步调用未修改的上游 `build-modules.ps1 -RowOpts -PrebuiltDir`，输出到
`exports/lmxxf-modules/`。包装器保留产品 LINE_STORES 宏，并验证预编来源、
两个阶段的 -real-true16 与 per-row opts，补齐 C64 的 HIP_BARRIER_FENCE 及源码哈希。
缺失或过期预编产物必须重新构建，不静默用 COMGR 替代。
`-ModulesPath` 仍可提供已验证的完整80模块树，照常经过人工来源审阅和双架构契约检查。
WSL 仅用于这些离线 GPU 模块；宿主/Runtime 的 MSVC 和备用 MSYS2 无需迁移。

## 审阅范围与工具测试复用

本地指纹保留lmxxf实现和共用选择、菜单、提交、效果消费者；排除
`backend/mochizuki_runtime/` 和 `MochizukiBackend.cpp/.h` 的私有实现。
这些文件不消费lmxxf源码或模块；共同ABI、ConfigKeys及共用代码仍受检查。
因此仅修改Mochizuki私有实现不要求重走lmxxf接入审阅；上游更新、lmxxf或共用
消费者修改照常失效。回归同时验证两类边界，不能任意扩大排除范围。

完整工具回归用 `tests\sync\run.cmd`，按内容/环境/UTC周复用成功记录；
`--force` 强制全跑。缓存不改变实际同步审计、模块校验或pending语义。
详见 [发版流程](../../docs/release.md#减少重复验证2026-10-05)。

原生 temporal history 的本地接入见 [架构与验证](../../docs/architecture/lmxxf-native-history.md)。float/b8 post 辅助输出与共享输出扩展接口已合入上游；模型系数和 History 策略由产品侧提供。
