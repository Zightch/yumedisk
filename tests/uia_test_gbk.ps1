param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [object[]]$PassthroughArgs
)

try {
    [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936)
} catch {
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& pwsh -NoLogo -NoProfile -File (Join-Path $scriptDir "uia_test.ps1") @PassthroughArgs
exit $LASTEXITCODE
