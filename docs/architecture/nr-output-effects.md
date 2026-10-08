# Shared NR output effects

`NrOverallIntensity` is a host-owned `[DlssNr]` setting, clamped to 0..2 with a
finite default of 1. It applies after the selected backend produces its final
colour texture, before SR in NR -> SR mode or before final copy-back in SR -> NR mode. It does not
change model parameters, trigger a model rebuild, or load an inactive backend.

With the stabilizer disabled and spatial controls neutral, at 1 the original NR result is returned directly. At 0, the original input is
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

The reusable pool has at most 16 recording storage sets and a 512 MiB texture-allocation
budget, counting outputs retained by old recordings as well as the active pool.
Descriptor/query storage is additional and bounded by that count. Pool exhaustion
bypasses the effect. Resize evicts unused old sizes; returning to intensity 0 or neutral settings
releases unused outputs. Closed or pending recordings retain their own resources.
Pure-backend configurations request the proxy when the effect is configured;
already-created raw lists require a restart to gain recording ownership.

Optional timing measures the blend pass with the same completion-certificate
rules as other D3D12 telemetry. CPU, network GPU and post-processing times remain
separate measurements. Automated WARP tests cover numerical attenuation,
amplification guard, HDR, alpha, default bypass, delayed execution, cross-queue
replay, pending Reset and pool exhaustion. Actual game image quality, performance
and three-backend switching remain part of local package testing.

## Experimental spatial residual controls

These host-owned `[DlssNr]` keys appear together in the existing Output controls,
with English/Chinese labels and the shared-effects reset. They apply to Daniel,
lmxxf and Mochizuki after backend history and matched-resolution reconstruction;
they do not alter either runtime ABI, backend defaults, or network pass count.

| Key | Menu label | Range | Default |
|---|---|---|---|
| `NrResidualLowGain` | Low-frequency gain / 低频增益 | 0..2 | 1 |
| `NrResidualDetailGain` | Fine-detail gain / 细节增益 | 0..2 | 1 |
| `NrResidualSkinProtection` | Skin detail protection / 肤色细节保护 | 0..1 | 0 |
| `NrResidualEdgeProtection` | Edge detail protection / 边缘细节保护 | 0..1 | 0 |

All four are display-only parameters, read from Config and frozen as root
constants per recording. INI/menu is their only source: there is no flags-file
or environment fallback. Non-finite values become neutral; finite values clamp
to the documented range. Pure-backend sessions without the proxy must save and
restart, as with Overall Intensity. Intensity zero overrides all controls.

For original RGB `b`, final (optionally stabilized) RGB `r`, and residual
`d = r - b`, the shader computes a 3x3 binomial average `L`, guided by
relative colour differences in the **original** image. Fine residual is
`H = d - L`. The adjusted result is
`b + lowGain * L + protectionScale * detailGain * H`, followed by the existing
Overall Intensity and its extrapolation limit. Increasing gains strengthens
an existing correction; it is not equivalent to a second network pass.

Skin confidence is a soft RGB-ratio heuristic. It restricts introduced detail
according to the original local detail and signal magnitude; warm scenery can
also match. It is not face/person segmentation. Negative RGB is not classified
as skin. Edge protection acts near strong original contrasts and only limits
new fine residuals. Both protections use a common RGB scale rather than clamping
each channel independently. Original detail and original alpha remain intact.

Math stays in the resource's sampling RGB domain, without an assumed SDR
transfer/Oklab conversion. Synthetic linear HDR exposure and signed scRGB
cases are tested; that does **not** certify skin classification or perceived
contrast for PQ/wide-gamut/tonemapped game content. Defaults therefore remain
neutral. Neither spatial protection is a temporal anti-flicker guarantee.

The point entrypoint retains neutral/uniform-gain processing without shared
memory or a group barrier. The neighbourhood PSO is compiled lazily. An 8x8
group stages a 10x10 halo in 2,800 bytes of group-shared memory, with every lane
participating in the barrier even at odd image boundaries. There is no extra
texture, descriptor layout, dispatch, GPU wait, or CPU wait relative to the
existing active effects pass. There is additional shader work when enabled;
no hardware timing or game-quality improvement is claimed.

When the shared Residual Stabilizer is enabled, the halo uses the **same
motion/depth-filtered correction** as its centre. Raw noisy neighbours are not
mixed back into stabilized pixels. History receives the unscaled/unshaped
compressed residual, before display gains/protection. Changing these controls
does not reset or amplify stored history. The existing backend history / ViT
mutual exclusion remains unchanged.

WARP tests in `tests/shader/run.cmd` cover neutral/bounded settings, constant and
impulse residuals, skin vs green, edge preservation, HDR exposure, signed RGB,
NaN/black, alpha, 1x1 and odd dimensions, delayed recordings, temporal-history
independence, changing exposure, and existing submission/lifetime regressions.
Game HDR, fast motion, model-specific artefacts and GPU cost still need controlled
comparison. Suggested starting comparison: one real pass with overall intensity
1, low gain 1.3, detail gain 0.7, protections 0; enable each protection separately.
These are comparison values, not tuned defaults.

