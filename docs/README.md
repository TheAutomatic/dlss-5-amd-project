# docs/

项目的公开技术文档。每个主题只有一处；其它地方链接过来，不复制。

兼容说明区分当前行为、已修复案例与待验证报告。历史测量必须注明当时的缺陷或版本；问题修复后，撤下对应的现行限制和绕过建议。没有新验证的旧报告不得直接写成当前缺陷。

| 文件 | 内容 |
|---|---|
| [architecture/overview.md](architecture/overview.md) | 管线、两个后端怎么加载与选择、代码归属与许可证、日志位置、如何查看 wilsjo2 上游 |
| [architecture/lmxxf-c-abi.md](architecture/lmxxf-c-abi.md) | `LmxxfNrRuntime.dll` 的 C ABI：为什么有 DLL 边界、`struct_size` 分档、四条演进规则、资源查找 |
| [architecture/installer.md](architecture/installer.md) | 安装器与卸载器：后端选择、升级流程、ini 与 flags 文件、双架构模块包校验、卸载范围 |
| [backends/lmxxf.md](backends/lmxxf.md) | lmxxf 后端专页 |
| [temporal-test.md](temporal-test.md) | 默认关闭的 lmxxf 时序实验、四种现场对照与画质验收 |
| [games/](games/) | 各游戏的兼容说明、已修复案例和排查入口 |
| [release.md](release.md) | 发版：版本号、构建、包内容与禁入文件、编码规则、双后端烟测、CI |
| [measurement.md](measurement.md) | 测量纪律：读数规则、PresentMon 列语义、开工前清单 |
| [dev-environment.md](dev-environment.md) | 工具与测试索引、Windows / PowerShell / Git Bash 的坑、git 查证的坑、协作约定 |
| [decisions.md](decisions.md) | 带日期的决策记录（新的在前） |
| [workspace.md](workspace.md) | 工作区使用规划：每类东西放哪、怎么命名、留多久、清理节奏 |

相关的 tracked 文档（不在本目录）：

- [AGENTS.md](../AGENTS.md)：给 agent 的仓库规则。
- [tests/RELEASE-TESTS.md](../tests/RELEASE-TESTS.md)：发版前无卡测试。
- [third_party/lmxxf/UPSTREAM.md](../third_party/lmxxf/UPSTREAM.md)：lmxxf 上游 pin、OURS/FOLLOW、本地补丁。
- [tools/lmxxf-sync/README.md](../tools/lmxxf-sync/README.md)：上游同步与审阅流程。
- `README.md` / `README.en.md` / `README.es.md`：用户安装说明，也是包内 README。

## 本地材料

`work/` 是只留在本机的工作区（交接、笔记、日志、不公开的资料），被 git 忽略。**tracked 文件（包括本目录、代码注释、脚本）不得引用 `work/` 下的任何路径**：它在别的 clone 和 GitHub 上都不存在，引用即死链，而且其中有不能公开的内容。需要的结论先提炼进 `docs/`，再引用 `docs/`。
