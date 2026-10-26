## 2026-10-23 — 启动器按钮"白边"事故:WPF 运行时 XAML 的模板静默回退(已修)

**给以后看的人(说人话版)**:启动器所有界面是运行时从嵌入资源读 XAML 字符串搭起来的
(不是常规的编译期 XAML)。按钮样式(MainWindow.xaml)和按钮模板(DialogTheme.xaml)
分在两个文件里,样式里用 DynamicResource 跨文件引模板。这条链在"样式先解析、模板后合并"
时解析不到,而 WPF 找不到模板时**不报错、直接套 Windows 默认按钮皮肤**——默认皮肤自带
一圈内侧白色高光,就是用户看到的"红按钮里一圈白边"。弹窗按钮没白边,因为它的样式和模板
在同一个文件里、用 StaticResource(加载当场解析,找不到会直接崩,崩了我们反而能发现)。

**本次处置**(收口后两层,全部已进 main):
1. ~~分享按钮用代码构建的极简模板~~(过渡方案,已删):它抹掉了灰按钮的 1px 描边导致
   "晒出"按钮看起来没框——根因修通后此保底成为回退源,探针验证新链路(元素级
   DynamicResource 样式 → DialogTheme 内 StaticResource 模板)渲染无白环后移除。
   dll 药丸(dxgi/d3d12/winmm)一直没这个病的原因:它们的模板内联在 MainWindow.xaml
   自己样式里,同文件同次解析,从无跨文件引用。
2. 根治:PrimaryButton/GhostButton/HeroButton 三个样式搬进 DialogTheme.xaml(与模板同
   文件),模板引用改 StaticResource;MainWindow.xaml 里 44 处按钮改用元素级
   DynamicResource 引样式(此模式已被验证可靠)。此前 44 处全部静默套默认皮肤。
3. DialogTheme 在窗口解析前先并入 Application 资源(合并顺序不再是变量)。

**最终元凶补记(2026-10-24,GPT 一轮定位)**:白边是两个病因叠加、症状相同——
(1)模板解析失败静默套默认铬框(样式合并修复);(2)链路修通后剩下的同色 1px 边框
抗锯齿缝(同色 BorderBrush 在饱和填充上留浅缝,BorderThickness=0 修复,含 DialogPrimary)。
修掉(1)才暴露(2),这就是纠缠五轮的原因。两个修复都在位、互相独立、缺一不可。

**长期可选:转标准 BAML 编译**(注:最终元凶与运行时 XAML 架构无关,BAML 也挡不住
同色边框缝——此项已从"值得考虑"降级为纯可选,仅当需要设计器/编译期校验/热重载时):
- 是什么:把 XAML 在编译期编成二进制(BAML)嵌进程序集,资源引用全部静态连接。
- 好处:引用错误在**编译时**报错(这次的坑直接变编译失败,到不了用户屏幕);VS/Rider
  设计器、智能提示、XAML 热重载全套;启动不再逐文件解析文本。
- 代价:构建链要从现在的自定义 csc 脚本(build-launcher.ps1)换成 MSBuild 的 WPF 工程
  (csproj + UseWPF),一次性迁移成本不小。
- 何时做:启动器 XAML 规模再明显增长、或想要设计器/热重载提效时;现在不为做而做。
- 迁移时注意:嵌入资源加载(Program.OpenXaml + XamlReader)全部退役,DialogTheme/Resources()
  的运行时字典机制一并清理。

## 2026-10-23 — 启动器 Mochizuki 安装检测降为纯提示(不再弹窗/移走/拦截)

实机截图反馈:f3ac3f7f 的检测把他安装记录里的惰性文件(LICENSE、OptiScaler.ini、nvngx
备份对、dlssnr-amd\dlssnr-int4.bin——本项目安装器 1231 行复用的同一份模型)全列进"移走"
清单,且记录文件重复列出。降级为只读提示:mochizukiInstall 字段保留(inspect 后 GUI 显示
一行说明),删除 mochizukiInstall 决策、AskMochizukiInstall 对话窗、二次确认、
Move-MochizukiInstall 与 keep 拒绝;安装器不再因他的安装移动或拒绝任何文件,同名代理互踩
回落到通用 foreignProxy 决策。检测范围收窄为可加载的真冲突文件:三个 marker
(dlssnr_core.dll/dlssnr_amd.addon64/OptiScaler.asi,任何 OptiScaler/ASI loader 都会捡起)、
记录中命中本项目候选代理名的 DLL、记录文件本身(清检测标志);只剩惰性残留时不报告。
本项目自身安装(dxgi.dll、mochizuki-owned.txt、dlssnr-amd\dlssnr.bin)永不触发,已钉测试。
uninstall 恢复无条件 retire dlssnr_core.dll(f3ac3f7f 之前的行为)。顺手修两个被
launcher-localization 挡住的旧失败:MainWindow.xaml 硬编码中文 ToolTip(f2668a30,英文
界面显示中文)改为代码内 L() 双语;模型对话框夹具按 2138e127 的 preparable 语义更正。
tests/install test_launcher_worker 27 例 + launcher-localization 571 checks 通过。

## 2026-10-22 — Proton 直连重做:ID3D12DXVKInteropDevice(源码完成、待实机)

取代 bd6d388a 的 gate off。不再找 d3d12.dll 不导出的 vkd3d_get_vk_*,改走 vkd3d-proton 的
ID3D12DXVKInteropDevice(与 mochizuki Linux 路线 nr_pe_interop.cpp 同一接口):GetVulkanHandles 借用设备
(Destroy 不销毁)、GetVulkanResourceInfo 取帧 buffer 的 VkBuffer+offset(不再做内存导入)、每帧提交前
LockCommandQueue(producer)(排空游戏队列,保证 toVk 的 Signal 先上 VkQueue,且持 vkd3d 队列锁),构建走自建
DIRECT 队列 + HostDevice queue_lock。fence 仍是共享 fence 导入(Proton 已证明可行)。入口点:按 c71b0f4 的
结论检查 vulkan-1.dll 是否转发到 winevulkan.dll,nativevulkanloader 时拒绝直连。直连下关闭共享纹理。任何一步
失败都回落自建设备路径,熔断器(85df65da/08293dff)保留。Windows 行为不变(全部以 adoptedVkd3d 门控,仅
Wine 下可能为真)。验收缺口见 PLAN.md"实现(源码完成、待实机)"。

## 2026-10-23 — Proton vkd3d 直连审查修复(方案回退 + 熔断真正生效)

六条审查意见全部属实并修复:P1×4(vkd3d 采用)——d3d12.dll 的 .def 不导出
vkd3d_get_vk_*(函数在 libvkd3d 里,d3d12.def 只有标准 D3D12 接口),采用路径永远
false;即使可达也缺 semaphore 导入初始化、SharedBuffer.buffer 始终为空(D3D12
resource 不自动填充 VkBuffer)、Destroy 会销毁游戏的 instance/device(借用型生命周期
未做)。AdoptVkd3dDevice 整体 gate off(return false + 注释说明四个未解问题),Proton
回退到自建设备路径,熔断器兜底。正确做法需研究 ID3D12Device 扩展接口或 winevulkan
原生句柄,另行立项。P1×5(熔断)——原实现在 >=50 后只 return,下一帧仍会调用
prepare;改为在 prepare 调用前检查 consecutivePrepareFailures>=50 即拦截,
失败计数达到 50 时打一次 ERROR 日志。P2×6(SDK 延迟挂钩)——菜单选择预设/模型时
只补挂了 DriverProvider 模块,漏了 SDK DLL 模块;现在同时补挂两者
(UpscalerModule(0) + LoadedModule()),按各自 IsModuleHooked/幂等保护。BUILD_OK。

## 2026-10-22 — 风格码入口移入 mochi 卡片 + 单弹窗双模式

分享码入口从诊断区底段(可发现性差)移至 mochizuki 卡片右上角"✨ 风格码"胶囊按钮
(与"主后端"同款样式);lmxxf/daniel 卡无此按钮,天然传达"仅 mochi 支持"。弹窗合并为
单窗口 + 顶部(导出/导入)胶囊切换,切换原位换内容(只读/可写、含游戏标识勾选显隐、
按钮文案 复制/预览+应用),两模式逻辑完整复用原 ShowShareCode 两分支。诊断区删分享码
整段,插件行与 INI 面板之间加一条浅分割线(与兼容修复|安装选项之间同款)。LAUNCHER_OK。

## 2026-10-22 — Linux/Proton 双层代理崩溃(command list 二次包装)

Dying Light: The Beast(GE-Proton,vkd3d-proton)开 NR 后游戏提交 command list 即崩
(空指针+0x2A80)。逆向定位(截图分析)+代码核实:链路为游戏调 CreateCommandList → 我们
hkCreateCommandList → o_CreateCommandList → vkd3d 内部调 CreateCommandList1(也被我们
挂钩)→ hkCreateCommandList1 把原生 list 包成代理 A 返回给 vkd3d → vkd3d 把 A 当"真
list"返回 → hkCreateCommandList 再把 A 包成代理 B 给游戏 → 提交时 ExecuteExpanded 展开
B 得到 A(是代理不是原生 list)→ 交给 vkd3d 的 ExecuteCommandLists → 按原生结构读 A 的
内存 → 崩。日志铁证:每条 CreateCommandList 后跟 CreateCommandList1
callerModule=d3d12core.dll。Windows D3D12 的 CreateCommandList 不走公开 CreateCommandList1
入口,故仅 Proton 触发。修复两处:①hkCreateCommandList 调 o_CreateCommandList 时加
SuppressProxyWrap(与 CreateProxiedCommandList:187 同款模式),内层 hkCreateCommandList1
走透传分支;②CommandListProxy::Create/CreateClosed 增加 QI ILogicalCommandList 守卫,
传入对象已是代理时拒绝再包(纵深防御)。BUILD_OK;待 Proton 用户实测。

## 2026-10-22 — 模型源红色导入条(所选后端缺源文件时)

