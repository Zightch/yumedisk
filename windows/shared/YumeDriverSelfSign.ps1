param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('SignBinary', 'FinalizePackage')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [string]$SubjectName,

    [Parameter(Mandatory = $true)]
    [string]$CertificateFileName,

    [string]$FilePath,
    [string]$PackageDir,
    [string]$ExportCerPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Get-Command New-SelfSignedCertificate -ErrorAction SilentlyContinue)) {
    Import-Module PKI -ErrorAction Stop
}

if (-not [string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
}

function Normalize-Thumbprint {
    param([string]$Thumbprint)

    if ([string]::IsNullOrWhiteSpace($Thumbprint)) {
        return ''
    }

    return ($Thumbprint -replace '\s', '').ToUpperInvariant()
}

function Get-SignToolPath {
    $candidates = @(
        'C:\Windows Kits\10\bin\*\x64\signtool.exe',
        'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe'
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

function Remove-EmbeddedSignatureIfPresent {
    param(
        [string]$SignToolPath,
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "File not found: $Path"
    }

    & $SignToolPath remove /s $Path
    if ($LASTEXITCODE -ne 0) {
        throw "signtool remove failed for $Path with exit code $LASTEXITCODE."
    }
}

function Get-SigningCertificate {
    param([string]$RawSubjectName)

    $subjectName = $RawSubjectName
    if (-not $subjectName.StartsWith('CN=', [System.StringComparison]::OrdinalIgnoreCase)) {
        $subjectName = 'CN=' + $subjectName
    }

    $store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
        [System.Security.Cryptography.X509Certificates.StoreName]::My,
        [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser)
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    try {
        $certificate = $store.Certificates |
            Where-Object {
                $_.HasPrivateKey -and
                $_.Subject -eq $subjectName
            } |
            Sort-Object NotAfter -Descending |
            Select-Object -First 1

        if ($null -ne $certificate) {
            return $certificate
        }
    } finally {
        $store.Close()
    }

    return New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $subjectName `
        -FriendlyName $subjectName `
        -CertStoreLocation 'cert:\CurrentUser\My' `
        -HashAlgorithm 'SHA256' `
        -KeyAlgorithm 'RSA' `
        -KeyLength 4096 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(10)
}

function Export-CertificateFile {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $directory = Split-Path -Path $Path -Parent
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    [System.IO.File]::WriteAllBytes(
        $Path,
        $Certificate.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
}

function Sign-FileWithCertificate {
    param(
        [string]$SignToolPath,
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
        [string]$Path,
        [bool]$RemoveExistingSignature
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'A file path is required for signing.'
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "File not found: $Path"
    }

    $thumbprint = Normalize-Thumbprint $Certificate.Thumbprint
    if ($RemoveExistingSignature) {
        Remove-EmbeddedSignatureIfPresent -SignToolPath $SignToolPath -Path $Path
    }
    & $SignToolPath sign /ph /fd sha256 /sha1 $thumbprint $Path
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed for $Path with exit code $LASTEXITCODE."
    }
}

function Get-SinglePackageFile {
    param(
        [string]$DirectoryPath,
        [string]$Filter
    )

    if ([string]::IsNullOrWhiteSpace($DirectoryPath)) {
        throw 'A package directory is required.'
    }
    if (-not (Test-Path -LiteralPath $DirectoryPath -PathType Container)) {
        throw "Package directory not found: $DirectoryPath"
    }

    $files = @(Get-ChildItem -LiteralPath $DirectoryPath -Filter $Filter -File)
    if ($files.Count -eq 0) {
        throw "No files matching $Filter were found in $DirectoryPath"
    }
    if ($files.Count -gt 1) {
        throw "Multiple files matching $Filter were found in $DirectoryPath"
    }

    return $files[0].FullName
}

function Copy-CertificateIntoPackage {
    param(
        [string]$SourcePath,
        [string]$DirectoryPath,
        [string]$FileName
    )

    if ([string]::IsNullOrWhiteSpace($SourcePath) -or -not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        throw "Certificate file not found: $SourcePath"
    }
    if ([string]::IsNullOrWhiteSpace($DirectoryPath) -or -not (Test-Path -LiteralPath $DirectoryPath -PathType Container)) {
        throw "Package directory not found: $DirectoryPath"
    }

    Copy-Item -LiteralPath $SourcePath -Destination (Join-Path $DirectoryPath $FileName) -Force
}

$certificate = Get-SigningCertificate -RawSubjectName $SubjectName
Export-CertificateFile -Certificate $certificate -Path $ExportCerPath
$signToolPath = Get-SignToolPath

switch ($Action) {
    'SignBinary' {
        Sign-FileWithCertificate `
            -SignToolPath $signToolPath `
            -Certificate $certificate `
            -Path $FilePath `
            -RemoveExistingSignature $true
    }

    'FinalizePackage' {
        if ([string]::IsNullOrWhiteSpace($PackageDir)) {
            throw 'A package directory is required for FinalizePackage.'
        }

        $packageBinaryPath = Join-Path $PackageDir ([System.IO.Path]::GetFileName($FilePath))
        if (-not (Test-Path -LiteralPath $packageBinaryPath -PathType Leaf)) {
            throw "Packaged driver binary not found: $packageBinaryPath"
        }

        $catalogPath = Get-SinglePackageFile -DirectoryPath $PackageDir -Filter '*.cat'
        Sign-FileWithCertificate `
            -SignToolPath $signToolPath `
            -Certificate $certificate `
            -Path $catalogPath `
            -RemoveExistingSignature $false
        Copy-CertificateIntoPackage -SourcePath $ExportCerPath -DirectoryPath $PackageDir -FileName $CertificateFileName
    }
}
