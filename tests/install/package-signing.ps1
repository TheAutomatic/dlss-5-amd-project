$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $root 'tools/release/package-signing.ps1')
$scratch = Join-Path $root ('work/scratch/signing-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$cert = $null
function Expect-Rejection([scriptblock]$Action, [string]$Message) {
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw }
        return
    }
    throw "Expected rejection: $Message"
}
try {
    $cert = Get-PackageSigningCertificate -Ephemeral
    $pe = Join-Path $scratch 'fixture.dll'
    Add-Type -TypeDefinition 'public static class SigningFixture { public static int Value() { return 42; } }' -OutputAssembly $pe
    $inputBytes = [IO.File]::ReadAllBytes($pe)
    $inputHash = Get-PackageSha256 $pe
    $stage = New-Item -ItemType Directory -Path (Join-Path $scratch 'stage')
    foreach ($name in @('OptiScaler.dll', 'dxgi.dll', 'version.dll', 'winmm.asi')) {
        Copy-Item -LiteralPath $pe -Destination (Join-Path $stage $name)
    }
    # An existing embedded signature from a different publisher must survive byte-for-byte.
    $vendor = Join-Path $stage 'vendor.exe'
    $vendorSource = @("$env:ProgramFiles/PowerShell/7/pwsh.exe", "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe") |
        Where-Object { (Test-Path -LiteralPath $_) -and (Get-AuthenticodeSignature -LiteralPath $_).SignatureType -eq 'Authenticode' } |
        Select-Object -First 1
    if (!$vendorSource) { throw 'Embedded signed Microsoft fixture unavailable' }
    Copy-Item -LiteralPath $vendorSource -Destination $vendor
    $vendorHash = Get-PackageSha256 $vendor
    Invoke-PackageSigning -Stage $stage -Thumbprint $cert.Thumbprint
    $manifest = Get-Content (Join-Path $stage 'SIGNATURES.json') -Raw | ConvertFrom-Json
    if ($manifest.binaries.Count -ne 5) { throw 'Missing binary manifest entries' }
    foreach ($entry in $manifest.binaries) {
        if ($entry.signedSha256 -ne (Get-PackageSha256 (Join-Path $stage $entry.file))) { throw 'Manifest hash mismatch' }
        if (!$entry.preservedSignature -and $entry.inputSha256 -ne $inputHash) { throw 'Original hash lost' }
    }
    if ((Get-PackageSha256 $vendor) -ne $vendorHash -or (Get-PackageSha256 $pe) -ne $inputHash) { throw 'Input/vendor mutated' }
    $signed = Join-Path $stage 'dxgi.dll'
    $signedHash = Get-PackageSha256 $signed
    Invoke-PackageSigning -Stage ($stage.FullName + '/') -Thumbprint $cert.Thumbprint
    if ((Get-PackageSha256 $signed) -ne $signedHash) { throw 'Repeated signing changed an existing signature' }

    # Corrupt mapped bytes (not the checksum/security-directory exclusions).
    $bytes = [IO.File]::ReadAllBytes($signed)
    $offset = [BitConverter]::ToInt32($bytes, 0x3c)
    $sections = $offset + 24 + [BitConverter]::ToUInt16($bytes, $offset + 20)
    $payload = [BitConverter]::ToUInt32($bytes, $sections + 20)
    $bytes[$payload + 64] = $bytes[$payload + 64] -bxor 1
    [IO.File]::WriteAllBytes($signed, $bytes)
    Expect-Rejection { Invoke-PackageSigning -Stage $stage -Thumbprint $cert.Thumbprint } 'Authenticode verification failed'
    Expect-Rejection { Assert-AuthenticodePayloadUnchanged $inputBytes $bytes } 'modified the executable payload'
    Expect-Rejection { Get-PackageSigningCertificate -Thumbprint '../bad' } 'Invalid signing certificate'
    $junction = Join-Path $scratch 'linked-stage'
    New-Item -ItemType Junction -Path $junction -Target $stage.FullName | Out-Null
    try { Expect-Rejection { Invoke-PackageSigning -Stage $junction -Thumbprint $cert.Thumbprint } 'reparse point' }
    finally { [IO.Directory]::Delete($junction) }
    Write-Host 'package-signing: PASS (actual PE signatures, aliases, vendor preservation, repeat, tamper, payload, reparse, hashes)'
} finally {
    if ($cert) { Remove-Item -LiteralPath "Cert:/CurrentUser/My/$($cert.Thumbprint)" -DeleteKey -ErrorAction Stop }
    # A fresh GUID path under this repository only; no user content or followed links.
    $resolved = [IO.Path]::GetFullPath($scratch)
    $allowed = [IO.Path]::GetFullPath((Join-Path $root 'work/scratch')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
