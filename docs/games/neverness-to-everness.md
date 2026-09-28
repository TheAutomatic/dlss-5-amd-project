# 异环（Neverness to Everness）

用户向兼容说明。代码里不按游戏名判断。

| 项 | 内容 |
|---|---|
| 引擎 | UE5 |
| 症状（已修，1.9.0.3） | lmxxf 下 session 失效后崩溃，日志出现 `QueueContract: targetQueue != sessionQueue` |
| 根因 | 视口渲染队列和 swapchain 呈现队列不是同一个，之前绑到了错误的队列 |
| 修复 | 在首帧锁定真正执行 DLSS-NR 命令列表的 Direct 队列。队列不一致时，零输出回退会显示原图，不崩（上游 PR #9 的内容，已被上游合并） |
| 相关 | UE5 在 swapchain 之前创建的 list 会被提前包装（Unreal 在默认白名单里）；已完成的 Query 不再否决切分 |
