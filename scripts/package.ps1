param([switch]$Production)
$ErrorActionPreference='Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $preset = if ($Production) { 'windows-package-production' } else { 'windows-package' }
    cmake --workflow --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CST packaging workflow failed' }
} finally { Pop-Location }
