# Stage the real production host and owned effect providers, never helper policy DLLs.
# A fresh directory prevents stale host logs/output from satisfying acceptance checks.
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$HostDll,
    [Parameter(Mandatory = $true)][string]$Providers,
    [ValidateSet('auto', 'indirect', 'split')][string]$Route = 'auto'
)
$ErrorActionPreference = 'Stop'
$Stage = [IO.Path]::GetFullPath($Stage)
$HostDll = (Resolve-Path -LiteralPath $HostDll).Path
$Providers = (Resolve-Path -LiteralPath $Providers).Path
if (Test-Path -LiteralPath $Stage) { throw "Use a new, nonexistent staging directory: $Stage" }
$names = @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_denoiser_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll')
foreach ($name in $names) {
    if (!(Test-Path -LiteralPath (Join-Path $Providers $name) -PathType Leaf)) {
        throw "Missing real provider: $name"
    }
}
New-Item -ItemType Directory -Path $Stage | Out-Null
Copy-Item -LiteralPath $HostDll -Destination (Join-Path $Stage 'dxgi.dll')
foreach ($name in $names) {
    Copy-Item -LiteralPath (Join-Path $Providers $name) -Destination (Join-Path $Stage $name)
}
# These are the actual production INI keys. No scalar camera overrides are supplied:
# the finite RH reverse-Z .05/800 regression must resolve the real NGX matrix alone.
# Early wrapping exercises the native no-swapchain startup path. The harness QIs
# the returned list, so this option alone is not treated as evidence of readiness.
$ini = @"
[Upscalers]
Dx12Upscaler=ffx
[FSR]
UpscalerIndex=0
[FSR-RR]
Enabled=true
Profile=0
DiffuseSignal=$Route
SpecularSignal=$Route
EstimateHitDistances=false
DenoiseDiffuse=true
DenoiseSpecular=true
DiffuseHitDistance=true
FloorEnabled=false
AlbedoBleedFix=false
Diagnostics=true
[DlssNr]
Enabled=false
NrConvenience=0
LmxxfEarlyExeWrap=true
[Plugins]
LoadAsiPlugins=false
[Libraries]
OptiDllPath=$Stage
FfxDx12Path=$(Join-Path $Stage 'amd_fidelityfx_loader_dx12.dll')
FfxDx12SRPath=$(Join-Path $Stage 'amd_fidelityfx_upscaler_dx12.dll')
FfxDx12RRPath=$(Join-Path $Stage 'amd_fidelityfx_denoiser_dx12.dll')
[Hotfix]
RestoreComputeSignature=false
RestoreGraphicSignature=false
[Log]
LogToFile=true
LogLevel=1
LogAsync=false
SingleFile=true
LogFileName=fsrd-feature-smoke-host.log
"@
[IO.File]::WriteAllText((Join-Path $Stage 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))
$paths = @((Join-Path $Stage 'dxgi.dll')) + @($names | ForEach-Object { Join-Path $Stage $_ })
$provenance = @($paths | ForEach-Object {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($_)
    try { $digest = ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $hash.Dispose() }
    [ordered]@{ path = $_; sha256 = $digest;
                fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($_).FileVersion }
})
[ordered]@{
    hostSource = $HostDll
    providerSource = $Providers
    route = $Route
    expectedGuidedMask = $(if ($Route -eq 'split') { 15 } else { 12 })
    camera = 'native NGX matrices only; RH reverse-Z, near .05, finite far 800'
    geometry = 'closed analytic room; real mirror/cosine secondary-ray intersections in metres'
    scope = 'production NGX FSRDFeature control-plane smoke; not standalone offline_chain, not image quality acceptance'
    files = $provenance
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Stage 'staged-runtime.json') -Encoding UTF8
