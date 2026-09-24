# NR backend selector

Contract only. Status: `.handoff/HANDOFF.md` · plans: `exports/lmxxf-*.md`. Players: root `README.md`.

| `[DlssNr] NrBackend` | Active |
|---|---|
| missing / `auto` / `daniel` / unknown | Daniel |
| `off` / `none` | Off (no AMD Record) |
| `lmxxf` + `LmxxfWired()` | Lmxxf |
| `lmxxf` without `LmxxfWired()` | log once → Daniel |

- `LmxxfWired()` (`Kind.h`) is compile-time. `SubmissionHooksWanted()` = wired ∧ requested lmxxf.
- lmxxf assets: `LmxxfNrRuntime.dll` (MinGW, `LmxxfNrApi.h` only) + `lmxxf-modules/` (or `LMXXF_MODULES_DIR`) + `LMXXF_WEIGHTS_DIR` = tiled assets, not `HIP/`.
- `LmxxfBackend::Record`: PrepareFrame → require proxy → RecordInputs → Split → RecordOutputs → pending EnqueueHip in the Execute between slot. Fail → ordinary SR. No Record-time HIP.
- Product Execute uses `ExecuteExpanded` when armed; `PendingListIndex` is -1 (Daniel-only).
- Split rules: `../submission/README.md`.
