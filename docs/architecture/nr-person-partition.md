# Person / scene partition

The optional `[DlssNr] NrPersonPartition=false` control uses a detected person's
first NR pass in multi-pass mode, or the final result in single-pass mode, and
uses final NR for the rest of the frame. The host can attenuate the person's
correction and its fine detail. These controls share NR's output reset button;
the common output effects run after person composition. It does not save background network
work. It is experimental; semantic quality, HDR, fast motion and game performance
still require real-game acceptance.

## Dependencies and scope

Put a CPU x64 ONNX Runtime (C API 23 or newer) at
`person-model/onnxruntime.dll`, beside the loaded OptScaler DLL's directory.
`[DlssNr] NrPersonModel=0` selects the default PP-HumanSeg FP32 model at
`person-model/pphumanseg.onnx`: `[1,3,192,192]` -> `[1,2,192,192]`.
The worker removes capture letterboxing before PP inference and restores it in
the returned mask. `NrPersonModel=1` selects YOLO11n-seg FP32 COCO at
`person-model/yolo11n-seg.onnx`: `[1,3,640,640]` -> `[1,116,8400]` and
`[1,32,160,160]`. `NrPersonModel=2` selects YuNet at `person-model/yunet.onnx`:
the official dynamic FP32 export is evaluated at `[1,3,S,S]` with `[DlssNr] NrFaceInputSize=320` (default), 384, 416 or 448 BGR `[0,255]`.
Its 12 named outputs (cls/obj/bbox/kps at strides 8/16/32) are validated at load
and inference. Scores use `sqrt(cls*obj)` with a 0.6 threshold and 0.3-IoU NMS,
bounded to 256 candidates and 16 faces. A soft ellipse inside each face box
produces the same 160-square letterboxed mask. It is not skin/hair segmentation;
small, turned or occluded faces may be missed, and clothing is not protected.
Only the selected model runs: YuNet does not load PP or add a second worker.
The menu/INI choice wins; changing the model restarts the worker
and resets host mask history. Existing explicit YOLO choices are preserved.
Other exports are rejected explicitly. Model availability depends on the installed
package; the provider never downloads missing files. Observe the model's own license; compatibility does not grant
redistribution rights. PP segments people; YOLO ignores COCO classes other than
person. YuNet detects faces without identifying a person. None tracks player identity.

YuNet source: OpenCV Zoo commit `47534e27c9851bb1128ccc0102f1145e27f23f98`,
`models/face_detection_yunet/face_detection_yunet_2026may.onnx` (229738 bytes),
SHA256 `ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0`.
The directory and weights are MIT licensed; the package includes `Licenses/YuNet_MIT.txt`.
This option narrows the protection area; it does not repair a backend's own temporal flicker.

The Microsoft MIT C headers are pinned to v1.23.2. PP uses its foreground
probability output; the YOLO decoder applies NMS and reconstructs instance masks.

`person-model/person-worker.exe` runs ONNX in a separate process with its own
app-local VC++ CRT DLLs. The installer does not replace the game's root CRTs.
The host and worker acknowledge IPC version 3; an old worker is rejected with a
complete-package update message. A kill-on-close job and parent-process handle
bound its lifetime to the game. Protocol offsets, sizes and frame identities are
validated before consuming responses. The receiver acknowledges initialization
before the host can submit; teardown never joins the receiver under its mutex.

The worker uses `clamp(logical_cpu_count / 4, 2, 4)` intra-op threads, one inter-op
thread, normal process priority and disabled ORT thread spinning. There is no
CPU-model whitelist, affinity mask or DirectML queue. GPU capture overlaps the single
CPU inference request: at most four readbacks are in flight, targeting 20 captures
per second. Deadlines retain their 50 ms phase when rounded to render frames,
so a 40 ms render interval does not reduce the target to one capture every 80 ms.
Scheduling uses a high-resolution monotonic clock to avoid 16 ms tick rounding
dropping slots near 20 FPS. Source-mask lifetime keeps its existing clock/limits.
Missed slots are skipped after stalls; there is no catch-up queue.
Collection submits only the newest completed, current-epoch source;
busy workers discard completed sources instead of queuing stale CPU work. A source
older than the last submitted frame is never sent after a late fence observation.
The existing recording lease retains each buffer until its GPU work completes.
Loading, inference and ORT teardown occur outside rendering. Missing dependencies
latch an explanatory state; toggling off/on retries them. Detection covers all
people, not a persistent player identity.

