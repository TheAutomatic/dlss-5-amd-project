# lmxxf 0.37 消费者重审（2026-09-30）

## 范围与结论

固定上游范围：`c0a61968874e438ca3f887506f09c185ce378020` →
`c809efb0ea2960f148624730898da61b8fb55a45`。本地基线为 `a27ec0b`。
本次没有追更该范围之后的作者提交，也没有发布安装包或改动 dist。

旧记录的统一排除理由不能证明消费者不可达。本次重新读取该范围的全部变更，
按消费者审阅生产代码、部署配置、生成器、实验和原始数据；逐项记录在
[upstream-review.json](../third_party/lmxxf/upstream-review.json)。同一实验的原始文件
共享实验结论，但每项保留具体路径、Git blob、数据角色及内容事实。
这些 excluded 表示该文件或独立消费者不进入产品构建，不表示忽略其中的证据。

## 对 mimo 修复的复核

| 原问题 | a27ec0b 的结果 | 本次处理 |
|---|---|---|
| submission proxy 启用时漏装 CreateCommandSignature hook | 已正确单独安装 signature hook，不与 ArmCreate 冲突 | 保留 |
| 老 ini 的 byte stream=true、stream=auto 被新默认 3 覆盖 | 显式 stream 0..3 优先；旧 byte=true 且未显式指定 stream 时恢复 stream=0 | 保留 |
| 超过 8 次 Evaluate 后清理已 enqueue 的任务 | 只保护未 enqueue 的任务；enqueue 后、continuation 提交前仍可过早 Retire | 去掉按 Evaluate 次数退役；只由实际 Submitted 消费 pending lease |
| 统一 excluded、WAVE_OWNED/pinned bridge 误判、vit_stream 指纹过期 | 尚未完成消费者审阅 | 本次重新审阅及认证 |
| 自定义 skip 4/69 与 FP8 raw chain 不兼容（前置版本已有） | 语法允许，但 SkipChainFinish 仍读 FP16 | Runtime 在网络分配/Record 前拒绝该组合，避免错误数据；菜单说明限制 |

`BetweenThunk` 清除回调 payload 只证明 HIP enqueue 已返回，不证明游戏已经提交
continuation。`LmxxfPendingSubmission` 独立保存 job/list 身份；后续 Record 和 off
轮询保持阻塞，匹配的 Submitted 才退役一次。测试覆盖真实 BetweenThunk 执行后
100 次无关提交/轮询、正确提交及重复提交，以及 32 轮 callback/disarm 竞态。
若游戏丢弃命令列表而永不提交，仍保留资源；不能用次数猜测 GPU 已不再引用它。
将来需要显式的丢弃/Reset 契约才能安全恢复此情形。

自定义跳过 C32 链末尾 4/69 会触发 `SkipChainFinish` → `c32_finish_crop_half`。
当前 `HIP_C32_BYTE_CHAIN=1` 的生产者写 FP8，消费者按 FP16 读取，格式不匹配；
关闭 wave-owned 不改变这个编译期契约。本次在本地 ProductionOptions 明确拒绝 4/69，
回退原始 Color，默认 42/43/46、none/auto 及其他选项保持原行为。
跨独立 CRT 的配置测试覆盖两种 wave-owned 状态、单独/混合 4/69、超长完整列表和恢复默认。
不能直接把 pooling 输入解释成 FP8 后宣称逐位，完整支持暂缓，见下表。

## 实际调用链和优先级

`LmxxfBackend::EnsureSession` → 产品 C ABI `Create`/`PrepareFrame` → 本地
`LmxxfProductionOptions` → FOLLOW `NativeApplyHipEnvironment` →
`hip_reference::Network` → pinned `D3D12Bridge` → 对应架构模块及 kernel。

本地宿主没有调用作者的 `NativeHipNetwork::Create`、`NativeGameFrame`、
`NativePreUpscale` 或 DX12 network70 推理消费者。profile 中的 C32/MH/ViT
DX12 开关须按这些实际消费者归类，不能因为含 TEST 就排除，也不能因为值为 1 就开启。
产品的 ini/Ins 先 PutEnvAlias；flags.txt 和外部环境只补未设项；编译默认最后。

