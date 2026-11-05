# FSR-RR 常量与描述符槽池

2026-10-31：修正固定环槽忙时直接中断 RR 转换的容量处理。该改动针对源码中可确定的槽位压力问题；尚未通过游戏复测证明它消除了 PBZ 的全部失败。

## 数据与生命周期

转换 shader 的常量通过 upload buffer 地址绑定，SRV/UAV 描述符由 shader-visible heap 提供。两者在 GPU 完成之前都不能覆盖；split 提交不能替代这个保护。

`FSRDPreprocessor_Dx12.cpp` 的每个 `ComputeState` 使用独立槽池。每个槽保留页面引用、页内常量偏移、独立描述符堆和退休票据。扩展只增加新页面/槽，不搬移原来的 GPU 数据。初始页保留原先容量；槽池达到使用高水位后保留到特性释放。

分配策略：

1. 查询全部现有槽，只复用无票据，或票据已退休且旧录制所有权已释放的槽。一次执行完成不能释放仍可重放的 closed command list 的 CB／描述符；wrapped 录制保持独立 Owner，直到 Reset／最终 Release 且所有执行完成。
2. 空闲槽不足时按原初始容量增加页面；旧常量地址和描述符堆保持不变，不等待 GPU 或强制退休。
3. 每个 shader 槽池最多 256 槽。长时间保留大量可重放录制也会占满容量；达到上限或分配失败报告阶段错误，不靠覆盖旧槽继续执行。
4. 转换在任何 barrier、计时命令或 ping-pong swap 前预留 FloorSeed、启用时的 3／5 次 FloorFilter 和 InputConv；合成独立预留 1／2 次 composition 及启用时的 evidence／6 次 propagate。独立 blit 仍按一次 dispatch 预留。
5. 预留本身不创建 GPU 票据；实际 dispatch 才提交对应槽的票据。未使用的预留在下轮可重新选择，已使用槽必须遵守真实录制／执行退休。预留解决容量失败的半录制，不声称所有分配、observer 注册或设备失败都可回滚已录制命令。

`RetiredSlotReservations.h` 是产品实际调用的 CPU 选择策略；payload 位于稳定的独立分配中。它不更改 `GpuBatchTracker` 的提交/完成判断，也不修复可能存在的录制放弃、重放或通知遗漏问题。若仍触及上限，应调查票据状态，不移除保护。

## 验证边界

- `tests/host/run.cmd fsrd-slots`：直接测试产品 CPU 策略，覆盖三槽之外的增长、非当前槽退休复用、五 pass 批量预留、放弃预留、扩展失败、零票据拒绝及 32 轮在途滤波后复用；接入 host `ci`。
- 本次按要求不编译/出包，C++ 用例与实际 GPU 路径尚未执行。后续须验证 DX12 debug layer、加载突发、RR 开关/重建、普通和 debug 输出，并复测 PBZ。失败采集应记录 shader 名称、槽容量及录制/提交/fence 状态。

## 2026-11-03：录制所有权与新合成历史

- Floor/recovery、单路与 Split 信号、匹配 albedo 调制、Additive Light Split、Albedo Bleed Fix 和合成变体已进入产品转换／合成路径；配置由 `[FSR-RR]` 和菜单拥有，不由离线运行器替代。
- 合成 decision/metadata 不再覆盖固定双缓冲。每个历史代次由写入及读取它的录制共同持有；复用要求这些录制失效且所有已提交访问 fence 完成。跨队列访问显式 Wait；同一有序提交批次内的生产者与消费者通过独立 admission epoch 识别，失败批次不授权后续提交。
- 后处理 helper 的不确定提交不能凭超时退休。有效 fence 使用完成回收；Signal 失败后的未知完成状态保留资源与模块引用，使用一次性设备移除通知，不持续轮询。
- RR 深度与 SR 深度分开：FloorSeed 同时写入 RR 使用的有界深度及 R32 未截断物理深度；SR 用最终生效 near/far 和深度约定重新编码后者，不能把 RR 的无限远诊断 horizon 当成真实远平面。
- 相机物理平面解析与诊断 horizon 分离；当前相关的 SL 标量与显式 SR/INI 偏好具有优先级。合法正向有限远根不因 `A/W` 很小而变成无限；反向投影噪声单独分类。
- 信号规划使用当前已验证的 hit-distance 可用性；历史“曾经见过”只保留作观察。到达、消失、返回通过现有延迟重建路径更新布局并清空 RR/SR 历史；缺失 guide 的显式 Indirect/Split 使用 Direct 回退，默认不估算次级射线长度。
- 启动启用 RR 时即准备录制代理。未在设备初始化准备的会话，菜单保留请求并明确要求 Save Settings 后重启，不声称勾选后能补包已有 native command-list 池；实际特性初始化还必须取得 `IRecordingResources`。

已实际执行：宿主发行构建；真实 Config/CSimpleIni 隔离磁盘重启读写、迁移、profile、49 个规范键和 reset；相机/信号 CPU 用例；后处理 Signal 失败、重放、pending Reset 及设备移除回收。合成历史 admission epoch 已独立源码复核，尚无多列表批次运行验收。

