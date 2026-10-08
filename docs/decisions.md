## 2026-10-09 — 菜单独立NR页面与staging自签名

菜单采用稳定页面ID和自适应侧栏/窄窗选择框；NR保留现有输入、模型、输出分组，不随
上游UI替换三后端控制和INI契约。模糊背景只在菜单需要时运行，未完成的GPU工作不复用
或释放；实现和验收边界见 [菜单导航](architecture/menu-navigation.md)。

本地及Actions仅签打包暂存副本，先验证原始构建身份，再签名、生成hash清单和ZIP。
保留第三方嵌入签名；不导入系统信任、不上传私钥。CI临时的是私钥留存时间，证书本身
有效五年；无时间戳，到期需更新包。自签名不等同受信任发布者，详见 [发版流程](release.md)。

## 2026-10-06 — lmxxf history 与 ViT adaptive reuse 暂时互斥

伊莫实测同时开启出现严重闪烁，关闭 adaptive reuse 后正常。撤下允许连续 seed 保留
ViT 缓存的实验；history 开启时菜单置灰复用及四个滑块，Runtime 按该网络的 history
请求禁止复用（包括预热/priming/无有效 guide），保留用户原配置，关闭 history 后恢复。
保留共享历史缓冲直写和单次写入优化，不通过降低采样精度换性能。兼容调查未完成，
恢复前必须有动态场景误差/刷新诊断和游戏闪烁、拖影验收；见 [native history](architecture/lmxxf-native-history.md)。

## 2026-10-05 — 三后端共用实验性 SR → NR

保留默认 NR → SR，通过已有 RunBeforeSR 键显式选择 SR → NR。顺序在宿主共用入口控制，
不为 Daniel、lmxxf、Mochizuki 分别增加配置或变更 runtime ABI。将 SR Output 和渲染分辨率
guide 映射到同一网格，保留输出格式/alpha/有效区域，并使用录制所有权保护中间纹理。
不支持的输入明确跳过并提示；不扩大 native RR 或 native Vulkan 的支持承诺。
设计边界及验收见 [SR 后 NR](post-sr-nr.md)。

## 2026-10-05 — 减少重复工具回归和构建

用户批准精简本地与Actions流程。保留每次发布的当前产物测试、安装保护和打包门禁；
同步工具及本地试包启动器回归只复用同输入/环境/UTC周的成功记录，任何变化自动全跑，
失败或中途变化不生成记录。Mochizuki构建按源码、工具链和产物哈希复用，ABI仍每次执行。
Actions宿主构建与CI并行，打包等待两者成功，不用跳过或扩大兼容来换取通过。

lmxxf接入审计排除Mochizuki私有实现，保留共用消费链；不相关后端改动不再引发整体补审。
868个上游审阅项的分类和0.40生产配方不变。当前68模块与原外部构建树的二进制、
每架构元数据和校验和完全匹配；本次外部输入改为已提交模块目录，额外记录随包的
README和runtime-manifest，未改内核产物或来源结论。工具行为及使用方法见docs/release.md。

## 2026-10-04 — lmxxf 剩余20项不再作为接入待办

用户决定跟随上游已采用的生产配方：未采用的实验、旧替代路径不接入，
不为它们安排产品侧重复数值/性能试验。将剩余20项由 `deferred` 改为 `excluded`，
保留逐项分类、消费者与冲突证据，清除待办。当前功能暂缓数为0。
这不是“全部优化已启用”或“全部验证已完成”，也不改变源码、模块或运行时默认。

