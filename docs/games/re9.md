# 生化危机 9（RE9）

用户向兼容说明。代码里不按游戏名判断，判据是输入格式和曝光是否存在。

| 项 | 内容 |
|---|---|
| 症状（已修） | lmxxf 下 NR 一直不生效，日志每帧只有一句 `codec unverified input format/geometry`，并且之后再也不恢复 |
| 根因 | 1. 场景色是 `R9G9B9E5_SHAREDEXP`：codec 支持它，但需要走私有 FP16 输出，之前没打开。2. 契约检查失败时抛了异常，session 被毒死（`GuardSession` 把它标为 failed） |
| 修复 | 输入是 RGB9E5 时切换到私有 FP16 输出，其他格式不受影响。契约违反改为返回可重试的 `INVALID_ARGUMENT`，并在日志里写明是哪一项。HDR（`IsHdr: true`）需要的曝光经 C ABI 传给 codec |
| 验证 | 只做了离线 GPU 哈希：`--rgb9e5` 无曝光 `150b4076be736fb6`，有曝光 `109a3dba1d8cf06d`（最后记录的值） |
| 现状 | 实机验证等网友反馈，不挡发版。有具体问题走小版本 |
| 排查 | 日志里 `fmt= WxH ... fitLarge=` 这一行会直接说明被拒原因；超过准入几何时报 `outside admitted geometry` |
