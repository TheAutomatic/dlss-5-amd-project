# RDNA3 回迁进度

日期：2026-10-28。移植从这里暂停。本文只记录已经落地的源码 pin，以及还没有做的产品工作。

当前分支不能给 RX 7000 或 RDNA3.5 APU 使用。设备创建仍要求 FP8，安装布局仍是一套 RDNA4 资源。

## 位置

| 项 | 值 |
|---|---|
| 实施工作树 | `C:\Users\Kenny\Downloads\dlssnr-rdna3-migration` |
| 分支 | `imi/rdna3-migration` |
| 分支起点 | `d59e3bdb`（v1.13.3.5） |
| 贡献源 | WSL `/home/kenny/src/DLSSNR-AMD-rdna3`，分支 `codex/rdna3-aco-port` |
| 贡献提交 | `c46a5edec8c89ca1c7645cde886335c345eebef0` |
| 上一产品 pin | `cc27501dc116bdcfbb43612e497c5ca12497574a` |
| 来源 | TheAutomatic/DLSSNR-AMD 上的贡献提交，不是 mochizuki0323 的正式 release |

主工作区 `C:\Users\Kenny\Downloads\dlssnr_on_amd_setup` 仍在 `main`。那里未提交的 `VERSION` 和 `tools/install/launcher/MainWindow.cs` 不在本分支上。

## 已完成

步骤 A 的 vendor 闭包已经接到固定提交。

- 从固定提交的 Git 对象取出闭包，再与当时的产品 vendor 做三方合并。冲突只出现在四个文件，并已解开：`windows/src/core/nrvk.hpp`、`nr_pipeline_binary.hpp`、`nr_runtime.hpp`、`nr_runtime.cpp`。
- 合并保留了产品侧的线程局部 ACO、inline compute、RADV 只编译、录制资源所有权和计时采集，也保留了贡献侧的运行时 `ctx.rdna3`、FP16 twin、alias 绑定、gfx11 PAL 和 NRPAL002。
- `tools/mochizuki-sync/check.py --upstream` 对该 WSL 仓库的结果是 `MOCHIZUKI_SOURCE_PIN_OK c46a5edec8c89ca1c7645cde886335c345eebef0 files=1029 added=9 patches=9`。
- 新增闭包包括 Linux/Windows `shaders/rdna3`、`arch/rdna3.sh`，以及 `windows/data/aco/rdna3` 和 `windows/data/aco/rdna4`。
- 平铺的 `windows/data/aco/records` 与 `windows/data/aco/rdna4/records` 是同一组 62 个 blob。两边的 `shell-aliases.txt` 不同：平铺 424 字节、9 行；`rdna4` 850 字节、22 行；`rdna3` 423 字节、9 行。
- 新数据体积：`linux/shaders/rdna3` 与 `windows/shaders/rdna3` 各 0.71 MiB、26 个文件；`windows/data/aco/rdna3` 12.00 MiB、227 个文件；`windows/data/aco/rdna4` 16.57 MiB、170 个文件。

有效补丁从 30 个收成 9 个。被删掉的补丁文件不是把行为删掉：同一文件上后一个补丁带着相对新 pin 的完整产品差异。`check.py` 证明「固定提交 + 这 9 个补丁 = vendor」。

| 仍在 manifest 中的补丁 | 现在携带的文件 |
|---|---|
| `07-drop-unused-up-shown.patch` | Linux/Windows `runtime_transfer.comp`、`runtime_upscale.comp` |
| `12-weights-only-extraction.patch` | `extract_model.sh`、`inspect_nr.py` |
| `14-plan-a-format-conversions.patch` | Plan A convert/unconvert |
| `19-inline-compute-convert-formats.patch` | Linux/Windows `runtime_alpha.comp` |
| `20-compute-motion-rescale.patch` | 两边 `build_network.py`、`runtime_mv_rescale.comp` |
| `26-exposure-continuity.patch` | exposure shader 与 `nr_model_profile.hpp` |
| `29-radv-driver-compile-policy.patch` | `nr_graph.cpp`、`nr_pipeline_binary.hpp`、`nr_shader_manifest.hpp`、`nrvk.hpp` |
| `31-renodx-recording-resource-retention.patch` | `nr_renodx.cpp`、`nr_renodx.hpp` |
| `32-recording-timing-harvest.patch` | `nr_runtime.cpp`、`nr_runtime.hpp` |

`29` 和 `32` 同时带着原先 01–06、08、10、11、13、15–18、21–23、25、27、28、30 里落在这些文件上的产品差异。恢复移植时不要按旧补丁号把这些文件再打一遍。

宿主常量除 `-DNR_ARCH_RDNA3=1` 外，`arch/rdna3.sh` 与 `arch/rdna4.sh` 相同。Windows 核心用 `ctx.rdna3` 做运行时选择，不靠把整个 DLL 编成只含一种架构。产品构建脚本还没有改成这样编。

## 未完成

| 步骤 | 状态 | 下一步要动的位置 |
|---|---|---|
| B 适配器 | 未开始 | `MochizukiNrRuntime.cpp` 的 `Vulkan::Create` / `AdoptVkd3d` 仍强制 `shaderFloat8` 和 `VK_EXT_shader_float8`。`MochizukiVulkanDevice.h` 的扩展掩码同样。`ConfigureAco` 仍指向平铺的 `dlssnr-amd/aco`。`HostDevice::rdna3` 和 `RuntimeConfig::rdna3` 没有从游戏 LUID 写入。 |
| C 资产与构建 | 未开始 | `tools/build/build-mochizuki-runtime.py` 仍只构建 RDNA4，输出平铺 `shaders/` 和 `aco/`。`tools/build/mochizuki-aco.py` 只认 `NRPAL001`。`tools/install/mochizuki-owned.py` 仍要求恰好 62 条平铺 record。 |
| D 验证 | 未开始 | 没有编 `MochizukiNrRuntime.dll`，没有跑三个 bundle 校验、`tests\mochizuki\run.cmd abi`，也没有 780M / 9070 XT 短测。 |
| E 试包 | 未开始 | 没有 `dist/` 测试包，没有安装、升级、卸载演练，没有发布。 |

贡献分支里的独立 Linux host、PE、微测工具和临时安装脚本没有产品消费者，没有另做一套宿主。`windows/src/pe` 不在本产品的编译列表里。

## 恢复时的顺序

1. 在本工作树继续，不要改主工作区那些未提交文件，也不要把 pin 自动跟到贡献分支的新 HEAD。
2. 设备创建按游戏适配器 LUID 决定 FP16 或 FP8，只对 RDNA4 要求 FP8。真实 AMD ICD 继续走已有的 `nr::driver::amd_reported`。
3. 一次编译 DLL。RDNA3 与 RDNA4 的 shader、record、shell 分目录安装。RDNA4 INT4 留在 RDNA4 bundle。不要把 RDNA4 INT4 record 装到 RDNA3。
4. 定向验证通过后再打本地试包。试包不是发布。
