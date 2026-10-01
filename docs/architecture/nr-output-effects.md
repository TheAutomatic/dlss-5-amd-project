# Shared NR output effects

`NrOverallIntensity` is a host-owned `[DlssNr]` setting, clamped to 0..2 with a
finite default of 1. It applies after the selected backend produces its final
colour texture, before that texture is passed to Super Resolution. It does not
change model parameters, trigger a model rebuild, or load an inactive backend.

At 1, the original NR result is returned directly. At 0, the original input is
returned directly, while the already-recorded NR job still completes normally.
These paths add no effect dispatch. Values between 0 and 1 interpolate the
original and final NR result in their shader-resource sampling domain. Values
above 1 amplify the correction, bounding only the extra extrapolation to half
the larger per-channel input/result magnitude. Output remains finite and within
FP16 range; alpha comes from the original input. This final-output blend is not
a claim of numerical equivalence with an add-on's internal network residual.

The shared pass accepts supported single-sample colour formats and an origin-zero
active extent contained in both inputs. Typeless views follow the product's
colour interpretation. Unsupported formats, dimensions and unsafe command-list
states bypass the effect and expose a menu status. Raw escaped command lists,
active render passes and unmodeled enhanced barriers are not appended to.

`IRecordingResources` attaches independent resource owners to the command proxy.
The existing `ObserveRecording` contract still allows only one NR job per
recording. Both owner types receive the same actual submission and invalidation
facts. Owners never retain a COM reference back to the proxy. An effect's output,
descriptors, input textures, root signature and PSO remain pinned until successful
Reset/final Release invalidates the recording and its GPU submissions complete.
A failed Signal remains unconfirmed; unrelated fence completion cannot release it.
Confirmed device removal permits collection after invalidation.

The reusable pool has at most 16 output textures and a 512 MiB texture-allocation
budget, counting outputs retained by old recordings as well as the active pool.
Descriptor/query storage is additional and bounded by that count. Pool exhaustion
bypasses the effect. Resize evicts unused old sizes; returning to intensity 0 or 1
releases unused outputs. Closed or pending recordings retain their own resources.
Pure-backend configurations request the proxy when the effect is configured;
already-created raw lists require a restart to gain recording ownership.

Optional timing measures the blend pass with the same completion-certificate
rules as other D3D12 telemetry. CPU, network GPU and post-processing times remain
separate measurements. Automated WARP tests cover numerical attenuation,
amplification guard, HDR, alpha, default bypass, delayed execution, cross-queue
replay, pending Reset and pool exhaustion. Actual game image quality, performance
and both-backend switching remain part of local package testing.
