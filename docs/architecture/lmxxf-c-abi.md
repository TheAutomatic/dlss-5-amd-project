# lmxxf C ABI（`LmxxfNrRuntime.dll`）

宿主 `OptiScaler.dll` 通过纯 C ABI 调用 `LmxxfNrRuntime.dll`。头文件是 `OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h`，改它之前先读完本页。上游 pin、sync 流程和 OURS/FOLLOW 划分见 [third_party/lmxxf/UPSTREAM.md](../../third_party/lmxxf/UPSTREAM.md) 与 [tools/lmxxf-sync/README.md](../../tools/lmxxf-sync/README.md)，本页不重复。

## 为什么有 DLL 边界

| 理由 | 说明 |
|---|---|
| 工具链隔离 | 宿主用 MSVC 编译。runtime 由 `tools/build/build-lmxxf-runtime.cmd` 编译：有 `cl.exe` 时走 MSVC，否则回退 MinGW（`LMXXF_GXX`）。两种编译器都可能产出 runtime，所以边界上不能有 C++ ABI：不传 STL、异常或 CRT 分配的对象 |
| 许可证防火墙 | OptiScaler 是 GPL-3.0；runtime 包着的上游代码是 MIT（`third_party/lmxxf/LICENSE`）。两者通过动态加载的 C 接口相接 |
| 崩溃隔离 | runtime 内部异常由 `GuardSession` 捕获并转成返回码，不会跨边界抛出；宿主拿到失败码后回退到不做 NR 的原始 Color |

## 加载与资源查找

| 项 | 规则（`LmxxfBackend.cpp`） |
|---|---|
| runtime | `OptiScaler.dll` 同目录的 `LmxxfNrRuntime.dll`，唯一导出 `LmxxfNrGetApi(abi_version, LmxxfNrApi*)`；调用前宿主填 `struct_size = sizeof(LmxxfNrApi)` |
| modules | 环境变量 `LMXXF_MODULES_DIR` 优先，否则同目录的 `lmxxf-modules/` |
| 权重 | 依次：同目录 `native-game-tiled-assets/`、同目录 `lmxxf-weights/`、同目录 `lmxxf-weights-dir.txt` 里的一行路径、已有的环境变量 `LMXXF_WEIGHTS_DIR`（路径不存在则忽略）。找到后写回 `LMXXF_WEIGHTS_DIR` 给 runtime 用。注意是 tiled assets，不是旧的 `HIP/` 目录 |
| FitLarge | 宿主把 ini 的 `LmxxfFitLarge` 写成进程环境 `DLSS5_FIT_LARGE`；runtime 另读 `DLSS5-AMD/native-game-flags.txt`。两者由安装器对齐，见 [installer.md](installer.md) |

## 版本协商

`LMXXF_NR_ABI_VERSION`（当前 `1`）**只管函数表** `LmxxfNrApi` 的布局。结构体靠各自的 `struct_size` 分档协商，不升版本号。原因：版本号一升，老 runtime 直接拒绝 `GetApi`，NR 整个起不来；按 `struct_size` 分档，老宿主配新 runtime 只是少几个功能。

`LmxxfNrFrameInfo` 在 d788963 有四档，runtime 的 `PrepareFrame` 接受其中任意一档：

| `struct_size` | 止于 | 含义 |
|---|---|---|
| `64` | `color_state` + `flags` | 最早的布局 |
| `LMXXF_NR_FRAME_INFO_V1_SIZE` = 80 | `model_scale` | 力度、debug view、model scale |
| `LMXXF_NR_FRAME_INFO_EXPOSURE_SIZE` = 104 | `exposure_scale`（其后 4 字节原为尾部填充） | + `exposure` / `exposure_state` / `pre_exposure` / `exposure_scale` |
| `sizeof(LmxxfNrFrameInfo)` | `paper_white` | + `reserved_after_exposure`（置 0）+ `paper_white`（缺省按 1） |

其它结构（`LmxxfNrCapabilities`、`LmxxfNrCreateInfo`、`LmxxfNrJob`）目前要求 `struct_size` 精确相等。`LmxxfNrCreateInfo.flags` 的未知位会被拒绝。

## 四条演进规则

1. **只往结构体尾部加字段。** 超出宿主 `struct_size` 的字段一律视为未提供，runtime 用默认值。
2. **尺寸常量用 `static_assert` 钉到 `offsetof`**（见 `LmxxfNrRuntime.cpp` 顶部），不留裸数字。
3. **宿主要能降级。** 当前实现：`PrepareFrame` 因 `struct_size mismatch` 返回 `INVALID_ARGUMENT` 时，宿主改用 V1 尺寸（80）重试一次并关掉自动曝光标志（`LmxxfBackend` 的 `frameInfoV1`）；`Create` 因 flag 被拒时去掉 flag 重试（此时没有零输出回退）。宿主不会退到 104 这一档。
4. **输入契约违反返回 `INVALID_ARGUMENT`，不要 throw。** `GuardSession` 见到异常就把 session 标成 `failed`，之后每个调用都返回 `UNAVAILABLE`（「session is poisoned」）。RE9 当初就是这样：不支持的输入格式抛了异常，session 再也没恢复，日志也看不出原因。现在契约提前检查，报出具体属性（`fmt= … WxH … fitLarge=`），可重试。宿主对 `INVALID_ARGUMENT` 不重建 session，因为重建治不了不支持的纹理。

## 其它约定

- **`hip_ready` 已从 `LmxxfNrCapabilities` 删除**（它一直是 0，宿主不读）。OptiScaler 宿主不调用 `QueryCapabilities`，所以游戏内无影响。但上游仓库存档分支的头文件里仍有这个字段：拿本仓库的 `tests/lmxxf/lmxxf_nr_gpu.cpp` 去测上游 runtime，`QueryCapabilities` 会因尺寸不符失败，需要用上游头文件重新编译测试。
- `gfx1201_target` 字段已标 DEPRECATED，不反映实际架构；实际架构用 `GetStatus` 查询。
- `LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK`：HIP enqueue 或队列契约失败时，runtime 把私有输出清零后返回 OK，本帧显示原始 Color。宿主连续 10 次（`kMaxConsecutiveRecoveries`）恢复后关闭该 session 的 NR。其它队列上的读者要由调用方自己同步，契约写在头文件注释里。
- Session 建立失败按 2、4、8… 次 Record 指数退避，上限 600 次（60 fps 下约 10 s）。
- 帧状态机见头文件 `LmxxfNrJobState`；游戏丢弃未提交的 command list 时，宿主调用 `CancelUnsubmitted` 把 job 退回，避免 bridge 卡在「已录未提交」而触发重建和显存泄漏（燕云切档 21.6 GB 显存的根因）。
- `LmxxfNrRuntime.cpp` / `LmxxfNrApi.h` / `LmxxfProductionOptions.h` 不在 sync 清单里，从上游到本仓库、从本仓库到上游都不会自动同步；上游若改了它们，我们不会自动知道。

## 改 ABI 时的验证

- `tests/lmxxf/lmxxf_nr_abi.cpp`、`tests/lmxxf/lmxxf_zero_fallback_abi.c`（C 冒烟）：`tests/lmxxf/run.cmd`，CI 也跑。
- `tests/lmxxf/lmxxf_nr_gpu.cpp --reject-formats` 会用 ABI v1 的 `struct_size` 跑一帧，证明老宿主还能用。
- 无卡回归见 [tests/RELEASE-TESTS.md](../../tests/RELEASE-TESTS.md)。
