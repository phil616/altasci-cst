param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
New-Item -ItemType Directory -Force $Destination | Out-Null

function Get-WebText([string]$Url) {
    $content = (Invoke-WebRequest $Url -MaximumRetryCount 3).Content
    if ($content -is [byte[]]) { return [Text.Encoding]::UTF8.GetString($content) }
    return [string]$content
}

function Get-Archive([string]$Url, [string]$Path, [string]$ChecksumUrl) {
    Invoke-WebRequest $Url -OutFile $Path -MaximumRetryCount 3
    if ($ChecksumUrl) {
        $expected = (Get-WebText $ChecksumUrl).Trim().Split(' ')[0]
        if ((Get-FileHash $Path -Algorithm SHA1).Hash -ine $expected) { throw "Checksum mismatch: $Url" }
    }
}
function Install-QtArchives([string]$Repository, [string]$PackageName, [string]$ExpectedVersion, [string]$Target, [string[]]$Prefixes) {
    [xml]$index = Get-WebText "$Repository/Updates.xml"
    $package = @($index.Updates.PackageUpdate | Where-Object Name -eq $PackageName)
    if ($package.Count -ne 1 -or -not $package[0].Version.StartsWith($ExpectedVersion + '-')) { throw "Unexpected Qt repository version for $PackageName" }
    $package = $package[0]
    New-Item -ItemType Directory -Force $Target | Out-Null
    foreach ($archive in $package.DownloadableArchives.Split(',').Trim()) {
        if (-not ($Prefixes | Where-Object { $archive.StartsWith($_) })) { continue }
        $url = "$Repository/$PackageName/$($package.Version)$archive"
        $download = Join-Path $Destination $archive
        Get-Archive $url $download "$url.sha1"
        & 7z x -y "-o$Target" $download | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $archive" }
        Remove-Item $download
    }
}

$qt = Join-Path $Destination 'Qt/6.11.2/msvc2022_64'
if (-not (Test-Path "$qt/bin/qmake.exe")) {
    Install-QtArchives 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_msvc2022_64' 'qt.qt6.6112.win64_msvc2022_64' '6.11.2' $qt @('qtbase-', 'qttools-')
    "[Paths]`nPrefix=..`n" | Set-Content "$qt/bin/qt.conf" -Encoding utf8
}
$ifw = Join-Path $Destination 'Qt/Tools/QtInstallerFramework/4.11'
if (-not (Test-Path "$ifw/bin/binarycreator.exe")) {
    Install-QtArchives 'https://download.qt.io/online/qtsdkrepository/windows_x86/ifw/tools_ifw_411' 'qt.tools.ifw.411' '4.11.0' $ifw @('ifw-win-x64')
}
$cmake = Join-Path $Destination 'cmake-4.4.3-windows-x86_64'
if (-not (Test-Path "$cmake/bin/cmake.exe")) {
    $archive = Join-Path $Destination 'cmake.zip'
    Get-Archive 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip' $archive ''
    $checksums = Get-WebText 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-SHA-256.txt'
    $line = ($checksums -split "`n" | Where-Object { $_ -match 'cmake-4.4.3-windows-x86_64.zip$' })
    $expected = ($line -split '\s+')[0]
    if ((Get-FileHash $archive -Algorithm SHA256).Hash -ine $expected) { throw 'CMake checksum mismatch' }
    Expand-Archive $archive $Destination -Force
    Remove-Item $archive
}
$git = Join-Path $Destination 'Git'
if (-not (Test-Path "$git/cmd/git.exe")) {
    $archive = Join-Path $Destination 'mingit.zip'
    Get-Archive 'https://github.com/git-for-windows/git/releases/download/v2.55.0.windows.1/MinGit-2.55.0-64-bit.zip' $archive ''
    Expand-Archive $archive $git -Force
    Remove-Item $archive
}
"CST_QT_ROOT=$qt" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"CST_IFW_ROOT=$ifw" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"CST_TEST_GIT=$git/cmd/git.exe" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"$cmake/bin" | Out-File $env:GITHUB_PATH -Append -Encoding utf8
"$qt/bin" | Out-File $env:GITHUB_PATH -Append -Encoding utf8
& "$qt/bin/qmake.exe" -query QT_VERSION
if ($LASTEXITCODE -ne 0) { throw 'Qt SDK failed verification' }
& "$ifw/bin/binarycreator.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'IFW failed verification' }
& "$git/cmd/git.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'Git failed verification' }
