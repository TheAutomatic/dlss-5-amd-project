# Offline network benchmarks

便携包用户：解压后双击 `Run_All_Benchmarks.bat`，选一次 GPU 即可；回传结束时生成的结果 ZIP。以下是维护者的运行、配置与打包说明。

独立开发工具，不属于 Mochizuki PR 源分支，也不参与产品安装。脚本、配置、探针源码进 Git；模型、后端 EXE/SPIR-V/records 和结果留在外部数据目录。运行端仅需 Windows PowerShell 5.1 与对应驱动，不需要 Python/WSL。

## 复用现有数据

`DataRoot` 指向已解压的双后端 v1.2 测试包；路径由参数传入，不绑定某位用户的目录。默认只跑 ACO Small/Original：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/run.ps1 -DataRoot D:/Bench/v1.2 -PlanOnly
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/run.ps1 -DataRoot D:/Bench/v1.2 -Gpu 0 -ResultsRoot exports/benchmark-results
# ACO / LLPC 对比
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/run.ps1 -DataRoot D:/Bench/v1.2 -Profile tools/benchmark/profiles/aco-vs-llpc.json -Gpu 0
```

不指定 `Gpu` 时，多卡只选一次；没有型号/驱动白名单。现有两份配置使用的是 **RDNA3/3.5 数据及独立图程序**，选择 RDNA4 不会自动将它们转换成 RDNA4 测试。

`-Smoke` 只跑配置第一档/第一模型、2 帧预热+3 帧测量+1 轮；TXT 会写出真实参数。默认配置为 648p/720p/1080p/1440p/4K，8 帧预热、20 帧测量、三轮交错。默认结果在 DataRoot/results/，可通过 ResultsRoot 放进仓库 exports/。

运行前关闭游戏和其它 GPU 测试。一次只安排一个 owner；不并行启动本工具。不会更改用户级环境变量、驱动、游戏或产品配置。超时只停止自有子进程，普通测试失败继续其余项，最终返回非零；中断/启动失败保留已有日志。结果自动生成回传 ZIP。

## 场景配置

schema 1 配置包含：resolutions、models、modes、warmup、frames、rounds、timeoutSeconds。

- `models`：id、模型 file、plan；plan 中 `{width}`/`{height}` 会替换。
- `modes`：id、label、exe、shaders、可空的 aco、source、extraArgs。所有文件/目录路径相对 DataRoot。
- `source` 是配置作者填写的源码版本；实际输入 SHA256 另存，换二进制/配方后应更新版本说明。
- `extraArgs` 追加 nr_graph 参数；运行器面向 nr_graph CLI，不是任意 ASI/游戏宿主框架。
- `referenceMode` 可空；设为某模式 ID 后，只对相同模型/分辨率的完整有效轮次计算其它模式的耗时降低和吞吐比。

新增 ACO 版本/配方可增加 modes 并使用各自子目录；无需改执行循环。比较 Original/Small 时分列显示，不把不同模型的性能比当作同等画质提升。其它图输入可通过 extraArgs 指定 `--in-image input.f32`，并在顶层 `extraFiles` 数组列出 `input.f32`；其哈希和便携包复制也会自动覆盖。extraFiles 既支持文件也支持目录。

## 构建探针和组便携包

已有 v1.2 的 gpu_probe.exe 可继续使用；重建仅需现有 MSVC、Vulkan headers 与仓库 Vulkan import lib：

```cmd
tools\benchmark\build-probe.cmd D:\Vulkan-Headers\include exports\benchmark-bin
```

将产出的 gpu_probe.exe 放入 DataRoot。工具只枚举属性，不执行网络。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/package.ps1 -DataRoot D:/Bench/v1.2 -OutDir dist
# 带 LLPC 的包
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/package.ps1 -DataRoot D:/Bench/v1.2 -Profile tools/benchmark/profiles/aco-vs-llpc.json -OutDir dist -Name RDNA3-Comparison
```

打包器只复制配置需要的模型、计划、后端、shader/record 目录及脚本，不复制旧结果、临时源码或宿主安装器。每次新建时间戳目录/ZIP，不覆盖历史包。包内双击 Run_All_Benchmarks.bat 即可；数据仍由使用者提供，本工具不下载模型。更换后端时携带对应许可证。

## 结果与检查

汇总 TXT **直接包含显卡型号和驱动**、UUID/LUID、CPU/内存/OS、电源计划、配置版本、真实轮次与失败状态。另有 system/profile/input-manifest JSON、每次命令、stdout/stderr、journal、CSV 和 summary JSON。记录 GPU 整图时间，排除冷编译/权重加载/输出读回；不等于游戏 FPS。没有实时频率/温度/功耗采样；图输出非零、退出成功也不代表画质验收。

输入哈希来自实际文件；DataRoot 含 package-manifest.json 时同时检查相应已声明输入的哈希。回传前可以检查 inheritedNrEnvironment 中自己的自定义值。

```cmd
tests\benchmark\run.cmd
```

该测试使用假后端验证失败/超时保留、交错顺序、结果统计、包重定位，不启动 GPU。真实图程序和 shader 的生成仍由对应项目的固定版本配方负责，本目录不维护另一套核心编译工程。

