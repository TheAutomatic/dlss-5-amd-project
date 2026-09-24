# NR backend selector

Tracked ADR for the lmxxf graft. Plan: `exports/lmxxf-main-backend-integration-plan-20260919.md`
(gitignored). Progress: `exports/lmxxf-backend-progress-20260920*.md`.

## Status (2026-09-20o; tip `8751305`, not pushed)

| Item | Now |
|---|---|
| Branch | `work/lmxxf-backend` off `main @ 2792909` (1.8.6 Daniel). **Not pushed.** |
| Default ini `NrBackend` | **daniel** (missing / empty / `auto` / `off` / `none` / unknown → daniel). Only `daniel` or `lmxxf`; `Enabled` is the on/off. |
| `LmxxfWired()` | **`true` for local E only** (`Kind.h`). Revert before push/default. |
| Active lmxxf | Only when **Wired ∧** `[DlssNr] NrBackend=lmxxf` → `ActiveKind==Lmxxf` → `LmxxfBackend` |
| `NrBackend=lmxxf` while Wired false | Logged once, falls back to Daniel |
| HasFiles / ECL / New wait / menu | Unchanged (HasFiles still expects Daniel `dlssnr_amd_pass1.dll`) |
| Capabilities | `history` / `overlap` / `graph` stay 0. No `hip_ready` field. |
| `third_party/lmxxf` | Vendored @ `68dc099` + local C ABI runtime |
| `submission/` | List1–10 proxy, Create/CL1 hooks when `SubmissionHooksWanted()`, continuation seed + admission reject + Execute-decay book |

Plan F is closed. Neither backend is the sole release default; Setup selects at install time.

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
The runtime owns one job per session. If another Evaluate arrives before the previous
game list is submitted, Record returns the original Color for that Evaluate and
keeps the earlier job intact. Cancelling the earlier job after its output has
already been handed to SR would invalidate the recorded continuation.
`Pending()` is a process-wide singleton because the product has one active NR
backend. Its slot is keyed by the logical list supplied to the between callback;
submitting other game or FG lists does not consume it. Multiple simultaneous NR
backends would require a per-backend or per-session registry.

## Product Execute

When `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always `ExecuteExpanded` (QI proxy → `ExecuteOnWithBetween`).
`ExecuteExpanded` forwards the current logical list as an explicit callback argument.
If `EnqueueHip` fails, the callback reads the runtime's thread-local error on the
submission thread and retains it for the next `PrepareFrame` failure log. `Retire`
and `ResetHistory` failures are also logged at their call sites. This preserves
the original error when the runtime subsequently reports only a poisoned session.
`PendingListIndex` stays **-1** (Daniel-only batch isolation); lmxxf intentionally does not use it.

## Admission / continuation (plans C–D)

- Min G1 reject: open query at the cut / invalid query scope / predication / enhanced barrier / open split barrier / render pass / RTAS / meta / root·sample overflow → Split fails → ordinary SR. A completed aliasing barrier does not reject (`803c8ba`). Completed queries and timestamp EndQuery remain eligible.
- Continuation seed: viewport/scissor/topology/PSO/rootsig/heaps/blend/stencil/OM + IA/SO/VRS/strip-cut/view-mask + RootBindState + sample positions + depth bounds.
- `ResourceStateBook::ApplyExecuteDecay` updates **our book** only (M3); does not rewrite game barriers. Live proof needs debug layer (plan E).

## Review notes (post-`ebd6072` → `8751305`)

See `exports/lmxxf-review-8751305.md`. Open E risks: query-whole-list reject, RootBindState 64 caps, decay bookkeeping-only, CL1 wrap whenever hooks armed.