| 路径 | 产品状态与前提 |
|---|---|
| WAVE_OWNED | 默认 true，shared env parser 可覆盖；要求 fast/packed/E4M3/FFN/MH byte prerequisites 和未跳过对应链；使用 c32-wave1 / c64-wave2 |
| Swin | 默认 true；pooled + wave-owned，1600×960 或 1920×1152；仅 C256 encoder 16..21 / decoder 49..54；720 档不运行 |
| ViT typed stream | 默认 mask=3：bit0 AV FP8、bit1 contract F16；与旧 byte stream 互斥，旧配置迁移如上 |
| QKV W5 | 仅 vit-stream 编译；HasFn(_w5) 且 count%3072==0 时用 160 threads、96×ceil(tokens/80) groups，否则保留原导出；400/640 token 尾部 wave 仍参加 barrier |
| ViT attention | 四个宏仅 deep_fast-packed=1；对应 400/640 bytein_bout；其他 deep 模块保持 0。转换只针对 clamped exp 可产生的 552 个正 normal half 编码 |
| PDL | 保留请求/有效状态查询和 preflight；Swin init/run/recover 使用普通 stream launch 边界，不把持久化任务嵌入 PDL 链 |
| FFN_LINE_STORES | 本地只在两个 multihead-fast-padded-wave 模块设 1；c512-m32-mh、c64-wave2、swin 保持 0 |
| C512_PROJ_M32 | 作者共享投影候选正式 ABBA 变慢，源码默认 0，生产配方未开启 |

Swin 为每层保留独立输出，原输入不覆写；任务依赖最多 2×2 窗口，参数按值传入。
GPU 超时设置 mapped error，排队的 recover kernel 在后续消费者之前串行重放原阶段，
后续帧禁用该实例的持久化路径。票号回绕先 drain、clear、drain，再重置；没有每阶段 CPU 等待。
生产构建的 `SP_*` 调试环境不生效，故障注入验证使用显式诊断构建。
产品模块包契约先检查 31+31 完整性；作者 Network 的缺模块 fallback 不是产品安装缺模块时的放行依据。

## pinned bridge 和暂缓

bridge 保留 `54e14de503431cd4536f8a7151b022af232178a9` 原始头及产品 patch。
范围内新增 SwinRunActive 查询用于作者宿主日志；产品不使用该宿主，不需要为日志换掉 bridge。
现有 patch 的 PdlRequested/Effective/Reason 与 HIP 输出 signal 后的可复用 completion event 保留，
后者帮助驱动回收启动记录，不增加 CPU wait；销毁时释放事件。

| 暂缓项 | 必须完成的下一步与验收 |
|---|---|
| Direct I/O（累积 pinned bridge 差异） | 定义 C ABI 输入/输出所有权，审查 UAV/格式/状态与跨队列屏障；带产品 patch 更新 bridge；验证 prepare/enqueue/submit/cancel/resize/NR off、历史和双端切换，再比较逐位输出和游戏帧时间 |
| HIP graph | 当前 ABI graph_supported=0，Create 显式拒绝；先定义 prepare/submit capture 边界和资源生命周期，再验证回放、resize、设备/队列变更、错误恢复及逐位输出 |
| FRAME_STATS | shared parser 可达，但产品没有调用作者的 frame stats 输出消费者；先增加产品诊断接口，验证配置优先级、线程安全、off/销毁和采样开销 |
| C32 链末尾 skip 4/69 | 增加独立 FP16 raw-chain 模块及对应选择，或证明可保留原 pooling 算术的收尾路径；与同一 skip 配置的参考路径比较 EXACT/AE、尾部/池化/资源；验证默认配置性能与热重建后再撤掉拒绝检查 |
| 未选中宏候选 | 每项 next_step 指明单模块构建及作用；需对应输入域 EXACT/AE、尾部/资源前提、同机 ABBA，无误差且有可重复收益才改变默认 |
| gfx1200 硬件 | 双架构编译和字节身份已验证；在真实 gfx1200 设备运行相同 GPU/故障/格式用例 |
| 游戏与性能 | 本次没有游戏画面、low 帧或负载 ABBA；在具体游戏验证 NR off/on、双向切换、new wait、帧时间和 VRAM 曲线后才作游戏级或发布就绪结论 |

Daniel 原生 shutdown 只释放 staging 和宿主资源，模型缓存仍保留；本次没有宣称它的显存全部释放，
也没有把本次 lmxxf 消费者认证当作 Daniel low 帧回归的性能结论。

## 证据验证

下列是本地实际验证，与作者归档分开记录：

- 从本次 vendor + canonical Swin types + 本地 module-defines 重建 gfx1200/gfx1201 各 31 个模块。
  62/62 SHA256 与 main 包逐字节一致，ELF .text/.rodata/.note/.data 也一致。
- `tools\build\build-release-local.cmd --fast` 成功；加入 skip 4/69 拒绝检查后重新编译成功。
- 同步落盘后 `tests\run-all.cmd --tier ci` 全部通过；后续 skip guard 的 host CI（含跨 CRT 测试）、Runtime ABI 再次通过，最新 Runtime 的 EXACT/AE 哈希再验证一致。
- `tests\run-all.cmd --tier ci,device --out exports\consumer-review\validation` 全部通过：
  Runtime、host、shader、ABI、WARP、installer、sync（58 项）、设备提交/状态测试。
