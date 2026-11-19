# Proxy recommendations

`proxy-profiles.json` is generated data, bundled and embedded in the launcher. Proxy
names describe the host's supported system aliases. Profile recommendations are
separate from the user's per-game choice and never modify game files.

## Where the data comes from

The seed is generated from the live
[OptiScaler compatibility list](https://github.com/optiscaler/OptiScaler/wiki/Compatibility-List)
(700+ tested games, including the *Upscaler mods support* and *Luma Unreal Engine*
sub-tables) by the launcher's own C# parser, merged over the hand-reviewed records in
`curated-profiles.json`. To regenerate after a Wiki change:

```powershell
powershell -File tools/compat/update-wiki-profiles.ps1
```

The generator downloads `Compatibility-List.md`, runs
`ProxyRecommendations.ParseWikiCompatibilityMarkdown` (the same code path the launcher
uses for a live refresh), validates the result and writes the pretty-printed seed.
Bundled rows for `[Broken]` games are skipped; rows that mention a proxy DLL inside a
negative phrase ("Do NOT use dxgi.dll", "d3d12.dll does not work") or a Linux-only
instruction never become the Windows `preferred` proxy.

`curated-profiles.json` holds the reviewed layer: this fork's tested games and popular
Chinese releases with aliases (`names`), install-folder and executable identities
(`pathSegments`, e.g. `b1-Win64-Shipping`, `Client-Win64-Shipping`, `yysls`),
subpage-verified proxy guidance and `upstreamVersion`. Curated records keep their
guidance when the live Wiki refresh merges over them; all other rows are regenerated
from the Wiki table (`sourceFormat: "wiki"`).

When adding curated records, inspect the game's wiki detail page and preserve its
source URL, conditions, exact names/path segments and upstream version when supplied.
Do not infer an injection alias solely from an API import or a passing mention.
Increase the numeric revision for each published data change. Never add commands,
downloadable DLLs or automatic INI edits to these files.

## Live refresh

Clicking "更新推荐资料" / "Refresh recommendations" fetches
`https://raw.githubusercontent.com/wiki/optiscaler/OptiScaler/Compatibility-List.md`,
parses the Markdown table in C# (auto-detecting a JSON payload fallback), merges it
over the bundled seed and caches the validated document under
`%LOCALAPPDATA%\OptScalerNR\Launcher\compatibility\proxy-profiles.json` together with
the server ETag (a `304` reply reports `not-modified`). The download is capped at
1 MiB with a 45-second deadline; failures keep the bundled data. Set
`OPTSCALERNR_WIKI_URL` to override the endpoint (used by hermetic tests); both Wiki
Markdown and the JSON document format are accepted.
