# lmxxf 上游同步与接入审阅

同步命令只完成可验证的步骤。新源码、新默认值、作者的部署选项是否适合本项目，需要人或 agent 逐项判断。测试开关既不能全部照搬，也不能仅凭 `TEST` 等名称认定不相关。

## 工作流程

1. 在独立 worktree 中工作，保留当前完成的 `third_party/lmxxf/UPSTREAM.md` pin。运行 `tools/sync-lmxxf-upstream.ps1`；可用 `-UpstreamPath` 指定本地作者仓库。脚本默认 fetch，然后只使用解析出的同一个 commit SHA。离线使用 `-SkipUpstreamFetch`，不要误把旧的 origin/main 当成刚拉到的代码。
2. 第一次运行会准备源码，然后生成 `exports/lmxxf-upstream/report.md`、`report.json`、`upstream.diff`、`pinned-headers.diff` 和 `review.template.json`。审阅尚未完成时退出非零，这是待接入状态。`sync-state.json` 记录当前尝试；`UPSTREAM.md` 的完成 pin 此时不前移。补丁冲突、必需文件消失会在任何 vendor 拷贝或删除前失败，保留完整旧快照。
3. 阅读整个上游 diff，包括没有进入 vendor 的脚本、部署配置、测试和文档。报告为每个变化路径保留审阅项，防止新的配置位置、生成器或开关命名绕过文本扫描。检查 `hip_d3d12_bridge.h` 的 diff，决定继续保留还是 `-UpdateBridge` 更新（并重新套上 `Pdl*` 与本地契约）。
4. 追踪相关功能的完整调用链：上游 Options/环境变量 → 本地 `LmxxfProductionOptions.h` 和 Runtime/C ABI → 模块选择和资源/尺寸前提 → 每个模块的编译宏 → 内核实际执行路径。对照作者的发布配置、deployments 和实验记录。特别检查默认从 0 变 1、从 1 变 0、数值变化、参数删除、只在生成器中启用的优化，以及绕过/消融/诊断分支。
5. 将模板复制到 `third_party/lmxxf/upstream-review.json`（或 `-ReviewFile` 指定的位置），逐项填写决策。需要接入的功能按依赖分批修改、构建和验证；暂缓的功能保留明确的下一步和验收条件。不要用脚本把所有项批量填成已接入或不相关。已审阅且证据不变的决策会带入新模板；整体验证仍需重新填写。
6. 修改本地接入代码、模块宏或补丁后，重新运行同步，使用新模板重新检查变化项。审阅绑定上游范围和本地接入内容；旧记录不能直接放行新源码。不要把普通 FOLLOW 文件中的手工改动当作已保存的接入成果：重新运行会用上游版本覆盖这些文件；持久兼容修正应有明确补丁和验证。
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

## 文件所有权

`manifest.json` 是唯一头文件清单，归档和拷贝不再分别维护同一批路径。

- 普通头文件：只维护 `src/*.h`、`Development/HIP/*.h` 的选定闭包。移出清单的旧头文件会删除。仍在必需清单中但上游消失的路径必须先修清单和调用者，不能留下混合快照后继续成功。
- `hip/*.hip`、`hip/*.inc`、`shaders/*.hlsl`：按顶层镜像同步；上游删除或改名后，旧文件删除。子目录、缓存和其他扩展名不属于这个镜像。
- HIP 的 `.hip` 和 `.inc` 都参与模块配方指纹、本地审阅指纹及发布新鲜度检查；片段变化同样需要重建模块和更新审阅。旧记录不含片段时，下一次同步会要求重新验证，不能直接改写已验证指纹放行。
- 固定保留头文件现在只有 `hip_d3d12_bridge.h`（含产品 `Pdl*` 查询）。`native_rgb_reflect.h`、`native_input_geometry.h` 在 PR #9 合入后改为 FOLLOW。默认不覆盖 bridge，也不删除，并验证本地契约标记。
- 本地 Runtime、模块构建输出及元数据属于各自流程，不属于头文件镜像。上游新依赖头文件需要审阅后加入闭包。