- `tests\lmxxf\run.cmd gpu exports\consumer-review\gpu` 在 RX 9070 XT 通过；
  包含 resize/queue mismatch/格式/曝光/ultrawide/subrect、D3D12/HIP zero readback。
  1080 固定输出：EXACT `fe40c904da05472e`、AE `79233836b6257864`、
  R10G10B10A2 `8ba14ef2db0dddfe`；scale16 AE 一致；Swin 正常 runs=4/fallback=0。
- 诊断 Runtime（MSVC `CL=/DHIP_SWIN_PERSISTENT_DIAGNOSTICS=1` 构建）额外比较 Swin off、正常、异步强制超时、强制回绕：900/1080 × EXACT/AE，
  每组四种输出哈希一致；timeout fallback=disabled=errors=1，rollover>0。

作者数据复算使用上面固定 commit 的脚本与原始数据；不是再次运行作者游戏：

- Swin：P/Q 各 168 候选黄金帧，144 个超时/回绕压力帧，off/missing 各 12 帧；AE 决策一致。
- C128/C64：四阶段 672 正确性帧 + 384 压力帧 = 1056，AE 528 行；128 个 timing slot
  支持负结果，未采用小通道持久化或 hand-off polling。
- ViT attention：168 黄金 + 48 回绕 = 216 候选帧、AE 108 行；123 个 micro job 的
  7 轮 median、guards/finite/EXACT 记录复算；552 个 half 编码转换逐位一致。
- 2184 个范围内结果文件及 1 个既有黄金清单的 JSON/CSV/log 内容逐一解析；CSV 共 182668 行。
  timing-only 数据只有被标 checked 的帧具备图像检查，不能宣称所有计时帧均验证输出。
- LLVM replay ZIP 的 168 个候选黄金哈希、84 行 AE 决策和 checked 帧 finite 复算通过。
  QKV micro ZIP 的 96 个成员逐个读取（保留重复文件名），7 轮 median、guards/finite/EXACT 通过。
- LLVM patch1 / C512 proj / ViT QKV 部分实验只归档日志/CSV/聚合、外部 artifact SHA，
  未归档可重跑的全部 frame 二进制。保留该限制，不把 aggregate/pass 文案当独立黄金验证。
  LLVM VOPD lookahead 未达作者 0.5% 门槛（1080 回退），本地继续 COMGR，不采用 LLVM fork。
- 作者常规/Magpie/RE9 profile 的 DIRECT_IO、MAKE_RESIDENT_EVERY、PRE_UPSCALE
  属于不同 host 的消费者；包配置只能佐证意图，不等于本地 C ABI 使用它们。

认证命令固定 `-UpstreamRef c809efb0 -SkipUpstreamFetch -ModulesPath <上述验证模块包>`，
不带 build/modules/audit waiver。最终审计 2515 项：92 integrated、2407 excluded、16 deferred；
同步包校验和 Runtime 编译成功后 `sync-state.json` 才恢复 `reviewed`。

## 每模块实际宏

以下值来自 31 个生成翻译单元的预处理结果；仅为提取条件值，将 builtin 探测设为可用。
实际 HIP 编译与 62 个产物的字节身份由 COMGR 构建另行证明。表只列定义的模块，未列者未定义。

