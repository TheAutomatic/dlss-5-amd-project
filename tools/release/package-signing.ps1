# Shared by local packaging and Actions. Sign staging copies only; never rewrite tested build artifacts.
# A Windows PowerShell child of pwsh may inherit only PowerShell 7 module paths.
# Import Windows modules explicitly instead of depending on that environment.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    foreach ($module in @('Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security', 'PKI')) {
        Import-Module (Join-Path $PSHOME "Modules/$module/$module.psd1") -ErrorAction Stop
    }
}

function Get-PackageSigningCertificate {
    [CmdletBinding()]
    param([string]$Thumbprint = '', [switch]$Ephemeral)
    $now = Get-Date
    if ($Thumbprint) {
        if ($Thumbprint -notmatch '^[0-9a-fA-F]{40}$') { throw 'Invalid signing certificate thumbprint.' }
        $cert = Get-Item -LiteralPath "Cert:/CurrentUser/My/$Thumbprint" -ErrorAction Stop
        if (!$cert.HasPrivateKey -or $cert.NotBefore -gt $now -or $cert.NotAfter -le $now -or
            !(@($cert.EnhancedKeyUsageList | ForEach-Object { [string]$_.ObjectId }) -contains '1.3.6.1.5.5.7.3.3')) {
            throw 'Selected certificate is not a current code-signing certificate with a private key.'
        }
        return $cert
    }
    if (!$Ephemeral) {
        $cert = Get-ChildItem Cert:/CurrentUser/My -CodeSigningCert |
            Where-Object { $_.Subject -match '(?:^|,\s*)CN=OptiScaler Dev SelfSign(?:,|$)' -and
                $_.HasPrivateKey -and $_.NotBefore -le $now -and $_.NotAfter -gt $now.AddDays(7) } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
        if ($cert) { return $cert }
    }
    # Ephemeral describes the CI private key, not the signature's validity period.
    # A two-day certificate would invalidate every downloaded package two days later.
    $expiry = $now.AddYears(5)
    # CurrentUser/My only: no administrator rights or system trust-store installation.
    return New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=OptiScaler Dev SelfSign' `
        -FriendlyName 'OptiScaler package signing' -CertStoreLocation Cert:/CurrentUser/My `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
        -NotAfter $expiry
}

function Initialize-PackageTrustVerifier {
    if ('OptiScaler.PackageTrust' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace OptiScaler {
    public static class PackageTrust {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct FileInfo {
            public uint cbStruct;
            [MarshalAs(UnmanagedType.LPWStr)] public string path;
            public IntPtr file, subject;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct TrustData {
            public uint cbStruct;
            public IntPtr callback, sip;
            public uint ui, revocation, choice;
            public IntPtr file;
            public uint action;
            public IntPtr state;
            public IntPtr url;
            public uint flags, uiContext;
            public IntPtr settings;
        }
        [DllImport("wintrust.dll", ExactSpelling = true)]
        static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
        public static uint Verify(string path) {
            var file = new FileInfo { cbStruct = (uint)Marshal.SizeOf(typeof(FileInfo)), path = path };
            IntPtr memory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
            try {
                Marshal.StructureToPtr(file, memory, false);
                var data = new TrustData {
                    cbStruct = (uint)Marshal.SizeOf(typeof(TrustData)), ui = 2, choice = 1,
                    file = memory, flags = 0x1010 // cache-only URL retrieval, no revocation network requests
                };
                var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
                return unchecked((uint)WinVerifyTrust(new IntPtr(-1), ref action, ref data));
            } finally {
                Marshal.DestroyStructure(memory, typeof(FileInfo));
                Marshal.FreeHGlobal(memory);
            }
        }
    }
}
'@
}

function Get-PackageSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function Assert-AuthenticodePayloadUnchanged([byte[]]$Before, [byte[]]$After) {
    if ($Before.Length -lt 256 -or $After.Length -le $Before.Length -or
        $Before[0] -ne 77 -or $Before[1] -ne 90) { throw 'Not a signable PE binary.' }
    $pe = [BitConverter]::ToInt32($Before, 0x3c)
    if ($pe -lt 64 -or $pe + 184 -gt $Before.Length -or
        [BitConverter]::ToUInt32($Before, $pe) -ne 0x4550) { throw 'Invalid PE header.' }
    $optional = $pe + 24
    $magic = [BitConverter]::ToUInt16($Before, $optional)
    if ($magic -notin @(0x10b, 0x20b)) { throw 'Unsupported PE optional header.' }
    $security = $optional + $(if ($magic -eq 0x20b) { 144 } else { 128 })
    $checksum = $optional + 64
    if ([BitConverter]::ToUInt64($Before, $security) -ne 0) { throw 'Refusing to replace an existing PE certificate.' }
    $certStart = [BitConverter]::ToUInt32($After, $security)
    $certSize = [BitConverter]::ToUInt32($After, $security + 4)
    $aligned = ($Before.LongLength + 7) -band -8L
    if ($certStart -ne $aligned -or $certSize -lt 8 -or $certStart + [long]$certSize -ne $After.LongLength) {
        throw 'Signing changed the PE overlay beyond the Authenticode certificate.'
    }
    # SHA of every original byte, ignoring only the two fields Authenticode may change.
    $normalized = New-Object byte[] $Before.Length
    [Array]::Copy($After, $normalized, $Before.Length)
    [Array]::Copy($Before, $checksum, $normalized, $checksum, 4)
    [Array]::Copy($Before, $security, $normalized, $security, 8)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        if ([BitConverter]::ToString($sha.ComputeHash($Before)) -ne
            [BitConverter]::ToString($sha.ComputeHash($normalized))) { throw 'Signing modified the executable payload.' }
    } finally { $sha.Dispose() }
    for ($i = $Before.LongLength; $i -lt $certStart; $i++) {
        if ($After[$i] -ne 0) { throw 'Unexpected nonzero signing alignment padding.' }
    }
}

function Invoke-PackageSigning {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage, [string]$Thumbprint = '')
    $Stage = (Resolve-Path -LiteralPath $Stage -ErrorAction Stop).Path
    if (!(Test-Path -LiteralPath $Stage -PathType Container)) { throw 'Signing stage is not a directory.' }
    $Stage = $Stage.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ((Get-Item -LiteralPath $Stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing to sign through a reparse point.'
    }
    $items = @(Get-ChildItem -LiteralPath $Stage -Recurse -Force)
    if (@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
        throw 'Refusing to sign through a reparse point.'
    }
    $files = @($items | Where-Object { !$_.PSIsContainer -and $_.Extension -in @('.dll', '.exe', '.asi', '.ocx') } |
        Sort-Object FullName)
    if (!$files.Count) { throw 'No release binaries to sign.' }
    Initialize-PackageTrustVerifier
    $cert = Get-PackageSigningCertificate -Thumbprint $Thumbprint
    $records = foreach ($file in $files) {
        $beforeHash = Get-PackageSha256 $file.FullName
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        # Catalog trust belongs to the installed OS, not to a portable copied PE.
        # Only an embedded signature travels with the package and is preserved.
        $hasEmbeddedCert = $false
        $peBytes = [IO.File]::ReadAllBytes($file.FullName)
        if ($peBytes.Length -ge 256 -and $peBytes[0] -eq 77 -and $peBytes[1] -eq 90) {
            $peOffset = [BitConverter]::ToInt32($peBytes, 0x3c)
            if ($peOffset -ge 64 -and $peOffset + 184 -le $peBytes.Length) {
                $magic = [BitConverter]::ToUInt16($peBytes, $peOffset + 24)
                $secOffset = $peOffset + 24 + $(if ($magic -eq 0x20b) { 144 } else { 128 })
                if ([BitConverter]::ToUInt64($peBytes, $secOffset) -ne 0) { $hasEmbeddedCert = $true }
            }
        }
        $preserved = ($null -ne $signature.SignerCertificate -and $signature.SignatureType -eq 'Authenticode') -or
                     ($hasEmbeddedCert -and $null -ne $signature.SignerCertificate)
        if (!$preserved) {
            if ($signature.Status -ne 'NotSigned' -and
                !($signature.SignatureType -eq 'Catalog' -and $signature.Status -eq 'Valid')) {
                throw "Invalid unsigned PE: $($file.Name) ($($signature.Status))"
            }
            $bytes = [IO.File]::ReadAllBytes($file.FullName)
            $signature = Set-AuthenticodeSignature -LiteralPath $file.FullName -Certificate $cert -HashAlgorithm SHA256
            Assert-AuthenticodePayloadUnchanged $bytes ([IO.File]::ReadAllBytes($file.FullName))
            if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $cert.Thumbprint) {
                throw "Signing failed: $($file.Name)"
            }
        }
        $status = [OptiScaler.PackageTrust]::Verify($file.FullName)
        # Trust chains may end at our self-signed certificate or an offline third-party root.
        # Other failures (bad digest, revoked, expired, wrong EKU, malformed) are never accepted.
        if ($status -notin @([uint32]0, [uint32]0x800B0109L, [uint32]0x800B010AL)) {
            throw ("Authenticode verification failed for {0}: 0x{1:X8}" -f $file.Name, $status)
        }
        $afterHash = Get-PackageSha256 $file.FullName
        if ($preserved -and $beforeHash -ne $afterHash) { throw 'Existing vendor signature was modified.' }
        [ordered]@{
            file = $file.FullName.Substring($Stage.Length + 1).Replace('\', '/')
            inputSha256 = $beforeHash
            signedSha256 = $afterHash
            signerThumbprint = $signature.SignerCertificate.Thumbprint
            preservedSignature = $preserved
            trustResult = ('0x{0:X8}' -f $status)
        }
    }
    $manifest = [ordered]@{
        schema = 1
        note = 'Authenticode signatures; self-signed certificates do not imply Windows publisher trust. No trust store is modified.'
        binaries = @($records)
    }
    [IO.File]::WriteAllText((Join-Path $Stage 'SIGNATURES.json'), ($manifest | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))
    Write-Host "Authenticode: verified $($files.Count) release binaries (existing signatures preserved)."
}

