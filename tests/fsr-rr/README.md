# FSR-RR 测试复用

从仓库根目录运行。Godot 是独立工程，当前不作为本套测试的验收依赖。GPU 工作串行；先确认没有游戏或其他 benchmark，碰到游戏时只停止自己的测试。

## 入口与选择

|目的|入口|能说明什么|
|---|---|---|
|相机、信号和配置契约|`tests\host\run.cmd fsrd-contracts`|相关 CPU 契约，不证明画质|
|离线 ABI/比较器|`tests\fsr-rr\run.cmd cpu`|ABI 和比较逻辑|
|构建离线运行器|下面的 build-only 命令|自动执行真实 C++ RGB 指标自检，不运行 GPU|
|原生 AMD 输入与转换路径|`--mode native-diffuse`|有限静态 Lambertian 合成场景，不是游戏原生 RR 或真实低采样 RT 验收|
|实际产品宿主|`fsrd_feature_smoke.cmd` 构建后单独执行|真实 DLL 经 NGX 的功能路径，不是画质认证|

只选与改动有关的检查，不因文档存在就全部运行。使用唯一输出目录，保留旧结果。

## 构建及 manifest

```cmd
tests\fsr-rr\run.cmd gpu exports\rr-check\runner --build-only
exports\rr-check\runner\offline_chain.exe --probe --manifest-out exports\rr-check\manifest.ini
```

运行前人工核对 manifest：adapter_luid、vendor、AMD denoiser/upscaler DLL 的绝对路径及 SHA256、信号设置、seed、帧数。探测模板不是已审核配置；移动工作树后必须重查路径，不能依赖旧 exports 的绝对路径。相同输入比较固定 provider/驱动、seed、相机、信号和采样，原件 manifest 随结果保存。

## 原生／转换对照（当前推荐起点）

短检查可设置 `warmup_frames=4`、`measured_frames=8`；它只覆盖12帧，不代表长期稳定。两种 clean/noisy 序列各自独立历史。

```cmd
exports\rr-check\runner\offline_chain.exe --mode native-diffuse --manifest exports\rr-check\manifest.ini --native-scene flat --out exports\rr-check\flat
exports\rr-check\runner\offline_chain.exe --mode native-diffuse --manifest exports\rr-check\manifest.ini --native-scene detail --out exports\rr-check\detail
```

- flat：有限平面、常量材质与照明，先检查输入与历史。
- detail：有限材质细节；目标 truth 每像素4×4无噪声采样。
- 原生信号由 renderer 独立生成 `E*max(NdotL,0)/pi`；与转换路径共享采样噪声及量化材质，不从转换器输出倒推。独立 RR/SR context，原生用独立 GPU pass 乘 albedo。
- 诊断用 `--native-signal-alpha 65504` 对照默认0；`--native-history reset-every-frame` 对照默认normal。每帧 reset 只是定位工具，不是产品修复或时域质量结论。
- 不同输入编码有量化差异，不要求所有纹理逐bit一致。

检查 `run.json` 的执行/设置/哈希；`frames/*/guide_comparison.json` 的实际尺寸、miss、越界、farSkip及量化误差；排除预热后分别比较 `native_presr/presr` 和 `native_sr/sr`；查看 `frames/` 实际图像而不只平均值。`segments.csv` 中 native pass 名称为 clean_native/noisy_native，串行带验证时间戳只作诊断，不是正式吞吐或游戏fps。

当前覆盖不包括反射、运动、遮挡变化和真实RT输入。既有有限场景两路接近，但两路共有轻微历史暗化，不把它自动归因为转换器或provider缺陷。不要据此宣传全面等同游戏原生RR。

## 旧／新版回归

```cmd
tests\fsr-rr\run.cmd gpu exports\rr-pair\runner --build-only --baseline-source <冻结源码zip或目录>
```

`--baseline-only` 必须同时提供 `--baseline-source`（包括 `--build-only`）；缺失时在构建或运行前非零退出，不报告 PASS。

分别用生成的 offline_chain_baseline.exe 与 offline_chain.exe，以相同 manifest、场景顺序和 seed 执行 `--mode serial --out <各自目录>`。随后：

```cmd
python -B tests\fsr-rr\compare_runs.py <baseline目录> <candidate目录> --policy <审核过的精确差异policy.json> --out exports\rr-pair\compare
```

policy 只许可明确的代际配置差异，不豁免质量或输入/provider漂移。`fixture_controls` 和 `feature_controls` 中完整、有序的逐帧 scene/pass/frame、specular_guide/diffuse_guide 绑定属于不可豁免的输入身份；即使 authored input 哈希及 forced-Direct 信号计划相同，实际 guide 绑定不同也不可比较。缺少逐帧绑定证据的旧结果必须重新生成。不要直接依赖 run.cmd 自动比较接受有意的代际设置变化。原生对照新增阶段不是普通 compare_runs 的输入。

指标必须声明 `rgb-radiance-v1;nonfinite=rgba`：颜色统计仅RGB，非有限检查仍RGBA。旧RGBA CSV必须重新生成，不得补标签冒充。noise_std 是真值残差，不是纯随机噪声；temporal MAD不是运动补偿闪烁。PSNR用参考亮度RMS归一化，不套固定峰值PSNR门槛。exit4表示质量门限未过，不等于某个产品shader根因已确定。

## 生产 DLL smoke

```cmd
tools\build\build-release-local.cmd --fast
tests\fsr-rr\fsrd_feature_smoke.cmd exports\rr-host-smoke exports\release-local\OptiScaler.dll <AMD provider目录> auto
exports\rr-host-smoke\fsrd_feature_smoke.exe --luid <HEX_LUID> --guided-mask 12
```

stage命令只构建/复制，不启动GPU。只在无游戏时单独启动consumer。检查stdout及stage内宿主日志；已知重建期间有失败及无新输出成功阶段，smoke的PASS不能抹去该风险。不要将测试DLL部署游戏作为默认步骤。

## 归档与恢复

正式代码和本文进git；结果留主树exports，本地调查留work/notes。迁移结果保留原始manifest/哈希和旧路径作为来源证据；复测需新建manifest与输出目录，不能改写历史证据。具体本地迁移目录见主树work/notes中的归档清单，不是运行时依赖。