所选后端(mochi/lmxxf/daniel)在能力检测中报"缺源文件"类 hint(mochi-source/lmxxf-weights/
daniel-weights/daniel-runtime 等)且游戏目录与包内都没有可用文件时,后端卡片下方亮起红色
警示条:动态文案指明缺什么(nvngx_dlssnr.dll 对应版本 / Daniel setup exe / 权重 bin),
"导入模型源文件…"按钮打开文件框 → PE 校验(MZ/PE/AMD64/4KB-300MB)→ 复制到启动器包目录
(_packageRoot,对所有游戏持久生效)→ 同名差异询问覆盖 → 自动重新 InspectGame 刷新徽章。
刷新挂点:RefreshInspectionText(检测更新)、后端勾选变化、语言切换。安装时的兜底
PrepareMissingModelSource 保留(红色条未处理/未导入时仍按次弹选),GPU 不支持类状态不
显示导入条(导不导都没救)。新增 MainController.ModelImport.cs(csproj+build-launcher
双清单登记)。LAUNCHER_OK。

## 2026-10-22 — 诊断卡布局重排 + OptiPatcher 锁定插件开关

布局(方案 A,按"作用时机 × 写入粒度"分四区):①安装选项小节标题(ReplaceIni/OptiPatcher,
下次安装生效,与"应用"无关);②第三方插件行(提示改为"随'应用所选配置'写入");③画面风格
分享码独立分区(仅读写风格参数,不替换整文件),置于插件行与共享配置面板之间;④"应用"
按钮移入共享配置面板头部并更名"应用所选配置"——位置即语义,消除其横跨插件行/面板的
歧义;面板标题追加"(整文件)"与分享码分区区分粒度。全部 FindName 引用不变,纯 XAML
移块+文案。另:OptiPatcher 勾选时"加载 ASI 插件"强制勾选并禁用(安装器会写
LoadAsiPlugins=true,可编辑的矛盾状态只会被覆盖或事后写坏),换游戏/默认刷新时同步
重算锁定。启动器放游戏目录自锁问题确认已由 D4 排除修复(用户报障为 alpha2 旧包)。
LAUNCHER_OK,VERSION → 1.13.2-alpha5。

## 2026-10-22 — Linux/Proton 创建 Vulkan 设备崩溃:NR 扩展查询用了悬垂 instance

用户在 Linux(Wine/Proton,vkd3d-proton)开 DlssNr 后每次 vkCreateDevice 崩溃,日志停在
扩展查询前。根因(截图诊断+代码核实):Vulkan_Hooks.cpp:679 查 NR 扩展用的是全局
State::VulkanInstance——vkd3d-proton 为 D3D12 建 instance 时不更新它(:537 有
ScopedCreatingD3DDevice 门),UntrackInstance 销毁时也不清空(:569),它指向的早期 instance
多半已销毁,拿死句柄过 winevulkan 枚举扩展即崩。修复:①扩展查询改用
GetPhysicalDeviceApiVersion 按 physicalDevice 反查的所属 instance(:602 同一 hook 内既有
现成机制),全局变量仅作反查失败(VK_NULL_HANDLE)时的兜底;②UntrackInstance 销毁 instance
时若全局指向它则同步清空,消除同类悬垂。Windows 原生路径(单 instance 常驻)行为不变;
Linux 实机回归待用户验证。

## 2026-10-22 — 能力检测逐模型来源判定二审修复

01c36d2b 的两条审查意见均属实:①单一 $mochiNv 取第一个受信源即停(包目录 310.8.0 遮蔽
游戏目录 310.9.16),smallState 误判 missing,而安装器实际会继续扫描并优先 310.9.16;
②某模型已就绪时早前哈希过滤被整体跳过,originalState 仅凭 mochiNv 非空即 preparable,
不受信源也能启用 Original,安装器必然后置拒绝。修复:providesOriginal/providesSmall
改为独立遍历全部候选源($sourceDlls:ModelSourceDll+包+游戏)逐个验哈希,与安装器
"任一受信源即可、新版本源才能提供 small"的语义对齐,不再依赖单选 mochiNv、也不因
模型已装跳过验证。新增两个回归测试(双源 310.8.0+310.9.16→small preparable;small
已装+不受信源→original missing),worker 全套 25 过,能力脚本解析通过。

## 2026-10-22 — 代码审查四问题修复(P1 能力状态 / P2×3)

审查范围 a2b45d02^..main,四条全部核实属实并修复:P1(我此前 D5 修复不完整的回归)——
mochizuki 能力只发 ready|missing、无 preparable,新 enable 矩阵会把"有受信源但未提取模型"
场景三个选项全禁用;install-capabilities.ps1 现按源哈希区分:任意受信源→originalState
preparable,仅 310.9.16(2bc19bc0…)→smallState preparable,并新增就绪+可提取混合组合与
310.8.0 组合的回归测试(test_launcher_worker 全套 23 过)。P2 三处:release.yml OptiPatcher
刷新步骤结尾显式 exit 0(Actions 的 pwsh 包装会用残留的非零 LASTEXITCODE 退出,导致
best-effort 失败跳过后续打包);菜单 FSR4 预设延迟补挂去掉 IsAnyModuleHooked 全局门
(SDK DLL 启动即被 FfxApi_Proxy 挂钩使恒真,驱动模块补挂被跳过;Hook 本身按模块幂等);
XeLLProxy::BundledVersion 按 LoadProxyLibrary 规则解析 XeLLLibrary(相对路径锚定游戏
EXE、目录取其内 libxell.dll、无效回退 Opti 副本),否则目录/相对配置读到 {0,0,0} 使配对
门限失效。C++ BUILD_OK、YAML 解析通过、能力脚本解析通过。

## 2026-10-22 — DxgiSpoofing"被自动改掉"反馈核验 + Int8 强制点补提示

7 处运行时改写点全部核验:6 处带 `!has_value()` 门(仅 auto 态调整,SoftDefault 显式值
持久化并受尊重,含 nvngx 缺失处——初判"无门"有误,外层整块有门且已有日志);唯一无门
的是 FSR4-Int8 强制开(dllmain:1510,上游故意,Int8 跑 FFX 必须伪装),但零提示覆盖显式
false,观感即 bug。已在该处加提示日志(显式 ini 值保留说明),行为零变化。全景分析与
判别口径记入 docs/upstream-issues.md U6,建议上游跟进提示或改门控。

## 2026-10-22 — Win10 Agility 盲开等五缺陷修复(work/notes 审查报告核验后)

