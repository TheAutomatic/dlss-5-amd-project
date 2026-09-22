param(
    [string]$OutputDir = (Join-Path $PSScriptRoot 'modules'),
    [string]$Compiler = (Join-Path $PSScriptRoot 'rtc_compile.exe'),
    [string]$SourceDir = $PSScriptRoot,
    [string]$Only = '',
    [ValidateSet('gfx1200','gfx1201')][string[]]$Targets = @('gfx1200','gfx1201')
)
# Builds 24 modules per target; by default gfx1200 and gfx1201 go into architecture subdirectories.
# One row per module: output name, extra #defines, source files (concatenated in order). Every row prepends HIP_ISA_HALF 1;
# names ending in -packed also prepend HIP_PREPACKED_WEIGHTS 1. The extra defines below are the production selections of
# 2026-09-17 (0.20); they coincide with the sources' defaults and are spelled out so the recipe does not depend on them.
# Compiler: rtc_compile.exe built from rtc_compile.cpp (see README.md); it uses the driver's amd_comgr_3.dll, no SDK needed.
$ErrorActionPreference = 'Stop'
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$modules = @(
    @{ name = 'c32_prefix_reference';               defines = @();                        sources = @('c32_reference.hip', 'prefix_reference.hip') },
    @{ name = 'multihead-reference';                defines = @();                        sources = @('multihead_reference.hip') },
    @{ name = 'deep_reference';                     defines = @();                        sources = @('deep_reference.hip') },
    @{ name = 'boundary_reference';                 defines = @();                        sources = @('boundary_reference.hip') },
    @{ name = 'c32_wmma';                           defines = @();                        sources = @('c32_wmma.hip') },
    @{ name = 'multihead-wmma';                     defines = @();                        sources = @('multihead_wmma.hip') },
    @{ name = 'deep_wmma';                          defines = @();                        sources = @('deep_wmma.hip') },
    @{ name = 'wave-pointwise';                     defines = @();                        sources = @('c32_reference.hip', 'wave_pointwise.hip') },
    @{ name = 'c32_tiled';                          defines = @();                        sources = @('c32_tiled.hip') },
    @{ name = 'multihead-tiled';                    defines = @();                        sources = @('multihead_tiled.hip') },
    @{ name = 'c32_fast';                           defines = @();                        sources = @('c32_fast.hip') },
    @{ name = 'c32_fast_attention';                 defines = @();                        sources = @('c32_fast_attention.hip') },
    @{ name = 'boundary-fast';                      defines = @();                        sources = @('c32_fast_attention.hip', 'boundary_fast.hip') },
    @{ name = 'c32_fused_attention';                defines = @();                        sources = @('c32_fused_attention_packed.hip') },
    @{ name = 'c32_fused_ffn_attention';            defines = @();                        sources = @('c32_fused_ffn_attention.hip') },
    @{ name = 'c32_fused_ffn_attention-packed';     defines = @('HIP_C32_DIAG_WEIGHTS 1'); sources = @('c32_fused_ffn_attention.hip') },
    @{ name = 'prefix_fast';                        defines = @();                        sources = @('prefix_fast.hip') },
    @{ name = 'multihead-fast';                     defines = @();                        sources = @('multihead_fast.hip') },
    @{ name = 'multihead-fast-padded-wave';         defines = @();                        sources = @('multihead_fast_padded.hip') },
    @{ name = 'multihead_fused_attention';          defines = @('HIP_MH_RTZ_ISA 1');      sources = @('multihead_fused_attention.hip') },
    @{ name = 'deep_fast';                          defines = @();                        sources = @('deep_fast.hip') },
    @{ name = 'deep_fast-packed';                   defines = @('HIP_BRANCHLESS_F 1');    sources = @('deep_fast.hip') },
    @{ name = 'multihead-fast-packed';              defines = @();                        sources = @('multihead_fast.hip') },
    @{ name = 'multihead-fast-padded-wave-packed';  defines = @('HIP_FFN_HOIST_RES 2');   sources = @('multihead_fast_padded.hip') }
)
$outputRoot=$OutputDir
foreach($target in $Targets){
$OutputDir=if($Targets.Count -gt 1){Join-Path $outputRoot $target}else{$outputRoot}
New-Item -ItemType Directory -Force $OutputDir|Out-Null
$manifest = @()
foreach ($m in $modules) {
    if ($Only -and $m.name -ne $Only) { continue }
    $text = "#define HIP_ISA_HALF 1`n"
    if ($m.name -like '*-packed') { $text += "#define HIP_PREPACKED_WEIGHTS 1`n" }
    foreach ($d in $m.defines) { $text += "#define $d`n" }
    foreach ($part in $m.sources) { $text += [IO.File]::ReadAllText((Join-Path $SourceDir $part)) + "`n" }
    $generated = Join-Path $OutputDir ($m.name + '.generated.hip')
    $hsaco = Join-Path $OutputDir ($m.name + '.hsaco')
    [IO.File]::WriteAllText($generated, $text, $utf8)
    & $Compiler $hsaco $generated comgr $target | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "COMGR failed: $($m.name)" }
    $manifest += [pscustomobject]@{ target = $target; module = $m.name; defines = (@('HIP_ISA_HALF 1') + $(if ($m.name -like '*-packed') { @('HIP_PREPACKED_WEIGHTS 1') } else { @() }) + $m.defines) -join '; '; sources = $m.sources -join '+'; sha256 = (Get-FileHash $hsaco).Hash }
    Write-Output ("{0} {1,-40} {2}" -f $target,$m.name, $manifest[-1].sha256)
}
[IO.File]::WriteAllText((Join-Path $OutputDir 'modules.json'), ($manifest | ConvertTo-Json -Depth 3), $utf8)
$sums = $manifest | ForEach-Object { $_.sha256.ToLower() + '  ' + $_.module + '.hsaco' }
[IO.File]::WriteAllText((Join-Path $OutputDir 'SHA256SUMS'), (($sums -join "`n") + "`n"), $utf8)

}

if($Targets.Count -gt 1){
 $all=@(foreach($target in $Targets){foreach($line in [IO.File]::ReadAllLines((Join-Path (Join-Path $outputRoot $target) 'SHA256SUMS'))){$line.Substring(0,66)+$target+'/'+$line.Substring(66)}})
 [IO.File]::WriteAllLines((Join-Path $outputRoot 'SHA256SUMS'),$all,$utf8)
}
