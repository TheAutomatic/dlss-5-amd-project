# 安装器与卸载器

实现：`tools/install-amd-presr.ps1`（包内叫 `Setup.ps1`，由 `Setup.bat` 启动）、`tools/uninstall-amd-presr.ps1`（包内与游戏目录里叫 `Uninstall_OptiScaler_NR.ps1`）、模块包助手 `tools/lmxxf-module-package.ps1`。回归测试见 [tests/RELEASE-TESTS.md](../../tests/RELEASE-TESTS.md)；包的组成见 [release.md](../release.md)。

## 后端选择

安装器检测三类文件，交互时选择本次安装的后端（见 [decisions.md](../decisions.md)）：

| 可装 | 判据 |
|---|---|
| lmxxf | 找到 `LmxxfNrRuntime.dll` 和 `lmxxf-modules/`（权重 `native-game-tiled-assets/` 另外查找） |
| daniel | 找到 SHA256 在白名单内的作者 runtime（`version.dll` 或 `dlssnr_amd_pass1.dll`），或 `dlssnr_on_amd_setup.exe`，或 `dlssnr_on_amd_weights.bin` |
| mochizuki | 找到 `OptiScaler/plugins/dlssnr-amd.asi`、同版 `dlssnr-amd/shaders/`、`dlssnr-amd/aco/` 和 `model-tools/dlssnr_extract_model.exe`；不再要求 `MochizukiNrRuntime.dll`。模型缺失时用官方工具从用户 DLL 提取 |

| 情况 | 行为 |
|---|---|
| 只有一边 | 装那一边，`NrBackend` 写成它 |
| 两边都在，交互 | 三选一：只装 lmxxf / 只装 daniel / 都装（再选默认后端） |
| 两边都在，`-NonInteractive` | 两套都装，`NrBackend=lmxxf`，之后可改 ini |
| 都没有 | 报错退出 |

存在 mochizuki 时可选全部可用后端或其中一个；全部安装后再选活动后端。
`-Backend daniel|lmxxf|mochizuki|all` 可显式指定安装范围，默认 `auto` 保留交互选择。
非交互 `auto/all` 安装全部可用后端，活动后端按 lmxxf、daniel、mochizuki 的顺序选取。
mochizuki 的 NR 由官方 ASI 自己运行：宿主菜单在插件加载时置灰，设置和 NR Control 在 `dlssnr-amd.ini`。安装只检查文件名/布局与提取成功，不设 hash/版本白名单。详见 [mochizuki.md](../mochizuki.md)。

GUI 使用 `backends` 数组明确指定安装范围，经 DecisionJson 交给脚本；`-ActiveBackend`
指定其中的活动后端。省略活动后端时仍按上述顺序选择；空列表、未知或重复后端、列表外的
活动后端均拒绝。参数 `$ActiveBackend` 与内部结果 `$selectedActiveBackend` 分开保存，
避免 PowerShell 变量名不区分大小写导致参数被清空或覆盖。

GUI 首次选择主后端按卡片顺序优先 Mochizuki，再 lmxxf、Daniel；就绪后端会自动勾选。
能力检测完成后统一确定主后端，避免检测结果的返回顺序改变默认值。
用户手动修改勾选项或主后端后，按游戏 ID 保存到启动器的 `settings.json/BackendChoices`，
切换游戏、重新检测和重启均恢复该游戏的选择。缺少文件时保留手选后端并显示提示，
不静默切换；主动取消主后端勾选时才改选其他已勾选项。全部取消也会保存。

GUI 普通模式只使用启动器 EXE 所在目录的完整安装包，不读取历史 `PackageRoot` 作为安装来源，
也不向父目录搜索。设置中的“诊断模式…”须确认后才允许选择其他包；授权仅本次启动有效。
历史目录仅作为诊断候选记忆，关闭模式或重启即恢复普通来源；诊断目录留空也使用当前包。
切换来源会取消旧只读检查并丢弃旧来源结果；安装、卸载等写操作及目标锁持有期间不能切换。
无效或不完整的包给出从完整包运行 Setup.bat 的提示，不向其他目录借文件。
开启后底栏在固定右侧“诊断日志”的左边显示红色“诊断模式”，日志按钮位置保持不变；
指示文字提示显示实际来源与 VERSION。底栏作者 TheAutomatic 为项目主页超链接。

worker 执行所选安装包目录中的安装/卸载脚本，不从旁边另一份包补脚本。能力检测按包内
`lmxxf-modules/`、runtime 与 shader 文件判断 lmxxf 组件；游戏目录仅供复用模型，不能补齐
更新包的 lmxxf 代码。Daniel 检测同时识别安装器、包内 `version.dll` 及已有 pass1 runtime。
这些检测只提示组件存在；runtime 哈希、模块清单及模型检查仍由正式安装流程执行。