## Frame and resource contract

The host captures a letterboxed 640 RGB input and 160-square motion/depth guides.
Only completed readback is submitted to the worker. Masks carry source frame,
dimensions, monotonic capture time and stream epoch. At most 24 guide frames and
250 ms are accepted. A valid mask fades out during its last 50 ms or last four guide frames; this
does not extend its lifetime. Submission checks the mask's original capture time
as well as the recording time, so a delayed command cannot reuse an expired mask.
Motion is traced through each intervening frame; invalid,
offscreen and depth-inconsistent samples are rejected. Missing guide frames are
never hidden by clamping a mask's source age. A failed warp never samples the
original mask at a partially traced coordinate.

The existing 160-square warp dispatch also reprojects the previous submitted
mask and smooths valid probabilities with time-based 30 ms rise / 50 ms fall
constants. This history is immutable while recordings refer to it and is kept
with the guide frame as RG32F (probability and validity). Reprojection failure,
reset, replay and expiry do not blend stale history. Fading is applied once at
composition, independently of stored probabilities. This adds small guide-size
textures and reads, not another network evaluation or full-resolution copy.
It reduces refresh pulses; it cannot recover motion absent from the game's
vectors or guarantee all fast-motion flicker is eliminated. A depth-aware feather
avoids copying foreground colour across disocclusions. Missing reliable guides
or jittered vectors bypass the effect.

First/final outputs must be shader-readable, same-frame and in the decoded game
colour domain. Composition preserves original alpha. Overall intensity, residual
shaping and the independent output stabilizer run after partition.

Recording observers retain textures, descriptors and dependencies through both
recording invalidation and execution completion. Each execution uses the existing
GPU temporal-control mechanism to reject replay, delayed or reordered recordings.
Cross-queue execution waits for the previous consumer on the GPU. Guide textures
are immutable while referenced. Retained output sets are capped at 24 and
2 GiB including conservative per-set overhead. Optional first-pass backend
allocations and ORT's model working set are additional memory.

The worker callback and delayed resource collection retain the host module.
Worker failures latch until off/on, rather than spawning a process every render
frame. Logs rotate at 64 KiB and contain startup policy/dependency paths, the first
three frame timings and a shutdown summary, not continuous per-frame output.
Unknown submission completion retains resources instead of freeing GPU-owned
objects. Off/rebuild resets the mask epoch immediately; a running inference may
finish but cannot publish into a new stream.

## Shared person controls

| INI key | Range | Default | Meaning |
|---|---|---|---|
| `NrPersonPartition` | boolean | false | Enable person composition on lmxxf or Mochizuki |
| `NrPersonStrength` | 0..1 | 1 | Scale the chosen person's NR correction |
| `NrPersonDetailGain` | 0..1 | 1 | Scale only its fine residual; preserve broad correction and original detail |

Both floats are host-owned and finite-clamped; nonfinite values become 1.
They do not alter backend settings, model history, pass count or ABI. New values
invalidate downstream output-stabilizer history without rebuilding the NR model.
With one pass and both values at 1 there is no person processing or inference;
the menu explains why. With multiple passes, 1/1 preserves the prior first-pass
selection for fresh masks. Single-pass uses final NR directly and requests no
extra first-pass output from either runtime.

