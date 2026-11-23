<#
Builds a release of Chatterino Noctis: the installer, a portable zip and
SHA256SUMS.txt.

  .\build-release.ps1 -Version 1.0.0 -Exe <built Chatterino Noctis.exe> -Runtime <folder with the Qt/OpenSSL/VC runtime> -QtSourceBundle <Qt-6.9.3-corresponding-source.zip> -Out <folder>

The runtime folder is a working installation (or a fresh deployment); only the
files on the allowlist below are taken from it, never a profile (Settings,
Cache, Logs, ...). The result is checked for profile files before packing.
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string] $Runtime,
    [Parameter(Mandatory)] [string] $Out,
    [string] $QtSourceBundle,
    [string] $Iscc = 'D:\devtools\InnoSetup6\ISCC.exe'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..')
$staging = Join-Path $Out 'staging'
$dist = Join-Path $Out 'dist'

if (Test-Path $Out) { throw "Output folder already exists: $Out" }
# The program compares itself with the releases by this number.
$versionHeader = Join-Path $PSScriptRoot '..\..\src\common\Mist1XVersion.hpp'
$inSource = [regex]::Match((Get-Content $versionHeader -Raw), 'MIST1X_VERSION "([^"]+)"').Groups[1].Value
if ($inSource -ne $Version) { throw "MIST1X_VERSION in src/common/Mist1XVersion.hpp is '$inSource', not '$Version'. Change it and rebuild first." }
# Validate the exact bytes that will be packaged. The public and Personal
# checkouts can share both the product name and the numeric release version.
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($exePath)
if ($versionInfo.ProductName -ne 'Chatterino Noctis' -or
    $versionInfo.OriginalFilename -ne 'Chatterino Noctis.exe') {
    throw "Expected the Chatterino Noctis executable: $exePath"
}
if ($versionInfo.FileVersion -ne $Version -or $versionInfo.ProductVersion -ne $Version) {
    throw "The main executable's version metadata does not match $Version."
}
$exeBytes = [IO.File]::ReadAllBytes($exePath)
$byteEncoding = [Text.Encoding]::GetEncoding(28591)
$binaryText = $byteEncoding.GetString($exeBytes)
function Test-CompiledMarker([string] $Marker) {
    $wide = $byteEncoding.GetString([Text.Encoding]::Unicode.GetBytes($Marker))
    return $binaryText.Contains($Marker) -or $binaryText.Contains($wide)
}
if (-not (Test-CompiledMarker ("ChatterinoNoctis/$Version" + [char]0))) {
    throw "The executable has a different Noctis update version than $Version."
}
foreach ($marker in @('Qt6Charts.dll', 'Qt5Charts.dll', 'QtCharts')) {
    if (Test-CompiledMarker $marker) {
        throw "The executable still depends on the removed Qt Charts module: $marker"
    }
}
foreach ($marker in @('twitch-auto-replies.json', 'NoctisCategoryTracker.exe',
                      'archive-bridge.json', 'archive-card', '/archive/socketName',
                      'ArchiveBanPopup', 'UsercardArchiveToggle', 'panel-control.txt',
                      'local-test-chat', 'LocalTestChat', 'AutoReplyPrimaryTabs',
                      'Mist1XUsersDialog', 'Bot Badge (Developer)')) {
    if (Test-CompiledMarker $marker) {
        throw "The executable contains a Personal-only feature: $marker"
    }
}
# Old shortcuts and existing Run entries launch this forwarding executable.
$launcher = Join-Path (Split-Path -LiteralPath $exePath) 'Leafyrino.exe'
$launcherInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($launcher)
if ($launcherInfo.ProductName -ne 'Chatterino Noctis' -or
    $launcherInfo.FileDescription -ne 'Chatterino Noctis compatibility launcher' -or
    $launcherInfo.OriginalFilename -ne 'Leafyrino.exe' -or
    $launcherInfo.ProductVersion -ne $Version) {
    throw "Expected the compatibility launcher beside the main executable."
}
if ([string]::IsNullOrWhiteSpace($QtSourceBundle) -or -not (Test-Path -LiteralPath $QtSourceBundle -PathType Leaf)) {
    throw "Provide the matching Qt corresponding-source ZIP with -QtSourceBundle."
}
if ((Split-Path -Leaf $QtSourceBundle) -ne 'Qt-6.9.3-corresponding-source.zip') {
    throw "Unexpected Qt source archive; review the matching runtime source manifest."
}
$qtSourceHashes = @{
    'qtbase-everywhere-src-6.9.3.tar.xz' = 'c5a1a2f660356ec081febfa782998ae5ddbc5925117e64f50e4be9cd45b8dc6e'
    'qtsvg-everywhere-src-6.9.3.tar.xz' = 'db76aa3358cbbe6fce7da576ff4669cb9801920188c750d3b12783bbe97026e2'
    'qtimageformats-everywhere-src-6.9.3.tar.xz' = '4fb26bdbfbd4b8e480087896514e11c33aba7b6b39246547355ea340c4572ffe'
}
$zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $QtSourceBundle).Path)
try {
    foreach ($name in $qtSourceHashes.Keys) {
        $entry = $zip.GetEntry($name)
        if ($null -eq $entry) { throw "Missing Qt corresponding source: $name" }
        $stream = $entry.Open()
        try { $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLower() }
        finally { $stream.Dispose() }
        if ($digest -ne $qtSourceHashes[$name]) { throw "Qt source checksum mismatch: $name" }
    }
} finally { $zip.Dispose() }
New-Item -ItemType Directory -Force $staging, $dist | Out-Null

