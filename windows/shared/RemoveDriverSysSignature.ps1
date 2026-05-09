[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [Alias('Path')]
    [string]$SysPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-FullPath {
    param([string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Get-SignToolPath {
    $candidates = @(
        'C:\\Windows Kits\\10\\bin\\*\\x64\\signtool.exe',
        'C:\\Program Files (x86)\\Windows Kits\\10\\bin\\*\\x64\\signtool.exe'
    )

    foreach ($pattern in $candidates) {
        $match = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($null -ne $match) {
            return $match.FullName
        }
    }

    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw 'Unable to locate signtool.exe.'
}

if ([string]::IsNullOrWhiteSpace($SysPath)) {
    throw 'A .sys file path is required.'
}

$resolvedSysPath = Get-FullPath -Path $SysPath
if (-not (Test-Path -LiteralPath $resolvedSysPath -PathType Leaf)) {
    throw "SYS file not found: $resolvedSysPath"
}
if ([System.IO.Path]::GetExtension($resolvedSysPath) -ine '.sys') {
    throw "Only .sys files are supported: $resolvedSysPath"
}

$beforeSignature = Get-AuthenticodeSignature -FilePath $resolvedSysPath
$beforeThumbprint = ''
if ($null -ne $beforeSignature.SignerCertificate) {
    $beforeThumbprint = $beforeSignature.SignerCertificate.Thumbprint
}

if ($beforeSignature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned -and
    $null -eq $beforeSignature.SignerCertificate) {
    [pscustomobject]@{
        SysPath              = $resolvedSysPath
        Action               = 'NoOp'
        RemovedAllSignatures = $false
        BeforeStatus         = [string]$beforeSignature.Status
        AfterStatus          = [string]$beforeSignature.Status
        PreviousThumbprint   = $beforeThumbprint
    }
    return
}

$signToolPath = Get-SignToolPath
& $signToolPath remove /s $resolvedSysPath
if ($LASTEXITCODE -ne 0) {
    throw "signtool remove failed for $resolvedSysPath with exit code $LASTEXITCODE."
}

$afterSignature = Get-AuthenticodeSignature -FilePath $resolvedSysPath

[pscustomobject]@{
    SysPath              = $resolvedSysPath
    Action               = 'Removed'
    RemovedAllSignatures = ($afterSignature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned)
    BeforeStatus         = [string]$beforeSignature.Status
    AfterStatus          = [string]$afterSignature.Status
    PreviousThumbprint   = $beforeThumbprint
}