其中19项为未采用的实验/旧路径，具体依赖和上游验证范围见
[0.40消费审阅](lmxxf-040-consumer-review.md#未采用路径与上游验证范围)。
另1项 `DLSS5_VIT_ADAPTIVE_IDLE_MS` 只排除额外 Config/菜单接入：
现有500 ms空闲重置与外部诊断覆盖保留，adaptive reuse继续正常工作。

**验证纪律**：上游有数值和性能记录的未采用候选，沿用其结论即可；
只有脚本或默认关闭的路径，不能宣称上游已验证通过或测得更慢。
未进入生产配方且用户已决定不接入的路径，不因缺少本地数值试验继续列为暂缓。
产品侧回归仍用于验证实际采用的配方、编译器与本地桥接契约，不能由上游报告替代。

后续同步沿用未变的排除证据，不重复推荐或重跑旧实验。若上游将某路径正式纳入
生产配方或出现新的匹配证据，按新版本实际变化复核；自行重开旧实验须有用户新指示。
本决定取代此前这20项的暂缓/验收待办；INPUT_POLL、IO_FUSE、C512_PROJ_FB8
继续按下方决定排除。

## 2026-10-04 — lmxxf 输入轮询、IO 融合与 C512 FB8 已决定不接入

用户确认停止推进 `DLSS5_HIP_INPUT_POLL`、`DLSS5_IO_FUSE` 和 `C512_PROJ_FB8`。
三项从 `deferred` 改为 `excluded`，表示已审阅、已决定不接入，不再作为待办或
“下一轮优化候选”重复推荐。原有代码保留关闭，不新增配置或菜单。

| 项目 | 决定依据 |
|---|---|
| INPUT_POLL | 上游交接微测略快，整帧两档反而略慢；产品还需适配录制、重放、取消、超时及退役。收益不足以支持改同步路径。 |
| IO_FUSE | 上游收益仅约0.004–0.035 ms，组合复测900p尾延迟退化；产品不走 NativeGameFrame 入口，还需适配网络输出缓冲与录制生命周期。 |
| C512_PROJ_FB8 | 上游19组逐位验证通过；早期变慢读数后来确认混用了不同宿主，同宿主复测及16字节向量版本均未取得稳定整网收益，最终默认0、不收。 |

依据：当前固定0.40 pin `c81a88bc8534f7193df08ec3cae21d06d10d285d` 的
`Development/results/{handoff-gpu-20260930,input-slim-20261001,bitexact-pm-20261001}/`；
FB8 以 `Development/results/c512-qkv-pipeline-20261001/README.md` §2 的后续纠正
为准，结合 `mochizuki-gap-20261001/README.md` §6 和 `night-20261001/README.md` §3。
这些是上游实验数据，不是本产品实测。

**重新评估条件**：出现匹配当前配方的稳定端到端收益证据，或产品分析定位到具体瓶颈，
再按用户新指示重开。新版本、WSL/编译器可用或审计再次列出同名条目，本身不构成
重新研究的理由。后续同步沿用已决定不接入的记录；证据未变时只引用本决定。
此决定取代2026-10-02的“暂缓输入轮询与IO融合”；其余20项随后按本页顶部决定关闭。

## 2026-10-04 — Enable the original lmxxf LLVM23/RowOpts recipe

Use WSL2 and the official AMDGPU-enabled Linux LLVM23.1.2 package for the five
upstream LLVM rows per architecture. Use the unchanged upstream Windows recipe
with RowOpts for the remaining COMGR rows, including c512-m32-deep max-ilp.
Keep MSVC for the product host/runtime and existing product bridge patches.
Record actual C64 HIP_BARRIER_FENCE, source/object hashes and compiler commands;
missing or stale LLVM prebuilds must fail rather than silently reverting to COMGR.

The complete gfx1201 GPU suite retains the old goldens and 0.40 control/geometry
outputs. This enables the compiler recipe, without a local game speedup claim or
gfx1200 hardware acceptance. See the 0.40 consumer review and sync build workflow.

## 2026-10-04 — lmxxf 0.40 defaults and update workflow

Use all 71 blocks, fast numerics on, one network pass and free resolution on. INI/menu remains authoritative. Show missing model dependencies at backend selection and in Ins; preserve invalid user model files and explain how to replace them. The flowchart presents three editable stages, with game input and downstream rendering as context.

Source updates and closed Daniel runtime updates have different costs. lmxxf requires reconciling our recording/ABI patches, generated modules, compiler-specific correctness, config ownership and upstream experiments that can change release defaults. Daniel leaves those internals inside the supplied binary, so our review mostly concerns its interface, packaging and behavior. Neither source staging nor reading an upstream changelog substitutes for this work.

There was also avoidable product maintenance: module counts repeated in runtime validators could disagree with the name array. Those checks now derive their count from that array; cross-language package consumers remain protected by the existing module contract test. Keep staged review and group related evidence once in the consumer review document. Do not add another approval gate or rerun unchanged historical experiments merely because a new release workflow starts. This iteration also spent time on downloading a compiler whose official Windows distribution lacks AMDGPU; record the limitation and an executable follow-up instead of claiming its optimization is enabled.

## 2026-10-03 — Navigate NR controls by processing stage

Adapt the node drawing and click navigation from wilsjo2's OptiScaler-NR v0.8.4
(`8802b2b470db0462fa1ed03a125e793a7c06d735`, GPL-3.0), retaining its visual style.
The three AMD backends use this product's existing pre-SR route: prepare input,
run the network, adjust its output, then Super Resolution. The chart describes
configuration; it does not establish that GPU work ran or change rendering options.
Game-dependent follow-up rendering is illustrative rather than a universal HUD order.

Keep backend selection and the existing performance-display option above the chart.
Input owns resolution/exposure/preprocessing, model owns active passes/styles/history
and reuse, and output owns final strengths/shared residual stabilization. Compatibility,
kernel tuning and diagnostics remain tools. Preserve Config keys, defaults, delayed
slider commits, inherited later-pass settings and startup-only hook policies.
Page resets touch the current backend's page only; shared effects retain a separate
reset, while Reset NR settings keeps its current backend plus shared-controls scope.

Read existing nonblocking timing snapshots without enabling sampling on menu open.
HIP/Vulkan network time is not whole NR latency; Daniel has no network snapshot in
the host interface. Missing/stale measurements stay unavailable. Do not subtract
network GPU time from frame interval to invent a remainder or combine overlapping
stabilizer/blend measurements. No new runtime ABI, rendering route or default logging.

## 2026-10-03 — Retain mochizuki capacity when reducing active passes

Separate the requested execution count from compiled network capacity. In automatic
capacity mode, reuse a network with sufficient capacity only when extent, colour
format, model scale, linear encoding and preprocessing capability match. A 2→1→2
cycle runs fewer/more passes without rebuilding. An explicit prebuild setting can
still request a smaller network; active passes remain bounded by available capacity.

Preserve a compatible active network and its existing frame geometry when expansion
is budget-refused, held for retry or fails to build. Do not drain it to make space
for that expansion. The existing status ABI reports requested/effective counts,
capacity and the deferred reason. Reject uninstalled candidates whose required
frame buffers cannot be prepared. Keep the process-budget guard and bounded retry
policy; do not release resources still owned by replayable or pending recordings.

Focused AMD GPU verification covers output comparisons, automatic reduction/reuse,
1→2→3 expansion, explicit reduction, budget/OOM recovery, candidate-buffer refusal,
old recording replay and delayed collection, and incompatible extent/format changes.
Use independent fresh sessions for two/three-pass output comparisons. This changes
the mochizuki runtime only; shared effects, host selection and the package ABI are
unchanged. Game acceptance, especially the separately reported backend hot-switch
symptom, remains distinct from runtime-level GPU verification.

## 2026-10-03 — Combine RE repairs with the mochizuki backend

Merge the tested mochizuki branch into the RE repair branch. Preserve caller-state
rollback, atomic FG resource readiness, asynchronous adaptive-history reset and
stop-before-drain ordering. Keep submission hooks mutually exclusive with the
new graphics-wait tracker. Backend selection remains available; new wait is
unavailable while submission hooks are armed, without rewriting its stored setting.

RE cold start and SR switching improved in user tests, but activated FSR FG
switching still hangs. The latest trace enters stop and never returns; it does not
identify which Deactivate operation blocks. Investigation is paused by the user.
A future investigation needs a full hung-process dump. Neither 1.9.6.3 nor the RE
special package establishes a proven baseline for this exact FG transition.

RE/FG phase traces and periodic colour/input diagnostics are DEBUG-only. Missing
FG depth/velocity warns once per instance, then logs at DEBUG. Default INFO and
actual failure reporting remain enabled. Mochizuki timing is opt-in and build
progress remains bounded. Review the merge intersections and compile the host;
no broad regression suite or new in-game validation is claimed for this merge.

## 2026-10-03 — Stop FG before swapchain drain/recreation

The FG hooks previously waited on the application queue before calling Deactivate.
Move pause/deactivation ahead of that wait in both factory recreation paths and
both public ResizeBuffers entry points. An active asynchronous presenter can still
be waiting for application work; the application must request stop before draining
and resizing. Retain the existing backend, INI settings, reference ownership,
internal-resize bypass and downstream resize behavior. This does not redesign
OwnedMutex or claim to resolve every SDK callback/lock cycle.

Add INFO entry/stop/wait/SDK boundaries with thread IDs and results, limited to the
first 24 transitions per process, so another stall identifies the missing return.
Correct the FSR FG preserved-swapchain warning that incorrectly named XeFG.
Review the final diff and compile the host; user explicitly requested no broad test
run for this diagnostic package. Real RE9 validation remains required. No runtime
or module rebuild, no upstream update, and no merge/push as part of this change.

## 2026-10-03 — Keep adaptive history reset asynchronous

Keep adaptive ViT enabled and preserve its thresholds and reset semantics. Reuse
same-geometry history buffers and clear the eight state words with hipMemsetAsync
on the network stream. Previously each idle/seed/mode reset called Upload, which
allocated a new state, synchronized the stream, copied synchronously and destroyed
the old allocation (including hipDeviceSynchronize). In a staged host this occurs
after an external producer wait inside the submission callback.

The maintained reference-network.patch carries this change across upstream sync.
The bridge GPU regression covers 720/1080 tiers, idle and seed invalidation, mode
changes and disable/re-enable, plus actual queue switching and output readback.
The old implementation fails the synchronous-call check in both tiers; the replacement
passes and all 20 per-frame output hashes match. General tensor-pool growth is reported
separately and is not claimed to be eliminated. Recording leases, PDL, queue/fence
ordering and the public ABI stay unchanged. This removes a demonstrated submission
hazard; RE9 cold-start and DLSS-switch hangs still require repeated game validation.

## 2026-10-03 — Frame-generation readiness and bounded RE diagnostics

Replace the mutable per-slot readiness maps with an atomic resource bitset. Frame
reset, publication and the combined depth/velocity check now share one atomic state;
XeFG, FSR FG and DLSSG no longer perform contains/at across concurrent map erasure.
Remove the unused resource-frame map. This fixes readiness bookkeeping only; it does
not establish that all frame resources, frame counters or GPU lifetime are synchronized,
and does not claim to fix all RE cold-start or channel-switch failures.

On the RE comparison branch, trace NR preparation, recording, splitting and actual
execution for re9.exe/OnimushaWotS.exe only: first three eligible Record calls and at
most four later dimension/format changes, at most two executions per traced recording.
The phase log distinguishes CPU recording from HIP submission; no new INI setting,
ABI change, wait, kernel toggle or submission-order change is introduced. Keep the
legacy-state comparison baseline until game evidence selects the next fix.

# 决策记录

## 2026-10-03：mochizuki 首次编译进度与边界 shader 去重复

参考 MatheusFerreiraS 的编译配方，在八个 shader 上使用 NR_EDGE_BODIES=0，并通过本地
冷编译与输出对照验证。该设置减少按图像边缘 mask 复制的计算主体和驱动编译量，保留
NR 边缘处理。构建阶段回调与右下角进度显示由本项目接入，具体来源见 third_party/mochizuki/UPSTREAM.md。

RX 9070 XT、1920×1080 R11G11B10、无本地预热清单/缓存、不同全新测试程序名对照：
原配方构建 133.7 秒（其中主网络 pipeline 130.81 秒），新配方 48.6 秒（pipeline 42.92 秒）；
固定测试图的完整输出字节一致。旧测试程序缓存命中为 2.2 秒。这不是多轮性能基准，也不代表
所有游戏均只需 48.6 秒；游戏内是否还有额外阻塞需复测，不用原版“约一分钟”替代实测。

右下角构建提示不依赖 Ins/FPS 浮层，显示真实阶段、已完成 shader 数和已耗时。没有分母的
阶段只显示循环活动条；主 shader 数不能冒充整个构建的时间百分比。30 秒无阶段进展显示
停留时间，不武断判死锁。完成或关闭 NR 后隐藏；日志约每 10 秒一条，模型/缓存仍由用户保留。
构建 telemetry 是当前整包必需导出；不为旧 runtime 加载增加回退。此轮为定向修复与本地试测，
不重复完整发版测试，不改 dist，产物放 exports。

## 2026-10-03：第三后端审查后的执行历史与 DRS 边界

时序历史按实际执行的录制、网络、有效尺寸和 pass 数判断连续性；丢弃、中途跳过、乱序与重放
均不能沿用不相邻帧的历史。ResetHistory 在执行时消费，使已录制命令也能收到 reset。
动态分辨率 Auto/Always 模式由 mochizuki runtime 的 bucket 管理，不进入宿主通用的 300 ms
尺寸等待与稳定帧门禁；Exact 及其他后端保留原处理。第三后端不使用 Daniel 的 scale 配置。

ABI 校验失败立即卸载被拒绝模块，避免 NR 关开后复用未验证函数表。输出只要求 copy/SRV 权限，
不额外要求 UAV。RX 9070 XT 上新旧权限的 RGB9E5/sRGB 都能运行，后者属于减少无用约束，
不宣称本机复现了格式创建故障。

实际 GPU 回归覆盖断帧、取消录制、延迟 reset、乱序/重放、RGB9E5/sRGB 和逐帧 DRS subrect；
宿主门禁与加载失败恢复通过代码审查及编译验证，游戏表现仍须用户验收。

## 2026-10-03：mochizuki 作为独立第三后端

从 `61e6618b` 分支接入官方 v0.0.3 对应 pin，网络核心与 shaders 来自 mochizuki 官方。
原生 D3D12/Vulkan 桥接、流水线预热和构建取消补丁复用并适配 MatheusFerreiraS 的实现；
宿主选择、配置菜单、安装打包和录制所有权由本项目接入。八个 shader 使用已做本地对照
验证的 NR_EDGE_BODIES=0 配方，详见本页编译进度条目及 `third_party/mochizuki/UPSTREAM.md`。

参考实现的单一当前帧接口不符合本产品可重放录制契约，因此宿主独立实现，runtime 适配当前
v2 录制租约：旧命令保留对应网络与几何资源；在 producer 前等待真实 consumer 尾 fence；
失效且确认完成后才回收，不凭后续帧猜测完成。ABI 必须整包匹配，不加入旧 ABI 回退。

第三后端使用独立 Mochizuki 配置、分组 reset 和用户自备模型；共享整体强度和稳定器。
默认 Auto DRS 采用参考桥接的 bucket 策略减少动态分辨率重建，保留 Exact 选项。
GPU 计时来自完成的 Vulkan query；核心仅增加完成序号，防止菜单轮询重复计样。
输入曝光纹理读取暂不接入：本后端使用手动 white point 或自身预处理测光，不借用其他后端开关。

本地 RX 9070 XT 已验证真实输出、14 次录制执行、双队列、尺寸切换后旧录制重放、延迟回收与
编译取消。此证据不是游戏兼容性或 FPS 结论；游戏验收和远端 Actions 均单独记录。
构建、打包、安装和使用细节见 [mochizuki.md](mochizuki.md)。


## 2026-10-02 · lmxxf 输入直写与无用 tile 副本

- **决定**：HIP 输入不再生成无消费者的 tile 副本；RGB pass 直接写入可 UAV 的 HIP 共享输入，并省去私有 PostBase 到共享输入的拷贝。输入轮询与 IO fusion 仍暂缓，FP16 输出直交沿用现有实现。
- **边界**：这是运行库内部的数据搬运优化，不新增菜单、环境变量控制或 ABI。保留 fence、录制租约、跨队列完成凭证和计时修复；COMMON→UAV→COMMON 由 RGB producer 保证，旧录制继续持有原 bridge。保留 RedirectOutput 的旧 color 分配，不宣传它也释放了显存。
- **收益口径**：1080 档每条 RGB 链少分配 33.75 MiB tile 缓冲并免相应写入；速度收益须以产品测量为准，不能套用上游数据或以 NR GPU 时长代替总耗时。这是分支内接入，游戏验收及发布验证另行完成。
- **落在**：`LmxxfNrRuntime.cpp` 两处 bridge 创建及 RGB 初始化；固定 bridge 与 `tools/lmxxf-sync/patches/bridge.patch`。验证及适用范围见 [0.39 消费者审阅](lmxxf-039-consumer-review.md#输入直写本地增量)。

## 2026-10-02 · 暂缓 lmxxf 输入轮询与 IO 融合

- **后续决定**：2026-10-04 用户确认停止推进，现为已决定不接入（`excluded`）；以本页顶部的三项关闭决定为准。以下保留当时依据，不再作为待办。
- **决定**：当前接入保持 `DLSS5_HIP_INPUT_POLL` / `DLSS5_IO_FUSE` 不启用，不添加无实际消费者的菜单或 ini 键。
- **原因**：上游整帧收益不足或尾延迟退化；产品固定桥接还需录制、重放、取消与退役适配。
- **落在**：[lmxxf 后端说明](backends/lmxxf.md#输入轮询与-io-融合暂缓接入)。仅在上游稳定端到端收益或产品瓶颈证据出现后重新评估，并先验证生命周期与逐位输出。
## 2026-10-01 · NR 运行期间覆盖 XeFG 倍率

- **决定**：`[DlssNr] XeFGInterpolationCount` 独立保存 NR 专用插帧数，默认 `0/auto` 不覆盖；数值 `3` 表示总倍率 `4X`。菜单位于 XeFG 设置的 `DLSS NR MFG override`，通过 Save Settings 持久化。
- **优先级**：仅在 XeFG 已启用、支持实时改倍率且 NR 实际运行时覆盖生效值；不修改 `[XeFG] InterpolationCount`，不打开 FG、不切换后端。覆盖期间修改基础值，退出 NR 后使用新的基础值。
- **状态**：原生路径以成功模型评估为准；daniel 以成功完成为准；lmxxf 以成功神经网络 enqueue 和完整生产/消费提交为准，排除诊断、零输出回退和旧会话。关闭、失败及实际会话重建撤销覆盖；普通跳帧、历史重置、暂停或无新帧不触发超时撤销。
- **兼容**：旧 XeSS 缺少实时 setter 时禁用覆盖，不自动重建交换链。超上限只限制生效目标，不回写两个保存值。失败保留最后成功倍率并显示失败，配置/NR 状态变化或重新初始化后才重试。XeSS 非参数错误会自行关闭 FG，按 SDK 契约等待重新初始化，不能假装仍在生成帧或循环调用 setter。
- **落在**：`ConfigKeys.h`、`Config.cpp`、`framegen/InterpolationOverride.h`、`XeFG_Dx12.cpp`、NR 后端状态入口及 `tests/host`；切换沿用暂停/重启 FG 节奏，不承诺无感。验证结果单独记录，不因实现完成而视为游戏/GPU 实测通过。

## 2026-10-01：固定 0.37 修复与录制所有权

- 录制身份、资源有效期与实际 GPU 执行分别追踪。成功 Reset / 最终 Release 加所有提交完成凭证才允许回收；关闭 NR 不撤销游戏仍持有的闭合列表。
- 产品宿主要求 runtime ABI v2；旧宿主仍可使用 v1 前缀。HIP 销毁移到固定模块寿命的后台回调，不在 submission 锁内等待 GPU。
- 本地与 Actions 统一 MSVC 14.44.35207 / SDK 10.0.26100.0，打包保留对已测试 runtime 哈希的约束。历史 Actions 失败不笼统归因上游同步。
- 继续保留 c809efb0 上游 pin、54e14de bridge pin 和 16 项明确暂缓。完成固定 0.37 回归后停下，0.38 接入另行启动；不重复旧 ini 与性能归因调查。
- 契约与边界见 [录制生命周期](architecture/lmxxf-recording-lifecycle.md)、[构建一致性](release.md)。


## 2026-09-30 · lmxxf 0.37 消费者重审与真实提交退役

- **决定**：重新审阅 c0a6196 → c809efb0 的消费者、部署配置、实验和原始数据；逐项替换统一排除理由。WAVE_OWNED 是产品默认启用项，pinned bridge 保留是经契约审查的选择；ViT stream=3 和每模块宏绑定当前实际集成指纹。
- **原因**：文件位于实验/文档目录或开关含 TEST 都不能证明生产不可达。外部包摘要也不能单独证明由当前源码生成；本次重建双架构 62 个模块并逐字节确认身份，再运行产品 GPU 黄金及 Swin 故障恢复验证。
- **跳块补正**：当前 C32 FP8 raw-chain 不兼容只读 FP16 的链末尾收尾；Runtime 在分配/Record 前拒绝 skip 4/69，完整支持须有匹配模块和同配置黄金验证。默认 skip 列表不变。
- **生命周期补正**：HIP enqueue 返回与 continuation 提交是两个事件，不能按 8 次 Evaluate 猜测安全。独立 pending lease 仅由匹配的 Submitted 消费；游戏永不提交时保留资源，未来须定义显式丢弃契约。
- **边界**：16 个具体暂缓项保留可执行验收条件；gfx1200 硬件、游戏热切换/VRAM/low 帧与负载 ABBA 未由本次测试证明。Daniel 模型缓存仍保留，不宣称全释放。没有发布新包。
- **落在**：[消费者重审](lmxxf-037-consumer-review.md)、`third_party/lmxxf/upstream-review.json`、`sync-state.json`、`LmxxfPendingSubmission` 和已接入 host/run.cmd 的回归测试。

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
- **落在**：三语 README、[Palworld](games/palworld.md)、[lmxxf 后端](backends/lmxxf.md)。当前 ini 键名为 `DLSS5_FIT_LARGE` / `DLSS5_HIP_PDL`，旧名仅用于迁移；模块数量以 [发布测试清单](../tests/RELEASE-TESTS.md#数字契约改模块列表时必须同步) 与 `check-module-contract.ps1` 为准。

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

## 2026-10-02：整包交付与按风险验证

宿主和 runtime 由安装器整包覆盖，移除旧 ABI/FrameInfo 尺寸及宿主降级重试；不为局部替换维持兼容。
日常以最终 diff 审查为主，高风险行为做直接相关专项，完整测试集中在发版前且不重复执行。
执行位置：AGENTS.md、docs/release.md、docs/architecture/lmxxf-c-abi.md。


## 2026-10-02：本地与 Actions 共用精简后的测试入口

删除只在 Python 内重写配置优先级的模拟测试；保留实际跨 CRT 的 C++ 行为验证和配置命名/写入契约。删除产品始终禁用的八槽 ProductionTiming 实现、调用和旧 GPU 专项（含160帧 ABBA 开销测量）；当前四槽 NetworkTiming 的默认关闭、启用/epoch、错误停采、重放和输出一致性验证保留。

Actions 删除独立 Release upload regression 步骤，该测试仍由 tests/run-all.cmd → tests/install/run.cmd 执行一次，与本地完整入口一致。GPU 专项由本地 GPU tier 执行；无 AMD GPU 的 Actions 仍不运行该 tier，不宣称远端已验证 GPU。

本次只做相关验证：配置测试6项、原始夹具补丁重放2项、runtime编译、bridge真实GPU录制/计时/错误回归通过；更新受影响的两项接入审阅证据。没有重跑完整发版测试或重打包。dist 内 bf4f6c9a 的1.9.10.1测试包及其验证记录保持原身份；下次正式发包需构建并验证新提交。


## 2026-10-02：SR 状态恢复统一原生命令列表身份

原生 D3D12 hooks 的状态以原生命令列表为键；SR/NR 的恢复资格检查和恢复查表统一解析代理当前的 recording native list。不能因为查不到代理键就把实际可恢复的 SR 每帧跳过，也不能通过关闭恢复保护来放行。

NR 状态作用域保留进入时的原生命令列表。若 NR 发生拆分，先把其记录迁移至 continuation，再恢复；缺少源记录时清除目标旧记录，不凭空认为状态完整。代理路径的恢复调用通过代理转发，以同步其 continuation seed，原生 trampoline 只接收原生对象。恢复期间禁止重新捕获临时绑定，保留既有恢复配置与完整 graphics replay 的选择。

SR 缺少捕获状态时只输出一次 warning，避免逐帧刷日志。没有修改 FG、曝光或后端默认值。

验证：新增 WARP 原生/代理身份、拆分迁移、代理恢复和缺失源状态回归；现有四项 lmxxf device 录制/提交回归通过。此结论验证宿主修复路径，鬼武者与 RE9 的实际 SR/NR、曝光结果仍待游戏复测；不等同于正式发版验证。


## 2026-10-02：lmxxf 拆分必须触发状态恢复

RE9 实机反馈暴露出前一项修复遗漏的调用条件：原生身份查找已通过，但 lmxxf 未设置录制标记，条件作用域仍跳过拆分后的状态迁移。恢复现在以实际 recording native list 变化作为独立触发条件；lmxxf 在 RecordInputs 前保守标记录制尝试，涵盖部分录制失败。WARP 回归增加真实代理拆分且没有 Daniel 标记的场景；此前只验证迁移/回放函数，不能证明后端会进入恢复路径。切换输入通道后的 DEVICE_HUNG 与 FSR FG 卡死仍需分别实机验证，不以本次修复声明全部解决。

## 2026-10-03：RE 冷启动回归采用 1.9.6.3 宿主状态路径做对照

用户确认 1.9.6.3 在游戏内选择 DLSS 或 FSR 帧生成通道时，均可带 NR 冷启动；新版先进入游戏再启用 NR 可运行，带 NR 冷启动仍失败。先保留 `61e6618b` 作为完整回退点，本分支暂时恢复 `7559a15` 的宿主状态捕获与恢复行为；此项是对照实验，暂时替代上面两项原生身份迁移方案，不是已验证的正式修复。

恢复范围：允许 late hook 捕获调用方对象（包括 proxy），状态查表与恢复使用同一对象和 late trampoline；撤回强制原生探针、proxy 跳过规则、producer 到 continuation 的状态迁移，以及新加的 lmxxf 录制后条件回放。恢复 graphics tracker 与 submission proxy 互斥的旧行为。因此在启用 submission proxy 的会话中，Daniel graphics wait 不会被启用；不改写用户的 AmdGraphicsWait 或 NrConvenience 配置。

保留当前 runtime/ABI、HIP 内核、配置默认值、菜单、效果功能，以及录制资源所有权、Reset/Release 和提交完成后的释放修复。不修改 FG 实现，也不把另一次 NR 尚未运行时的 XeFG 字典异常等同于 NR 冷启动故障。仅切回这组宿主状态路径，不能宣称整个程序与 1.9.6.3 等价。

旧的原生状态迁移辅助代码及对应测试随该实验移除，新增真实 Detours + WARP proxy 捕获/拆分/恢复回归并接入现有 lmxxf warp 入口。该专项、录制生命周期与同帧提交专项均通过；未运行完整 CI、HIP 性能测试或 RE 实机。交付仅为当前包的宿主 DLL 对照件；验收必须覆盖两种帧生成通道带 NR 冷启动、进游戏后启用 NR及切换通道，不能用延迟启用 NR 代替冷启动通过。


## 2026-10-04：passes 提交方式与 lmxxf 有效输入区域

三后端的 passes 统一使用整数滑条，并共用结束编辑后才提交的实现。lmxxf 继续通过原有 Config/环境别名提交值及重置历史；不在拖动途中逐帧重建。Mochizuki 的最大 passes 容量仍表示预分配上限，样式的不同枚举仍按各模型本身含义展示。

lmxxf 的网络和 codec 采样都使用宿主给出的有效 Color 区域；整张纹理的分配尺寸只用于输出分配、raw-buffer 行距及完整写回。decode 保留有效区域外的原始像素，不能用取消边界检查或缩放整个分配来掩盖自由分辨率的几何冲突。该修复维护在 codec-active-subrect.patch，后续同步必须重放；没有改变 LLVM23/RowOpts 模块、实验功能选择或 ABI。

验证范围见 docs/lmxxf-040-consumer-review.md 的有效输入区域补修记录。游戏内自由分辨率和菜单操作仍须用户实测验收。

## 2026-10-04：Mochizuki Windows cooperative-matrix shader 修复

- 直接在 main `85e92e7` 上接入 Storm 的 [be7bf0a](https://github.com/MatheusFerreiraS/neural-amd-opti/commit/be7bf0a3d542b96894b61f42f2c3a389592952c8) shader 子集。`unroll_glsl.py`、ViT include 与其父提交相同；构建脚本只有本地 shader 编译器参数差异。
- 导入展开器；构建脚本仅增加四个 shader 的 `UNROLLED` 选择；ViT include 应用尾块补丁。保留本地 `--glslang`、进度回调与 recording lease，不导入 Daniel async、菜单、DirectInput 或 runtime 改动。
- 完整重编 SPIR-V，不用 `--skip-shaders`；旧预热模块必须通过当前 shader 内容校验。测试资源中的旧缓存保留为迁移验证输入，缓存不入包。
- 720p、约 635p、360p 的 ViT 最后一个 64-token key chunk 不满；原输出在旧驱动也有计算错误，不用旧哈希强制判定修复回退。数值变化须结合有限值、网络实际执行、重复录制与独立正确性证据判断。
- 本机驱动是 `32.0.31041.1004`；作者的 `32.0.32015` 验证是外部证据，本机旧驱动验证不能冒充新驱动实测。这里只按 Windows 驱动版本记录，不把第三方营销版本号作为源码证据。
- 本地完整 shader 构建、ABI、GPU 录制/跨队列/历史/取消/DRS/格式回归、三个尾块分辨率与 1→2→3 passes 验证通过。旧预热清单被当前 shader 校验拒绝并重建；三个尾块分辨率在独立进程中的修复版输出逐字节一致。
- 固定 R11G11B10 输入下，新旧输出 RGB 平均绝对差：1080p 0.000856、720p 0.002749、635p 0.002984、360p 0.041726。额外用同一 shader 的非转置路径 `NR_VTRANS=0, NR_VKMASK=1` 作数值交叉检查，各分辨率与修复版差为 0.000850～0.001012，最大单通道差 0.015625。该路径是补充证据，不是独立模型 oracle；未开 mask 的替代路径在部分尾块仍明显偏离，不能当参考。游戏观感、新驱动实测与完整发布验证尚未覆盖。

## 2026-10-05：跳块菜单保留原有高级分类

用户明确要求不常用参数保留原分类，减少无意义的折叠层。基础跳块原在
`Advanced Kernels`，基础列表和第 2/3 层追加列表都直接放在这里、相邻显示，
不另设 `Block skipping` 分类，也不移到 Model 页外露。两项 reset 随 Advanced Kernels。
已有 `ViT / image reuse` 内的四个滑块直接跟随 adaptive reuse，关闭时置灰；不恢复
`Reuse tuning` 中间层。以后扩展同类选项时沿用这些位置。

## 2026-10-05：Unity 启动命令列表接管范围

lmxxf 与 Mochizuki 共用的早期接管补入 UnityPlayer.dll 调用方过滤。默认只对
`Aniimo.exe + UnityPlayer.dll` 启用，保留 Unreal/Forza 的 EXE 规则；其他 Unity
游戏可用已有 `LmxxfEarlyExeWrap=true` 主动启用，false 同时关闭两条早期路径。
不自动扩大到全部 Unity 游戏：引擎相同不能证明启动时序和列表使用方式相同。
有同类保留列表证据且完成启动/录制验证后，再考虑扩充默认名单。

两后端在原 `Compatibility & Scheduling` 内共用 `Early command-list wrap` 菜单，
INI 键名不变，保存后重启。只接管 DIRECT，先确认提交钩子就绪再开放早期代理；
保留正常交换链后的接管、内部创建抑制、录制与 Reset 生命周期。设备回归和宿主
构建已通过，游戏验证待进行；此修复不代表消除所有 Unity 闪烁或模型历史问题。

## 2026-10-07：NR 在外层命令列表包装下统一使用自身逻辑代理

采用 MatheusFerreiraS/neural-amd-opti 的 [894c2dd0](https://github.com/MatheusFerreiraS/neural-amd-opti/commit/894c2dd0d640b0dd0146d143801e881416ab7cd9) 中的命令列表身份修复，适配到公共 `AmdBridge::Evaluate`，不带入该提交的旧版本推荐和安装器改动。

ReShade 包在本项目代理外时，Evaluate 收到外层列表，队列提交收到本项目代理。入口通过 `ILogicalCommandList` 查询本项目的 `ID3D12GraphicsCommandList`，在获取设备、确认队列及后端录制之前统一身份。返回值用 `ComPtr` 持有至 Evaluate 结束，不沿用上游立即 Release 后借用指针的写法。保留代理的分段能力，不能替换成 producer/continuation 原生列表。不支持该私有接口的列表按原路径处理；无配置、ABI、Unity 接管或 Vulkan 调度变更。

ReShade 源码核对点为 `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0` 的 `source/d3d12/d3d12_command_list.cpp::QueryInterface` 和 `d3d12_command_queue.cpp::ExecuteCommandLists`。作者的 [77889203 交接](https://github.com/MatheusFerreiraS/neural-amd-opti/commit/778892032382eaa6d09007f886db75480649f4c6) 记录了 Conan Exiles Enhanced / RX 9070 用户确认 NR 恢复运行；没有将启动崩溃或 Device Removed 归因于这一缺陷。

验证：`lmxxf_wrapped_command_list` 已接入 lmxxf 的 WARP 层，覆盖无包装/一层/两层 COM 转发、查询拒绝、队列观察、分段前后字节一致性及引用回收；现有 recording lifecycle、same-frame boundary 专项与宿主编译通过。包装 fixture 只模拟 COM 查询边界，不能替代实际 ReShade、滤镜/add-on、加载顺序及游戏启动/退出验收；不据此宣称所有 ReShade 共存问题均已解决。

## 2026-10-07：amdxc64 Hook 采用可重试、非阻塞初始化

`getGpuInfo` 与驱动加载拦截都可能调用 `Amdxc64Hooks::Init`。以前以尚未提交的全局原函数指针判断初始化状态，两个调用者可以并发开启 Detours 事务，并在失败时清空对方使用的指针。改为 `RetryableDetour` 的 Idle/Installing/Ready 原子状态；并发或加载重入立即返回，不持有等待锁跨越 Windows 加载器。模块暂缺、配置要求延迟加载或安装失败都允许后续 Init 重试；不使用首次正常返回后就永久封闭的 `call_once`。

每一步检查 Detours 返回值。Begin 的 `ERROR_INVALID_OPERATION` 表示其他事务已存在，直接退回，不碰它的 Attach/Commit/Abort；Begin 在取得所有权后遇到页保护错误、或本次 Update/Attach 失败，清理自己的事务。Commit 自行完成或回滚。没有加入忙等、后台无限重试或全进程 Hook 调度改造。

原函数发布与 Detours 修改变量分离：AttachEx 准备跳板后，将其原子发布给 Hook，再提交入口跳转。这样提交刚生效、Init 尚未返回时也能正确转发；不把已被改跳转的入口当作原函数，避免递归。Detours 修改的变量只属于当前事务；安装成功后跳板永久保留。原有 Fsr4DoNotLoadAmdxc64/LoadCustomAmdxc64OnRdna2 配置和驱动接口功能保持不变，模块继续固定在进程内。

CPU 专项 `tests/host/amdxc64_hook_init.cpp` 使用真实 Detours 和合成函数，覆盖 12 线程竞争、同线程重入、缺模块/异常后的重试、逐阶段故障、真实 Commit 回滚、保留同线程/跨线程的外部事务，以及提交窗口转发与并发调用；已接入 host CI。该验证确认初始化缺陷修复，不代替剑星重复冷启动或 GPU/模型验收。

## 2026-10-07：Mochizuki v0.0.4 按 Windows 适用范围定向接入

官方 [v0.0.4](https://github.com/mochizuki0323/DLSSNR-AMD/releases/tag/v0.0.4)
只发布 Linux 版，Windows 仍为 v0.0.3。已审阅 `82560c4f..9e4574e1` 完整差异，
更新 vendor Windows 注释，并从 Linux 移植 HDR 高饱和色按峰值统一缩放的修复，
同时覆盖模型输入和低模型分辨率的合成，防止分别裁切通道导致偏色。

不为追版本号替换本地宿主/调度，不把 Linux INT4 混合精度和 robust access 优化
标成 Windows 已支持。INT4 涉及驱动管线二进制 WMMA 指令改写及校准模型；robust
access 优化针对借用 DXVK/vkd3d 设备，而本项目自建 Vulkan 设备未启用该特性。
其余放大算法、内存和计时改动的边界与后续要求见
[来源记录](../third_party/mochizuki/UPSTREAM.md#v004-directed-integration-2026-10-07)。

无新增配置键、权重、ABI 或默认值；菜单保留官方 Windows 版本。GPU 着色器专项
覆盖原始帧/alpha 不变、有效区域、高亮色相、普通亮度及缩放/原尺寸合成，且旧输入
着色器与旧合成着色器分别触发对应失败；不把该验证等同游戏视觉或性能验收。

## 2026-10-09：公共残差分频与细节保护先作为可选输出效果

新增低频/细节增益与肤色/边缘细节保护，分别默认1/1和0/0。四个键归
`ConfigKeys`、INI及菜单所有，不传给后端环境变量。复用现有输出资源租约、
预算和时序完成证明，不新增网络推理或runtime ABI，不解除History/ViT互斥。

公共稳定器先产生未放大的历史残差；邻域处理取稳定后的RGB，输出增益不回灌历史。
中性和等比例增益用独立逐像素入口，避免默认路径增加组共享内存和同步屏障。
肤色仅是颜色启发式，不表示已实现人物识别或“人物单层、场景多层”。
算法、范围、验证与后续边界见[公共输出效果](architecture/nr-output-effects.md)。