后端卡片下方按实际缺失项显示中英文短提示，并显示 worker 实际使用的安装包目录。
lmxxf 分别报告 Original / Small，安装时显式选择 `original/small/both`。
按模型复用、补装及从游戏回存；已有权重目录不再阻止补齐另一套模型。
Original 检查完整张量尺寸，Small 安装前校验固定哈希；缺来源时不将 block0 存在视为完整模型。
Mochizuki 按包内资产清单检查组件，缺模型时识别支持的
`nvngx_dlssnr.dll` SHA256，并区分缺来源、可准备与来源不支持。
Daniel 分别报告缺运行库、权重、安装器和来源 DLL；安装器存在不等同于运行库就绪。
提取材料齐全时提示安装时准备；权重已在游戏中可复用时不再要求重复提供。
完整模型与 Daniel 运行库仍由安装流程校验，检测过程不执行外部 DLL 或安装器。

### 启动器语言

首次启动且没有有效语言偏好时，读取 Windows 显示语言（`CurrentUICulture`）：所有 `zh`
区域（包括简体、繁体及港澳台）默认使用现有简体中文界面，其余默认英文；不按数字/日期
区域格式判断，也不启动外部检测进程。结果保存在 `%LOCALAPPDATA%/OptScalerNR/Launcher/settings.json`
的 `Language` 字段（`zh` / `en`）。已有有效偏好优先，手动切换后后续启动不再自动改选。
切换同时刷新游戏库、设置、后端状态和模型对话框，不重置所选代理、后端或 INI 处理方式。
GUI 安装将语言传给 worker，免费提示使用同一语言；原始安装日志及第三方/系统错误保留原文。

Mochizuki ASI 的 `[NRControl] Language` 也跟随启动器：中文写 `zh-CN`，英文写 `English`（均为
`dlssnr-amd/lang` 中实际文件名去 `.txt`，不写 `zh-CN.txt` 或 `en`）。新建 ASI 模板时写入；
已有用户 `dlssnr-amd.ini` 保留显式值（包括显式空值），仅在键缺失时补默认；最终文件同步到
`dlssnr-amd/dlssnr-amd.default.ini`，由安装器完成，不在 GUI 事后双写。

### 启动器外观与状态

原生 WPF 右侧依次为游戏横幅、紧凑后端卡片、七种代理及“配置与运行库兼容诊断”。
小标题使用弱化层级；提取提示统一位于后端卡片下方，技术性缺文件清单折叠在“组件详情”。
模型提取仍使用本项目原生流程：NVIDIA 来源 DLL、Daniel EXE/DLL 的校验与自动提取未替换。
本地图像发现不联网、不按名称跨游戏匹配：Steam 只查 appid 缓存根及一级 32–64 位十六进制哈希子目录，兼容本地化文件名；按 PNG/JPEG 头尺寸区分封面与横幅，排除 blur/placeholder，不按文件体积选图。Xbox 比较当前游戏根/Assets 及受限 manifest 视觉资源映射，优先高分辨率方图；仅横向 hero/header/splash 作横幅，Logo 不作横幅。Splash 可能包含发行商品牌，不保证场景截图。每游戏最多 192 条目、24 目录、64 图片、150ms，整轮图像预算 1000ms；不跟随链接。图像不超过 32 MiB、8192 边长、3200 万像素，JPEG 头搜索限 64 KiB/256 段；超额、缺失及损坏文件跳过。Epic/GOG 无已验证本地美术映射时保留缺图，不伪造横幅；Manual 不模糊匹配其他游戏。

列表统一使用 42×42 DIP 真圆角裁剪：EXE/方图保持完整比例，已识别竖封面居中裁切。Windows Shell 按设备像素请求本地多分辨率图标，后台单队列解码，最多缓存 192 项；每个当前界面只保留一个可替换的惰性批次游标，刷新丢弃旧批次，不按图片数量饱和后永久丢图。轮流服务游戏列表、当前横幅和启动程序弹窗，每次仅保留一个解码及一个 dispatcher 回调，释放 HBITMAP/COM，不将低分辨率资源宣传为高清。横幅只解码实际宽图（至少 320×160、宽高比至少 1.5），缺失时使用设计背景，不把 EXE 或竖海报铺满。旧 cover/hero/customCover 字段继续可读，自选图片仍优先；自选竖图用于列表而非拉伸横幅。

