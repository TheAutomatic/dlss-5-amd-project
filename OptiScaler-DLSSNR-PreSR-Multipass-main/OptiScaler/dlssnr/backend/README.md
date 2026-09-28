# NR backend selector

Contract only. Design notes: `docs/backends/lmxxf.md`, `docs/architecture/`. Players: root `README.md`.

| `[DlssNr] NrBackend` | Active |
|---|---|
| missing / empty / `auto` / unknown | Prefer Daniel when installed; use lmxxf if it is the only installed host |
| `daniel` / `lmxxf` | Use the requested host when installed; otherwise use the other installed host |
| neither runtime installed | `HasFiles()` is false; NR cannot run |
| legacy `off` / `none` | Config load migrates the choice to `Enabled=false` and `NrBackend=daniel` |

- `LmxxfWired()` (`Kind.h`) is compile-time. `SubmissionHooksWanted()` checks the resolved host.
- Live vs restart: switching applies immediately only when the command-list proxy was armed at startup (session began on lmxxf). Starting on daniel leaves that proxy off; a pick of lmxxf is stored for the next launch and the menu says so. The combo labels that case `lmxxf (after restart)`. ProxyWrap is sticky-on once lmxxf enables it (daniel -> lmxxf hot switch stays live).
- lmxxf assets: `LmxxfNrRuntime.dll` (MinGW, `LmxxfNrApi.h` only) + `lmxxf-modules/` (or `LMXXF_MODULES_DIR`) + `LMXXF_WEIGHTS_DIR` = tiled assets, not `HIP/`.
- `LmxxfBackend::Record`: PrepareFrame → require proxy → RecordInputs → Split → RecordOutputs → pending EnqueueHip in the Execute between slot. Fail → ordinary SR. No Record-time HIP.
- Product Execute uses `ExecuteExpanded` when armed; `PendingListIndex` is -1 (Daniel-only).
- Split rules: `../submission/README.md`.
- Exposure binding changes drain pending readers and rebuild codecs, retaining the HIP bridge, weights and warmed kernels. Geometry/format/allocation/network-tier or ViT byte-stream changes retain the full teardown path. Exposure switching therefore still has codec/drain cost; it is not a zero-cost operation.
- `lmxxf runtime snapshot` reports `perf=v2`, codec recreation and bridge creation counts, and CPU wall times in milliseconds for PrepareFrame, EnqueueHip, codec rebuild and DrainGpu. Each triple is last call / peak since the preceding GetStatus / session maximum (including startup). These are CPU call durations including internal waits, not GPU inference timestamps or displayed-frame percentiles. The host samples with its throttled continuity report. `tests/lmxxf/run.cmd gpu` covers exposure pointer/format changes, six submitted game/auto/manual exposure transitions, repeated output hashes, and model retention versus geometry teardown.
