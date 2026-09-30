# lmxxf 私有着色器编译器

所有 lmxxf codec、RGB helper 和 runtime exposure meter 通过
`third_party/lmxxf/src/native_shader_cache.h` 使用私有编译器。
它从 GetSystemDirectoryW 构造绝对 d3dcompiler_47.dll 路径，验证实际模块路径、
三个函数的所属模块以及版本资源。初始化由 C++ 局部 static 保证并发安全；
成功加载的模块保留至进程结束，避免返回 blob 或并发调用遇到卸载。
加载或验证失败返回诊断，不回退到游戏导入表，也不改变全局 DLL 搜索规则。

codec 默认 cs_5_1，meter 保持 cs_5_0。只有明确的
`error X3506: unrecognized compiler target 'cs_5_1'` 才允许重试 cs_5_0，
且限定 main 入口和通过测试的 codec/RGB 宏组合。其他编译错误不降级；
重试结果保留两次 HRESULT 和错误文本。PSO 创建错误由原调用者处理。

cache v2 包含编译器路径、版本、文件大小/摘要、实际 target、flags、源码、
入口、宏，以及可快照 include 的文件名和内容。磁盘文件保存完整 key、
payload 长度和校验和，以临时文件原子替换；旧缓存、截断、损坏和 key 不符均为 miss。
未知或嵌套 include 使用私有 D3DCompileFromFile，保留相对路径解析行为。

正式补丁为 `tools/lmxxf-sync/patches/shader-compile-system32.patch`；
重放输入来自 fixtures/snapshot.json 记录的原始上游头，不反向生成。

验证入口：`tests/lmxxf/shader-compiler.cmd`，已接入 WARP/CI tier。
测试预载同名模拟游戏 DLL，并发首次内存/文件编译、进程间冷/热缓存、
加载/符号失败、精确 target 错误、缓存身份/损坏和中文/空格/嵌套 include。
136 个生产 shader/target 变体各执行三组 WARP 场景，对照两个 target 的实际输出；
RGB untiled 同时读取 post_base，避免比较未写入的 tiles。
这些自动化证据不等于特定游戏安装环境的实测。
