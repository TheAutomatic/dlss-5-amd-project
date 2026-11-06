# D3D12 Direct ELF (ASD) vs. Vulkan Bridge — Performance & Optimization Reference

This document summarizes the architecture, zero-copy optimizations, unified logging, and reproducible 4K (`3840x2160`) benchmark results comparing the **Native D3D12 Direct ELF (ASD)** route against the **D3D12↔Vulkan External-Memory Bridge** route on RDNA 4 (`AMD Radeon RX 9070 XT`).

---

## 1. Key Findings & Why Earlier Comparisons Were Misleading

When comparing the native D3D12 Direct ELF route (`nr_d3d12_asd_backend.cpp` + `nr_renodx_d3d12.cpp` + `nr_d3d12_asd_net.cpp`) against the Vulkan Bridge route (`nr_pe_session.cpp` + `nr_renodx.cpp` + `nr_runtime.cpp`), three factors previously skewed measurements:

1. **Inconsistent Log Metrics**:
   - The Vulkan route logged `[nr] evaluate #1 (D3D12, N) ... XX.XX ms (network XX.XX, route X.XX) -> ran` in `dlssnr-amd.log` (measuring total RenoDX stage + network GPU time).
   - The Direct ELF route previously bypassed `log_evaluate()` in [`nr_dlssnr_model.cpp`](windows/src/pe/nr_dlssnr_model.cpp) and only logged `[asd] timing: ... GPU avg XX.XX ms` in [`nr_d3d12_asd_net.cpp`](windows/src/pe/nr_d3d12_asd_net.cpp) (pure network + alpha pass, excluding the RenoDX stage).
   - Furthermore, on frame 0 (`first == true`), `nr_d3d12_asd_net.cpp` records a one-time `CopyBufferRegion(b->wgt, 0, b->wgt_upload, ...)` (~27 ms over PCIe) *outside* the network's own timestamp query pair, but *inside* `renodx_frame`'s outer stage timestamp query pair, which polluted the 32-frame stage moving average (`g_stage_avg_ms`) for the first 32 frames.

2. **Redundant Full-Screen 4K Copies ("冤枉路") in `nr_renodx_d3d12.cpp` & `nr_d3d12_asd_backend.cpp`**:
   - On Vulkan (`nr_renodx.cpp`), `proxy` (`VkImage`) and `lin_proxy` (`VkBuffer`), as well as `neural` (`VkImage`) and `lin_neural` (`VkBuffer`), alias the **same `VkDeviceMemory`** when pitch matches (`rowPitch == NW * 8`), so zero copies occur between the RenoDX stage and the ACO network.
   - On D3D12 (`nr_renodx_d3d12.cpp`), `proxy`/`neural` were separate `TEXTURE2D` resources and `lin_proxy`/`lin_neural` were `BUFFER` resources, causing per-frame `CopyTextureRegion` transfers across both SDR and HDR:
     - **SDR (`legacy`)**:
       1. Unconditional `CopyTextureRegion(f.colour -> original)` (33 MB at 4K `R10G10B10A2_UNORM`) even when `sdr_restore`, `look`, and `debug_view` were off and the target supported typed UAV loads.
       2. `proxy` texture write followed by `CopyTextureRegion(proxy -> lin_proxy)` (66 MB at 4K `RGBA16F`).
       3. `CopyTextureRegion(lin_neural -> neural)` (66 MB at 4K `RGBA16F`) followed by `neural` texture read.
       - **Total redundant SDR bandwidth eliminated**: **165 MB/frame** at 4K.
     - **HDR (`v6`)**:
       1. `proxy` texture write followed by `CopyTextureRegion(proxy -> lin_proxy)` (66 MB at 4K `RGBA16F`).
       2. `CopyTextureRegion(lin_neural -> neural)` (66 MB at 4K `RGBA16F`) followed by `neural` texture read.
       3. Missing `w0_frame` fast path: when `f.colour != f.output` and the input is already linear `RGBA16F` (`encoding == 1u && !prep_on`), `w0` now aliases `in_src` directly, skipping the full-screen 4K `kLinearize` pass.
       - **Total redundant HDR bandwidth eliminated**: **132 MB/frame** (plus 66 MB/frame when `w0_frame` applies).
     - **SDR `sdr_restore` (`kDecodeReduceL`)**:
       - When `sdr_restore` is active without `shaped`/`transport`, `nr_renodx_d3d12.cpp` now dispatches the fused `kDecodeReduceL` PSO instead of separate full-screen 4K `kDecodeL` + `kReduceL` dispatches.
     - **Non-RenoDX (`wilsjo2`) D3D12 Path (`nr_d3d12_asd_backend.cpp`)**:
       - `to_linear`, `from_linear`, and `to_motion` previously used `StructuredBuffer<float4>` (`RGBA32F`, 16 B/px) while the RenoDX path used `RGBA16F` (8 B/px). Upgrading `nr_d3d12_asd_backend.cpp` to pack/unpack `f16x4` (`StructuredBuffer<uint2>`, 8 B/px) cuts `wilsjo2` buffer memory and bandwidth in half (**saving 198 MB/frame** at 4K).

3. **Benchmark Harness Upload Pitfall (`test_d3d12_offline.cpp`)**:
   - Seeding the 4K test texture from a `D3D12_HEAP_TYPE_UPLOAD` buffer inside the per-frame command list adds ~5.1 ms of PCIe bandwidth overhead per frame, which runs *in parallel* with the Vulkan queue submission on the bridge route (hiding the PCIe copy time in wall-clock wait) while serializing on the single D3D12 queue in Direct ELF.
   - Pre-uploading once to a VRAM-resident `D3D12_HEAP_TYPE_DEFAULT` texture (`init_tex`) and seeding `target` via VRAM-to-VRAM `CopyResource(target, init_tex)` isolates the true GPU execution cost.

