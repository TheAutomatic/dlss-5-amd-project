# NR backend selector

Tracked ADR for the lmxxf graft. Plan: `exports/lmxxf-main-backend-integration-plan-20260919.md`
(gitignored). Progress: `exports/lmxxf-backend-progress-20260920*.md`.

## Status (2026-09-20o; tip `8751305`, not pushed)

| Item | Now |
|---|---|
| Branch | `work/lmxxf-backend` off `main @ 2792909` (1.8.6 Daniel). **Not pushed.** |
| Default ini `NrBackend` | **daniel** (missing / empty / `auto` / unknown → daniel) |
| `LmxxfWired()` | **`true` for local E only** (`Kind.h`). Revert before push/default. |
| Active lmxxf | Only when **Wired ∧** `[DlssNr] NrBackend=lmxxf` → `ActiveKind==Lmxxf` → `LmxxfBackend` |
| `NrBackend=lmxxf` while Wired false | Logged once, falls back to Daniel |
| `NrBackend=off` | No AMD Record; original colour to SR |
| HasFiles / ECL / New wait / menu | Unchanged (HasFiles still expects Daniel `dlssnr_amd_pass1.dll`) |
| `hip_ready` | Still **0** in QueryCapabilities — trust OptiScaler.log, not the menu bit |
| `third_party/lmxxf` | Vendored @ `68dc099` + local C ABI runtime |
| `submission/` | List1–10 proxy, Create/CL1 hooks when `SubmissionHooksWanted()`, continuation seed + admission reject + Execute-decay book |

Do not change `main`'s release default until **G3∧G4∧G5**.

## Local E (燕云)

- GameDir: `...\yysls_medium\Engine\Binaries\Win64r - NR`, proxy `winmm.dll`
- Deploy notes: `exports/lmxxf-yysls-E-deploy.md`, progress `20260920o`
- Smoke: 燕云 only (no 鬼武者). Acceptance = playable + no device-removed + explainable logs — not “FSR proved NR output” (that is G3/F).

## Toolchain

`LmxxfNrRuntime.dll` is MinGW. OptiScaler (MSVC) talks through `LmxxfNrApi.h` only.
Modules: `lmxxf-modules` beside the DLL (or `LMXXF_MODULES_DIR`). Weights: `LMXXF_WEIGHTS_DIR` tiled assets (**not** 0.24.2 `HIP/`).

## Record sandwich (fail-closed)

`LmxxfBackend::Record`: PrepareFrame → **require** `ILogicalCommandList` proxy → RecordInputs → Split → RecordOutputs → `SetPendingEnqueue(EnqueueHip)`.
Non-proxy / Split fail → `CancelUnsubmitted`, return **nullptr** (ordinary SR). No Record-time EnqueueHip.
`Pending()` is a process-wide singleton (one NR session for v1).

## Product Execute

When `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always `ExecuteExpanded` (QI proxy → `ExecuteOnWithBetween`).
`PendingListIndex` stays **-1** (Daniel-only batch isolation); lmxxf intentionally does not use it.

## Admission / continuation (plans C–D)

- Min G1 reject: query / predication / enhanced barrier / open split barrier / aliasing / render pass / RTAS / meta / root·sample overflow → `MarkSplitIneligible` → Split fails → ordinary SR.
- Continuation seed: viewport/scissor/topology/PSO/rootsig/heaps/blend/stencil/OM + IA/SO/VRS/strip-cut/view-mask + RootBindState + sample positions + depth bounds.
- `ResourceStateBook::ApplyExecuteDecay` updates **our book** only (M3); does not rewrite game barriers. Live proof needs debug layer (plan E).

## Review notes (post-`ebd6072` → `8751305`)

See `exports/lmxxf-review-8751305.md`. Open E risks: query-whole-list reject, RootBindState 64 caps, decay bookkeeping-only, CL1 wrap whenever hooks armed, outdated menu `hip_ready`.