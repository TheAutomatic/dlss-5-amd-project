# NR backend selector (P0)

Tracked ADR for the lmxxf graft. Implementation plan remains
`exports/lmxxf-main-backend-integration-plan-20260919.md` (gitignored). Progress:
`exports/lmxxf-backend-progress-20260920.md`. This file is what `work/lmxxf-backend` ships in-tree.

## Status (2026-09-20; last code `4ce1f72`)

| Item | Now |
|---|---|
| Branch | `work/lmxxf-backend` off `main @ 2792909` (1.8.6 Daniel). Not pushed. |
| Default host | Daniel. Missing / empty / `auto` / unknown `[DlssNr] NrBackend` → daniel |
| `NrBackend=lmxxf` | Logged once, **still Daniel**. `LmxxfWired()` false |
| `NrBackend=off` | No AMD Record; original colour goes to SR |
| HasFiles / ECL / New wait / menu | Unchanged |
| `third_party/lmxxf` | Vendored @ `68dc099`; MinGW runtime HIP enqueue wired |
| `submission/` | `LogicalList` + base `CommandListProxy`. List1–10 and hooks **not** done |
| `hip_ready` | 0 |

## Product behaviour (later, when lmxxf is actually selected)

Same-frame serial NR, one HIP network, history off. Slots / Every-frame / New wait stay Daniel-only.
Do not change `main`'s release default until G3∧G4∧G5.

## Toolchain (later P2)

`LmxxfNrRuntime.dll` is MinGW. OptiScaler (MSVC) talks to it through a versioned C ABI only.
Upstream source is a vendored closure at `third_party/lmxxf` pinned to `68dc099`, not the
`analysis/` clone and not a full-repo submodule.

## This increment's code

- `Kind.h` — parse `daniel` / `lmxxf` / `off`
- `Host.h` — vtable the live `AmdBridge` already calls
- `DanielBackend` — owns `AmdPreSr::Backend`, no protocol change
- `LmxxfBackend.h` — declared, not constructed
- `Selector` — requested vs active kind

## P2 3b (HIP enqueue wired; product still Daniel)

- Encode/decode no longer pull `native_split.h`.
- `D3D12Bridge` RecordInputCopy / EnqueueAfterProducer / RecordOutputReadable; graph off.
- RecordInputs = encode + RGB tiles + shared copy; EnqueueHip = fence/HIP/wait; RecordOutputs = RGB texture.
- Modules: `exports/lmxxf-modules-68dc099` only. Weights: `LMXXF_WEIGHTS_DIR` (tiled assets, not 0.24.2 `HIP/`).
- `QueryCapabilities.hip_ready` stays 0. `LmxxfWired()` stays false.

## P1 (no-NR split bookkeeping)

EvaluateFeature / `AmdBridge::Before` records on a still-open game list; the game keeps that
pointer. No natural submit boundary. `LogicalList` + base `CommandListProxy` prove split
Execute-once passthrough (QI List1+ fail-closed). Not hooked, not default.

**Next (no user action):** forward `ID3D12GraphicsCommandList1`–`10` on the proxy. Do not hook
`CreateCommandList` until that is done.