Person-first-pass/scene-final-pass composition, segmentation workers and
game-material masks are not implemented here. Those need separately versioned
first-pass output and mask frame/generation/lifetime contracts; a colour rule
must not be presented as that feature.

## Residual Stabilizer

`NrStabilizerEnabled` defaults to false. `NrStabilizerAlpha` defaults to 0.8
(range 0..0.95) and `NrStabilizerThreshold` to 4 (range 0..16, in units of
1/255 of the compressed colour domain). These are host-owned ini/menu settings.
The defaults reduce variation in the synthetic alternating-correction fixture;
they are not a claim of optimal quality in every game. Alpha or threshold zero
bypasses stabilization. Intensity zero returns the original and resets history.

The shader filters the unscaled residual `E(result) - E(original)`, where
`E(c) = sRGB((c / preExposure) / (1 + c / preExposure))` for nonnegative,
finite input. Here `c` is the final texture's sampling-domain value, not an
inferred physical scene luminance. Previous residual samples are reprojected
with motion vectors and previous-minus-current jitter (unless vectors already
include jitter). A nearest-surface motion tap is selected in a 3x3 neighbourhood.
Each bilinear history tap must pass a 10% relative depth-key test; rejected taps
contribute no colour. Accepted history is clamped to the current residual plus
or minus the threshold, then blended by alpha. Overall Intensity and its extra
amplification guard are applied afterwards. History remains independent of the
intensity setting. Negative/nonfinite colour or invalid per-pixel guides do not
seed valid history. Missing/unsupported guide resources bypass the stabilizer
while retaining requested spatial controls and ordinary intensity blending.

The shader adapts the residual-filtering algorithm from the GPL-3.0 implementation in
[MatheusFerreiraS/neural-amd-opti at 2f39f19](https://github.com/MatheusFerreiraS/neural-amd-opti/blob/2f39f1908b6b03ae16be79d4b072ef6261c666bf/OptiScaler/dlssnr/amd/ResidualStabilizer.h):
compressed-colour residuals, motion reprojection, depth rejection, and clamped temporal blending.
Local shader changes add depth-weighted bilinear history sampling, invalid-guide rejection,
and composition with Overall Intensity. The product implements its own shared-backend pass,
recording ownership, history publication, configuration and menu controls.

The product's history management retains history across ordinary preExposure changes,
since each frame removes its own preExposure before storing residuals. This fixes a local
integration bug that repeatedly discarded usable history as exposure changed. The fix
changes history invalidation and status reporting; it does not replace the filtering algorithm.

Only successful actual submissions publish history. Recording or discarding a
list does not publish it. A recording freezes its previous history binding and
scalar settings; replay uses those frozen bindings, and never republishes an
older recording over newer history. A pipeline-wide completion chain orders
all readers and replay writers on different queues with GPU waits. The same
queue relies on queue order; no CPU wait is added by this effect. Failed Signal
leaves that chain unconfirmed and disallows its replay. New recordings start an
independent generation without reading the unconfirmed resources.

History is invalidated by backend/session reset, NR off, skipped NR evaluation,
unsupported inputs, colour extent changes, exposure-scale changes, depth/jitter
convention changes, or a submission gap exceeding one second. Epoch checks stop
late old submissions from repopulating reset history. Old recordings still own
the resources their frozen bindings need until invalidation and completion.
Each frame removes its own preExposure before storing the residual, so ordinary
preExposure changes retain history. Scene discontinuities still use the reset flag.
Status distinguishes building history from available history; individual pixels
can still reject reprojection/depth even when history is available.

Each temporal storage set contains two RGBA16F textures (output and next
residual/depth), about 16 bytes per pixel before allocation alignment. Keeping
previous history and multiple recordings raises actual usage above that minimum.
The shared 512 MiB allocation budget counts both textures and storage retained by
history readers/old generations; descriptors and queries are additional. No
fixed number of frames is used as proof that textures can be reused.

Ins shows the controls, bypass status, and cached allocated texture bytes/set count. Ins and the detailed FPS overlay report
`Stabilizer + blend` GPU time for the single combined dispatch; there is no
separate blend dispatch in this mode. Stabilizer GPU cost is additional to NR,
and cross-queue history dependencies can reduce overlap. Actual game performance,
HDR/SDR appearance, motion trails, and both-backend switching require local game
acceptance. Automated tests cover flicker reduction, translation/jitter, depth
rejection, invalid guides, intensity independence, delayed/abandoned recordings,
replay, cross-queue ordering, pending Reset, failed Signal and resource limits.