后台对已安装游戏的记录代理和当前安装包内 OptiScaler.dll 做 SHA256 比较；列表显示黄色“版本不同 / Version differs”，横幅显示“已安装 · 版本不同”，技术说明放在提示中：此处只表示与当前选择的安装包文件不同，**不表示已安装版本较旧**。横幅只保留一个安装主操作：未安装时为“安装 OptScaler NR”；已安装且与当前包哈希相同时为“重新安装 / 应用更改”；哈希不同时为“更新 OptScaler NR”。黄色差异信息为不可点击状态，不再提供第二个更新按钮。主操作继续走现有 install/update 决策与安全覆盖流程，不先卸载；“覆盖现有 INI”未勾选时保留当前 INI，后端、模型、代理选择与写入锁不变。异步哈希比较完成或语言切换时刷新同一主操作，不自动应用更改。配置区的“应用”仍为独立 INI 写入动作。
包或游戏路径变化及写操作会使旧比较失效，后台结果不切换游戏或重新执行检测。检查请求保存发起时的安装包选择；检查期间切换目录后，旧结果不能覆盖新目录的能力或版本比较。显式刷新游戏库和重新检查会重新读取文件哈希，识别同路径、同大小甚至同时间戳的原地 DLL 替换；普通后台状态更新仍按路径集合去重。

顶栏搜索在 112–190 DIP 范围让位；中英文退款警示均为固定 220 DIP、不省略文本。游戏库标题显示当前可见数量；右侧细型下拉以稳定 ID 单选全部/已安装/未安装或 Steam/Xbox/Epic/Manual/实际其他平台，搜索与该选择叠加，切语言保留筛选，空结果不更改当前目标。“添加游戏”下拉保留单 EXE 与文件夹两种语义。
设置是覆盖工作区底部的抽屉，不替换游戏库；底栏不重复提供已有的更换 EXE/目录动作。
外观选择写入 `settings.json` 的 `Theme`（`auto` / `light` / `dark`），默认跟随 Windows
应用主题；加载旧设置不为补齐主题字段重写文件。共享语义画刷实时更新已有主界面与对话框，
不重新检测或重置手选项。新增源码需同时列入 csproj 与 build-launcher.ps1 的构建清单。

按钮共用细边、轻渐变材质与独立键盘焦点层；Hero 次动作使用暗色半透明材质，代理选中为暗渐变、红点、白字而非红色外环。辅助资料为 11 DIP，DLL 名使用 Consolas；主面只显示随包/缓存资料，审核日期在提示中，实际规则来源仍可见。后端卡片目标 90 DIP（英文长说明允许内容驱动增高），原有 GPU 适用描述保留；右上角重新检查仅调用 InspectGame 检查后端/模型来源，不下载或提取，提示仍位于卡片下方。

版本胶囊固定为 90×26 DIP，alpha 与正式版本号均居中，不因移除后缀缩窄。游戏库筛选采用有边线的轻量按钮与明确可见、不可选中的分隔线。横幅主操作垂直居中，三个次操作底部对齐并与主操作分开；底栏状态、分隔线和硬件文字共用居中行。设置的“关于与社区”保留项目与 Bilibili 链接，另列 Mochizuki、lmxxf、Daniel 三个后端的作者仓库入口。

顶栏“设置 / Settings”固定 76 DIP 宽（12 DIP 原字号、左右各 9 DIP 内边距），切语言不缩窄。最小化、最大化/还原和关闭均使用 32×32 DIP 的实际命中与 hover 区，置于 60 DIP 标题栏垂直中央；关闭红色只填充该方形。保留 WindowChrome 点击命中、原生窗口操作和独立键盘焦点指示，不把整个标题栏染红。


启动测量从 `Program.Main` 首条语句到 WPF Loaded 时记录，避免静态字段惰性初始化产生接近零的伪时间；不代表外部进程创建到首帧的冷启动。`test_launcher_startup.py` 真实执行一次首样本和十次重复启动，仅记录目标差距，不以慢机器跳过记录。

底栏在窗口加载后后台读取物理 GPU，排除 GameViewer、UU、远程和虚拟显示适配器；
只展示一个优先独显，不声称它就是游戏实际渲染适配器。可靠的 RadeonSoftwareVersion
可标为 Adrenalin；Windows 驱动构建号只放在提示详情，不与软件版本混排。
探测不阻塞首次交互、不覆盖业务 StatusText，窗口关闭后不更新 UI。

### 共享 INI 配置

第四卡片支持保存当前游戏 INI、快速保存、导入、搜索/选择、重命名和删除共享配置。
生产库位于 `%LOCALAPPDATA%/OptScalerNR/ini-profiles`，每游戏选择和插件偏好位于
启动器设置目录的 `ini-profiles.json`；测试注入设置目录时库也留在该目录内。
同名保存/导入生成独立名称；重命名修复所有游戏引用，删除清除引用但不改游戏 INI。
共享配置可跨游戏使用；切换游戏同步清空旧选择再恢复本游戏偏好，不沿用上一游戏的目标。

