# Official AMD FidelityFX Denoiser (FSR Ray Regeneration) 1.2.1

Extracted unmodified from the official AMD FidelityFX SDK repository:

- **Repository**: `https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git`
- **Release Tag**: `v2.4.0`
- **Commit**: `7539ae8b6568ad29ed78a753404351daf3b5cca7` (`AMD FSR SDK 2.4.0`)
- **Previous Submodule Pin**: `v2.3.0` (`60f4ea81909200d8542eca14dccb2628b763a9a3`, `amd_fidelityfx_denoiser_dx12.dll` `1.2.0.2740`, `sha256=48f1e5888ba6a0a3d59a98b9751e37c392b0f7b8c223d0082d5c1f40642879d3`, preserved in `OptiScaler-DLSSNR-PreSR-Multipass-main/external/FidelityFX-SDK-v2`)

## Files & SHA-256

| File | Version | Bytes | SHA-256 |
|---|---|---:|---|
| `amd_fidelityfx_denoiser_dx12.dll` | `1.2.1.4555` (`ProductVersion 1.2.1.0`) | `8,993,552` | `4ca5182d5c8a855441e0c82673716e4776f9afcfadfdb4225d2fc6113b0f853c` |
| `include/ffx_denoiser.h` | `1.2.1` | `30,866` | `64deddd5482a302903b06d1a319b83e66d1792f493447204a2356de2d48f3ca5` |
| `license.md` | MIT (`v2.4.0:docs/license.md`) | `65,681` | `410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe` |
| `3rdpartynotice.md` | `v2.4.0:3rdpartynotice.md` | `2,519` | `3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f` |

## Scope Isolation

Per `docs/dev/fsr-rr-1.2.1-migration-plan.md`, only the Denoiser (`amd_fidelityfx_denoiser_dx12.dll` `1.2.1.4555` and `ffx_denoiser.h`) is upgraded here. The `external/FidelityFX-SDK-v2` submodule remains at `v2.3.0` (`60f4ea81909200d8542eca14dccb2628b763a9a3`) so `amd_fidelityfx_upscaler_dx12.dll` (`4.1.1.2740`), `amd_fidelityfx_framegeneration_dx12.dll` (`4.0.1.2740`), `amd_fidelityfx_radiancecache_dx12.dll`, and `amd_fidelityfx_loader_dx12.dll` (`2.3.0.2740`) are not silently upgraded in the same change.
