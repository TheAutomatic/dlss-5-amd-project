# Schema 1 mock worker for UI bring-up. No game files are changed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RequestJson,
    [Parameter(Mandatory = $true)][string]$EventsJsonl,
    [Parameter(Mandatory = $true)][string]$ResultJson
)
$ErrorActionPreference = 'Stop'
function Write-Event([string]$stage, [string]$message) {
    $line = (@{ schema = 1; stage = $stage; message = $message } | ConvertTo-Json -Compress)
    [IO.File]::AppendAllText($EventsJsonl, $line + [Environment]::NewLine)
}
function Write-Result($obj) {
    $tmp = $ResultJson + '.tmp'
    [IO.File]::WriteAllText($tmp, ($obj | ConvertTo-Json -Depth 8))
    [IO.File]::Copy($tmp, $ResultJson, $true)
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}

if (-not [IO.Path]::IsPathRooted($RequestJson)) { throw 'RequestJson must be absolute' }
$req = Get-Content -LiteralPath $RequestJson -Raw -Encoding UTF8 | ConvertFrom-Json
if ([int]$req.schema -ne 1) { throw "unsupported schema $($req.schema)" }
$rid = [string]$req.requestId
Write-Event 'inspect' 'mock inspect'
$exe = [string]$req.gameExe
$dir = if ($exe) { [IO.Path]::GetDirectoryName($exe) } else { '' }
$result = @{
    schema = 1
    requestId = $rid
    status = 'success'
    effectiveGameDir = $dir
    resolvedProxy = 'dxgi.dll'
    candidates = @(@{ name = 'dxgi.dll'; rank = 1; confidence = 'low'; reason = 'mock default' })
    conflicts = @()
    requiredDecisions = @()
    logPath = $EventsJsonl
    error = $null
}
if ($req.action -eq 'inspect') {
    Write-Event 'done' 'mock inspect complete'
} else {
    Write-Event 'validate' 'mock validate'
    Write-Event 'done' 'mock complete'
}
Write-Result $result
exit 0