选择配置和第三方插件开关只是待应用状态。“当前游戏 INI”选项仅更改 `[Plugins] LoadAsiPlugins`。
首次读取尊重当前游戏显式 true/false；`auto`、缺少配置项或缺少 INI 均沿用原生运行时默认 false，不因应用共享配置而擅自启用 ASI。已有明确保存的用户偏好优先；只有点击“应用”才写入。应用捕获游戏身份与选项，
与安装/卸载共用目标互斥锁、忙碌和关闭保护。原内容备份为 `OptiScaler.ini.bak-时间戳`，
同卷暂存后用原子替换提交；失败不回退到删除/覆盖写入。保留编码/BOM、换行及非插件配置。
此面板不绕过模型验证、安装决策或兼容修复回滚流程。

### 游戏库扫描

Xbox 游戏优先读取 `Content/MicrosoftGame.Config` 中存在的 PC 游戏 EXE；
`appxmanifest.xml` 可能只指向 `GameLaunchHelper.exe`，不作为实际游戏程序的依据。
配置缺失、无效或入口已不存在时，回退到限定深度与扫描数量的候选搜索。
排除安装器、崩溃工具、Xbox 启动辅助程序和本包的人物识别 worker；跳过本包组件及常见运行库目录。
回退搜索在排序后才截取前八个候选，优先 GDK/Win64 Shipping 程序。
启发式搜索仍不能保证识别所有游戏的渲染进程，可通过“更换 EXE”指定。
同目录选错程序虽然通常不改变文件安装位置，仍会影响基于 EXE 导入表的代理推荐。

扫描按游戏 ID 合并缓存，并消除旧缓存中同一游戏的重复项；自动扫描的 EXE 可随检测结果更新。
手动添加或“更换 EXE”会保存 `exeOverride` 标记，后续扫描保留该路径。
旧缓存没有此标记时，辅助程序或失效路径可自动纠正；来源不明但仍有效的游戏程序保守保留。
所选项迁移后刷新详情并重新检查代理推荐，不沿用旧 EXE 的检测结果。

“更换 EXE”始终从当前游戏 EXE 所在目录打开。左侧游戏条目的右键菜单提供“从列表移除”，
只移除对应库条目；右键其他游戏不会切换当前详情或启动其自动检查。
不卸载或删除游戏文件；移除记录按 ID/EXE 保存在 GUI 设置中，自动刷新与重启不重新加入。
主动“添加游戏”选择同一路径可重新加入。旧缓存仍按游戏 ID/EXE 去重，安装失败不添加条目。

“添加文件夹”和拖入目录允许先保存没有 EXE 的游戏，使用稳定条目 ID，根目录与所选 EXE 分开保存。
未选择 EXE 时不能安装，可编辑信息、打开目录、移除或扫描启动程序。
“编辑信息”的显示名称、PNG/JPEG 封面独立于游戏身份；封面保存受控副本到
`%LOCALAPPDATA%/OptScalerNR/Launcher/user-assets/`，不依赖原图片位置。
“扫描启动程序”后台读取 PE 的直接/延迟导入及有界本地依赖，不执行 EXE，不跟随目录链接。
扫描限时 10 秒、深度 8、最多 256 个 EXE 和 10000 个目录；到限展示部分结果。
用户在结果弹窗确认程序，未识别 API 的候选通过“显示全部 EXE”查看；仅允许选择 x64。
图形 API 证据用于排序，不保证程序身份或注入成功。确认后保留原条目的 ID、根目录和自定义信息。

代理支持 `dxgi.dll`、`winmm.dll`、`d3d12.dll`、`winhttp.dll`、`wininet.dll`、`dbghelp.dll`、`version.dll`。
推荐依次采用用户手选、精确游戏规则、已安装项目代理及 PE 导入候选；无证据时要求人工选择。
手选按游戏保存，可点击“使用推荐”清除该覆盖。游戏规则来自审阅后的
`assets/compatibility/proxy-profiles.json`，内嵌于启动器并随包提供，保留 Wiki 来源和版本限制。
启动立即采用本地资料，后台一次请求共享 10 秒总期限，载荷上限 256 KiB；失败保留旧资料。
有效缓存位于 `%LOCALAPPDATA%/OptScalerNR/Launcher/compatibility/`，不会因重新打开启动器清空。
维护入口和资料更新规则见 [推荐资料说明](../../assets/compatibility/README.md)。
上游 Wiki 记录不等同于本 fork 的 NR 实机验证。所有自建弹窗使用共享动态主题，系统选择器保留系统实现。

