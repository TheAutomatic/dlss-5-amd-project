# NR backend selector

Contract only. Status: `.handoff/HANDOFF.md` · plans: `exports/lmxxf-*.md`. Players: root `README.md`.

| `[DlssNr] NrBackend` | Active |
|---|---|
| missing / empty / `auto` / unknown | Prefer Daniel when installed; use lmxxf if it is the only installed host |
| `daniel` / `lmxxf` | Use the requested host when installed; otherwise use the other installed host |
| neither runtime installed | `HasFiles()` is false; NR cannot run |
| legacy `off` / `none` | Config load migrates the choice to `Enabled=false` and `NrBackend=daniel` |

- `LmxxfWired()` (`Kind.h`) is compile-time. `SubmissionHooksWanted()` checks the resolved host.
- Live vs restart: switching applies immediately only when the command-list proxy was armed at startup (session began on lmxxf). Starting on daniel leaves that proxy off; a pick of lmxxf is stored for the next launch and the menu says so. The combo labels that case `lmxxf (after restart)`.
- lmxxf assets: `LmxxfNrRuntime.dll` (MinGW, `LmxxfNrApi.h` only) + `lmxxf-modules/` (or `LMXXF_MODULES_DIR`) + `LMXXF_WEIGHTS_DIR` = tiled assets, not `HIP/`.
- `LmxxfBackend::Record`: PrepareFrame → require proxy → RecordInputs → Split → RecordOutputs → pending EnqueueHip in the Execute between slot. Fail → ordinary SR. No Record-time HIP.
- Product Execute uses `ExecuteExpanded` when armed; `PendingListIndex` is -1 (Daniel-only).
- Split rules: `../submission/README.md`.