---

## 2. Benchmark Results (RX 9070 XT, `3840x2160`, `CC_SILVER_AARDWOLD_OPTIMIZED` `ultra performance`)

All measurements below are averaged over **55 steady-state frames** (after 5 warmup frames) using [`windows/tools/test_d3d12_offline.cpp`](windows/tools/test_d3d12_offline.cpp) at `3840x2160` (`RenoDX Present` stage, 123 merged dispatches).

### A. SDR (`legacy`, `DXGI_FORMAT_R10G10B10A2_UNORM`)
- **Output Verification**: `100.00%` pixels modified by NR, `mean |diff| = 0.02163`, `black pixels = 0.00%` (bit-for-bit match between Direct ELF and Vulkan Bridge).

| Metric | Direct ELF (Before Opt) | **Direct ELF (Zero-Copy)** | Vulkan Bridge | **Direct ELF vs. Vulkan Bridge** |
| :--- | :---: | :---: | :---: | :---: |
| **D3D12 Queue End-to-End GPU Span** | 19.222 ms | **18.440 ms** (min 18.055) | 18.572 ms (min 18.233) | **Direct ELF is 0.132 ms faster** (-0.782 ms vs before) |
| **Submit + Fence Wait Wall Time** | 19.532 ms | **18.743 ms** | 18.869 ms | **Direct ELF is 0.126 ms faster** |
| **Pure Network GPU Avg (Internal)** | 17.995 ms | **17.952 ms** | 17.796 ms | +0.156 ms (123 D3D12 dispatches vs Vk) |
| **RenoDX Stage Overhead (Internal)** | 0.903 ms | **0.498 ms** | 0.178–0.459 ms | **Saved 0.405 ms of stage copy overhead** |
| **CPU `evaluate_d3d12` Recording Time** | 0.307 ms | **0.283 ms** | 0.638 ms (spikes to 1.52 ms) | **Direct ELF is 2.25x faster (-0.355 ms)** |

### B. HDR (`v6`, `DXGI_FORMAT_R10G10B10A2_UNORM` HDR10 PQ)
- **Output Verification**: `100.00%` pixels modified by NR, `mean |diff| = 0.01515`, `black pixels = 0.00%` (bit-for-bit match between Direct ELF and Vulkan Bridge).

| Metric | **Direct ELF (Zero-Copy HDR v6)** | Vulkan Bridge (HDR v6) | **Direct ELF vs. Vulkan Bridge** |
| :--- | :---: | :---: | :---: |
| **D3D12 Queue End-to-End GPU Span** | **18.361 ms** (min 17.994, max 18.487) | 18.694 ms (min 18.309, max 18.852) | **Direct ELF is 0.333 ms faster** |
| **Submit + Fence Wait Wall Time** | **18.731 ms** | 18.990 ms | **Direct ELF is 0.259 ms faster** |
| **Pure Network GPU Avg (Internal)** | **17.596 ms** | 17.700 ms | **Direct ELF is 0.104 ms faster** |
| **RenoDX Stage + Network GPU Avg** | **18.382 ms** | 18.068 ms (+ ~0.62 ms D3D12↔Vk semaphore/copy) | End-to-end faster on D3D12 queue by **0.333 ms** |
| **CPU `evaluate_d3d12` Recording Time** | **0.288 ms** | 0.639 ms (spikes to 1.20 ms) | **Direct ELF is 2.22x faster (-0.351 ms)** |

---

## 3. Reproducing the Offline Benchmark (`test_d3d12_offline.cpp`)

You can build and run [`windows/tools/test_d3d12_offline.cpp`](windows/tools/test_d3d12_offline.cpp) without launching a game:

```powershell
$cxx = "C:\msys64\ucrt64\bin\x86_64-w64-mingw32-g++.exe"
$wtd = "C:\Users\Kenny\Downloads\wt-dx12-passthrough"
$vk_inc = "C:\Users\Kenny\Downloads\DLSSNR-AMD-mochizuki\toolchain\Vulkan-Headers\include"
$out = "$wtd\artifacts\windows\asi"
$flags = @("-std=c++17", "-O2", "-DNDEBUG", "-DNR_INT4=1", "-I$vk_inc", "-I$wtd/windows/src", "-I$wtd/windows/src/core", "-I$wtd/windows/src/pe", "-I$out")
$libs = @("-ld3d12", "-ld3d11", "-ldxgi", "-lgdi32", "-luuid", "-lpsapi", "-lshlwapi", "-ldwmapi", "-lversion", "-lole32")

$testObjs = Get-ChildItem "$out\*.o" | Where-Object { $_.Name -ne 'asi_main.o' } | ForEach-Object { $_.FullName }
& $cxx @flags "$wtd/windows/tools/test_d3d12_offline.cpp" @testObjs -o "$out/test_d3d12_offline.exe" @libs

# Copy to game plugin folder (where dlssnr-amd-asd/ and dlssnr-amd/ reside) and run:
# 1. SDR Direct ELF vs Vulkan Bridge (60 frames):
& "$pbz\OptiScaler\plugins\test_d3d12_offline.exe" "$pbz" 60
& "$pbz\OptiScaler\plugins\test_d3d12_offline.exe" "$pbz" 60 --vulkan

# 2. HDR v6 Direct ELF vs Vulkan Bridge (60 frames):
& "$pbz\OptiScaler\plugins\test_d3d12_offline.exe" "$pbz" 60 --hdr
& "$pbz\OptiScaler\plugins\test_d3d12_offline.exe" "$pbz" 60 --hdr --vulkan
```