用户勾选的 Mochizuki/lmxxf 缺少模型且无提取来源时，GUI 在安装前弹出本地
`nvngx_dlssnr.dll` 选择框，重新检查所选来源后再继续模型选择。来源路径经 DecisionJson
传给安装器，离线提取直接读取所选文件；不要求用户手动剪切 DLL，也不把来源缺失当成
运行库安装成功。Small 仍需受支持的 310.9.16 来源，完整 Original 可直接复用。

GUI 的 worker 进程标准输出和错误分别保存为 `worker.stdout.log` / `worker.stderr.log`，
包含脚本解析和启动错误。安装脚本的标准输出、错误都会合入事件日志；写操作失败时
显示错误与日志路径，worker 提前退出仍能通过“打开完整日志”定位原始错误。

### 用户 ASI 插件

安装后检查解析出的插件目录，包括本次复制和原有保留的用户 ASI。
存在自定义 ASI 且 `[Plugins] LoadAsiPlugins` 缺失或为 `auto` 时启用加载，
避免替换 INI 后插件文件仍在但不再加载。显式 `true` / `false` 保持原值；
插件自己的配置文件也保留。OptiPatcher 由其独立安装选择处理：控制台交互保留原提示流程（60 秒时限）；
启动器通过独立复选框发送决策 `optiPatcher=install` 或 `skip`，默认按所选游戏的 Wiki 推荐预选，手动选择按游戏持久化；勾选后走同一安装内核，30 秒时限，下载失败回退包内离线副本并
自动开启 `LoadAsiPlugins=true`。CI 打包前 best-effort 拉取最新 rolling 版并做 PE（MZ/签名/AMD64/尺寸）
校验，通过才替换 `assets/plugins/OptiPatcher.asi`；本地打包永不联网。

daniel runtime 白名单覆盖 0.3.0 / 0.3.1 / 0.3.2 / 0.3.3 / 0.4.0 / 0.4.1 / 0.4.2 / 0.4.3 / 0.5.0 的完整 SHA256（`$expectedAuthor`），与 `AmdLayout.h` 的 `kAmd*` 行对应；未知 SHA 一律拒绝（fail closed）。加新版本时两处一起改，并验证「同尺寸改 1 字节」的文件会被拒。

## 安装顺序与权重分支（2026-10-15）

1. 选择游戏目录，检查写权限、运行中的游戏及文件锁。
2. 如有旧安装，选择先卸载或覆盖；这里只记录选择，不立即卸载。选择代理 DLL。
3. 检查包内组件、探测已有模型，选择安装后端；选择多个时再选活动后端。
4. 只校验、准备所选后端的权重。未选 Mochizuki 不加载其 INT4 助手、不运行转换器、不检查 NVIDIA DLL 的 Mochizuki 版本要求。
5. 完成所选包的校验及权重准备后，保存重装所需来源，执行已选择的卸载，然后处理代理冲突、复制组件及 INI。
6. 报告实际安装结果；缺少活动后端模型时明确显示 `MODEL SETUP REQUIRED`。已有游戏模型仍可缓存到安装包供下次使用，不覆盖包内现有模型。

| 后端 / 权重情况 | 行为 |
|---|---|
> 以下旧内置 runtime 的模型准备细则只保留作历史安装与升级清理参考。新安装走官方 ASI：
> 模型由 `dlssnr_extract_model.exe`/`dlssnr-int4-weights.exe` 生成，只检查文件名、布局与
> 提取成功，不要求 Python、hash 或特定 NVIDIA DLL 版本。

