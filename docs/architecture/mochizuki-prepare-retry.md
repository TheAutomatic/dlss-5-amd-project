# Mochizuki Prepare 等待与熔断

2026-10-31：修正显存准入等待被宿主误计为永久 Prepare 故障的问题。

runtime 的显存检查拒绝新尺寸时，`PrepareFrame` 返回 `LMXXF_NR_UNAVAILABLE` 和 `insufficient VRAM for ...`。这是分配之前的准入等待；runtime 已按输入 key 使用 `Hold::Refused` 节流，当前间隔为 10 秒。原宿主仍将每帧收到的同一等待信息累加为故障，50 帧后停止调用 Prepare，堵住了 runtime 的预算重查和回到旧尺寸的恢复路径。

宿主通过 `MochizukiPrepareRetry.h` 区分：成功、网络构建等待、输入尺寸稳定等待、显存准入等待、真正失败。前三类等待不累计永久故障，并清除“连续失败”计数；成功也清除计数。显存等待显示 `waiting for VRAM (original frame)`，计时没有新样本时仍正确显示 stale。

只有返回 `UNAVAILABLE` 且原因是已知准入消息，或 `candidate frame buffers failed at ...: insufficient VRAM for ...` 包装后的帧缓冲准入拒绝，才归为显存等待。后一条在网络建成后仍可能发生，由 runtime 的 OOM build hold 重试；不能再被宿主提前熔断。未知错误、格式不支持、错误参数、实际分配异常等继续累计；连续 50 次仍熔断，保护可能泄漏的失败路径。关闭/重新开启会话仍重置状态。不放宽 runtime 的显存预算，不自动降低用户的模型比例、游戏画质或分辨率。

该修正允许预算或尺寸恢复后重新尝试，不保证预算始终为零时也能运行 NR。需要更大网络而预算不足时仍使用原画面；旧网络被保留不等于它能处理所有新尺寸。

测试：`tests/host/run.cmd mochizuki-prepare-retry` 直接验证产品失败门控，包括分别 2000 次网络预算等待和候选帧缓冲预算等待后恢复、永久失败的 50 次上限、等待中断连续失败、构建/尺寸等待和会话重置。源码接线检查为 `tests/host/test_mochizuki_prepare_retry_source.py`。两者接入 host `ci`；本修正不改变 runtime ABI。
