param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$Admin)
$ErrorActionPreference='Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $preset = if ($Admin) { 'windows-admin' } else { 'windows-' + $Configuration.ToLowerInvariant() }
    ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CST tests failed' }
} finally { Pop-Location }
