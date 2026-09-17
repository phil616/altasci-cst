param([ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    cmake --workflow --preset ("windows-" + $Configuration.ToLowerInvariant())
    if ($LASTEXITCODE -ne 0) { throw 'CST build workflow failed' }
} finally { Pop-Location }