产品 `[DlssNr] DLSS5_SKIP_BLOCKS` 与菜单的 Skipped residual blocks 共用一个键。默认 `42,43,46`；`none` 表示不跳块，`auto` 恢复编译默认。列表允许 `1..38`、`40..69`，会去重排序；非法值在 ini 读取时警告并回退默认，菜单拒绝提交。改动在下一次网络重建时生效。当前模块不能跳过 C32 链末尾的 `4` 或 `69`（Runtime 明确拒绝，避免 raw-chain 格式不匹配）；完整支持须有匹配模块。与上游一样，跳过 `5..22` 或 `48..65` 时须关闭 MH byte stream。Config 设置该键后优先于 flags / 外部环境；直接使用 Runtime 时未设置的键仍可由 flags / 环境补齐。

## 补丁维护

`patches/bridge.patch` 是保留头 `hip_d3d12_bridge.h` 的 unified diff（更新该头时使用）。当前 pin `54e14de503431cd4536f8a7151b022af232178a9` 已包含恢复/清零逻辑；补丁保留产品的 `PdlRequested`、`PdlEffective`、`PdlReason` 查询，并在 HIP 输出信号后记录可复用完成事件，让驱动回收启动记录，不增加 CPU 等待。完成事件及诊断事件在销毁 bridge 时释放。另含产品录制租约的显式 opt-in、输出 COMMON 状态封存、实际队列执行与完成凭证；这些改动维护在 pinned bridge.patch，不能放入每次都会应用的 local_patches，否则默认保留 bridge 的同步会重复套补丁。`reflect.patch`、`input-geometry.patch` 对应的本地改动已进上游，仅作历史留存，sync 不再依赖它们。`reference-network.patch` 维护本地 PDL preflight、状态查询和分配失败清理。

测试使用 `tests/sync/fixtures/lmxxf/` 中的原始快照：FOLLOW 补丁输入来自 `82ce821f0ea1a12925d04c353cb2d1b9ee006c11`（0.39 已审阅快照，完成 pin 以 UPSTREAM.md 为准），pinned bridge 来自 `54e14de503431cd4536f8a7151b022af232178a9`；`snapshot.json` 记录来源路径和 SHA256。测试覆盖所有生效补丁的目标文件，按 manifest 顺序逐个执行 `git apply --check` 和正常应用，并比较结果与现有 vendor 源码（仅规范化 checkout 换行）。不能反向应用待测补丁来生成夹具，也不能用已打补丁的 vendor 文件伪装上游输入。每次同步都对归档应用补丁；若未来上游吸收了部分或全部改动，必须重新审阅并重做补丁，不能仅凭方法名跳过。

`manifest.json` 的 `local_patches` 列出「文件继续跟上游、只携带我们几处改动」的补丁，按顺序打在归档上，任何一个打不上都会在改动 vendor 之前失败：

