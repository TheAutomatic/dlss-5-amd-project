# lmxxf Small 模型

lmxxf 菜单的 **Network model** 可选 Original（71块）和 Small（310.9.16，31块）。
默认 Original；Small 是另一套训练权重，实际执行一次网络，画面可能改变。
切换从下一次网络重建生效，没有 F9 热键。Original 的叠层、预测和肤色偏好会保留。

启动器安装 lmxxf 时可选 Original、Small 或两个模型。安装脚本支持
`-LmxxfModels original|small|both`；缺少明确选择的非交互 CLI 按可用来源决定范围。
两套权重分别从安装包和游戏目录检测、复用与补装，不删除未选中的现有模型。
Original 检查 184 个必需张量的尺寸；Small 检查清单、尺寸与 SHA256。
Original 的历史投影行由宿主内置，不要求根目录存在 `post70-history-head`；
Small 的独立历史投影行仍是必需资产。
安装包已有 Original 时仍可从游戏补回 Small，反之亦然；已有有效模型不会被覆盖。

只选 Small 时不要求同时安装 Original；新建或覆盖 INI 默认选 Small。
保留 INI 时保留显式模型偏好，若缺少该偏好需要的模型，会在写入前明确拒绝。
现成 Small 权重位于 `native-game-tiled-assets/small`，复用不需要 Python。
仅勾选 lmxxf 后才显示该后端的状态与提示；“lmxxf Small 未安装（可提取）”不代表 Mochizuki Small 缺失。
仅选择 Original 会直接复用 Original，不提取 Small。
选择 Small 且只有本地 `dlssnr-small.bin` 或受支持 DLL 时，安装器会先转换、校验，
再写入游戏目录 `native-game-tiled-assets/small`。安装器原生转换、校验，不需要 Python，与是否安装 Mochizuki 无关。
后续更新直接复用游戏目录中校验通过的模型；删除旧安装包不影响复用。
没有该包时，可从用户自己提供的受支持 `nvngx_dlssnr.dll`（310.9.16）离线准备：

```powershell
python tools/install/lmxxf-model.py --dll C:/path/to/nvngx_dlssnr.dll --output C:/game/native-game-tiled-assets/small
```

解包安装目录中的工具位于顶层，命令改用 `python lmxxf-model.py ...`。
也可以用 `--pack C:/path/to/dlssnr-small.bin` 代替 `--dll`。
不加载执行 NVIDIA DLL，不下载或随包分发权重。只接受固定受支持版本，
93 项输出必须与固定上游模型哈希完全一致才会写入最终目录。

已有目标目录时需显式加 `--replace`；完整验证后才替换，旧目录保留为
`small.previous-*`。可用 `--verify C:/game/native-game-tiled-assets/small` 检查安装。
准备好后重启游戏，菜单选择 Small。INI 是 `[DlssNr] LmxxfNetworkModel=1`，0为Original。

Small 支持720/900/1080和自由尺寸、Off/Native history。
Temporal Mode2/3或不兼容的高级内核组合会使用Original，菜单显示原因。
缺失/损坏资产同样尝试回退Original；若只安装了 Small，Original 不可用时明确报错，
不会把缺失模型当成可用回退。修复模型后重启。
保留 INI 时安装脚本不会替用户更改显式模型偏好。来源和验证边界见[消费审阅](lmxxf-044-consumer-review.md)
及[后续冷启动修复审阅](lmxxf-044-hotfix-consumer-review.md)。