# 1. Program: the built exe and the runtime it needs (allowlist).
$qtRuntime = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Runtime 'Qt6Core.dll'))
$qtVersion = "{0}.{1}.{2}" -f $qtRuntime.FileMajorPart, $qtRuntime.FileMinorPart, $qtRuntime.FileBuildPart
$qtSources = Get-Content -LiteralPath (Join-Path $repo 'LICENSES\Qt-sources.md') -Raw
if (-not $qtSources.Contains("Windows $qtVersion)")) {
    throw "Update the Qt runtime corresponding-source and license manifest before packaging Qt $qtVersion."
}
[IO.File]::WriteAllBytes((Join-Path $staging 'Chatterino Noctis.exe'), $exeBytes)
Copy-Item -LiteralPath $launcher -Destination $staging
$runtimeFiles = @(
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6OpenGL.dll',
    'Qt6OpenGLWidgets.dll', 'Qt6Svg.dll', 'Qt6Widgets.dll',
    'D3Dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll',
    'libcrypto-3-x64.dll', 'libssl-3-x64.dll',
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll'
)
foreach ($file in $runtimeFiles) { Copy-Item (Join-Path $Runtime $file) $staging }
# Release packages must not inherit an existing installation's nightly mode.
[IO.File]::WriteAllText((Join-Path $staging 'modes'), "portable`n", [Text.Encoding]::ASCII)
$runtimeDirs = @('generic', 'iconengines', 'imageformats', 'networkinformation',
                 'platforms', 'styles', 'tls', 'crashpad')
foreach ($dir in $runtimeDirs) { Copy-Item (Join-Path $Runtime $dir) $staging -Recurse }

# 2. Icon, licences, the guide and release notes.
Copy-Item (Join-Path $repo 'resources\icon.ico') (Join-Path $staging 'Chatterino-Noctis.ico')
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $staging 'LICENSE.txt')
Copy-Item (Join-Path $repo 'LICENSES') $staging -Recurse
# licenses of the bundled libraries, fonts and data (the same texts as in About)
Copy-Item (Join-Path $repo 'resources\licenses') (Join-Path $staging 'LICENSES\third-party') -Recurse
Copy-Item (Join-Path $repo 'CREDITS.md') (Join-Path $staging 'Авторы и лицензии.txt')
Copy-Item (Join-Path $here 'Руководство.html') $staging
$notes = Join-Path $here "notes-$Version.md"
if (-not (Test-Path -LiteralPath $notes -PathType Leaf)) {
    throw "Release notes not found: $notes"
}
Copy-Item -LiteralPath $notes -Destination (Join-Path $staging 'Что нового.txt')
# Noctis Dark is built into the client. Never ship a writable profile theme
# in an overlay update: users may have edited even the old default filename.

# 3. Nothing from a profile may be in the package.
$forbidden = @('Settings', 'Cache', 'Logs', 'Misc', 'Plugins', 'ProfileAvatars', 'IPC', 'Crashes', 'Themes')
foreach ($name in $forbidden) {
    if (Test-Path (Join-Path $staging $name)) { throw "Profile folder in the package: $name" }
}
$bad = Get-ChildItem $staging -Recurse -File | Where-Object {
    $_.Extension -in '.json', '.txt', '.ini', '.log' -and
    (Select-String -Path $_.FullName -Pattern 'oauth:|"oauthToken"|access_token' -Quiet)
}
if ($bad) { throw "Token-like text in: $($bad.FullName -join ', ')" }

# 4. Installer.
& $Iscc "/DAppVersion=$Version" "/DStagingDir=$staging" "/DOutputDir=$dist" (Join-Path $here 'ChatterinoNoctis.iss') | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed: $LASTEXITCODE" }

# 5. Portable zip: the same files in one folder.
$portableName = "ChatterinoNoctis-$Version-portable"
$portable = Join-Path $Out $portableName
Copy-Item $staging $portable -Recurse
Compress-Archive -Path $portable -DestinationPath (Join-Path $dist "$portableName.zip") -CompressionLevel Optimal
$resolvedPortable = (Resolve-Path -LiteralPath $portable).Path
$expectedPortable = Join-Path (Resolve-Path -LiteralPath $Out).Path $portableName
if ($resolvedPortable -ne $expectedPortable) { throw "Unexpected portable staging path: $resolvedPortable" }
Remove-Item -LiteralPath $resolvedPortable -Recurse -Force

# Keep transitional asset names so versions published before Noctis can update
# normally. Both names point to the same verified installer or portable ZIP.
Copy-Item (Join-Path $dist "ChatterinoNoctis-Setup-$Version.exe") (Join-Path $dist "ChatterinoByMist1X-Setup-$Version.exe")
Copy-Item (Join-Path $dist "$portableName.zip") (Join-Path $dist "ChatterinoByMist1X-$Version-portable.zip")

# 6. Checksums.
Copy-Item -LiteralPath $QtSourceBundle -Destination $dist
$sums = Get-ChildItem $dist -File | Where-Object Name -ne 'SHA256SUMS.txt' | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllLines((Join-Path $dist 'SHA256SUMS.txt'), $sums)
Get-ChildItem $dist | Select-Object Name, Length | Format-Table -AutoSize | Out-Host