| Macro | Value → modules |
|---|---|
| `HIP_BRANCHLESS_F` | `1` → c512-m32-deep, c512-m32-mh, c64-wave2, deep_fast-packed, deep_fast, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, multihead_fused_attention, swin-persistent, vit-stream, vit-wide-deep |
| `HIP_BRANCHLESS_Q8` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_C32_ABLATE` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_ALIAS_FFN_V` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_BRANCHLESS_F` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_BYTE_CHAIN` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_CU_MODE` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_DIAG_WEIGHTS` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_DIRECT_WEIGHT8` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_FOLDED_FFN` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_HOIST_PW` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_IN16_ALIAS` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_INPUT_DWORD` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_LANE_STAGE` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_LDS_VECTOR` | undefined → no selected recipe (ifdef candidate off) |
| `HIP_C32_LOCAL_ATTN_SYNC` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_LOCAL_FFN_SYNC` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_LOCAL_QKV_SYNC` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_NO_UNROLL` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_REGISTER_FFN` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_RTZ_ISA` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_STAGE_PREFETCH` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_TRANSPOSED_TAIL` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_VT` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_WAVES_PER_EU` | `0` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C32_WEIGHT_DWORD` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_C512_HOIST_RES` | `0` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_DEC_HOIST_SCALE` | `0` → c512-m32-deep, deep_fast-packed, deep_fast, vit-stream, vit-wide-deep |
| `HIP_FFN_ABLATE_ACT` | `0` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_COOP_INPUT` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_HOIST_RES` | `2` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_INPUT_PACK4` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_LINE_STORES` | `0` → c512-m32-mh, c64-wave2, swin-persistent; `1` → multihead-fast-padded-wave-packed, multihead-fast-padded-wave |
| `HIP_FFN_PK_ACT` | `0` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_TRANSPOSED_TAIL` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FFN_WAVE_NORM` | `0` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FMED3_CLAMP` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FP8_CLAMP` | `(x) __builtin_amdgcn_fmed3f((x),-448.f,448.f)` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_FP8_SAT_MODE` | `3` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention |
| `HIP_ISA_HALF` | `1` → boundary-fast, boundary_reference, c32-wave1, c32_fast, c32_fast_attention, c32_fused_attention, c32_fused_ffn_attention-packed, c32_fused_ffn_attention, c32_prefix_reference, c32_tiled, c32_wmma, c512-m32-deep, c512-m32-mh, c64-wave2, deep_fast-packed, deep_fast, deep_reference, deep_wmma, multihead-fast-packed, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, multihead-fast, multihead-reference, multihead-tiled, multihead-wmma, multihead_fused_attention, prefix_fast, swin-persistent, vit-stream, vit-wide-deep, wave-pointwise |
| `HIP_LDS_FENCE` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c32_fused_ffn_attention, c512-m32-deep, c512-m32-mh, c64-wave2, deep_fast-packed, deep_fast, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, multihead_fused_attention, swin-persistent, vit-stream, vit-wide-deep |
| `HIP_MH_ABLATE` | `0` → multihead_fused_attention |
| `HIP_MH_INPUT_DWORD` | `1` → multihead_fused_attention |
| `HIP_MH_REGISTER_EX` | `1` → multihead_fused_attention |
| `HIP_MH_RTZ_ISA` | `1` → multihead_fused_attention |
| `HIP_MH_SCALAR_DIAGONAL` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_MH_VT` | `0` → multihead_fused_attention |
| `HIP_NATIVE_FP8_F` | `1` → boundary-fast, c32-wave1, c32_fast_attention, c32_fused_ffn_attention-packed, c32_fused_ffn_attention, c512-m32-deep, c512-m32-mh, c64-wave2, deep_fast-packed, deep_fast, multihead-fast-packed, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, multihead-fast, multihead_fused_attention, swin-persistent, vit-stream, vit-wide-deep |
| `HIP_NATIVE_RTZ` | `1` → boundary-fast, c32_fast_attention, c512-m32-deep, deep_fast-packed, deep_fast, vit-stream, vit-wide-deep |
| `HIP_PDL_KERNELS` | `0` → c512-m32-mh, c64-wave2, swin-persistent; `1` → multihead-fast-padded-wave-packed, multihead-fast-padded-wave, multihead_fused_attention |
| `HIP_PREPACKED_WEIGHTS` | `1` → c32-wave1, c32_fused_ffn_attention-packed, c512-m32-deep, c512-m32-mh, c64-wave2, deep_fast-packed, multihead-fast-packed, multihead-fast-padded-wave-packed, swin-persistent, vit-stream, vit-wide-deep |
| `HIP_QKV_ROW_SUM` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_QKV_SPECIALIZE` | `1` → c512-m32-mh, c64-wave2, multihead-fast-padded-wave-packed, multihead-fast-padded-wave, swin-persistent |
| `HIP_SWIN_PERSISTENT_KERNELS` | `1` → swin-persistent |
| `HIP_VIT_ATTN_NATIVE_HALF` | `0` → c512-m32-deep, deep_fast, vit-stream, vit-wide-deep; `1` → deep_fast-packed |
| `HIP_VIT_ATTN_PROB_PAIR` | `0` → c512-m32-deep, deep_fast, vit-stream, vit-wide-deep; `1` → deep_fast-packed |
| `HIP_VIT_ATTN_TRANSPOSED_AV` | `0` → c512-m32-deep, deep_fast, vit-stream, vit-wide-deep; `1` → deep_fast-packed |
| `HIP_VIT_ATTN_TRANSPOSED_SCORE` | `0` → c512-m32-deep, deep_fast, vit-stream, vit-wide-deep; `1` → deep_fast-packed |
| `HIP_VIT_EXPAND_PAIR` | undefined → no selected recipe (ifdef candidate off) |
| `HIP_VIT_HOIST_SCALE` | `0` → c512-m32-deep, deep_fast-packed, deep_fast, vit-stream, vit-wide-deep |
| `HIP_VIT_QKV_ORDER` | `0` → vit-stream |
| `HIP_VIT_QKV_W5` | `1` → vit-stream |
| `HIP_VIT_STREAM_KERNELS` | `1` → vit-stream |
| `HIP_VIT_STREAM_QKV_HOIST` | `0` → vit-stream |
| `HIP_VIT_STREAM_QKV_UNROLL` | `0` → vit-stream |