| Mochizuki：已有模型 | 优先尝试游戏目录，再尝试安装包；文件头识别后，用当前转换器生成 INT4 临时文件并校验固定哈希。不需要 Python，也不检查无关 NVIDIA DLL |
| Mochizuki：已有匹配 INT4 | 仍检查当前基础模型能否生成匹配 INT4；成功后保留现有 INT4，不重复覆盖。准备 INT4 不等于默认开启 INT4 |
| Mochizuki：游戏模型不匹配、包内模型匹配 | 使用匹配模型；安装时将原游戏模型另存为 `.mismatch-<时间>`，不直接丢弃 |
| Mochizuki：所有已识别模型均不匹配 | 不安装该后端；只选它则失败且不替换游戏文件。选全部时跳过它，继续安装其余通过检查的后端，必要时调整活动后端 |
| Mochizuki：无可用模型，有 310.8.0 DLL | 交互询问“提取并安装”或“只装运行库”；非交互默认提取。提取成功后再生成并校验 INT4，之后安装 |
| Mochizuki：无模型、DLL 缺少或版本不支持 | 不提取；可安装运行库，明确提示模型待准备 |
| Mochizuki：拒绝提取、Python 缺失或提取失败 | 不假报模型就绪；继续安装运行库并提示后续准备方法 |
| Mochizuki：已有坏文件头模型 | 提示并保留。若已有匹配的包内模型可替换，安装时备份旧游戏模型；提取目标已有坏文件则不自动覆盖，提示先移走 |
| lmxxf：有有效 Original / Small 权重 | 按所选模型分别复用与补装；Original 为尺寸检查，Small 为固定哈希检查 |
| lmxxf：Small 缺少，但有受支持的 Small 包或 310.9.16 DLL | 选择 Small 时先离线转换并校验，再安装；不要求同时安装 Mochizuki |
| lmxxf：显式所选模型缺少或损坏 | 写入游戏前报错；未显式选模型的非交互 CLI 在完全无模型时仍允许只安装组件，并明确提示待准备 |
| Daniel：已有受支持 runtime 和权重 | 校验 runtime 白名单与权重大小后复用 |
| Daniel：缺 runtime 或权重，有官方安装器 | 只在选中 Daniel 后启动官方安装器；完成后重新查找。仍缺组件则停止；不因模型文件存在就认定 GPU 兼容 |
| Daniel：已有权重明显截断 | 跳过该后端；若没有其他选中且可用后端则停止 |

包缺文件属于损坏安装包，不等同于用户尚未准备权重。前者要求重新解压完整包；后者按上表处理。

## 旧安装的升级

检测到已有 OptiScaler（代理 DLL 被识别为 OptiScaler，或存在 `amd-presr-install.txt` 安装记录）时，在选完目录后立即问一次：

| 选项 | 行为 |
|---|---|
| Y（推荐） | 新包校验通过、需要复用的来源文件先保存后，自动调用**新包**的卸载器，成功后继续安装。权重和已有备份保留，OptiScaler 设置重置。卸载失败则停止安装 |
| N | 覆盖安装。旧的扁平模块目录也能升级（见下） |
| `-NonInteractive` | 默认覆盖；显式加 `-UninstallExisting` 才先卸载 |

## XeSS 兼容检查、修复与恢复

`Setup.bat` 启动的现有 GUI 在选择游戏后自动对比 EXE 同目录与所选安装包
`OptiScaler/` 内的四个 XeSS DLL，显示版本和内容差异；无版本信息时仍按 SHA256
区分文件。版本不同或缺失不等于已确认故障，不自动替换游戏 DLL。

游戏出现 XeSS 不可用、插帧倍率受限或启动异常时，在游戏详情点击“兼容修复”：
预览具体文件，显式勾选 XeSS 超分（DX12/Vulkan）、XeSS 超分（DX11）或 XeSS 插帧。
插帧固定将 `libxess_fg.dll` 与 `libxell.dll` 配套处理。包内配套文件缺失或不是 x64 PE
时拒绝该组；检查只读元数据，不执行 DLL。修复不需要重新选择 NR 后端、模型或代理。
日志仅显示最近文件中相关的历史记录，不据此自动宣称查明本次原因。

关闭游戏后点“备份并尝试修复”，先完整保存原件及 SHA256，再暂存新文件，持独占文件
句柄校验并移走目标，最后以不覆盖现有文件的移动放入新文件。并发更新的新文件不会被
替换；中途退出留下的受管移动记录可用于恢复。内层 DLL 和用户 INI 保留。文件占用或路径带链接
时停止。失败时尝试回滚；中断或回滚未完成的记录仍可从同一 GUI 恢复。

“恢复游戏原文件”仅恢复这次修复记录的文件，并移除该次新增的文件。开始恢复前检查
全部备份与当前文件；若游戏更新或用户修改了其中任一文件，停止恢复，保留当前文件
和备份，不以旧文件覆盖新文件。只有当前文件与修复时/修复前哈希匹配时才恢复。
恢复操作校验预览指纹及修复记录身份，拒绝操作预览之后出现的另一批修复文件。
若游戏更新导致无法恢复，可在恢复窗口选择“保留当前文件并结束修复”：不修改任何 DLL，
归档旧记录并保留全部备份，然后允许再次修复。备份和记录位于游戏目录 `OptiScaler-compat-backup/`，
已恢复的旧备份也保留；卸载不会删除这些备份，也不会自动撤销独立的兼容修复。
卸载后仍可通过完整安装包的 GUI 选择游戏并恢复。