For original input `b` and person source `p`, split `p-b` into a 3x3
original-guided low component `L` and fine component `H`. Person colour is
`b + strength * (L + detail * H)`, blended with the final scene colour by the
motion/depth-aligned mask. At detail=1 the neighbourhood reads are skipped; at
1/1 the original source is retained directly. No additional network evaluation
or composition dispatch is introduced. Active masking/inference and extra shader
work still have a cost. Shared spatial/temporal output effects and Overall
Intensity follow this step, so their settings can further change the result.

Mochizuki's `automatic_mask/skin_structure` conditions the network and may also
be set per pass. It is not equivalent to a host-controlled final correction under
an independent person mask. Both remain available, with no automatic changes to
the native controls; users should compare them separately before combining.
Multi-pass paths without an exported first output continue to bypass person
composition, including the runtime's existing preprocess/control-mask/native-
composition restrictions. Daniel remains out of scope.

Colour-specific person protection is deferred: the common sampling RGB contract
does not establish one physical colour space for SDR, linear HDR, PQ and signed
scRGB. Applying a guessed hue/chroma transform would not be a general solution.
Inter-pass conditioning is also deferred: it changes the input to later network
evaluations and must be validated independently against model history and
predicted-third-pass scheduling. Final compositing does not claim to implement it.

## lmxxf interface and source ownership

Upstream base remains `48a41fccb89300cd6636b16bc7b86010384c4cc1`.
`tools/lmxxf-sync/patches/first-pass-output.patch` is an explicit temporary,
opt-in interface patch in the synchronization manifest; there are no file pins.
It adds a caller-owned RGB32F first-pass sink to Network and a shared output to
D3D12Bridge. RGB is copied directly; RGBA uses chunked pitched copies to remove
alpha before any later pass overwrites its feed tensor. MP1, actual MP2/3 and
predicted MP3 preserve the existing network schedule. Graph/experimental history
are refused for the new sink. Off performs no extra allocation/copy.

The runtime ABI is now 4: frame flag FIRST_PASS and job first_pass_output.
A second codec decodes the captured first pass using the same exposure,
pre-exposure and strength parameters as the final codec, into private FP16.
Its resources are pinned to the same recording job. Resize/pass/settings changes
rebuild the chain through existing deferred retirement. Old host/runtime pairs
are rejected; install a matching whole package.

History/ViT exclusion remains. This interface does not repair the previously
reported predicted-third-pass flicker. First-pass history is not independently
added by the partition feature.

## Mochizuki adapter

The default-off `RuntimeConfig::first_pass_output` uses a separate full-frame
RGBA32F image and the existing transfer shader after pass one. It resolves the
same detail, colour, white point and model-scale controls as the final transfer.
The Vulkan adapter converts to the game's colour format and copies both outputs
through one recording lease and producer/consumer fence chain. Bucketed DRS crops
the valid subrect from both outputs. Optional allocations are included in network
and frame budgets and keys, and retired with the existing geometry owner.

Active preprocessing, control masks and native composition do not export a first
pass. During a rebuild, a network without a valid multi-pass first output also
bypasses partition. This preserves the user's final NR settings.

Daniel is intentionally deferred at the user's request. Its menu control is
disabled without altering the stored preference, and its frames never start the
person inference provider. Existing Daniel behavior is unchanged.

## Verification and remaining acceptance

- Worker isolation repair: the real CPU model produces identical direct/IPC
  masks on a constant input, with dependency paths inside the worker directory.
  IPC regression covers startup handshakes (including legacy workers), repeated
  stop/response races, invalid offsets/frame identities and failure latching.
- Mask repair: existing WARP disocclusion regression failed before the repair
  and passes afterwards. Added refresh-pulse smoothing and guide-history
  exhaustion coverage; the shader suite also exercises shared effects/Post-SR.
- Build/packaging uses a freshly compiled worker beside the selected host and
  checks its source/binary receipt. Clean CI supplies ONNX 1.23.2 and app-local
  MSVC redistributables. Never ship an incidental worker executable from assets.

