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
| runtime | `OptiScaler.dll` 同目录的 `LmxxfNrRuntime.dll`，主入口 `LmxxfNrGetApi(abi_version, LmxxfNrApi*)`；调用前宿主填 `struct_size = sizeof(LmxxfNrApi)` |
| modules | 环境变量 `LMXXF_MODULES_DIR` 优先，否则同目录的 `lmxxf-modules/` |
| 权重 | 依次：同目录 `native-game-tiled-assets/`、同目录 `lmxxf-weights/`、同目录 `lmxxf-weights-dir.txt` 里的一行路径、已有的环境变量 `LMXXF_WEIGHTS_DIR`（路径不存在则忽略）。找到后写回 `LMXXF_WEIGHTS_DIR` 给 runtime 用。注意是 tiled assets，不是旧的 `HIP/` 目录 |
| FitLarge | 正式 ini 键是 `[DlssNr] DLSS5_FIT_LARGE`，宿主通过同名环境变量传给 runtime；flags 只补宿主未设置的键，安装器不再把 ini 复制到 flags。旧键仅用于配置迁移，见 [installer.md](installer.md) |

## 整包 ABI 契约

宿主与 runtime 随整包覆盖安装，只接受当前 `LMXXF_NR_ABI_VERSION=2`、176 字节函数表和
112 字节 FrameInfo。旧 ABI v1、旧函数表及历史帧尺寸直接拒绝；宿主不再降级或去掉曝光重试，
版本不匹配时提示替换完整安装包。计时函数 `GetTimings` 放在本产品录制扩展之后，
不能直接把上游 v1 的函数表当成本产品 v2 使用。

`LmxxfNrTimings` 沿用上游 fe4d1d73 的 24 字节载荷：valid、network_ms、frame_id。
GetTimings 由渲染线程调用，首次请求才开启事件，返回最近完成的帧；UI 只读缓存。
产品的 SetEnabled(false) 会停止事件采集；当前 PDL 下事件区间不可靠，返回 valid=0。
其它结构的 struct_size 同样要求精确相等。C ABI 不传 STL 或 CRT 对象，未知 flags 拒绝。
输入契约违反返回 INVALID_ARGUMENT；所有权和 GPU 完成证明不因接口更新而放宽。

## 其它约定

- **`hip_ready` 已从 `LmxxfNrCapabilities` 删除**（它一直是 0，宿主不读）。OptiScaler 宿主不调用 `QueryCapabilities`，所以游戏内无影响。但上游仓库存档分支的头文件里仍有这个字段：拿本仓库的 `tests/lmxxf/lmxxf_nr_gpu.cpp` 去测上游 runtime，`QueryCapabilities` 会因尺寸不符失败，需要用上游头文件重新编译测试。
- `gfx1201_target` 字段已标 DEPRECATED，不反映实际架构；实际架构用 `GetStatus` 查询。
- `LMXXF_NR_CREATE_FLAG_ZERO_OUTPUT_FALLBACK` 用于独立串行诊断路径；与 `LMXXF_NR_CREATE_FLAG_RECORDING_LEASES` 互斥。产品宿主采用录制租约模式，执行错误会停止新 NR 录制，已提交资源继续保留到可证明安全。
- Session 建立失败按 2、4、8… 次 Record 指数退避，上限 600 次（60 fps 下约 10 s）。
- 录制的有效期由成功 Reset / 最终 Release 结束，GPU 完成与录制失效分别追踪；提交一次不会销毁可重放录制。见 [录制生命周期](lmxxf-recording-lifecycle.md)。v2 不使用旧 CancelUnsubmitted / Retire 接口回收。
- `LmxxfNrRuntime.cpp` / `LmxxfNrApi.h` / `LmxxfProductionOptions.h` 不在 sync 清单里，从上游到本仓库、从本仓库到上游都不会自动同步；上游若改了它们，我们不会自动知道。

## 改 ABI 时的验证

- `tests/lmxxf/lmxxf_nr_abi.cpp`、`tests/lmxxf/lmxxf_zero_fallback_abi.c`（C 冒烟）：`tests/lmxxf/run.cmd`，CI 也跑。
- `tests/lmxxf/lmxxf_nr_gpu.cpp --reject-formats` 检查旧 FrameInfo 尺寸被拒绝。
- 无卡回归见 [tests/RELEASE-TESTS.md](../../tests/RELEASE-TESTS.md)。
