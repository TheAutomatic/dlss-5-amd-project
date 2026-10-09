# Person / scene partition

The optional `[DlssNr] NrPersonPartition=false` control keeps a detected person's
first NR pass and uses the final NR pass for the rest of the frame. It follows
NR's shared output settings and reset button. It does not save background network
work. It is experimental; semantic quality, HDR, fast motion and game performance
still require real-game acceptance.

## Dependencies and scope

Put a CPU x64 ONNX Runtime (C API 23 or newer) at
`person-model/onnxruntime.dll`, beside the loaded OptScaler DLL's directory.
Put a separately obtained YOLO11n-seg FP32 COCO export at
`person-model/yolo11n-seg.onnx`. Required tensor shapes are
`[1,3,640,640]` -> `[1,116,8400]` and `[1,32,160,160]`.
Other exports are rejected explicitly. No weights or inference DLL are included
or downloaded. Observe the model's own license; compatibility does not grant
redistribution rights. All classes other than COCO person are ignored.

The Microsoft MIT C headers are pinned to v1.23.2. The implementation independently
decodes detections, applies NMS and reconstructs masks.

A single asynchronous CPU worker uses two intra-op threads, one inter-op thread
and no DirectML queue. Capture and inference queues have one in-flight item each.
Loading, inference and ORT teardown occur outside rendering. Missing dependencies
latch an explanatory state; toggling off/on retries them. Detection covers all
people, not a persistent player identity.

## Frame and resource contract

The host captures a letterboxed 640 RGB input and 160-square motion/depth guides.
Only completed readback is submitted to the worker. Masks carry source frame,
dimensions, monotonic capture time and stream epoch. At most 24 guide frames and
250 ms are accepted. Motion is traced through each intervening frame; invalid,
offscreen and depth-inconsistent samples are rejected. A depth-aware feather
avoids copying foreground colour across disocclusions. Missing reliable guides
or jittered vectors bypass the effect.

First/final outputs must be shader-readable, same-frame and in the decoded game
colour domain. Composition preserves original alpha. Overall intensity, residual
shaping and the independent output stabilizer run after partition.

Recording observers retain textures, descriptors and dependencies through both
recording invalidation and execution completion. Each execution uses the existing
GPU temporal-control mechanism to reject replay, delayed or reordered recordings.
Cross-queue execution waits for the previous consumer on the GPU. Guide textures
are immutable while referenced. Retained output sets are capped at eight and
512 MiB including conservative per-set overhead. Optional first-pass backend
allocations and ORT's model working set are additional memory.

The worker callback and delayed resource collection retain the host module.
Unknown submission completion retains resources instead of freeing GPU-owned
objects. Off/rebuild resets the mask epoch immediately; a running inference may
finish but cannot publish into a new stream.

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