宿主 Ins 菜单的“兼容性”和“状态”页面显示 OptiScaler 实际使用的 XeSS DLL 完整路径
及文件版本，并指向上述入口。未使用的组件显示未加载，不作为故障判断。
`[Libraries]` 的 `XeSSPath`、`XeFGPath`、`XeLLPath` 分别绑定独立配置字段；
库覆盖相对路径与 `OptiDllPath` 一样以游戏 EXE 所在目录为基准，失败日志记录具体路径
和 Windows 错误。修复后的实际游戏兼容性仍需启动游戏验证。

实现：`tools/install/xess-compat.ps1`，由 `launcher-worker.ps1` 调用并随完整包分发。
无 GPU 专项：`tests/install/xess-compat.ps1`，已接入安装领域 `run.cmd`；
路径绑定及相对覆盖路径回归在 `tests/host/test_config_priority.py`。

## `OptiScaler.ini`

- 游戏目录没有 ini：放入包内文件。
- 已有 ini：第一项是 `Overwrite OptiScaler.ini (Recommended: major version update!)`，第二项保留旧 ini。`-NonInteractive` 走覆盖。
- 不论覆盖还是保留，之后都改写 `[DlssNr]`：

| 类别 | 键 |
|---|---|
| 强制写入（本次安装的选择） | `Enabled=true`、`RunBeforeSR=true`、`NrBackend=<本次选择>`、`LmxxfDiagnostic=off` |
| 只在缺失时补（用户偏好） | `DLSS5_FIT_LARGE`、`DLSS5_HIP_PDL`：优先迁移对应旧键的值，否则补 `true`；其余补 `LmxxfPaperWhite=1`、`AmdModelScale=1`、`AmdEncoding=0`、`AmdEveryFrame=true` |

包内模板里的 `NrBackend=lmxxf` 只是占位，安装器会改掉。程序在键缺失时的默认值与包内值不同：`Enabled` 与 `RunBeforeSR` 缺失时为 false，`NrBackend` 缺失时为 `daniel`。FitLarge 和 PDL 默认均为 true，正式键名由 `ConfigKeys.h` 的 `CfgKey::FitLarge` / `CfgKey::Pdl` 定义。`LmxxfFitLarge` / `LmxxfPdl` 仅供读取旧配置；新键已有有效值时优先，保存设置写正式键名。

## `DLSS5-AMD/native-game-flags.txt`

安装 lmxxf 时，安装器保留已有 flags 文件；文件不存在时只创建带说明和示例注释的空模板，不把 ini 设置复制进去。卸载器兼容旧安装留下的 `DLSS5_FIT_LARGE=` 行：移除这一行，保留其它内容；没有其它内容时才删除文件。

配置优先级：Ins 菜单 / `OptiScaler.ini` 优先；flags 文件和外部 `DLSS5_*` 环境变量只补宿主未设置的键；最后才用编译默认值。宿主通过 `CfgKey::PutEnvAlias` 传递设置，runtime 的 `ApplyFlagsFileFallback` 不覆盖这些选择。此前安装器把 FitLarge 强制写回 flags 的行为已移除。

## lmxxf 模块包校验（双架构）

包内 `lmxxf-modules/` 必须是双架构布局，`tools/lmxxf-module-package.ps1` 在改动游戏目录**之前**完成全部校验：

