# 发布相关测试入口

发版流程、打包命令、模块消费者清单与 Actions 故障处理统一见
[docs/release.md](../docs/release.md)。本页只说明测试怎样运行和覆盖什么。
工作目录为仓库根目录。

## 统一入口与范围

```cmd
tests\run-all.cmd --tier ci
```

| tier | 覆盖 | 运行条件 |
|---|---|---|
| `ci` | host/config、shader、mochizuki ABI、lmxxf ABI/C 冒烟/WARP、安装卸载、模块打包、上传回归、同步工具及 Git 二进制往返 | 无独立 GPU 要求；Windows/MSVC/WARP |
| `device` | host graphics 与实际 D3D12 列表/proxy 测试 | D3D12 硬件 |
| `gpu` | mochizuki Vulkan 输出/重放/跨队列/分辨率/取消（自备模型）、lmxxf 实际 HIP、格式/曝光、输出黄金哈希、录制租约、bridge 与故障回归 | AMD GPU，`LMXXF_ASSETS` 指向有效权重目录 |
| `all` | 上述全部 | 满足全部依赖 |

可组合 `--tier ci,device`，可用 `--out exports/test-run` 指定结果目录。
`tools/build/build-release-local.cmd` 已跑 `ci,device`，无需在完整构建前再重复运行。

成功的完整 CI 会生成 `runtime-ci.sha256` 并确认测试期间 DLL 未改变。
失败或 `--skip-sync` 不生成凭证；单跑 ABI 也不生成完整 CI 凭证。
`LMXXF_TEST_RUNTIME` 可以指定待测 DLL，发布验证必须核对它的真实路径与哈希。
不要把分项通过或 `--fast` 编译当成完整发版验证。

## 何时必须跑

- 日常修改先审最终 diff，按实际风险运行受影响领域的专项；不因目录或文件名触发全套。
- 发版前对最终产物完整运行一次 `--tier ci`，加适用的 device/GPU 验证；完整构建入口已包含 ci,device，不重复执行。
- 增删模块：先核对模块契约及消费者，运行相关专项；发版时纳入完整 CI。
- 仅文档/注释变化：可跳过编译和运行回归，检查命令、路径及链接。
- 游戏、显卡不可用时明确记 SKIP；不把模拟、WARP 或别的型号测试等同该游戏/硬件实测。

## 分项定位失败

每个领域的 `run.cmd` 是该领域入口。修复后先重跑受影响专项；最终发版仍需完整 CI 凭证绑定实际 runtime。不要删除断言或跳过失败的领域。

```cmd
tests\host\run.cmd ci
tests\shader\run.cmd
tests\lmxxf\run.cmd abi
tests\lmxxf\run.cmd warp
tests\lmxxf\run.cmd device
tests\lmxxf\run.cmd gpu
tests\install\run.cmd
tests\sync\run.cmd
tests\mochizuki\run.cmd abi
tests\mochizuki\run.cmd gpu
```

ABI 入口包含 C++、C 和 Python runtime 校验，覆盖当前 ABI v2 函数表边界、旧 ABI 拒绝与模块状态。
同步入口使用临时仓库和测试模块，验证补丁、完整性、失败退出与字节往返；
它不等于已经完成真实上游集成审阅。后者仍按
[同步说明](../tools/lmxxf-sync/README.md) 执行。

GPU tier 的 EXACT/AE/R10 黄金哈希用于固定输入回归，不证明游戏帧率或整个游戏矩阵。
需要性能结论时按 [测量规则](../docs/measurement.md) 记录实际配置与产物身份。