- `reference-network.patch`：见上。
- `auto-white.patch`：`shaders/native_codec_encode.hlsl`、`shaders/native_codec_decode.hlsl`、`src/native_game_codec.h` 里无游戏曝光时的均值白点（`Reserved.x` 的 0x10000 位）。基于 `24986ae094bbd150f4d86a0ca76159a43f374884`。着色器目录本来会被整体镜像成上游版本，没有这个补丁，sync 会把它悄悄冲掉。当前线上路径是 runtime 侧 meter（带时间平滑），本侧不再设置 0x10000；补丁保留以固定着色器契约，并与 meter 互斥（有 meter 时不会同时开 shader 估白点）。
- `r10g10b10a2.patch`：`src/native_lab_paths.h` 接受 R10G10B10A2 颜色输入（Horizon）。基于同一提交。
- `typeless-float16.patch`：在前述格式补丁之后，为 `R16G16B16A16_TYPELESS` 增加产品可选的 FLOAT 解释，默认仍为上游的 UNORM。
- `codec-hue-safe-preexp.patch`：在 `auto-white.patch` 之后，补齐编码/解码共用的主机 pre-exposure 回退，以及解码端保留游戏色相的 ColorStrength 混合。必须包含从原始快照到产品源码所需的完整改动，不能依赖未由前序补丁生成的中间版本。
- `shader-compile-system32.patch`：私有加载绝对 System32 编译器并验证模块/符号来源；线程安全绑定供内存、文件和 blob 入口共用。缓存按编译器身份、目标及 flags 隔离；仅在明确 X3506 target 拒绝且宏组合通过生产着色器对照时降级到 cs_5_0。原始输入仍为 snapshot.json 中固定的 native_shader_cache.h，回归包含旧同名 DLL 预载、冷/热缓存、include、错误注入与 WARP 输出对照。

- `recording-leases.patch`：codec/RGB 私有输出采用固定 NSR→UAV→NSR 录制状态，并提供精确资源/heap/root/PSO pin（包括可选 neural_buffer）；仅包含 FOLLOW 头。
- `module-load-cleanup.patch`：HIP 模块成功加载但 Style 初始化抛异常时，卸载尚未交付给 Network 的模块；成功后才发布句柄。原始 hip_api.h 快照参与补丁重放，GPU tier 的故障注入测试覆盖 Style 拷贝失败、缺失导出、加载失败和正常所有权交付。

新增本地改动时，改 vendor 文件后必须同时生成补丁并加进 `local_patches`，否则下一次 sync 就会丢失这些改动。

更新固定头文件时，先对临时归档执行 `git apply --check`，成功后应用，再检查本地契约。任何 hunk 对不上均停止，不使用模糊替换、`--reject` 或部分应用。上游挪动上下文、改变契约或吸收了补丁时，在临时干净副本中重新审阅和生成对应 `.patch`，检查 diff 仅含预期修改，再验证 Runtime/相关测试。主同步脚本中不再存放 C++ 代码替换字符串。

`module-defines.json` 按模块维护本地明确启用的宏。目前保留两个 multihead-fast-padded-wave 模块的 `HIP_FFN_LINE_STORES 1`。单个模块启用了某宏，不能代表其他模块也启用。上游出现同名同值定义时不重复注入；值冲突会失败，要求审阅。

## 有意跳过与重试

- 审计脚本缺失、Python 不可用、上游证据读取失败、审计退出非零都不能完成同步。可用 `-PythonPath` 指定解释器。
- `-SkipEnablementAudit` 仅供无 Python 机器准备源码以便后续审阅：返回 0 表示准备完成，状态仍为 pending，不构建、不前移完成 pin，也不打印同步完成。后续必须不带该开关重新执行。
- `-SkipBuild`、`-SkipModules`、`-AllowStaleModules` 是明确的验证例外，必须在审阅中说明并安排后续验证。跳过构建绝不意味着产物已可发布。
- 首次使用此流程还没有模块验证基线，需构建模块，或明确使用 `-AllowStaleModules` 并记录后续验证；不能仅因当次源码没有变化就认定已有模块有效。
- `-ModulesPath` 只接受完整的 gfx1200 + gfx1201 构建树/模块包：两架构各 **$PerArch** 个受控模块（当前 **31**，以 `tools/release/check-module-contract.ps1` 为准）、根/叶子 `SHA256SUMS` 和两份 `modules.json` 必须一致。上游构建树可不含产品 `runtime-manifest.json`，同步时在候选目录补齐；安装和打包则必须已经包含它。缺少一个架构、清单不完整、哈希错配或链接路径均在目标改变前失败；`-AllowStaleModules` 不能绕过包完整性校验。旧扁平目标需先移出同步目录。增删模块时见 [docs/release.md](../../docs/release.md)「模块数量契约」。
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
