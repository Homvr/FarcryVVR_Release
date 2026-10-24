# Packages the mod built by build.bat into out\FC5VR_<Release>.zip: one folder bin\ that mirrors the game's (dxgi.dll,
# openxr_loader.dll, nvngx_dlss.dll, FC5VR_Launcher.exe, fc5vr_settings.txt = the mod's defaults, FC5VR_README.txt,
# KNOWN_ISSUES.txt, ACKNOWLEDGMENTS.txt, LICENSE.txt), so it is extracted into the game folder; out\SHA256.txt beside it.
# Nexus Mods quarantines nested archives, password-protected and self-extracting files, and archives it cannot preview
# (help.nexusmods.com/article/117): the zip is a plain zip written by Windows' tar (/ separators), checked for archives.
# Usage (repository root, after build.bat and deps\ngx\nvngx_dlss.dll): release\package.ps1 -Release alpha_0_11 [-Title "alpha 0.11"]
param([string]$Release = 'test', [string]$Title = '', [string]$Commit = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $Commit) { $Commit = (git -C $repo rev-parse --short HEAD).Trim() }
$name = "FC5VR_$Release"
$shown = if ($Title) { $Title } else { $Release }
$date = (Get-Date).ToString('d MMMM yyyy', [Globalization.CultureInfo]::InvariantCulture)
$out = Join-Path $repo 'out'
$root = Join-Path $out 'zip'
$stage = Join-Path $root 'bin'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# The mod, the launcher and the runtime DLLs testers need.
Copy-Item "$repo\fc5vr\build\build-release-msvc\dxgi.dll", "$repo\launcher\out\FC5VR_Launcher.exe" $stage
Copy-Item "$repo\fc5vr\shipping\openxr_loader.dll", "$repo\deps\ngx\nvngx_dlss.dll" $stage
# The shipped settings are the mod's defaults (FarCry5Defaults.h): the file the mod writes when there is none.
$defh = Get-Content "$repo\fc5vr\src\FarCry5\FarCry5Defaults.h" -Raw
$defs = [regex]::Match($defh, '(?s)R"FC5VR\(\r?\n(.*?)\)FC5VR"').Groups[1].Value -split '\r?\n' | Where-Object { $_ -ne '' }
if (-not $defs) { throw 'no default settings found in FarCry5Defaults.h' }
$defs | Set-Content (Join-Path $stage 'fc5vr_settings.txt') -Encoding ascii
# The texts.
(Get-Content "$PSScriptRoot\FC5VR_README.txt" -Raw).Replace('{RELEASE}', $shown).Replace('{DATE}', $date).Replace('{SHA}', $Commit) |
    Set-Content (Join-Path $stage 'FC5VR_README.txt') -Encoding ascii -NoNewline
Copy-Item "$PSScriptRoot\KNOWN_ISSUES.txt", "$repo\ACKNOWLEDGMENTS.txt" $stage
(Get-Content "$repo\fc5vr\LICENSE" -Raw) + "`r`n---- VR Framework (fc5vr\extern\vrframework) ----`r`n`r`n" + (Get-Content "$repo\fc5vr\extern\vrframework\LICENSE" -Raw) |
    Set-Content (Join-Path $stage 'LICENSE.txt') -Encoding ascii -NoNewline

# The zip, by Windows' own tar (/ separators; Git's GNU tar earlier on the PATH cannot write zips).
$zip = Join-Path $out "$name.zip"
& (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $zip -C $root bin
if ($LASTEXITCODE -ne 0) { throw 'zip failed' }
Remove-Item $root -Recurse -Force

# Checks: every entry under bin/, no archive inside, no \ in a path.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $bad = @()
    foreach ($e in $z.Entries) {
        if (-not $e.FullName.StartsWith('bin/')) { $bad += "outside bin/: $($e.FullName)" }
        if ($e.FullName -match '\.(zip|7z|rar|tar|gz|tgz|bz2|xz|zst|cab|iso|msi|nupkg|jar|apk|sfx)$') { $bad += "nested archive: $($e.FullName)" }
        if ($e.FullName.Contains('\')) { $bad += "backslash in path: $($e.FullName)" }
    }
    $z.Entries | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName }
    if ($bad) { $bad | ForEach-Object { "  $_" }; throw "$zip is not fit for Nexus Mods" }
} finally { $z.Dispose() }
"$((Get-FileHash $zip -Algorithm SHA256).Hash)  $name.zip" | Set-Content (Join-Path $out 'SHA256.txt') -Encoding ascii
Get-Content (Join-Path $out 'SHA256.txt')
