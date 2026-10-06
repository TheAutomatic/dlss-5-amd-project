# Fail if the module-count contract drifts across sources (40 per arch / 80 dual).
# Single bump point: $PerArch. Update every listed site when the module list changes.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$failures = New-Object System.Collections.Generic.List[string]

$PerArch = 40
$Dual = 80

function Read-Root([string]$rel) {
    [IO.File]::ReadAllText((Join-Path $root $rel))
}

function Require-Contains([string]$rel, [string]$needle, [string]$what) {
    if (-not (Read-Root $rel).Contains($needle)) {
        $failures.Add("${what}: '$needle' not found in $rel")
    }
}

function Extract-Block([string]$text, [int]$start, [char]$open, [char]$close) {
    $depth = 0
    for ($i = $start; $i -lt $text.Length; $i++) {
        $c = $text[$i]
        if ($c -eq $open) { $depth++ }
        elseif ($c -eq $close) {
            $depth--
            if ($depth -eq 0) { return $text.Substring($start, $i - $start + 1) }
        }
    }
    return $null
}

function Require-QuotedCount([string]$rel, [string]$headerPattern, [char]$open, [char]$close, [string]$what) {
    $text = Read-Root $rel
    $m = [regex]::Match($text, $headerPattern)
    if (-not $m.Success) {
        $failures.Add("${what}: /$headerPattern/ not found in $rel")
        return
    }
    $block = Extract-Block $text ($m.Index + $m.Length - 1) $open $close
    if ($null -eq $block) {
        $failures.Add("${what}: could not extract list block in $rel")
        return
    }
    $n = ([regex]::Matches($block, "['`"]([^'`"]+)['`"]")).Count
    if ($n -ne $PerArch) {
        $failures.Add("${what}: $rel list has $n names, expected $PerArch")
    }
}

# --- numeric sites ---
Require-Contains 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp' `
    "kKnownModuleNames[$PerArch]" 'runtime name table size'
Require-Contains 'tools/release/check-release-freshness.ps1' "`$hs.Count -ne $PerArch" 'freshness hsaco per arch'
Require-Contains 'tools/lmxxf-module-package.ps1' "if (`$Arch) { $PerArch } else { $Dual }" 'module-package expected counts'
Require-Contains 'tools/lmxxf-module-package.ps1' "exactly $Dual known .hsaco" 'module-package dual total'
Require-Contains 'tools/lmxxf-module-package.ps1' "module_count = $Dual; module_count_per_arch = $PerArch" 'module manifest counts'
Require-Contains 'tests/lmxxf/lmxxf_nr_abi.cpp' "modules_ok=$Dual" 'ABI dual status'
Require-Contains 'tests/lmxxf/lmxxf_nr_abi.cpp' "modules_ok=$PerArch" 'ABI leaf status'
Require-Contains 'tests/lmxxf/test_runtime_validation.py' "modules_ok=$Dual" 'runtime validation dual'
Require-Contains 'tests/lmxxf/test_runtime_validation.py' "modules_ok=$PerArch" 'runtime validation leaf'

# --- name-list sites ---
Require-QuotedCount 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp' `
    'kKnownModuleNames\[\d+\] = \{' '{' '}' 'runtime module names'
Require-QuotedCount 'tests/_lib/lmxxf_fixtures.py' 'MODULE_NAMES = \(' '(' ')' 'test fixtures MODULE_NAMES'

$bm = Read-Root 'third_party/lmxxf/hip/build-modules.ps1'
$recipeCount = ([regex]::Matches($bm, "name = '([^']+)'")).Count
if ($recipeCount -ne $PerArch) {
    $failures.Add("build-modules recipe: third_party/lmxxf/hip/build-modules.ps1 has $recipeCount name = '...' rows, expected $PerArch")
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
    Write-Host "Module contract is NOT $PerArch per arch / $Dual dual. Update every site together." -ForegroundColor Red
    exit 1
}
Write-Host "Module contract OK: $PerArch per arch / $Dual dual." -ForegroundColor Green
exit 0
