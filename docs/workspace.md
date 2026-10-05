# 工作区使用规划

这份文件规定仓库里每类东西放在哪、怎么命名、留多久。所有人和所有 agent 都按这里放东西；不确定就问，不要自己新开一个文件夹。

## 一、总原则

1. **仓库只放长期有效、可复现的东西**：产品代码、构建和发布需要的脚本、在用的测试、仍然成立的文档。
2. **过程材料只放 `work/`**（本地，不进 git）：调查记录、日志、交接、临时脚本，以及不公开的资料。
3. **编译中间产物和测试输出放 `exports/`**（本地，可重新生成）；交付用户的安装包统一放 `dist/`。
4. **安装包统一放 `dist/`**：本地测试包、正式发布包及对应的打包目录均放这里；也保留第三方权重和 runtime 安装器。不跟踪进 Git，不清理其他版本或第三方输入。文件在此目录不代表已经发布。
5. **别的仓库的克隆放在仓库外**（`..\<名字>`），不嵌进本仓库。例外：必须带在手边的第三方小件放 `work/external/<名字>/`。
6. **一件事只有一个权威位置**。其他地方引用它，不复制它。

## 二、目录表

### 进 git

| 目录 | 放什么 |
|---|---|
| `OptiScaler-DLSSNR-PreSR-Multipass-main/` | 产品代码（宿主、菜单、三个后端及 runtime） |
| `third_party/` | vendored 上游代码，规则见 `third_party/lmxxf/UPSTREAM.md` |
| `docs/` | 仍然成立的文档，索引见 `docs/README.md` |
| `tools/build/` `tools/release/` `tools/install/` `tools/lmxxf-sync/` `tools/diag/` `tools/dev/` | 在用的脚本，按用途分组。`tools/` 根目录不放新文件 |
| `tests/<领域>/` | 测试。每个领域一个 `run.cmd`，统一入口 `tests\run-all.cmd --tier ci|device|gpu` |
| `assets/` | 随包发布、可由源码重建的资源 |
| `.github/` `.githooks/` | CI 和提交检查 |

### 不进 git

| 目录 | 放什么 | 命名 | 保留 |
|---|---|---|---|
| `work/handoff/HANDOFF.md` | 当前交接：状态、未完事项、本周关闭项 | 固定文件名 | 不超过 80 行；超过 7 天的段落移入 `archive/` |
| `work/handoff/archive/` | 旧交接和已合入文档的原始材料 | `<时间段>-<主题>/` 加同名索引 `.md` | 长期 |
| `work/notes/` | 值得留下的调查记录 | `YYYY-MM-DD-主题.md` 或同名文件夹 | 结论成熟后提炼进 `docs/`，原文可移入归档 |
| `work/logs/<游戏>/<日期>/` | 游戏和网友日志、截图、采集 | 游戏名用英文短名 | 30 天；被 note 引用的除外 |
| `work/private/` | 不公开的资料和脚本（任何情况下都不进 git，也不在公开文档里描述其内容） | 按主题建子目录，根目录放一份 README 说明 | 长期 |
| `work/external/<名字>/` | 需要带在手边的第三方件 | 原名 | 长期；能重新下载的直接删 |
| `work/plan/` | 整理计划、清单 | — | 按需 |
| `work/scratch/` | 一次性脚本和临时输出 | 随意 | 14 天，到期整个清空 |
| `exports/<用途>/` | 编译中间产物、测试输出 | 按用途 | 可重新生成；不放文档或用户安装包 |
| `dist/` | 本地测试包、正式发布包、打包目录；第三方安装器和权重 | `OptScaler(NR)-<版本>` 及同名 zip | 保留其他版本与第三方输入 |

## 三、东西该放哪（速查）

| 你手上的东西 | 放到 |
|---|---|
| 一次性脚本、试验代码 | `work/scratch/` |
| 调查结论 | `work/notes/YYYY-MM-DD-主题.md`，定论后写进 `docs/` |
| 做出的决定 | `docs/decisions.md`（日期、决定、原因、在哪里执行） |
| 游戏日志、网友反馈 | `work/logs/<游戏>/<日期>/` |
| 对某个游戏仍然成立的兼容说明 | `docs/games/<游戏>.md` |
| 不公开的资料、脚本 | `work/private/` |
| 构建输出 | `exports/<用途>/` |
| 新测试 | `tests/<领域>/`，同时接入该领域的 `run.cmd` |
| 新工具 | `tools/<分组>/`，并写进 `docs/dev-environment.md` 的索引 |
| 交接 | `work/handoff/HANDOFF.md` |

## 四、禁止

- 在仓库根目录、`tools/` 根目录、`tests/` 根目录放临时文件。
- 在 `exports/` 放文档或用户安装包；在 `dist/` 放临时脚本、分析材料或编译中间文件。
- 重新启用已退役的 `analysis/`、`.analysis-tools/`、`.handoff/`。
- 进 git 的文件把 `work/` 里的某个文件当作依赖或证据引用（本地文件在别人机器上不存在）。说明约定时提到 `work/` 的目录名可以。
- `git add -f`（pre-commit 会拦）。
- 把别的仓库整个克隆进本仓库目录。
- 合并完的工作树留着不收。

## 五、清理节奏

每周一次，或每次发版后：

1. `work/handoff/HANDOFF.md` 轮转：超过 7 天的段落移入 `archive/`。
2. 清空 `work/scratch/`。
3. 删除 30 天前且没有被 note 引用的 `work/logs/`。
4. `exports/` 可整个删除，需要时重建。
5. `git worktree list`：已合并分支的工作树用 `git worktree remove` 回收。
6. 运行 `python tools/dev/scan-mojibake.py`，处理乱码。

## 六、从旧布局迁移

旧布局的交接、调查、临时文档与第三方源码，按本页的目录表分类。迁移前先在本地保存清单，核对来源、最终目标及文件版本，并演练；遇到缺失来源或同名目标时先解决冲突。涉及私有资料的具体清单和操作步骤只留本地。嵌套工作树和带联结点的目录单独核查后处理。