外部审查报告(work/notes/2026-10-22-win10-agility-and-launcher-defects-review.md)五条,
逐行核验:1-4 认可,5 部分认可(报告称前端仅查 PreparationState 不确——对话框另有
OriginalState/SmallState 门控与版本感知提示,但 `|| canExtract` 通道确能在"工具就绪+
源 DLL 仅 310.8.0"时放开 Small,安装后端才报致命错)。修复:D1 Win10 Agility 自动
升级加 `gameAgilityVersion > 0 &&` 下限(无 Agility 导出的游戏返回 0,原 0<615 恒真,
kernelbase Detours 会被内核反作弊击杀;仅 Win10,build<22000);D2
`FsrAgilitySDKUpgrade` 改 `CustomOptional<bool, NoDefault>`(消费点 3 处随之
`.value_or(false)`)——`value_for_config` 会把"值==默认"映射回
auto,显式 false 保存即丢,与 D1 组成无法自救的崩溃循环(同类风险键见
docs/upstream-issues.md U4);D3 INI 注释对齐新语义;D4 安装器进程检测排除自身
(Launcher/提取器/person-worker,包解压进游戏目录时自锁);D5 模型对话框 enable 矩阵
纳入 `preparable`(Small 需 `small || (canExtract && smallPreparable)`)。D1/D2 上游
同款(dllmain.cpp:1160-1168 / Config.h:484),已入上游台账 docs/upstream-issues.md。

## 2026-10-22 — HSR 内核反作弊与 FSR4 驱动补丁（2026-10-25 改为显式 opt-out）

崩坏:星穹铁道(kernel AC HoYoKProtect)上 1.13.2 起开 FSR4 超分,超分初始化与首帧
交接全部成功后约 85ms 进程被外部终止(异常码 0x00000000、模块 unknown、无栈无 dump)。
对照实验:1.13.1-alpha 同游戏同配置(ffx_12)存活 9.5 分钟;差异是 1.13.2 的
`fsr4/FSR4DriverProvider.cpp` 在加载驱动签名 amdxcffx64.dll 时无条件执行
`FSR4Presets::Hook`(.text 页 Detours)与 `FSR4Models::Hook`(provider 虚表改写)。
对内核反作弊进程修改驱动签名模块 = 击杀;与 DX11/DX12 无关(D3D12 游戏同样被钩但无
内核 AC 故存活)。

初版 `e9767600` 用 `Fsr4Preset` / `Fsr4Model.has_value()` 决定是否安装钩子，
但这使 Auto 模式失去 `DetectedModel` 的 INT8 支持绕过/FP8 拒绝，以及 preset 0→1
（render ratio >= 1.49）修正与 active preset 跟踪。2026-10-25 改为 `[FSR]`
`Fsr4DisableDriverHooks`（默认 false）：正常模式无条件安装 preset/model 钩子；
true 同时跳过驱动和 SDK DLL 的两类钩子，日志/菜单明确提示自动策略及显式覆盖不可用。
显式菜单选择尊重 opt-out，不偷偷补挂；false 时保留延迟补挂路径。只读 RTTI 模型列表
和按模块缓存不变。启用/关闭该启动选项需重启；并非全面关闭其它钩子或 AC 安全保证。
原始反作弊游戏复现证据不变，本次不声称已完成游戏实测。

## 2026-10-22 — 启动器恢复 OptiPatcher 推荐安装（下载限时 + 包内回退 + CI 刷新）

启动器（e15c2981 引入 worker 时）把 OptiPatcher 降级为 `optiPatcher='skip'` 且安装器对
GUI 决策硬性拒绝（"GUI installs do not download it in this version"），控制台交互流程
保留。现恢复为推荐注入式：`Decisions` 增加 `optiPatcher`（GUI 默认 `install`，worker
缺省仍 `skip`），安装器抽出共享内核 `Install-OptiPatcherPlugin -TimeoutSec`——拉取
optiscaler/OptiPatcher rolling 版并做 PE 校验，成功装最新并更新包缓存，失败回退包内
离线副本，装后开启 `LoadAsiPlugins=true`；控制台 60s、GUI 决策 30s 时限。
CI（release.yml 打包前）best-effort 拉取最新版并做 PE 校验（MZ/PE 签名/AMD64/尺寸
4KB-20MB），通过才覆盖 `assets/plugins/OptiPatcher.asi` 参与打包，失败保留已提交副本；
本地打包（BUILD_LOCAL_PACKAGE/PACKAGE_RELEASE）固定使用仓库内副本、永不联网，副本
更新只经 CI 或手动。验证：安装脚本解析通过、release.yml YAML 解析通过、PE 校验四例
（真文件/坏 MZ/i386/超小）行为正确、launcher 构建 LAUNCHER_OK。

## 2026-10-22 — 1.13.2 帧生成崩溃回归修复（菜单下转 / XeFG-XeLL 配对）

用户日志（TW3 fsrfg 输出、TW3 xefg 输出、鸣潮 xefg 输出）确认 1.13.2 一开帧生成就崩溃：
切"帧生成"标签页即崩、或 XeFG 从启动就进不去游戏；1.13.1-alpha8 及更早同游戏正常。

根因一：`bdd11179`（M10 迁移）把菜单四处
`reinterpret_cast<IFGFeature_Dx12*>(state.currentFG)`（同类型恒等）改成了对
`state.CurrentFG()` 的下转。`IFGFeature_Dx12` 虚继承 `IFGFeature`，`CurrentFG()`
返回的是经虚基偏移调整后的基类子对象地址，reinterpret_cast 不还原偏移；
`IsUsingUIAny()` 等基类成员调用在错误地址上做运行期虚基调整（MSVC 把 vfptr 当
vbptr 读）→ 野指针。修复与上游 9df512d 逐处对齐（上游无此 bug，是集成时把上游的
访问器和我们旧 cast 拼接出来的）：4034/4089 处用上游的不带 cast 的
`state.CurrentFG()`，5571/5611 处用上游的恒等
`reinterpret_cast<IFGFeature_Dx12*>(state.currentFG)`；这些卡片是 DX12 专属，
Vulkan FG 不经过 menu_common 的这些分支。
`work/scratch/fgtab_downcast_repro.cpp` 实测：基类地址与派生地址相差 0x68 字节，
错误下转的 `IsUsingUIAny()` 读到错误结果（MSVC 上即崩溃）。

根因二：LOW_LATENCY_INPUTS 把全进程 XeLL 调用引流到最先加载的 libxell。带旧
XeSS2 栈的游戏/解锁器（TW3+MFG unlocker：libxell 1.2.1）会把自己的旧 XeLL 上下文
交给 OptiScaler 的 libxess_fg 1.3.1（日志佐证：TW3 "XeFG Log: libxell.dll version
1.2.1" 崩溃，鸣潮 1.3.2 正常），违反 "XeFG and XeLL contexts must come from the
same pair of libraries"。`XeFG_Dx12::CreateSwapchainContext` 现在在
`SetLatencyReduction` 前做配对校验：不配对则不挂 XeLL（XeFG 无延迟加速运行）并
`release_xefg_xell()`，不再把跨版本上下文喂给 XeFG。初版规则是 "XeLL 与 libxess_fg
同 major 且 minor 不小于 XeFG"；【10-22 修订】Intel 按 SDK 整套配对 fg/xell、两者
版本号独立演化（官方 XeSS SDK 3.0.2 即 fg 1.3.4 + xell 1.3.2），该规则会误拦官方
组合。现规则：真实加载的 libxell（`XeLLProxy::Version()`）不得低于捆绑副本
（`XeLLProxy::BundledVersion()`，读 Opti 目录或 XeLLLibrary 覆盖的文件版本资源）；
TW3 的 1.2.1 仍拦、官方跨 minor 组合放行、捆绑文件缺失时放行（回到无门限行为），
六组合在 `work/scratch/xefg_pair_gate_test.cpp` 验证。上游 9df512d 同样缺该校验
（随低延迟切片引入的上游潜在 bug）；alpha8 二进制无 "XeFG paces" 字符串，走的是
fakenvapi 可选路径（失败即跳过、不强配对），故 alpha8 不崩。此门限是本地加固，
值得上报上游。

追加审查（4dbaea06..e3c0c35a 全量、同类缺陷专项）：菜单 `_ffxFGIndex` 两处
（4247/5475）集成时丢了上游的上界保护（上游 4294/5639 有
`|| _ffxFGIndex >= (int) ffxFGVersionNames.size()`），FG 上下文重建使版本表变短时
静态索引越界读 `const char*` → 同一 FG 标签页崩溃面；已按上游原句还原并重编通过。
其余核查干净：所有 `CurrentFG()` 消费点（菜单/Streamline/FrameLimit/input_common）
均无 cast、只调基类接口；`IFeature` 升采样器层级同构但零下转（Vulkan_Hooks 用
dynamic_cast）；FSRFG 回调 `pUserCtx` 恒等往返；`currentVkFG` 类型化直用；
LowLatencyTech 非虚继承，static_cast 合法；XeFGInputs 原生钩子 target 空指针不可达
（挂钩即已解析）；xefg_marker 未知 marker 先拒绝后索引。遗留待观察：XeFG
passthrough 输入（FGInput=XeFG+FGOutput=XeFG）在 LL 构建下把游戏旧 libxess_fg 的
导出重定向到 Opti 的 1.3.1 副本、而其 XeLL 上下文仍落在最先加载的旧 libxell 上
（非 LL 构建是整对交换、正确；LL 构建不是）——同族版本混搭，无现场报告，正确修法
需 XeLL 输入在 passthrough 重定向时改用 Opti 配对，留待上游讨论。

## 2026-10-20 — Narrow Storm input, shortcuts and menu closure

Preserve the existing input implementation and the low-level hook
`CallNextHookEx` repair. Keyboard/mouse menu policy never zeroes other DirectInput
axes or buttons. Actual method targets have bounded stable proxy registrations
for CreateDevice, Release and state/data reads; shared targets attach once and
each proxy calls its own original. Release generations protect address reuse.
Failed detach transactions retain live flags/trampolines. WndProc restore/query
failures and unknown subclass chains retain the forwarding root and refuse unsafe
retargeting. Foreign-thread removal posts tokenized owner-thread work without a
cross-thread send or wait. Pending input-window switches retry independently of
the published game target; cancellation, reinstall and destruction invalidate old
tokens without discarding late-callback forwarding roots. Focus changes preserve
owed releases and defer keyboard/mouse queue draining outside the input state
mutex. No Raw/exclusive mouse compensation added.
Canonical Ctrl/Shift/Alt presses retain the first held side's
original-down modifiers through left/right rollover and fast release sequences.
Dockspace submission requires an active menu frame; a fully hidden no-NewFrame
render path does not call ImGui DockSpace, including its keep-alive mode.

Callback closure refuses active teardown rather than waiting on a callback holding
the input mutex. USER32 restoration/destruction is owner-thread-only. Exact IAT
owner references release after known restoration and after callback gates reopen;
uncertain chains and bounded late-entry proxy identities retain ordinary module
references. No permanent loader PIN is introduced. This safety retention is not
a claim that arbitrary DLL unload is supported. Real CPU Detours and message-only
USER32 nested-callback fixtures exercise dispatch and reentry separately from game
compatibility. The input hook fast path has callback accounting; it is not described
as zero-overhead or a game FPS improvement.

Daniel retains loaded HIP modules as before, but host pass state becomes initialized
only after InitPass completes. A retained half-initialized pass rejects later reuse
instead of enabling it; no retry, extra checker or scheduler is added.

Shortcut/docking/history behavior and the focused CPU/WARP verification entries are
documented in [menu navigation](architecture/menu-navigation.md). Defaults, existing
INI preferences, native extraction, NR algorithms and precision policies remain.

## 2026-10-19 — Mochizuki 0.0.7 剩余能力统一评估

一次覆盖尺寸/格式复用、plan/slot/capacity、activation、heap、residency/offload、pipeline registry、motion point 与 resize history，不再按多轮评估拆分。建议将受限后台 core/权重复用和同 Session 的 split activation 共享纳入一个实现方案；保留当前桶尺寸、失败服务、录制回放和退休框架。不直接采用使旧 runtime stale 的两区域 heap，不为 RAM offload 先增加多闲置网络缓存。全局管线复用保持关闭，采样与历史语义保持当前行为。临时 native 探针验证了实际资源共享、变化输入输出一致、容量拒绝和卸载恢复；不代表 temporal/ACO/游戏验收，尚无产品路径修改。详见 [统一评估](architecture/mochizuki-007-remaining-assessment.md)。

## 2026-10-19 — Mochizuki split 共享颜色按完整现有格式表准入

共享颜色复用 `ColourFormat` 的全部 23 个 DXGI 枚举、14 种 typed 载体，不按游戏名逐个
放行，不增加用户开关。运行时仍查询当前驱动的实际外部导入能力与预算，失败保持 buffer。
颜色字节/编码/alpha 策略沿用现有映射：sRGB 使用 UNORM 同族，TYPELESS 使用既有 typed
代表。BGRA/BGRX 的 carrier DXGI 身份进入缓存匹配，防止同 VkFormat 的不同资源混用。
共享输入仅用于复制与采样，移除无用途的 UAV/storage 要求；不修改核心或放宽几何限制。
离线矩阵自动创建全部格式，两种模型逐帧对照，并验证别名/格式切换、保留录制、跨队列、
旁路及导入拒绝；复用现有检查，不新增跟踪测试项目。实际游戏效果与性能仍单列验收。
详见 [split 数据传输](architecture/mochizuki-split-transport.md#全颜色格式覆盖与验证2026-10-19)。

## 2026-10-19 — Mochizuki 性能优化推进受限 split 共享输入

先优化 Mochizuki 适配器的数据搬运，不重构 lmxxf，也不继续 Inline 调研或修改。
临时共享颜色纹理探针在相同工作量和输出下有约 0.08–0.16 ms 的配对中位收益；
该范围属于不同离线会话，不能当作游戏 FPS 或全部 0.3 ms 桥接差距已回收。
简单跳过上传会改变核心输入与耗时，不能直接当作可用优化或收益上限。

产品已接入受限共享颜色输入：符合准入的 split 默认异步申请共享 texture，就绪后绑定；
不支持、未就绪或预算不足继续 buffer NR。颜色独立所有者贯穿 recording/退休/丢失保留，
回收同时核对实际 Vulkan 完成与 D3D12 尾部；已提交后记账失败不重试。
已核对 image→buffer 旁路/缓存失效、失败退避、独立峰值预算、旧载体释放和受影响现有检查。
最终完整宿主离线 NR+SR 配对中位约省 0.084 ms，1920/1920 次 NR；仍不代表游戏总 FPS。
保留不进菜单的内部 A/B/回滚入口 `MZ_TEST_SHARED_INPUT=0`；详细日志、屏障合并和提前录制不重开优化。
motion 先独立判断，输出最后评估或不做；用户 5% 总 FPS 反馈并行核对实际配置与帧时间。
后续独立 scratch 已支持继续受限 motion 共享输入：Small RG16F 关闭额外阶段标记中位
省约 0.062 ms；变化颜色/非零 motion 在 ACO 与原生编译各自对照通过。保留颜色现状，
不补回私有 image 复制；核心标记增长涉及输入准备/编码和波动，不能称为固定网络退化。
随后已完成受限 motion 产品接入：只在共享颜色可用且完整 RG16F/RGBA16F motion 无
缩放/填充时启用。独立所有者贯穿录制与退休；初始化未完成、导入失败或预算不足保留
共享颜色 + buffer motion，退避到期自动重新申请，不重建网络。完整 Release 宿主正常
日志下 motion 增量中位省 0.044 ms，颜色与 motion 合计 0.180 ms（3240/3240 次 NR）；
原版三路离线对照仍余约 0.107–0.128 ms，不能宣称追回全部差距。内部 motion 单项回滚、
生命周期/完成凭证/DRS 回退及现有检查通过；没有新增测试项目、打包或推送。
证据范围与未验收项见 [split 数据传输](architecture/mochizuki-split-transport.md)。

## 2026-10-19 — 离线代理规则与独立游戏库信息

启动器支持宿主现有的七种系统代理。精确游戏规则采用随包审阅资料，后台更新共享
10 秒期限；断网不阻塞，Wiki 记录不视为 NR 实测。用户手选优先且按条目保存。
手动条目使用稳定 ID，将根目录、可空 EXE、显示名称和受控封面副本分别保存；
文件夹扫描只读取有界 PE 证据，始终由用户确认候选。自建弹窗共用深色主题。
维护与实现约定见[安装器说明](architecture/installer.md)及[推荐资料](../assets/compatibility/README.md)。

## 2026-10-18 — 保留依赖子目录，XeSS 替换通过安装器显式修复

默认维持 `OptiScaler/` 依赖布局，不按 DLL 年龄自动覆盖游戏原文件。
安装器自动显示 XeSS 文件差异，提供明确选组、先备份的兼容修复与同入口恢复；
XeFG 与 XeLL 配套替换，游戏后续更新优先，恢复不覆盖未知新内容。
修复不改变用户 INI 或 NR 后端选择；宿主显示实际加载路径，避免要求用户自行猜测 DLL。
实现及验证入口见[安装器说明](architecture/installer.md#xess-兼容检查修复与恢复)。

## 2026-10-18 — lmxxf 0.44 Small与W16接入

采用固定目标d50cde4b的31块Small网络、独立几何/历史头及decoder/down W16优化。复用Mochizuki离线Small pack与提取流程，转换为lmxxf布局并逐项验证93个SHA；不移植Mochizuki网络算子。模型只通过菜单切换，不引入F9；默认Original，保留其1..3遍偏好。Small不兼容选项/缺损资产/Mode2或3时回退Original并显示原因。模型切换重建录制链，不热改在用权重。80个双架构模块从固定源码重编，RX9070XT各档、往返切换和History专项通过；游戏、长时、gfx1200实机和Small低频适配仍有明确暂缓。详见[lmxxf 0.44消费审阅](lmxxf-044-consumer-review.md)和[模型安装](lmxxf-small-model.md)。没有准备或发布版本包。

## 2026-10-18 — lmxxf 0.44 hotfix跟进，模型仍只经菜单切换

追加接入到固定00a7d047的7提交；采用bridge在producer等待前准备冷上传的修复。
显式禁止F9双模型预热与备用head加载，不调用热切setter。自适应许可属于每条录制链，
时域策略变化通过重建生效，避免前帧执行时Prepare重复设置bridge策略。
内核与配方未变，沿用本日固定源码重建的80模块；详见[hotfix审阅](lmxxf-044-hotfix-consumer-review.md)。

## 2026-10-16 — lmxxf 3a1be68 完整同步（80 模块重编 + Temporal Mode 2/3），回收只剪多余几何

- **决定**：将 `lmxxf` 完整推进至 `3a1be68c35dbac7d7edeb2073723e82076bb1a39`。使用 WSL LLVM 23.1.2 预编译 12 个 wave1/wave2 模块并配合 COMGR 重编全部 80 个 `gfx1200`/`gfx1201` `.hsaco` 模块，使 W16（`HIP_C512_W16=2`、`HIP_C512_FFN_W16=1`、`HIP_C32_W16=1`、`HIP_VIT_W16=7`）、`HIP_VIT_INTER_BYTE=15`、`HIP_BYTE_F_ADD0=1` 与紧凑 QKV 布局在运行时正式生效。同时在调用方录制命令列表上接入 Temporal Mode 2（低频时间稳定）与 Mode 3（单遍残差增强 + 强度 0..2），由宿主统一枚举 `LmxxfTemporalMode`（默认 1 即完整模型历史，Mode 2/3 显式可选）与 `LmxxfTemporalEnhanceStrength`（默认 1.0）管理；Mode 0/1 零低频额外开销，Mode 2/3 仅在 `MultiPass() == 1` 时启用（层数不为 1 时状态设为 `low-freq-requires-mp1` 并跳过历史）。上游已停止的实验与 addon Mode 1 保持不接入。
- **决定**：Mochizuki 闲置录制池只保留当前几何和上一个几何，使用单调生成序号识别最旧几何。`PollReleases` 对共享录制表只扫描一次。
- **不改**：Daniel 回收、lmxxf 共享 import 池、可重放的未 Reset 列表、Mochizuki 核心时间戳。
- **落在**：[消费审阅](lmxxf-042-consumer-review.md)、[开销与回收](2026-10-16-cost-and-retirement.md)、`third_party/lmxxf/upstream-review.json`、`third_party/lmxxf/UPSTREAM.md`、`third_party/lmxxf/sync-state.json`（状态 `reviewed`）。

## 2026-10-16 — 普通 HDR 合成使用非负参考 RGB

**决定与实现**：普通 Mochizuki HDR 合成先将参与亮度比计算的原图参考 RGB 夹到非负，与 OptiScaler 原版 encode/resolve 的参考语义一致。修复提交 `eb64e5a5`，记录在 Mochizuki 补丁 24；Windows/Linux 的 `runtime_transfer.comp` 同步修改，不更新上游 pin，不改用户开关偏好。处理放在 transfer 边界，同时覆盖原尺寸与缩小模型；原始输入和 keep、alpha、SDR、RenoDX、白点、高光保护、网络及历史处理保持原有行为。

**缺陷与路线的区别**：在 OptiScaler 内先编码，或将线性 HDR 交给 runtime 编码，是可以分别成立的架构选择；本次保留 runtime 编码。具体缺陷是旧亮度比校正公式使用可能含负 RGB 的原图参考，失去数值稳定性，将很小的色差放大为极亮的单色输出。负 RGB 本身并非一概非法，问题在于该公式不能可靠处理这类参考值。

**验证与游戏反馈**：有限负 HDR 输入配合有界的模拟网络输出，曾在 RX 9070 XT 上产生约 27 万的单色输出；这不是实际游戏帧或真实网络输出的捕获。`tests/mochizuki/hdr_shader.cpp` 中的负值回归覆盖 126 组原尺寸/缩小模型、白点与合成模式组合，旧 shader 失败，修复后与显式非负参考图一致并通过有限性、幅度和 alpha 检查。包内 shader 的完整 HDR 回归及普通/RenoDX 模式切换冒烟通过。2026-10-16 用户反馈剑星红蓝闪点已消失；配套日志确认加载 `1.12.1.2 / eb64e5a5`、RenoDX 关闭，普通线性 HDR 路径仍在运行。此前 MV 修改和关闭动态分辨率均未改善闪点。数值复现与实机反馈支持本次修复，但尚未捕获游戏出错像素来逐点证明因果，也未完成所有游戏的画质验收。

**可能的回归范围**：

- 参考 RGB 原本逐通道非负的像素不受此次取最大值操作影响。没有设置高光上限，超过 1 的 HDR 值仍可保留。
- HDR/广色域处理中的负 RGB 可以有意义。将合成参考的负通道归零，可能改变相关像素的暗部层次、颜色或色域边界表现；这是理论风险，目前没有观测到该修复引起的画质退化。若要完整保留有符号颜色，应重新设计并验证合成公式，不能直接恢复旧公式。
- 没有新增处理轮次或缓冲，仅增加逐通道取最大值；预期成本很小，但未单独测量帧率差异。

**画质验证边界与后续方法**：普通游玩可能察觉不到细微退化，NR 开/关对比也无法隔离这次改动；旧版存在数值错误，其输出不能直接作为正确画质基准。若继续评估，应由开发侧捕获相同实际 HDR 输入及必要的历史状态，在相同参数下对比修复前后，定位受负值处理影响的像素，并检查颜色、暗部层次及连续帧稳定性。现有回归证明数值缺陷受控，用户反馈证明该游戏闪点消失，二者都不等于所有场景细微画质完全一致；不要求普通用户持续做对照。

## 2026-10-16 — 撤回 pre-SR 运动范围全局清零

撤回 `78cbed3d` 对全部 pre-SR 运动范围清零的处理，恢复按 `MVLowRes` 区分渲染/输出分辨率运动向量。剑星实机反馈该改动未改善红蓝闪点；资源分配尺寸和原版 guide 日志不足以证明运动纹理只有渲染子矩形有效。真正的输出分辨率 MV 仍须保留完整输出范围，不能因 NR 在 SR 之前执行就裁取左上角。后续若有游戏实际 MV 内容/有效范围证据，再独立评估兼容处理。

## 2026-10-15 — 直接 tag 发布，精简默认检查

用户选择直接打 tag 一次完成构建、发布检查、签名打包和自动上传；取消强制预验证出包和跨 run 产物搬运。默认使用 `--tier release`，保留实际 ABI/Shader、关键 host/WARP、安装冒烟及最终包校验；完整安装异常矩阵、开发工具、完整 host/WARP 与上游重放改为手动完整回归。测试不删除，修改相关功能时按需运行。本地优先使用 MSBuild 增量、已有 Mochizuki/Worker 校验复用；lmxxf 仍直接重编，支持 `-Rebuild` 全编。使用 GitHub 原生人物资源缓存，不另建缓存或发布凭证系统。范围见 [发版流程](release.md)。

## 2026-10-14：mochizuki 自己测光，白点成为其裁切

## 2026-10-15 — 暂缓 GPU-armed CPU 输入核验，保留研究代码

用户选择先交付：撤回引入1440p三/四槽约8–12%吞吐回退的CPU watch完成核验，恢复原GPU-armed异步预提交；不更改用户配置、槽位、格式或等待预算。cap/abort后网络仍可能读取未完成输入，输出丢弃与回收时history reset不是安全证明；此风险明确接受并暂缓，不标为已修复。曝光/诊断寿命、实际End完成凭证和PS5出包stderr修复继续保留。完整撤回代码及无效两阶段实验作为不参与构建的文本补丁保存；今后研究须重新验证性能与输入/历史/回收安全。证据、失败方案、研究方向与90分钟/最多2小时止步线见[输入核验暂缓记录](architecture/nr-capture-proof-deferral.md)。本决定不是发布门禁或游戏验收通过声明。

- **决定**：游戏不提供曝光时，mochizuki runtime 自己测线性帧的平均亮度（每帧一次 16x16 的 GPU dispatch，写入 1 个 float，下一帧经录制自己的完成 fence 读回，log 域 0.25 平滑），把均值放到关键亮度 0.2073 上：曝光 `0.2073/mean` 让白点等效于 `4.826*mean`，与 lmxxf 和 AMD 两条 lane 各自的测光一致。宿主仍可用 `MochizukiNrSetExposure` 给曝光，给了就以它为准；`MochizukiWhitePoint` 在两种情况下都是对基准的裁切（参考亮度 0.2073 处等于原值）。
- **理由**：`MochizukiNrFrameInfo` 没有曝光字段，宿主也从不调用 `PFN_MochizukiNrSetExposure`，这条 lane 因而完全没有层级基准。白点被当成绝对值时，网络看到的总量与场景亮度差约 4.8 倍，模型答案崩塌；宿主再按 `NrOverallIntensity` 把残差加回去，皮肤就变红。测光把基准交还给图像本身，手动值只做裁切。
- **边界**：没有可测内容的帧（全黑、非有限样本）不算测量；连续 60 帧没有测量就把 `meteredExposure` 清零并记一行日志，手动白点重新接管。测光自己的绑定不进入游戏录制的绑定快照：宿主用 `ILogicalCommandList::BeginPrivateBindings` 只包住 `RecordInputs`，不覆盖切分点。
- **接管修订（PR #9 整合）**：测光与游戏曝光都改为默认关闭的开关（现合并为单一枚举 `MochizukiWhitePointSource`：0 关闭/1 跟随游戏/2 自行测光/3 自动，默认 0），原本正常的游戏画面不变。游戏曝光贴图经同一套 fence 读回接入（texel × scale / pre，与 lmxxf 编码器一致），无贴图时 `1/pre`；优先级：曝光贴图 > 标量曝光 > 测光 > 手动白点。测光改为 64x64 采样、高光截断均值（min(luma, 16×对数均值) 的算术均值，保留与其他 lane 相同的标定）、1/16 EV 死区、±6 EV 钳位，非线性帧不测。去掉了 PR 中的 ABI N-1 放宽与 SysUtils.h 改动。
- **未验证**：本机只有离线夹具（`tests/mochizuki/runtime.cpp --image`，`run.cmd meter` 断言层级）与 RX 9070 XT 上的运行；游戏内验收与远端 Actions 单独记录。

## 2026-10-13：lmxxf inline 使用私有捕获，槽位即时可选

- `NrInline` 仍默认关闭；lmxxf 勾选后才显示 `LmxxfInlineSlots`，范围 1–5、默认 3。沿用本项目 Daniel 接入层的宿主槽位配置，但不复用其网络资源所有权假设。
- 一份网络与共享计算区；每个录制独占输入快照和结果。全部共享 codec/history/HIP 工作在 COMPUTE side queue 上串行，游戏捕获可提前。缩容保留在途资源，切回 split 先排空，重放重新准入。
- 整包 ABI 升至 6。固定上游 pin 和模块不变，新增 opt-in compute recording 补丁并严格重放验证。
- 离线确认多槽相对单槽有收益，但 split 仍更快；4/5 槽留给用户实测选择，不承诺增加槽数没有成本。方法、数字及游戏验证边界见 [实测记录](lmxxf-inline-slots.md)。
- 同日测量修订：夹具从整批排空改为连续补帧，并移除 paced 五槽的额外等待；撤回首轮“3 槽为不限帧最优点”的判断。实际 2560×1440 下 inline 不限帧 2 槽优先、目标 60Hz 时 3 槽成功处理率较高；游戏选型仍需同会话实测。
- Mochizuki 的 ACO 独立开关不受任何 reset 按钮覆盖，包括已保存的下次启动选项。

## 2026-10-12：外部审查修复的完成证明与发布来源

- 内联失败也必须追加成功的 side tail Signal，并同时等实际 game tail；runtime 的 HIP/Vulkan 回收证书独立保留。未知提交继续持有资源，不周期轮询无法产生的新证据。
- NrInline 保持默认关闭；有限 GPU 工作预算不等于帧时间上界。资源池化先要求 allocation/VRAM 与 replay 寿命证据。
- Release 正文按版本文件维护；已有 Release 的源码身份须在修改正文前核验。所有明确的随包许可是必需项，权重由用户按适用许可准备。
- 实施和专项范围见 [审查核实记录](2026-10-12-review-verification.md)。

## 2026-10-11 — Mochizuki 允许到 30 层，层数仍可选

`MochizukiUnlockPasses` 默认关闭，层数控件是 1–3。打开后同一控件可选 1–30，不会跳到固定 30。`MochizukiLayerMode` 不再决定层数。30 层在同一个网络里顺序跑 30 次，每层一张历史，最后合成一次；第 2、3 层仍可覆盖，其后继承第 1 层且局部色调为 0。核心上限从 16 提到 30，见补丁 08。Daniel 和 lmxxf 仍是 1–3。

## 2026-10-11 — Mochizuki 的 SR → NR 使用运行时自己的运动缩放

2026-10-05 的共用适配层仍然服务 Daniel 和 lmxxf。Mochizuki 在 `RunBeforeSR=false` 时不再替换网络看到的运动矢量：颜色是超分输出，运动区域保持渲染子矩形，尺度由运行时除以该区域。默认顺序仍是 NR → SR。深度仍不进入网络，因为包装层没有 Vulkan 深度图像，官方的深度/模板计算拷贝也不适用于先进入 D3D 共享缓冲再上传的路径。私有 FP16 结果仍写回 SR 输出。引导重采样失败时只保留写回；再失败则保留 SR 图像。菜单不新增未接入的项，层数仍为 1–3。见 [SR 后 NR](post-sr-nr.md)。

## 2026-10-09 — ReShade 包装设备与 Mochizuki Fence 归属

通过本会话亲自创建的同步 Fence 证明底层设备身份，允许 ReShade 队列的包装设备与原生
Fence 共存；不以显卡 LUID 代替设备身份，不取消跨设备与完成信号校验。回调线程保存首次
错误，避免菜单读取另一个线程的 GetLastError 并一直显示旧构建状态。真实 ReShade 离线
对照和燕云游戏验收通过，适用范围见 [Mochizuki 兼容说明](mochizuki.md#reshade-compatibility)。

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
HIP network time is not whole NR latency; Mochizuki's explicitly labelled Vulkan
core total includes core input/composition, not bridge copies/shared effects.
Daniel has no network snapshot in the host interface. The fixed 360-point sampled
history uses completed-measurement counters (`gpu_samples` / stage `samples`), not
execution frame counts or UI poll timestamps. Runtime/measurement epochs, backend,
route, NR off and stale/missing observations clear/partition it even while hidden.
Zero is a valid measured duration; unavailable data stays backend-neutral N/A.
Cached snapshots may skip executions, so the curve does not claim every frame.
Do not subtract network GPU time from frame interval to invent a remainder or
combine overlapping stabilizer/blend measurements. No new runtime ABI, rendering
route or default logging.

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
输入曝光纹理仍不接入（宿主以 `MochizukiNrSetExposure` 传标量）：本后端使用手动 white point、自身
的 GPU 测光或预处理测光，不借用其他后端开关；自动测光的取舍见 2026-10-14 条目。

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

- **决定**：交接文档、工作稿、调查材料、本机路径只留本地，不进 git；tracked 文件不引用它们。仓库里只留能复现发布的东西。
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

## 2026-10-09: Share final person controls with Mochizuki and lmxxf

Use the host person compositor for optional person strength and fine-detail
attenuation in single- and multi-pass modes. Mochizuki's native automatic mask
and skin-structure settings condition its network; retain those independently
rather than rewriting them when host protection changes. Single-pass needs no
extra first-pass export. Keep new controls neutral and the feature default-off.
This is a mechanism/compatibility decision, not a claim of improved game quality.
Colour-specific protection and inter-pass conditioning require separate colour
contracts and model/history validation and remain deferred. See
[person partition](architecture/nr-person-partition.md) for the active contract.

## 2026-10-10: Default person segmentation to PP-HumanSeg

Keep PP-HumanSeg as `NrPersonModel=0`, with YOLO available as model 1 and explicit
INI/menu preferences preserved. Offline comparisons and the user's Where Winds
Meet test support this CPU/quality tradeoff: nearby people are useful protection
targets, while occasional distant-NPC misses are acceptable for the default.
This is whole-person segmentation, not face detection. The user's two-pass test
was satisfactory; three-pass clothing flicker remains unresolved and is not
claimed fixed. Shared Overall Intensity still applies after person composition.
The release asset preparer fetches the fixed OpenCV Zoo PP model with SHA256
verification; both models and their licenses remain in the package.

## 2026-10-11 · 保留切分，增加默认关闭的内联执行

- **决定**：lmxxf/Mochizuki 共用 `NrInline` 实验选项，默认关闭。保留查询未闭合时的切分保护，不人工 End/Begin 游戏查询、不丢弃前半段结果。
- **实现**：独立 D3D12 compute 队列确认输入捕获后启动 HIP/Vulkan；游戏列表有限等待并冻结输出选择，迟到则使用本帧原始颜色。已知完整增强布局经 COMMON 互操作后恢复；未知、局部或未完成布局拒绝。ABI v5 要求整包匹配。
- **验证**：RX 9070 XT 上真实 HIP/Vulkan 输出与切分基线哈希一致，跨队列重放、捕获前 Reset、双侧完成回收、查询保留、有限等待耗尽和第一层原帧回退通过。补丁在原官方 pin 上严格重放并与 vendor 逐字节匹配；内核及模块配方未变。
- **边界**：红沙本体、其它驱动/适配器、完整 temporal/multi-pass 与长时间热切压力尚未验收；不宣称红沙已经兼容或可以正式发布。人物分区原有硬件/语义蒙版验收延期保留。
- **落在**：[内联执行](architecture/nr-inline-recording.md)、`NrInlineRecording.h`、录制代理/所有权、`inline-side-queue.patch` 与已接入运行入口的专项测试。

## 2026-10-11：Mochizuki 以官方 Windows 源 a9b13dd 为准

官方提交 `a9b13dd4cdb5f4bf43db358492d72e6e5e2acd67`（2026-10-11）已包含 Windows
原生 D3D12、NR 自有 Vulkan 设备、0.0.4 能力、AMD 26.9.2 着色器修复和 ACO。
本产品的 D3D12 宿主接入该闭包。不再把 Windows 维持在 v0.0.3，也不再混用仅 Linux
的 v0.0.4 标签 `9e4574e`。

官方已经覆盖的本地差异删除：HDR 色相移植、`NR_EDGE_BODIES=0`、be7bf0a 的展开与
ViT 尾块补丁、构建进度回调、旧的 `enlarge_mode` 和 40 字节 transfer push。
放大使用官方 `transfer` 0 匹配残差、1 边缘引导、2 经典，以及 `classic_scaler`
（0 双线性、1 Catmull-Rom 默认、2 Lanczos3、3 FSR1）。官方 `store_native` 负责
列出的原生格式。产品仍默认开启 RGBA32F 紧凑复用（`MochizukiCompactTransfer=true`）；
这是宿主策略，核心默认保持关闭，不改变官方调用方。

ACO 默认关闭，由 INI/菜单拥有。扩展在创建设备时决定。设备创建时没有管线二进制时，
再打开要等下次游戏启动。每个构建线程自己选择 bundle，已录制的命令保留自己的管线。
预检哈希的是实际加载的 `aco/shaders` 及其 temporal 目录，并核对 record 内的
`spv_hash` 与 shell/alias。Windows `dlssnr-amd/shaders` 是 native shell 和 runtime
来源。包不完整、record 内容不匹配，或导入没有得到管线时，丢弃该图并完整重建一套
驱动编译的网络。2026-10-11 在 AMD Radeon RX 9070 XT（device 0x7550，Vulkan 驱动
26.8.1 LLPC，driverVersion 0x0080018b）上，记录 58/58 匹配，导入 29 条管线，
一帧 256x256 得到 `ACO_FRAME_OK`。缺包、缺别名和损坏 record 都完成了原生帧。
同日复审后的三处修正：有效状态只在 `Install` 与网络一起发布，候选帧缓冲失败时保留已安装网络的状态；`vkCreateDevice` 因可选管线二进制被拒绝时，除设备丢失和内存不足外，去掉该扩展后按普通设备重试；卸载和打包使用 `tools/install/mochizuki-owned.txt` 的 214 条精确路径，不再按 `g_*.spv` 或 16 位十六进制 `.nrp` 通配。复跑 `tests/mochizuki/run.cmd aco` 打印 `ACO_SWITCH_NATIVE_OK`、`ACO_SWITCH_ACO_OK`、`ACO_CONTRACT_OK effective=1`、`ACO_DEVICE_FALLBACK_OK` 和 `ACO_BUDGET_KEEP_OK effective=1`。构建过程中把请求改回 ACO 时，已开始的原生候选安装后有效状态仍取该候选的实际模式；`tests/mochizuki/run.cmd aco-rapid` 打印 `ACO_RAPID_REENABLE requested=1 effective=0` 和 `ACO_RAPID_OK`。模型分辨率低于全帧时，官方会再分配一张从不读写的全帧 RGBA32F `up_shown`；本产品去掉这张图和它的绑定，经典放大仍写入 `up_result`。
未加入显式 barrier 开关。不接入官方 DX9/DX10/DX11、ReShade、PE 或 OptiScaler 钩子。三后端仍互斥。
INT4 仍不启用。可重放的核心补丁和放弃项见
[来源记录](../third_party/mochizuki/UPSTREAM.md)。

## 2026-10-11 · 内联提前录制不再把上一帧完成当成准入

宿主最多保留 4 帧未回收的内联录制。两侧 fence 都完成后才回收；未提交的预约不占完成名额，槽满时本帧走原图，不等待。同一条闭合列表在自己的 fence 未完成时重放仍返回 `E_PENDING`。网络按入队顺序提交，排队不持有 `RecordingMutex`。ABI 仍是 5。

Mochizuki 每个在途录制使用自己的帧缓冲，池上限 4。lmxxf 用 `inline-ahead-capture.patch` 把下一帧的拷贝记到独立捕获相位，只在该帧轮到入队时武装桥；跨队列提前录制直接拒绝。lmxxf 的 codec 和桥接缓冲仍是一套，靠同一条游戏队列的 GPU 顺序。超时后迟到的网络仍可能碰到这套共享缓冲。Vulkan 网络命令缓冲槽仍是 3，第 4 笔入队可能在锁里做最多 250 ms 的 `vkWaitForFences`。

RX 9070 XT 上 `tests\lmxxf\run.cmd inline-gpu` 和 `tests\mochizuki\run.cmd inline` 通过：连续 120 帧、强制超时、提前 2/3/4 帧、先提交再录下一帧，以及 Mochizuki 2560×1440 连续时序（history 85%）。没有同一场景的切分/内联游戏计时，不能据此说网络区间已经不再翻倍。提前录制下的时序历史、跳帧、尺寸或层数变化、跨队列、失效 fence 和设备丢失没有测。游戏安装和已有 1.12.0 包都不含这次改动。

## 2026-10-11 Crimson Desert inline follow-up

Reuse one side compute queue per NR session, retaining independent recording buffers and both completion proofs. Keep real queue/history discontinuity resets. Route the lmxxf HIP 7 loader through the existing controlled product search using an explicit replayable vendor hook; no HIP 6 fallback or global DLL directory mutation. Add throttled selection/lifetime diagnostics and explicit Mochizuki history-reset reasons. See [inline architecture](architecture/nr-inline-recording.md#crimson-desert-follow-up-fixes-2026-10-11) for focused tests and outstanding game/performance acceptance.

## 2026-10-11 Mochizuki 接口扩展采用显式补丁队列

固定官方 SHA 与源码闭包，六项本地扩展以功能补丁重放，不保留旧整文件覆盖。
产品策略、录制生命周期、人物合成和整包校验留在 adapter。编译器路径和构建取消
先准备独立上游提案；计时、首层输出、缓冲复用按实际消费者和所缺验证决定后续提交。
当前 ACO 线程局部配置改变官方跨线程默认行为，必须整理成显式可选隔离接口，不能
将本地整份 header 直接送上游。作者接受后更新 pin 并删除被覆盖的补丁。
见[逐项扩展决策](architecture/mochizuki-upstream-extensions.md)与
[只读来源复核流程](../tools/mochizuki-sync/README.md)。这不代表游戏或发布验收完成。

## 2026-10-12 ACO 保留 decoder-tail 必要依赖

接入 Overclockers PR #5 / `60f9202f` 的 C64→融合 C32 边界修复及长时序测试，
保留其他官方重叠调度。全显式等待只用于独立测试 oracle，不添加同步菜单开关或
产品 ABI/INI 字段。测试与固定来源补丁记录作者归属；本侧适配作为共同署名提交。
本地只运行实际风险所需的 GPU 对照与短 ABI/来源核验；不跑完整发布 CI。
这次全矩阵按用户要求中止，已完成 28 个对照用例和一个 inline ACO 时域用例。
详见 [ACO 依赖与验证](mochizuki.md#aco-decoder-tail-dependency-2026-10-12)。

## 2026-10-12 补审：层数持久化与未执行 inline 回收

读取已保存层数时使用解锁状态对应的 3/30 上限，保持 4–30 的明确选择；无效值仍有界。
未开始执行的 inline 录制在 Reset/Release 失效时归还名额，并在成功收集时兜底清理。
已开始的录制保持既有队列完成证明与延迟回收，不提前释放。
针对性验证使用真实 INI 保存再加载及实际 Reload 片段（14 个边界用例、缺键），
WARP/mock runtime 下 12 次实际命令列表丢弃与已开始资源保留验证；不运行完整 CI。

## 2026-10-12：Mochizuki 内联网络使用独立计算队列

隔离实验复现同尺寸一层网络在内联中经过时间翻倍；只改变 CPU fence 唤醒或
移出轮询循环内的屏障不能解决。独立计算队列使 FP32 网络恢复至切分基线，
分段的 FP16 路径也恢复网络时间并保持完整输出字节一致。

`NrInline` 默认仍为 false。Mochizuki 内联作业在有兼容计算队列时，把每个 pass 的
网络循环录在 compute-only family，格式转换、缩放和历史操作保留原 graphics family。
同设备资源声明并发 family 访问，时间线信号量串起所有阶段；最后一个 graphics
fence 才是 slot 复用和整个网络链回收的证明。普通切分不改变原有队列路线。
无兼容计算 family 或设备拒绝该可选队列时，保留原 graphics 路线。
未确认的分段提交失败保留资源；跨段录制失败不自动重试已改变的核心状态。

宿主捕获使用唯一 fence event 和私有高精度回退 timer，避免微秒 sleep 退化为整 tick；
保留完成 fence 值核对及录制资源生命周期，不复用仍可能迟到的通知。
GPU 等待仍是 64×32768 的工作预算，未扩大预算，也不承诺通用 GPU 时间上界。

验证：真实 WARP/硬件丢弃与迟到结果冻结；原生/ACO 的重放、提前 Reset、2–4 ahead、
2560×1440 时序和三层（实际369 dispatch）通过。提交第2/3阶段失败仍拒绝单凭D3D12
尾部回收；官方173文件闭包和10项补丁回放一致。未做其他GPU/驱动与完整游戏验收。

## 2026-10-11: Mochizuki adopts upstream first and minimizes local divergence

Use the official Mochizuki core, driver fixes and build policies by default.
Delete local patches superseded by upstream; move host-specific configuration,
recording ownership and package policy into the product adapter. A previous local
implementation or different API is not sufficient reason to retain a fork.
Generic missing capabilities and demonstrated local improvements should become
small, default-preserving upstream contributions, with reproducible evidence and
preserved attribution. Necessary pending extensions remain explicit, replayable
patches rather than unexplained vendor edits. Preserve required product behavior
through equivalent APIs; reducing divergence does not authorize silent feature loss.
Implementation and its internal validation run continuously, followed by one final
review. Public PR submission and release remain separate from this planning decision.

## 2026-10-13：减少 inline 等待分段，消除额外多槽调度损失

lmxxf 宿主等待由 64×32768 调整为 32×65536；总循环工作预算仍为 2097152，
但不代表相同的毫秒超时或通用 TDR 安全界限。Mochizuki 的 graphics/compute 分段
路径在 32 段下实测回落，因此保留 64×32768，不能直接推广 lmxxf 的参数。
本项仅取代上文 2026-10-12 的 lmxxf 参数；槽位上限、默认值、私有输入/结果、
冻结选择与双尾回收契约保持不变。

依据同进程正反序、等预算对照，减少等待 dispatch/UAV barrier 对可消除已定位的
lmxxf 额外多槽吞吐回落；更少的 16/8 段没有进一步收益，因此采用 32 段。
这是源代码层面的受控因果定位，尚未由 GPU trace 确定具体 WDDM/CU/cache 机制。
控制资源池化和缩小录制锁范围不能消除该回落，不混入本次修复。

验证、两种 Mochizuki 路径及剩余 split 差距见 [离线槽位对比](lmxxf-inline-slots.md)。
共用路径须覆盖取消、重放、满槽旁路、超时冻结、失败回收与真实 GPU；不能以限制为
2–3 槽代替调度修复。inline 继续默认关闭，游戏和其它驱动验收另行进行。

## 2026-10-15: Mochizuki 0.0.5 review decisions

- Ctrl+F11 changes only the Mochizuki default/INT4 request; holds and input capture do not repeat it, and the chord owns F11 through release. ACO remains independent.
- Upstream NR Control is excluded from compilation/installation: End continues to toggle host NR; Ins is the only menu.
- RenoDX disables host person protection while retaining its saved settings. Native white-point controls are hidden; preprocessing remains functional inside the RenoDX stage.
- Patch 09 is upstream-equivalent, including arena lifetime and barrier emission; keep the official implementation after GPU oracle comparison.
- Detailed scope, rebase SHAs, evidence and maintainer game matrix: [Mochizuki 0.0.5 review](architecture/mochizuki-005-review.md).

## 2026-10-18: Mochizuki extent and exposure continuity

- Actual bucket-size shrink requests use the 300 ms extent settle window; incompatible frames pass through their own colour while waiting.
- Temporary missing game exposure holds for at most 0.5 s of elapsed time. Source transitions start from the last applied divisor; normal game exposure tracking resumes after the transition.
- Compatible network replacements retain independently owned 1x1 meter resources on the session execution queue. They do not copy from an already retired network or retain its large model arenas. Explicit camera/queue resets remain distinct from network history resets.
- Preparation logs identify requested settings; submission logs identify the actual network, model and pass styles. Reference-drop logs are distinct from object destruction and process-wide VRAM measurements.
- `mochizuki_nr.log` rotates to `mochizuki_nr.1.log` at 4 MiB to preserve the newest diagnostics while capping total log footprint at 8 MiB.
- Vendor changes replay as patch 26. CPU coverage is `tests\mochizuki\run.cmd renodx-feed`; game/GPU acceptance of visual continuity and repeated-action performance remains outstanding.

## 2026-10-19: Mochizuki 0.0.7 core and style integration

- Adopt fixed original `cc27501dc116bdcfbb43612e497c5ca12497574a`, with complete native/default-ACO/INT4-ACO grade assets and an exact 833-path install/uninstall pin. Natural/Cinematic use the corrected grade; ordinary models retain non-ACO compilation and existing preferences.
- Rebase local source patches by feature. Original covers HDR patch 24 and the unused up_shown allocation; retain only residual binding/layout changes. Preserve local pack_img destruction, capacity/failed-upgrade behavior, recording proofs, model verification and exposure continuity.
- Product construction policy (patch 27) keeps independent pipeline import/fallback accounting, fixes the no-depth contract, skips unconsumed Facts readback and requires usable grade variants. Official standalone defaults and the public host/runtime ABI remain unchanged.
- Keep current split transport and size scheduling. New cross-model arenas, image heaps and RAM-offload scheduling are deferred until their lifetime/order and net benefit are demonstrated against this host; they do not replace the local destruction fix. Motion point sampling is also deferred until the preceding LINEAR rescale contract is reconciled.
- Evidence and limits: the source ownership record and extension decisions describe focused offline/GPU checks; this is not game-FPS, 4K stress or full Inline certification. No packaging or publishing is part of this change.

## 2026-10-22: Launcher-only Mochizuki visual share codes (MZ1)

- Share only Mochizuki visual-style settings from an existing game's `[DlssNr]`, plus the shared overall/residual/person/stabilizer controls and Mochizuki white-point source. Exclude `MochizukiNetworkModel`, `MochizukiInt4Mixed`, `MochizukiAcoMode`, `MochizukiModelScale`, `MochizukiDynamicResolution`, `MochizukiMaxPasses`, `MochizukiCompactTransfer`, and `MochizukiUnlockPasses`; all other sections and non-table keys are outside the contract.
- Format is `MZ1:idx=value,idx=value;CCCC`, ascending numeric indices and uppercase four-digit CRC16/CCITT-FALSE (polynomial 0x1021, initial 0xFFFF) over the exact payload between colon and semicolon. Floats use integer hundredths; integers and booleans use raw integer / 0 or 1. Empty payload is valid. Encoding is sparse: absent means default, and default values are omitted.
- The initial 136 table indices are frozen compatibility identifiers: append new keys only; never reorder or renumber. Reject unknown indices, duplicate indices, checksum errors and out-of-range values; float boundary rounding tolerates one hundredth before clamping.
- Import previews the visual summary and changed-setting count. Apply neutralizes every existing table assignment only inside `[DlssNr]`, retaining original lines/comments, and writes the supplied non-default values. Encoding/BOM, existing line endings, unrelated keys, comments, blank lines and bytes outside that section are preserved.
- Launcher apply uses target locking and a flushed temporary file followed by atomic `File.Replace` with a `.bak-<timestamp>` backup. Export never writes files. Share-code lifecycle is independent of NR installation: it operates on the selected game's already-existing `OptiScaler.ini`.

## 2026-10-22: Standalone XeFG unlocker plugin import

- The launcher card offers `导入 XeFG 解锁插件… / Import XeFG unlocker…` only for a selected game with an existing `OptiScaler` directory and an active/planned ASI-loading signal: the ASI toggle, OptiPatcher toggle, or current INI `LoadAsiPlugins=true`. Game selection, both toggles, and action completion re-evaluate visibility; busy operations disable import.
- Accept only 4 KB–100 MB AMD64 PE plugins with MZ/PE signatures. Keep `.asi` filenames; copy `.dll` under the same basename with an `.asi` extension. Resolve `[Plugins] Path` relative to the game directory or as an absolute path; otherwise mirror the installer's `[Libraries] OptiDllPath`/`plugins` resolution. Invalid paths fall back to `OptiScaler/plugins`.
- Refuse writes while the selected game is running; verify same-name process paths and conservatively refuse an inaccessible matching process. Import acquires the launcher's target lock. Missing destinations are copied; SHA256-identical files (including the same source/destination path) offer optional force re-copy. Different existing files offer overwrite / keep / cancel. Overwrite copies the original to `<name>.bak-<yyyyMMdd-HHmmss-fff>` before a flushed same-volume stage and atomic `File.Replace`; normal IO errors remain visible in the card and log.
- After copying, warn when at least two `.asi` filenames match `(mfg.*unlock|xefg.*unlock).*\.asi`: keep only one frame-generation unlocker because multiple unlockers can conflict.
- After copying, missing/empty/`auto` `LoadAsiPlugins` becomes `true` automatically; explicit `false` asks first, with a plugin-will-not-load hint if declined. Existing `true` is unchanged. Reuse the INI document editor to preserve encoding/BOM and unrelated content, with a flushed temporary file, atomic replace and timestamped INI backup.
- This operates on an already-installed game's plugin directory directly. No installer round-trip, package installation, or automatic game launch is involved; cancellation and all resulting copy/INI outcomes use the card status and launcher log.

## 2026-10-22: Optional MZ2 game tags and direct plugin-folder access

- Tagged Mochizuki codes use `MZ2:<payload>|<game>;CCCC`, retaining the frozen MZ1 key table and payload/apply semantics. The game tag is the source executable basename without its optional `.exe` suffix. CRC16/CCITT-FALSE covers the exact UTF-8 `payload|game` string, so edits to either part fail validation. Untagged exports remain byte-identical MZ1, and imports accept both versions; tag matching ignores case and the `.exe` suffix.
- Export defaults to `包含游戏标识（推荐） / Include game tag (recommended)` checked; changing it regenerates the displayed/copied code and adds/removes ` · 来自 <game> / · from <game>` in the summary. After preview, applying a mismatched tagged code first asks `该分享码来自「<game>」，与当前游戏不符，仍要导入吗？ / This share code was made for '<game>', which does not match the current game. Import anyway?`; No leaves the INI unchanged. MZ1 has no mismatch prompt. Successful imports show `已导入通用分享码，效果请以实际画面为准 / Generic share code imported; judge the result by eye.`; matching tags apply without an extra prompt.
- `打开插件文件夹 / Open plugins folder` sits beside the XeFG unlocker import action. It is available for a selected game with an existing `OptiScaler` directory and disabled while INI actions are busy; ASI/OptiPatcher toggles are not prerequisites. It reuses the unlocker destination resolver, including `[Plugins] Path` precedence and `[Libraries] OptiDllPath`-based `plugins` resolution with the existing fallback, and opens Explorer on the directory without selecting a file.
- Opening never creates a directory. A missing resolved directory reports `未检测到插件目录，请先安装本工具 / Plugins folder not found; install this tool first.` The package already ships `plugins/` with its README and installation ensures the resolved directory exists, so a missing directory is treated as a missing/deleted or nonstandard installation rather than repaired by browsing.


## 2026-10-25 janblade PR 230 "Superior Scaling":仅借鉴方法论,不吸收

- **背景**:降分辨率 NR 的放大质量(多 pass 缩放的锐化/膨胀、运动不稳定)。真实手写量 30 文件/+6,659 行(diffstat 24.7 万是字节码/测试)。
- **否决整吞/摘取**:私有残差稳定器经 NVNGXProxy 直连真 NVIDIA NGX,不走 OptiScaler 门面——AMD 上无 FSR 回落,时间稳定部分 N 卡专属;字面替换面连锁 190 文件/~7 万行 fork 私有体系;AMD 特性化估 30-45 文件/8-12k 行。
- **采纳**:P50/P100 全帧 FP64 标定方法论可迁(数值表不可——估计器/重建域不同);C5 运动端点 bug 我们无同类病灶,不跟进。
- **重启条件**:用户群实际压 NR% 求性能且反馈 bloating 时,先做离线拟合工具(3-6 文件)验证收益再决定。详见 work/notes/2026-10-25-superior-scaling-study.md。

## 2026-10-26: Opt-in split query splice（Crimson Desert open_query 全拒）

- **背景**：红色沙漠引擎把 FFX evaluate 包在开着的 `PIPELINE_STATISTICS` 查询里（TraceOpenQueries: type=3 index=5，list 10/18/23 全会话复现）。split 必须在 evaluate 点切列表，游戏的 EndQuery 落到 continuation——非 timestamp 查询对不能跨两个原生列表（D3D12），故 `open_query` 全拒；此前唯一出路是实验性 inline（不切列表，查询保持开着合法）。
- **决定**：新增 `[DlssNr] NrSplitQuerySplice`（默认 false，实时生效）。开启后 `CommandListProxy::SplitSegments` 在切点把每个开着的非 timestamp 查询对在 producer 末尾补 `EndQuery`、continuation 种子后补 `BeginQuery`；queryBook 保持 open，游戏自己的 EndQuery 照常闭合。画面/NR 效果不变；唯一变化是游戏查询结果只覆盖切分后半段（引擎 profiler 数字改变），因此默认关闭，菜单"兼容性与调度 → Split query splice"显式开启。
- **执行**：ConfigKeys.h（入 kKnown）/Config.h/Config.cpp（加载+保存+`SetSplitQuerySplice` 下发）、submission/SubmissionPolicy.h（原子策略）、submission/CommandListProxy.h（IsSplitIneligible/SplitRejectionReason 放行 + SplitSegments 拼接与限频 WARN）、DlssNr_Menu.cpp + MenuStrings.inl（复选框/双语/reset）、OptiScaler.ini 模板、tests/host/nr_recording_admission.cpp（策略往返+两遍遍历稳定性）。
- **验证**：admission 测试通过；宿主 Release x64（v145 / 14.44.35207 / SDK 26100）编译链接通过。红色沙漠实机（split 生效、画面、引擎是否消费该统计、长跑）待验证；证据与时间线见 work/notes/2026-10-26-crimson-desert-inline-visual-inactive.md。

## 2026-10-26(二): Crimson Desert 自动策略与 NrSplitQuerySplice 三态化

- `GetNrGamePolicy`（misc/GameConfigs.h）新增 `splitQuerySplice` 字段并加入 `crimsondesert` 条目：`enhancedBarriers=true, splitQuerySplice=true`（Match::Contains）。实机依据：第二局 split+拼接约 19,000 次切分全净（0 failed/0 bypass）。
- `NrSplitQuerySplice` 改为三态（`CustomOptional<bool, NoDefault>`）：显式 true/false 优先，auto=按策略表（红沙 on，其余 off），经 `Config::SplitQuerySpliceEffective()` 解析；菜单改为 auto/force on/force off 三态组合框（实时生效），与 LmxxfAllowEnhancedBarriers 完全同构。
- `ExeIsForza()` 更名 `ExePolicyEnhancedBarriers()`（它本来就按策略表取值，现在含红沙，旧名失真），新增 `ExePolicySplitQuerySplice()`；Config 加载与 lmxxf 后端构造处统一经 Effective() 下发双策略，日志带 policy_auto。
- 文档：docs/games/crimson-desert.md（新建）、OptiScaler.ini 模板、菜单双语、release.yml 中英已知问题。

## 2026-10-26(三): 修复启动器中文界面写错 ini 语言键

- 现象: 中文启动器安装后游戏内 Ins 菜单仍英文。实证(红沙游戏 ini): 启动器写了 `[Menu] MenuLanguage = zh`,
  宿主读的是 `[Menu] Language`(CfgKey::MenuLanguage="Language"), 键名不匹配 → 写入被无视。
- 修复(tools/install/launcher/MainWindow.cs 安装/更新成功回调): 段内定位 `[Menu]`, 写 `Language=zh-CN`(宿主规范值);
  现值为 en 或缺失才写(用户显式选择被尊重, 契合 ini 归属规则); 顺带剥离旧版写错的 `MenuLanguage` 行(宿主从未读过);
  UTF-8 BOM 与安装器 Set-IniSettings 一致。冒烟(throwaway, work/scratch, 未进 tests/): 模板覆写/用户已选/无段追加/
  他段同名键不误伤/剥离旧行 5 例通过; 启动器经 tools/build/build-launcher.ps1 编译通过。

## 2026-10-26(四): OptiScaler.ini 模板 [DlssNr] 段备注补全与纠错

- 方法: 以 Config.cpp 实际 read*/SaveIni 键集 diff 模板键集(251→259), 语义以 ConfigKeys.h/Config.h 默认值/
  DlssNr_Menu.cpp/AmdBridge.cpp 消费点为准, 不凭键名臆造。
- 补漏(8 个代码在读写、菜单在用、模板缺失的键): LmxxfInlineSlots(NrInline 同族, 疑似随 inline 功能漏放),
  ToneCurve/ToneLift/AmdToneChannels/AmdUseGameExposure(共享外观), Quality/QueuePriority/AmdInline(Daniel)。
- 纠错: DLSS5_MULTI_PASS 的注释原挂在 LmxxfNetworkModel 头上(错位); LmxxfEarlyAnyModuleWrap 的 auto 规则
  注释漏了 dyinglight/yysls(与 GameConfigs 表不符); NrInline 注释"no query splitting"改为明确"列表不切分,
  查询拼接属 NrSplitQuerySplice"。
- 结构化: RenoDX 家族(~60 键)按 pass1/pass栈/合成/分辨率/look/编码归组逐键注释; person/residual/stabilizer
  三块注释与键重新对齐(原先 residual 注释被 person 文案隔断); NrTiming*/Nr*Diag 逐键一行; SkinProtection 块
  逐键; DLSS5_HIP_*/FIT_LARGE/VIT_* 补一行语义。
- 刻意不补: 仅迁移用的旧拼写(QueuePriorityLegacy/LmxxfFitLarge/LmxxfPdl 等, SaveIni 只写新拼写);
  readUnifiedBool 循环读的 DLSS5_* 均已在模板。验证: 键集 diff 无丢失/无重复, 恰好 +8。