- 恰好 `gfx1200/` 与 `gfx1201/` 两个架构目录，不允许其它 `gfx*`；根目录不允许旧的 `modules.json`。
- 每个架构包含契约规定的已知模块；数量以 [发版模块契约](../release.md#模块数量契约追-lmxxf--增删-hsaco-时最容易漏) 为准。根 `SHA256SUMS` 与各叶子 `SHA256SUMS` 一致，且与实际文件哈希一致；`modules.json` 与 `runtime-manifest.json`（schema、ABI、targets、计数）一致。
- 覆盖升级时先生成并验证候选目录，再把旧目录临时改名为 `.lmxxf-previous-*`，最后切换；失败时恢复旧目录，成功后删除临时旧目录。普通用户文件留在新目录；不符合模块包布局的用户自加 `.hsaco` 或其它架构内容，单独保存在 `backup-amd-presr-*/lmxxf-modules`，不夹带整套旧模块。这只保证模块切换，不代表整套安装具备事务回滚。
- runtime 自身在读清单和模块前也检查根目录与每级路径的 reparse 属性，拒绝 junction 和 symlink。

## 卸载器

- 对自己所在目录（游戏目录）执行，也可传 `-GameDir`。
- 先问是否保留 `backup-amd-presr-*` 和原版后端的 `dlssnr-amd-backup`（`-RemoveBackups` 可在非交互时清理），再列出计划删除的文件，Y/N 确认。选择清理备份也保留其中的模型文件；仅剩模型时目录仍会保留。
- 只删明确依赖：识别为 OptiScaler 的代理、本项目的 pass/配置/日志、清单记录的依赖、受控名称的模块。不按扩展名清扫，不递归删整个 OptiScaler 目录。
- 保留：`nvngx_dlssnr.dll`、两类权重、作者的 setup、其它代理、用户插件和未知文件。`_storage_`（商店版游戏的作者日志位置）按同样规则处理。
- 日志清理同时识别 `OptiScaler.1.log`（spdlog 当前格式）和 `OptiScaler.log.1`，只接受数字轮转后缀；不匹配用户命名的笔记或其它模组日志。
- 清理受控 lmxxf 模块对应的 `.generated.hip` / `.hsaco.s` 旧构建附件，以及旧包的四份 `OptiScaler/dlssnr` 说明文件；未知模块、源码和用户文档保留。
- mochizuki 按 `dlssnr-amd-install.txt` 删除本次安装的 ASI、probe 与资源文件；旧内置 runtime 的文件只按 `mochizuki-owned.txt` 清理。模型与 `dlssnr-amd.ini` 保留。
- `DLSS5-AMD/native-game-flags.txt` 若仅剩安装器生成的三行示例注释则删除；存在用户配置或其它注释时保留。
- Daniel/Mochi 的 `dlssnr_on_amd`、`dlssnr-amd` 日志和旧 INI、`dlssnr-amd-crash.dmp`、`dlssnr-amd-install.txt` 一起清理。旧安装记录只用于识别 `L` 行中安全的顶层 `*_dxgi/d3d11/d3d9.log` 和 `vkd3d-proton.cache[.write]`；不执行其 DLL、目录、备份恢复操作，游戏自己的 `logs/` 不整目录清扫。
- 覆盖和卸载都清理旧独立 NR 的 `dlssnr_core.dll`、`dlss-enabler.log` 及数字轮转日志，覆盖前先检查占用；游戏根目录与 `_storage_` 均覆盖。保留 DLSS Enabler 的 DLL/INI、相似名称的用户文件和链接目标，不为这些残留创建备份。
- 本项目正常覆盖直接更新本包文件，不再自动备份旧 DLL/INI/模块，也不创建空备份目录。只有需要单独保护的用户模块、作者 runtime，或用户明确选择移开的其它代理会留一份 `backup-amd-presr-时间`；移开的代理不再重复保存两份。原版后端的 `dlssnr-amd-backup` 属于另一套安装记录，卸载器本身不创建备份。

## 其它规则

### 为下一款游戏保留模型

安装结束时，安装包目录与游戏目录不同且可写，安装器会自动将游戏内已有的模型
补回 `Setup.bat` 所在目录，供下一次安装使用。这一步不受本次所选后端限制。

- Daniel：受支持的作者 runtime 保存为 `version.dll`，另保留 `dlssnr_on_amd_weights.bin`。
- lmxxf：复制 `native-game-tiled-assets/`；至少需要非空的 `block0-ffn.f16` 或 `.f32` 标记。
- Mochizuki：模型按存在且非空判断并复制；不再用头部/哈希作准入，不复制游戏的 pipeline cache。

lmxxf/Mochizuki 仅在目标目录或文件不存在时回存，已有内容（包括不完整或无效副本）
保持原样，不合并或覆盖。新副本先写入临时目录，逐文件核对复制哈希，再移动到正式位置；
不跟随源/目标的符号链接或 junction。复制失败会清理临时副本并显示提示，
不使已经完成的游戏安装失败。模型头部/标记检查不是完整模型验证，运行时仍负责加载校验。

### 通用约束

- **不往 `WindowsApps` 写。** 路径含 `\WindowsApps\` 时安装器拒绝（ACL/TrustedInstaller）。商店版游戏装到可写的 `Content\` 之类目录。
- 本项目的代理与作者原生的 `version.dll` 注入互斥，不要在同一目录双注入；安装器不在游戏目录留下作者的 `version.dll`，而是复制成 `dlssnr_amd_pass1-3.dll`。
- 命令行代理默认 `dxgi.dll`，交互时可选 `winmm.dll` / `d3d12.dll` / `winhttp.dll` / `wininet.dll` / `dbghelp.dll` / `version.dll`；GUI 按上述证据推荐，无证据不自动选 dxgi。
- 含中文的 `.ps1` 必须是 UTF-8 BOM + CRLF，否则 Windows PowerShell 5.1 按 GBK 解析（见 [dev-environment.md](../dev-environment.md)）。