Godot 独立环境已完成最终宿主八帧 Vulkan 光栅输入→CPU 桥→DX12 宿主 RR/SR→Godot 后处理及正常释放。它既不是普通光追噪声，也不是路径追踪噪声；没有物理次级 hit distance，specular guide 是 F0 而非完整 BRDF 反射率。该结果只证明集成链可运行，不证明光追降噪画质、Vulkan 原生 RR 或实时性能。合成离线噪声对照也不能替代游戏路径追踪验收。

首轮 Floor 对照因冻结运行器遗漏旧策略字段而不可用于生产画质结论；修正实际控制后需重新比较。执行 PASS 不等于画质通过。不得依据未执行的 shader fixture、PSO 创建或配置转发声称数值等价或发布就绪。

修正冻结策略后，13 个合成场景、各 48 帧、clean/noisy 两轮的两代执行均完成：每代 1248/1248 票据退休，D3D12/SDK 错误及警告均为 0。六项 RR 参数在这组实验中统一使用实际查询的 AMD 默认值，而不是发行版调参；旧 Floor 使用固定提交 Config/feature 的完整控制，新 Floor 使用 manifest 中显式 fixture 策略，不能称为同数学或发行默认画质排名。

比较结果不是全通过：逐帧噪声残差与能量存在回退，细节与图像空间时间残差在既定界限内。多数场景的 pre-SR 平均噪声 RMSE 改善，奇数尺寸 RH 无限远例外；SR 后 HDR 能量及部分运动/线性深度场景仍有边界失败。未放宽门槛、未隐藏帧，不能声称新版所有画质优于旧版。纯光栅 Godot 也不能消除此验证边界。

实际宿主 guide 生命周期 smoke 已执行 absent→arrival→dropout→return，信号 mask 为 `0x3→0xc→0x3→0xc`，稳态均读回新输出；现有延迟重建边界仍出现一次 NGX failure 与两次 success-but-no-output，随后恢复。它证明不再永久卡在缺失 guide 的旧布局，不证明切换无损或零丢帧。

## 2026-11-05：丢弃录制的历史准入

录制阶段保留合法的 pending 跨列表历史候选，不因 writer 尚未提交而提前牺牲历史。完整的 composition（包含两个 tile pass）录制完成后才附加 reader／writer 历史观察者、设置 writer identity；提交钩子在 `RecordingMutex` 下以原始连续 native batch 顺序准入，非零 admission epoch 仅授权同批已准入的前序 writer。没有全局拆批，也不改变原有 barrier promotion／decay 边界或 queue Wait 的位置。

reader 第一次 `BeforeExecute` 在 writer 已提交、同录制前序 writer、或同一 ordered batch 已准入前序 writer 时保留历史。否则只对从未提交、由录制 Owner 固定持有的 composition 私有槽将 `HistoryValid` 及两个 typed SRV 一次性改为无历史。无私有槽不能假装成功；真正的 Wait／observer HRESULT 仍传播失败。无历史选择不可恢复；历史选择若所在整批未提交，可在重试准入时降为无历史；实际提交后所有 CB／descriptor 永不重写，若其历史来源之后变得不确定则拒绝重放，而不是修改 GPU-live 内存。

Root Signature 1.1 的静态 descriptor 不能在录制绑定后修改，即使 GPU 尚未提交。composition SRV 表因此保持索引布局，但仅把历史 t9／t10 的 range 标为 `DESCRIPTORS_VOLATILE | DATA_STATIC_WHILE_SET_AT_EXECUTE`；其它 SRV 与 UAV range 不放宽。前者允许提交前最终确定 descriptor，后者仍要求所指资源内容在执行期间稳定，不等同 `DATA_VOLATILE`，也不允许修改 GPU-live heap。CBV 的数据只在未提交私有槽中修改；提交后 CB／descriptor 全部固定。

部分 composition 录制始终先绑定无历史；所有 pass 和观察者成功建立后，仍在录制锁内才启用候选。任何中途失败留下的命令均不会读取未生产历史，writer identity 保持空；每个已录制 dispatch 的 Owner 保留 CB、heap、PSO、root signature、输入输出 COM 对象及输出 generation，不让未发布的部分 generation 被池复用。

`uncertain` 表示退休证明丢失，`contentUncertain` 表示 writer 内容不可信。reader Signal 失败只隔离退休，不抹掉已知 writer 内容；writer 每次实际提交（包括 replay）先撤销 `produced` 和内容信任，只有成功 completion Signal 才恢复，失败不会留下旧 `produced=true`。所有已知跨队列访问使用 GPU Wait；无 completion 证明的资源保留到设备移除，不超时回收。

代价／边界：reader 先于 writer 提交、writer 被丢弃或无法证明有序时，这次录制永久使用无历史；后续 writer 提交不回写它。仍保留可合法复用的预录制多列表。batch 准入中的真实失败仍可拒绝该原始 batch；可选历史缺失本身不会再制造这种失败。本节描述源码不变量，不是新增 GPU／游戏画质验收；本轮执行检查由集成方统一运行。