- Shared-controls extension: host build, configuration/CRT checks, translated
  menu checks and 144 font/language/layout cases passed. CPU tests cover neutral
  settings, finite bounds and mask fading. WARP covers single/multi-pass strength,
  fine-detail attenuation, unchanged scene pixels, HDR-range interpolation,
  alpha and masks that expire between recording and submission.
- MSVC host and both open-source runtimes built; Mochizuki ABI checks passed.
  Mochizuki reused unchanged shaders for this C++ iteration; no release build
  receipt or fresh shader-generation validation is claimed.
- Actual CPU model smoke: first inference about 79 ms locally; a constant input
  verified inference, dimensions and finite output, not person recognition quality.
- WARP: person/background numeric composition, alpha, two-frame backward-motion
  alignment, offscreen rejection, time expiry/camera reset, stale epochs, cross-queue
  replay, depth disocclusion and closed-list lifetime; D3D12 debug checks passed.
- Shader tests and CPU decoder tests are wired into their area run.cmd.
- No game deployment, hardware HIP image/performance test or release certification.

Before release, compare lmxxf RGB/RGBA first sinks against an independent MP1
baseline on AMD hardware at MP1/2/3, predicted and real third pass, multiple input
sizes and history states. Measure extra copies/decoding, worker latency, VRAM,
fast turns, near hair, occlusion, DRS and HDR. CPU/WARP evidence does not substitute
for this acceptance.


Mochizuki additionally needs an AMD Vulkan first/final comparison for FP16 and
sRGB, scale 1 and reduced scale, passes 2/3, history on/off, dynamic buckets,
preprocess enable/disable and old-recording replay. Measure the extra full-frame
transfer and format conversion as well as memory. None of these hardware/game
checks was run in this implementation phase.

## Diagnosing intermittent masks

The existing **Show person mask** overlay also enables a summary in OptiScaler.log
at most once every two seconds. `Person mask diagnostic` separates record-time
rejections (missing result, epoch, dimensions, 250 ms expiry, 24-frame limit,
missing guides), submission-time expiry, history resets and capture delivery.
Age begins when the source image is recorded, not when CPU inference finishes;
`arrival_age_ms` is the age when the render thread first observes that result.

Coverage is counted above 0.5 on the 160x160 mask grid, in ten-thousandths
(10000 = the entire grid). Raw coverage describes model output; warped coverage
and validity describe the GPU reprojection **before** expiry fade and final
depth-aware upsampling. Their difference is diagnostic, not a direct quality
score. The log records sample count, minimum, mean and maximum.
GPU readbacks are sampled at most ten times per second, collected only after
the existing completion fence, and retained by the existing recording lease.
No new GPU wait, model change, lifetime extension or relaxed rejection is used.
Disabling the overlay disables sampling and summaries. These measurements still
require game reproduction; an empty overlay alone does not identify the cause.

## Local sync review

The same-pin audit uses the previous official integration range
`297b032ac55f005d78568e684f30608651044f62` to
`48a41fccb89300cd6636b16bc7b86010384c4cc1`. The `integration` decision explicitly
remains deferred for the hardware acceptance above, with completed code/CPU/WARP
evidence retained. It needs no custom skipped-check or external module-bundle
argument: no module build/supply was requested for this local interface change.
`sync-state.json` still describes the earlier full upstream synchronization;
this local audit does not certify new GPU artifacts or advance that baseline.

At matching full-frame extents, Mochizuki's extra first-pass storage is roughly
79 MiB at 1080p or 316 MiB at 4K with FP16 game colour (one RGBA32F core image,
one Vulkan colour image, shared buffer and D3D colour texture). Alignment,
bucket/allocation differences and retained recordings add overhead. Host masks,
composition and the CPU model are additional. This is a storage estimate, not a
measured hardware VRAM or latency result.

The size control is shown only for YuNet. Other models ignore it. Invalid sizes
fall back to 320. Switching size restarts the single worker and resets host mask
history; source dimensions and the 160-square motion/mask grid remain unchanged.
IPC v3 rejects older workers that cannot honor the new input size.
