# 决策记录

## 2026-10-03：mochizuki 作为独立第三后端

从 `61e6618b` 分支接入官方 v0.0.3 对应 pin，网络核心与 shaders 来自 mochizuki 官方。
原生 D3D12/Vulkan 桥接、流水线预热和构建取消补丁复用并适配 MatheusFerreiraS 的实现；
宿主选择、配置菜单、安装打包和录制所有权由本项目接入。shader 编译配方使用官方默认设置。

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

- **决定**：当前接入保持 `DLSS5_HIP_INPUT_POLL` / `DLSS5_IO_FUSE` 不启用，不添加无实际消费者的菜单或 ini 键。
- **原因**：上游整帧收益不足或尾延迟退化；产品固定桥接还需录制、重放、取消与退役适配。
- **落在**：[lmxxf 后端说明](backends/lmxxf.md#输入轮询与-io-融合暂缓接入)。仅在上游稳定端到端收益或产品瓶颈证据出现后重新评估，并先验证生命周期与逐位输出。

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
