# NR backend selector (P0)

Tracked ADR for the lmxxf graft. Implementation plan remains
`exports/lmxxf-main-backend-integration-plan-20260919.md` (gitignored). This file is what the
`work/lmxxf-backend` branch ships in-tree.

## Freeze

| Item | First increment |
|---|---|
| Branch | `work/lmxxf-backend` off `main @ 2792909` (1.8.6 Daniel) |
| Default host | Daniel. Missing / empty / `auto` / unknown `[DlssNr] NrBackend` → daniel |
| `NrBackend=lmxxf` | Logged once, **still Daniel**. Runtime DLL is not wired |
| `NrBackend=off` | No AMD Record; original colour goes to SR |
| HasFiles / ECL / New wait | Unchanged; still Daniel pass-DLL and graphics-wait hooks |
| `third_party/lmxxf` | Vendored HIP+codec closure, pin `68dc099` (see `third_party/lmxxf/UPSTREAM.md`) |
| `submission/` | Not in this increment |
| Menu | No new control |

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
