# lmxxf 后端

## 角色

lmxxf 后端把 Kien 的 MIT 项目 [lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) 包装成 C ABI 运行时 `LmxxfNrRuntime.dll`，由 OptiScaler 通过 `LmxxfBackend.cpp` 动态加载。工具链和 DLL 边界统一见 [C ABI 专页](../architecture/lmxxf-c-abi.md)。

- 运行时源码：`OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/`（`LmxxfNrApi.h`、`LmxxfNrRuntime.cpp`、`LmxxfProductionOptions.h`）。这些是**我们写的**，从不从上游复制。
- 上游 vendor 代码：`third_party/lmxxf/`。哪些文件是我们的（OURS）、哪些跟上游（FOLLOW）、哪些排除、以及本地补丁，见 [third_party/lmxxf/UPSTREAM.md](../../third_party/lmxxf/UPSTREAM.md)。
- 同步与审阅流程见 [tools/lmxxf-sync/README.md](../../tools/lmxxf-sync/README.md)。sync 必须附一份绑定到上游提交的审阅记录，`UPSTREAM.md` 里的 completed pin 才会前进。
- 权重不进仓库。完整查找顺序见 [加载与资源查找](../architecture/lmxxf-c-abi.md#加载与资源查找)。
- 后端选择：`[DlssNr] NrBackend`，由安装器写入。只装了一边就用那一边；两边都装了就让用户选。

## 同帧执行契约（路线甲）

- NR 与游戏帧在同一帧内完成：代理 list 被拆成逻辑段，HIP 在两段之间入队。
- 以下情况 fail-closed，拒绝拆分：query、predication、RT/meta 命令、无法建模的 viewport/scissor 调用，以及非代理 list 上的 Record。
- 使用 enhanced barrier 的 list 默认拒绝，因为 layout/access 和 `SYNC_SPLIT` 都没有建模。需要时用 `LmxxfAllowEnhancedBarriers=true` 显式放开。
- 已录完、`Flags` 为 none 的 alias barrier 不否决拆分（`803c8ba`）。
- 游戏创建 swapchain 之前，只对 Unreal 和 Forza 做早期 `CreateCommandList` 包装。`LmxxfEarlyExeWrap` 可以强制打开或关闭。燕云这一类游戏如果早包装会崩。
- 证据：lmxxf 早期整幅画面发糊，根因是 Color 迟了 1 帧，改成同帧后解决，在 5400 帧上验证过。

## 准入

| 项 | 规则 |
|---|---|
| 维度 | 必须是 TEXTURE2D，array=1、mip=1、单采样，不能带 `DENY_SHADER_RESOURCE` |
| 格式 | RGBA16F、RGBA8 UNORM、R11G11B10、R10G10B10A2（`d831b0b`），外加 RGB9E5。RGB9E5 使用私有 FP16 输出；`DLSS5_FORMAT_FALLBACK=true` 还允许支持的额外 typed SRV 格式（如 RGBA32F），并使用私有 FP16 输出。须通过设备 TEXTURE2D / SHADER_LOAD 能力检查；改变此开关需重启 |
| 几何（FitLarge 关） | `w ≤ 2560 && h ≤ 1080 && w*h ≤ 1920*1080`，也就是水平方向最多缩 25%。21:9 和 32:9 都在范围内，例如 Townfall 的 2024×848 → 1920×804 |
| 几何（FitLarge 开） | 最大 16384×16384，缩放到 1080 网络 |
| 作业尺寸 | 用 NGX 渲染子矩形；子矩形缺失或比纹理还大时，退回到纹理分配尺寸（`c1e5595`）。子矩形原点不为 0 的一律拒绝 |
| 网络档位 | ≤1280×720 用 720 档，≤1600×900 用 900 档，其余用 1080 档（上游 `Near()`，随 `d788963` 跟进） |

违反契约时返回可重试的 `INVALID_ARGUMENT`，日志写明是哪一项不满足（`fmt= WxH arr= mips= samples= flags= fitLarge=`），不抛异常，所以 session 不会被毒死。host 遇到 `INVALID_ARGUMENT` 不会重建 session。

### FitLarge

正式配置键为 `[DlssNr] DLSS5_FIT_LARGE`，默认 **true**；旧名 `LmxxfFitLarge` 仅用于读取旧配置。菜单 / ini 优先，flags 只补宿主未设置的键，具体见 [安装器与配置](../architecture/installer.md#dlss5-amdnative-game-flagstxt)。

帕鲁曾出现的秒级卡顿由 allocation 与渲染子矩形比较错误造成，已在 `a129c5f` 修复。截至 2026-09-28，维护者未发现修复后 FitLarge 仍有问题；旧耗时不能作为现行性能结论，也不构成关闭 FitLarge 或限制分辨率的建议。修复经验与回归依据见 [Palworld](../games/palworld.md)，默认值变更历史见 [决策记录](../decisions.md)。

## 用户控制

样式、1080 紧凑几何、格式兼容、共享整体强度、稳定器和计时的默认值与操作见 [用户说明](../../README.md#本分支新增控制尚未发布)。源码模板与打包器生成的 `[DlssNr]` 段均需维护；打包器会替换整个段，不能只修改源码 ini。

## 曝光

- **游戏提供了曝光**：曝光经 C ABI 传给 codec。`LmxxfNrFrameInfo` 尾部有 4 个字段：`exposure`、`exposure_state`、`pre_exposure`、`exposure_scale`。
  - `RecordInputs` 把 1 个 texel 拷进我们自己的 1×1 稳定副本，codec 绑定的是这个副本，所以游戏每帧换曝光纹理也不会重建链。
  - 只有源格式变化时才重建副本。旧副本先退役，等 drain 完再释放（`5d7fd5a`）。
  - 不能用的曝光（比如 2×2）按“没有曝光”处理，这一帧照常出图。
- **游戏没有曝光，且 `LmxxfAutoExposure=true`（默认）**：走运行时的测光（`e6ecf77`）。
  1. host 设置 `LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE`。
  2. `RecordInputs` 在 encode 之前，用一个 16×16 的 group 扫过有效区域，从平均亮度算出让编码均值落在 0.45 的曝光值，然后在对数域用 0.25 的系数平滑。
  3. 结果写进我们的 1×1 R32_FLOAT 纹理，codec 像绑定游戏曝光副本一样绑定它，所以 codec 路径不变。
  4. Color 的 SRV 在一个 16 槽的环里轮转，GPU 可能还在读的描述符不会被改写。
  5. 如果运行时太旧，退回 V1 结构体时这个 flag 会被清掉，不发给它。
- 测光生效时，shader 那一侧的 `0x10000` 白点位保持关闭，避免 encode/decode 重复应用（`c6075d3`）。`auto-white.patch` 仍保留着色器契约，具体用途见 [补丁维护说明](../../tools/lmxxf-sync/README.md#补丁维护)。
- **`LmxxfAutoExposure=false`**：游戏没有曝光时，用 `LmxxfAutoExposureScale`（默认 8）作为手动白点缩放。
- `LmxxfPaperWhite`（默认 1，取值在 (0, 64]）是 codec 的白点微调，不是 HDR Paper White 锚点。

## C ABI 要点

函数表版本、结构体尺寸协商、兼容降级与测试头文件要求统一见 [C ABI 专页](../architecture/lmxxf-c-abi.md#版本协商)。修改接口时以该页和 `LmxxfNrApi.h` 为准。

## host 侧行为

- **零输出回退**：Create 时带上 `ZERO_OUTPUT_FALLBACK`。连续 10 次回退就在本 session 关闭 NR，显示原始 Color。运行时不认这个 flag 时，去掉它再 Create 一次。
- **提交内等待**：回退时最多等 3 s，拆除时最多等 30 s。FitLarge 探测最多每秒一次。
- **失败重试**：Create 或 PrepareSession 失败后按指数退避重试。
- **切档不爆显存**：`CancelUnsubmitted` 和 `NotifyOutputSubmittedIfRecorded`（在 `hip_d3d12_bridge.h` 里，这个文件被 pin 住）会回滚未提交的 bridge 状态。燕云以前切档时显存涨到 21.6 GB，就是这个原因。
- **重建判断**：allocation 与 allocation 比、valid 与 valid 比（`a129c5f`）。以前拿 alloc 去和 valid 比，UE5 在非原生档位下每帧都会重建整条链。
- **viewport**：显式清空 viewport 记为 unset，不回放清空之前的旧状态。
- **PDL**：`[DlssNr] DLSS5_HIP_PDL` 默认 true；设为 false 时传递同名环境变量值 0，给不支持的驱动用。旧键 `LmxxfPdl` 仅用于配置迁移。

## 输出哈希 A/B（判断“优化”还是“改画质”）

`tests/lmxxf/lmxxf_nr_gpu.cpp` 用固定的 FP16 图案和固定 seed 跑一帧真实帧，把 `private_output` 读回来算 FNV-1a，只哈希每行的有效字节。

- 哈希相同：逐位等价，是纯优化，可以直接开。
- 哈希不同：数学变了，按画质变更处理。

运行方法：

- 构建和运行都要在仓库根目录。`tests/lmxxf/run.cmd` 用 MSVC dev prompt 编译 `lmxxf_nr_gpu.exe`；运行时也可以用 MinGW 编。
- 参数：`lmxxf_nr_gpu.exe <LmxxfNrRuntime.dll> <assets_dir> [mode]`。

模式：

| 类别 | 参数 |
|---|---|
| 输入格式 | `--output-hash`、`--rgb9e5`、`--r10g10b10a2` |
| 曝光 | `--exposure`、`--exposure-bad`、`--auto-exposure`、`--scale16` |
| 几何 | `--ultrawide`、`--subrect` |
| 拒绝用例 | `--reject-formats` |
| 流程 | `--resize`、`--queue-mismatch` |

**最后记录的哈希**（RX 9070 XT，1920×1080，seed=1。各值取自括号里的提交说明，在 `d788963` 上没有重新跑）：

| 场景 | 哈希 |
|---|---|
| RGBA16F，无曝光（`--output-hash`；`--exposure-bad` 与它相同） | `5e30fd5e768d35bb` |
| RGBA16F，有曝光（`--exposure`） | `26b22b8369a5d7e1` |
| RGB9E5，无曝光（`--rgb9e5`） | `150b4076be736fb6` |
| RGB9E5，有曝光 | `109a3dba1d8cf06d`（`743bb40`） |
| 超宽屏 2024×848（`--ultrawide`） | `2f4cbf6ce4a39ec7` |
| R10G10B10A2（`--r10g10b10a2`） | `ed30d52b0aa422fc`（`2be5bc3`） |
| 测光，输入 ×16 后再 /16（`--scale16`） | `9417d81940875916`（`e6ecf77`） |
| 第六条基线（提交说明里没写对应哪个模式） | `df07cd6f0dca8350`（`e31a8b2`/`5d7fd5a`） |

局限：只用了一个输入图案、一种分辨率、一个 seed。逐位一致是强证据，但不是对所有输入的证明。性能数字按上游口径引用，我们只测桥接。

## 启用点审计

`tools/audit-lmxxf-enablements.py` 在每次 sync 时自动运行，把上游的开关分成几类：

- 运行时 `DLSS5_HIP_*` 选项；
- 内核编译期的 `HIP_*` 宏；
- 作者部署脚本里开了、但我们配方里没写的开关；
- 有意排除的 D3D12 网络主体；
- 没人开的消融或死实验。

开关是否接入，以实际消费者、上游部署证据和逐项审阅记录为准；不能仅凭名称或某次部署没有启用就排除。

## 暂缓 / 不做

逐项的暂缓、排除理由及下一步，以 [upstream-review.json](../../third_party/lmxxf/upstream-review.json) 为准；实际跳过的验证以 [sync-state.json](../../third_party/lmxxf/sync-state.json) 的 `skipped_checks` 为准。旧能力评审的列表不能直接当作当前结论；`reviewed` 也不代表所有功能都已启用或所有验证都已完成。

## 与上游的关系

- 产品维护 `LmxxfNrApi.h`、`LmxxfNrRuntime.cpp`、`LmxxfProductionOptions.h`；vendor 中的 codec 等文件也携带本地补丁。文件归属与完成 pin 以 [UPSTREAM.md](../../third_party/lmxxf/UPSTREAM.md) 为准，生效补丁以 manifest 和 [补丁维护说明](../../tools/lmxxf-sync/README.md#补丁维护) 为准；同步时应逐项核对上游是否已吸收对应契约。
- `d788963` 之后，PR #9（零输出回退）已并入上游。我们随之把 `native_rgb_reflect.h` 和 `native_input_geometry.h` 改回跟上游，只剩 `hip_d3d12_bridge.h` 还 pin 着。
- 当前源码 pin 和同步状态分别读取 [UPSTREAM.md](../../third_party/lmxxf/UPSTREAM.md) 与 [sync-state.json](../../third_party/lmxxf/sync-state.json)，本页不维护第二份提交号。2026-09-28 文档核对时，状态为 `reviewed`，但记录了 `SkipBuild` 和 `AllowStaleModules`；这些是该次同步的验证例外，不能据此宣称完成构建或 GPU 验证。

### 输入轮询与 IO 融合：暂缓接入

这两项均为上游默认关闭的实验路径，目前没有产品菜单或 ini 绑定。

- `DLSS5_HIP_INPUT_POLL`：上游 `Development/results/handoff-gpu-20260930` 中，输入交接微测约省 0.05 ms，但整帧两档测试反而慢约 0.01–0.04 ms。产品固定的桥接版本承担录制、重放、取消及异步计时契约，不能只打开宏；需移植标记提交、等待、超时回退和资源退役，验证丢弃录制、跨队列重放与长时间运行。收益不足以支持现在改动同步路径。
- `DLSS5_IO_FUSE`：上游普通 NativeGameFrame 路径让解码直接读取网络缓冲，省去中间输出步骤；本产品不走该入口。`Development/results/input-slim-20261001` 的收益约 0.004–0.035 ms；`bitexact-pm-20261001` 的组合复测中 900p p99 从 7.508/7.520 ms 升至 7.565/7.578 ms。需先设计输出缓冲的录制所有权、重放和重建退役，再做逐位与产品端平均值/p99 对照，才考虑启用。

以上数字是上游实验结果，不是本产品实测。重新评估条件：上游提供稳定端到端收益，或本产品分析确认相关交接/拷贝是瓶颈。完整同步审阅仍以 pending 状态和逐项审阅记录为准。
