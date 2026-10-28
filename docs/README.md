# docs/

项目的公开技术文档。每个主题只有一处；其它地方链接过来，不复制。

兼容说明区分当前行为、已修复案例与待验证报告。历史测量必须注明当时的缺陷或版本；问题修复后，撤下对应的现行限制和绕过建议。没有新验证的旧报告不得直接写成当前缺陷。

| 文件 | 内容 |
|---|---|
| [architecture/menu-navigation.md](architecture/menu-navigation.md) | 独立NR页面、窄窗口导航、卡片布局与菜单背景模糊的资源约束 |
| [architecture/streamline-fg-activation.md](architecture/streamline-fg-activation.md) | 可选的 Streamline 帧生成就绪帧等待、默认值及验证范围 |
| [architecture/optiscaler-upstream.md](architecture/optiscaler-upstream.md) | OptiScaler定向整合来源、已接模块、生命周期边界与延期 |
| [mochizuki.md](mochizuki.md) | 第三后端：安装、模型提取、菜单、生命周期、构建和验证范围 |
| [rdna3-migration-progress.md](rdna3-migration-progress.md) | RDNA3 回迁暂停点：已接入的 vendor pin、未做的适配器/构建/试包 |
| [architecture/mochizuki-upstream-extensions.md](architecture/mochizuki-upstream-extensions.md) | Mochizuki 核心扩展的必要性、固定来源补丁与上游提案边界 |
| [architecture/mochizuki-split-transport.md](architecture/mochizuki-split-transport.md) | Mochizuki split 传输开销、共享颜色纹理离线结果及产品接入前的资源/回退约束 |
| [post-sr-nr.md](post-sr-nr.md) | 三后端实验性 SR → NR：设置、分辨率、输入限制和验收 |
| [xefg-mfg-notes.md](xefg-mfg-notes.md) | XeFG 多帧生成插件宿主兼容、`HighResMV` 自动检测与 4K 性能验证边界 |
| [architecture/overview.md](architecture/overview.md) | 管线、两个后端怎么加载与选择、代码归属与许可证、日志位置、如何查看 wilsjo2 上游 |
| [architecture/lmxxf-c-abi.md](architecture/lmxxf-c-abi.md) | `LmxxfNrRuntime.dll` 的 C ABI：为什么有 DLL 边界、`struct_size` 分档、四条演进规则、资源查找 |
| [architecture/lmxxf-native-history.md](architecture/lmxxf-native-history.md) | 原生 temporal history：执行顺序、资源、ABI 与支持范围 |
| [architecture/lmxxf-recording-lifecycle.md](architecture/lmxxf-recording-lifecycle.md) | 录制租约、实际提交凭证、重放与异步回收 |
| [architecture/nr-flicker-diagnostics.md](architecture/nr-flicker-diagnostics.md) | NR 闪烁一轮采集：统一诊断开关、处理覆盖统计、日志文件和开销边界 |
| [architecture/nr-inline-recording.md](architecture/nr-inline-recording.md) | 实验性内联执行：查询、增强布局、有限等待与双队列回收 |
| [architecture/nr-capture-proof-deferral.md](architecture/nr-capture-proof-deferral.md) | GPU-armed输入核验暂缓：性能证据、风险、保留补丁、无效方案与研究止步线 |
| [lmxxf-inline-slots.md](lmxxf-inline-slots.md) | lmxxf inline 1–5 槽、即时切换、离线一致性及相对 split 的同会话性能 |
| [architecture/nr-output-effects.md](architecture/nr-output-effects.md) | 三后端整体强度、残差分频与肤色/边缘保护、稳定器、历史/资源归属及验证范围 |
| [architecture/installer.md](architecture/installer.md) | 安装器与卸载器：后端选择、升级流程、ini 与 flags 文件、双架构模块包校验、卸载范围 |
| [lmxxf-037-consumer-review.md](lmxxf-037-consumer-review.md) | 0.37 消费者重审：实际宏、验证范围、pinned bridge 和暂缓项 |
| [lmxxf-039-consumer-review.md](lmxxf-039-consumer-review.md) | 0.39 完整上游审阅：实际生成配方、配置差异、验证范围和暂缓项 |
| [lmxxf-041-consumer-review.md](lmxxf-041-consumer-review.md) | 0.41：预测三层、肤色保护、76模块、布局优化与本地验证 |
| [lmxxf-044-consumer-review.md](lmxxf-044-consumer-review.md) | 0.44：Small31、decoder/down W16、菜单与资产接入、实际验证及暂缓 |
| [lmxxf-044-hotfix-consumer-review.md](lmxxf-044-hotfix-consumer-review.md) | 0.44后续7提交：冷上传/producer等待、菜单策略及F9消费边界 |
| [lmxxf-small-model.md](lmxxf-small-model.md) | lmxxf Small菜单选择、离线模型转换和回退说明 |
| [backends/lmxxf.md](backends/lmxxf.md) | lmxxf 后端专页 |
| [games/](games/) | 各游戏的兼容说明、已修复案例和排查入口 |
| [2026-10-12-review-verification.md](2026-10-12-review-verification.md) | 17 项外部意见核实、失败回收与发布门禁修复、验证边界 |
| [release.md](release.md) | 发版唯一入口：构建、测试产物、试包、Actions 预验证、发布核验与故障处理 |
| [measurement.md](measurement.md) | 测量纪律：读数规则、PresentMon 列语义、开工前清单 |
| [dev-environment.md](dev-environment.md) | 工具与测试索引、Windows / PowerShell / Git Bash 的坑、git 查证的坑、协作约定 |
| [dev/upstream-integration-review.md](dev/upstream-integration-review.md) | 上游更新共用审查约定：变更分类、按功能追踪、接入决定与完成边界 |
| [decisions.md](decisions.md) | 带日期的决策记录（新的在前） |
| [workspace.md](workspace.md) | 工作区使用规划：每类东西放哪、怎么命名、留多久、清理节奏 |

相关的 tracked 文档（不在本目录）：

- [AGENTS.md](../AGENTS.md)：给 agent 的仓库规则。
- [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)：发版前无卡测试。
- [third_party/lmxxf/UPSTREAM.md](../third_party/lmxxf/UPSTREAM.md)：lmxxf 上游 pin、OURS/FOLLOW、本地补丁。
- [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)：上游同步与审阅流程。
- [tools/mochizuki-sync/README.md](../tools/mochizuki-sync/README.md)：Mochizuki 源码 pin、补丁回放与验证流程。
- `README.md` / `README.en.md` / `README.es.md`：用户安装说明，也是包内 README。

## 本地材料

`work/` 是只留在本机的工作区（交接、笔记、日志、不公开的资料），被 git 忽略。**tracked 文件（包括本目录、代码注释、脚本）不得引用 `work/` 下的任何路径**：它在别的 clone 和 GitHub 上都不存在，引用即死链，而且其中有不能公开的内容。需要的结论先提炼进 `docs/`，再引用 `docs/`。
