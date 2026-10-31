# 游戏原生 RR 与移植 FSR-RR 的 provider 隔离

来源：neural-amd-opti 提交 `2b38b3277336ad72be061435a4a3fd4df478c72a`。
其修复将游戏原生 FidelityFX denoiser 调用送回游戏自带 DLL，避免原生 1.1 描述符进入移植 RR 使用的 1.2 provider。本项目采用该分流意图，并调整归属、加载顺序和失败处理。

## 当前规则

- 游戏通过 FFX 输入层创建原生 denoiser context 时，优先选择游戏 loader 目录的 denoiser；没有已知游戏 loader 时查找 exe 目录。显式加载请求在被 OptiScaler 重定向之前保留原始目录。
- 普通 SR 创建不触发原生 denoiser 查找。只缓存成功加载的模块，不永久缓存第一次未找到的结果；后续发现游戏 loader 后可重新选择目录。
- 每个成功创建的 context 保存具体 provider。configure/query/dispatch/destroy 按该归属路由，不因后续目录变化改用别的 DLL。
- 归属表与成功模块缓存有线程保护，provider 调用和 DLL 加载/释放不持有这些锁。调用方仍须遵守同一 context 的使用和销毁顺序。
- destroy 成功后才删除原始 handle 的归属；失败保留，允许重试。已加载 provider 的 DLL 引用保留到进程退出。
- 游戏 DLL 存在但加载或导出校验失败时返回 `FFX_API_RETURN_NO_PROVIDER`，不尝试用另一套 ABI 接收该调用。未找到独立游戏 DLL或它就是当前 provider 时，保留原有代理路径。
- 移植 FSR-RR 仍从 `FfxApiProxy::D3D12_*` 直接调用产品 provider；本改动不降低其 1.2 ABI 要求，也不修改产品 runtime 的包兼容规则。

## 验证与边界

`tests/host/run.cmd ffx-native-source` 是无需编译的源码接线检查，检查原生调用分流、成功创建归属、失败闭锁及延迟 loader 登记顺序；它不证明 Windows DLL 加载或 ABI 可用。

`tests/host/run.cmd ffx-context-owners` 是需要编译的 CPU 测试，覆盖并发独立 context、provider 归属、销毁失败重试、成功清理及 provider 回调重入；同时接入 host `ci`。

本次按要求未编译或出包。仍须运行上述 C++ 测试，并在后续构建后验证游戏原生 RR、移植 RR 同时存在、子目录 loader、先 SR 后 RR 以及缺少导出的实际 DLL 场景。源码检查不能替代这些验证。

本修复不改变 RR 常量缓冲槽的复用或 GPU 退休策略，不能作为 PBZ `constant slot busy` 的修复证据。
