<#
.SYNOPSIS
    Build, package, and optionally publish a SmallTV Custom release.

.DESCRIPTION
    Runs the PlatformIO firmware and LittleFS builds, copies the binaries into
    release/, packages the full source archive into dist/, and reports SHA256
    hashes for the CHANGELOG.

    Publishing to GitHub is opt-in. Without -Publish the script only produces
    local artifacts, so it is safe to run repeatedly while iterating.

.PARAMETER Version
    Release version tag, e.g. v1.0.2.

.PARAMETER Publish
    Create the git tag and the GitHub Release, uploading all three assets.
    Requires a clean tree, main checked out and pushed, and an authenticated gh.

.PARAMETER NotesFile
    Release notes markdown. Defaults to dist/release-notes-<Version>.md.

.PARAMETER Repo
    Target GitHub repository in owner/name form.

.PARAMETER SkipBuild
    Reuse existing .pio build output instead of rebuilding.

.EXAMPLE
    Build local artifacts only:
    scripts/build_release.ps1 -Version v1.0.2

.EXAMPLE
    Build, tag, and publish the GitHub Release:
    scripts/build_release.ps1 -Version v1.0.2 -Publish
#>
[CmdletBinding()]
param(
    [Parameter()]
    [ValidatePattern('^v[0-9]+[.][0-9]+[.][0-9]+')]
    [string]$Version = "v1.0.2",

    [switch]$Publish,
    [string]$NotesFile,
    [string]$Repo = "pilos011/smallTV-custom",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

# Native tools write ordinary progress to stderr - platformio prints compiler
# warnings there, git push reports the ref it updated - and under Stop that is
# surfaced as a terminating error, killing the script on a successful command.
# Exit codes are what actually say whether a native command worked, so each call
# below is wrapped: stderr is echoed, and only $LASTEXITCODE decides.
function Invoke-Native {
    param([scriptblock]$Command, [string]$What)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Command 2>&1 | ForEach-Object { Write-Host $_ }
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

$BSLASH = [char]92
$FSLASH = [char]47

$root    = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root (Join-Path "firmware" "sdpro-clock-weather")
$release = Join-Path $root "release"
$dist    = Join-Path $root "dist"

$fwName  = "SDP_ClockWeather_$Version.bin"
$fsName  = "littlefs-clock-weather-$Version.bin"
$zipName = "SmallTV-Custom-$Version.zip"

$fwTarget = Join-Path $release $fwName
$fsTarget = Join-Path $release $fsName
$zipPath  = Join-Path $dist $zipName

function Write-Step {
    param([string]$Text)
    Write-Host ""
    Write-Host "==> $Text" -ForegroundColor Cyan
}

# Sort-Object is culture-aware and would order "clear.bmp" before "clear-28.bmp".
# Archive entry order must be ordinal so repeated runs stay byte-comparable.
function Sort-Ordinal {
    param([string[]]$Values)
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($value in $Values) { $list.Add($value) }
    $list.Sort([System.StringComparer]::Ordinal)
    return $list
}

# Returns file paths in python os.walk order: files in a directory, then subdirectories.
function Get-WalkFiles {
    param([string]$Path)
    $result = @()

    $fileNames = Sort-Ordinal (Get-ChildItem -LiteralPath $Path -File | ForEach-Object { $_.Name })
    foreach ($name in $fileNames) { $result += (Join-Path $Path $name) }

    $dirNames = Sort-Ordinal (Get-ChildItem -LiteralPath $Path -Directory | ForEach-Object { $_.Name })
    foreach ($name in $dirNames) { $result += Get-WalkFiles -Path (Join-Path $Path $name) }

    return $result
}

function Get-RelativeEntry {
    param([string]$Base, [string]$FullPath)
    $baseFull = (Resolve-Path -LiteralPath $Base).Path.TrimEnd($BSLASH, $FSLASH)
    $rel = $FullPath.Substring($baseFull.Length + 1)
    return $rel.Replace($BSLASH, $FSLASH)
}

# --------------------------------------------------------------------------
# 1. Build
# --------------------------------------------------------------------------
if ($SkipBuild) {
    Write-Step "Skipping build (-SkipBuild)"
} else {
    Write-Step "Building firmware and LittleFS image"
    Push-Location $project
    try {
        Invoke-Native { py -m platformio run } "platformio run"
        Invoke-Native { py -m platformio run --target buildfs } "platformio buildfs"
    }
    finally {
        Pop-Location
    }
}

# --------------------------------------------------------------------------
# 2. Collect binaries
# --------------------------------------------------------------------------
Write-Step "Collecting binaries into release/"

$buildDir = Join-Path $project (Join-Path ".pio" (Join-Path "build" "sdpro-clock-weather"))
$fwSource = Join-Path $buildDir "firmware.bin"
$fsSource = Join-Path $buildDir "littlefs.bin"

foreach ($src in @($fwSource, $fsSource)) {
    if (-not (Test-Path -LiteralPath $src)) {
        throw "Build output not found: $src. Run without -SkipBuild."
    }
}

New-Item -ItemType Directory -Force -Path $release | Out-Null
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item -LiteralPath $fwSource -Destination $fwTarget -Force
Copy-Item -LiteralPath $fsSource -Destination $fsTarget -Force

Write-Host "  $fwName"
Write-Host "  $fsName"

# --------------------------------------------------------------------------
# 3. Package the full source archive
# --------------------------------------------------------------------------
Write-Step "Packaging $zipName"

# Entry list, flattened to the firmware project root. Order matches v1.0.0.
$entries = New-Object System.Collections.Generic.List[object]

function Add-Entry {
    param([string]$Source, [string]$EntryName)
    if (-not (Test-Path -LiteralPath $Source)) { throw "Missing archive input: $Source" }
    $entries.Add([pscustomobject]@{ Source = $Source; EntryName = $EntryName })
}

Add-Entry (Join-Path $root (Join-Path "docs" "DEVICE_RECOVERY.md")) "docs/DEVICE_RECOVERY.md"
Add-Entry (Join-Path $root (Join-Path "scripts" "build_release.ps1")) "scripts/build_release.ps1"

$srcRoot = Join-Path $project "src"
foreach ($file in (Get-WalkFiles -Path $srcRoot)) {
    Add-Entry $file ("src/" + (Get-RelativeEntry -Base $srcRoot -FullPath $file))
}

foreach ($sub in @("weather-icons", "web")) {
    $subPath = Join-Path $project (Join-Path "data" $sub)
    $names = Sort-Ordinal (Get-ChildItem -LiteralPath $subPath -File | ForEach-Object { $_.Name })
    foreach ($name in $names) {
        Add-Entry (Join-Path $subPath $name) ("data/$sub/" + $name)
    }
}

foreach ($doc in @("README.md", "CHANGELOG.md", "HANDOVER.md")) {
    Add-Entry (Join-Path $root $doc) $doc
}

Add-Entry (Join-Path $project "platformio.ini") "platformio.ini"
Add-Entry $fwTarget $fwName
Add-Entry $fsTarget $fsName

# Windows PowerShell 5.1 needs both: ZipFile/ZipFileExtensions live in
# System.IO.Compression.FileSystem, ZipArchive/ZipArchiveMode in System.IO.Compression.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

$archive = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $entries) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive, $entry.Source, $entry.EntryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally {
    $archive.Dispose()
}

Write-Host "  $zipName ($($entries.Count) entries)"

# --------------------------------------------------------------------------
# 4. Hashes for the CHANGELOG
# --------------------------------------------------------------------------
Write-Step "SHA256 (paste into CHANGELOG.md)"

$fwHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fwTarget).Hash
$fsHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fsTarget).Hash

Write-Host "  release/$fwName"
Write-Host "    SHA256: $fwHash"
Write-Host "  release/$fsName"
Write-Host "    SHA256: $fsHash"

# --------------------------------------------------------------------------
# 5. Publish
# --------------------------------------------------------------------------
if (-not $Publish) {
    Write-Step "Local artifacts ready"
    Write-Host "  Re-run with -Publish to tag and create the GitHub Release."
    return
}

Write-Step "Publishing $Version to $Repo"

if (-not $NotesFile) {
    $NotesFile = Join-Path $dist "release-notes-$Version.md"
}
if (-not (Test-Path -LiteralPath $NotesFile)) {
    throw "Release notes not found: $NotesFile. Write it before publishing."
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh CLI not found. Install it or publish the release manually."
}

gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "gh is not authenticated. Run: gh auth login" }

Push-Location $root
try {
    if ((git status --porcelain)) {
        throw "Working tree is not clean. Commit the release changes first."
    }

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne "main") {
        throw "Expected to publish from main, but HEAD is on '$branch'."
    }

    git fetch origin main --quiet
    $localHead  = (git rev-parse HEAD).Trim()
    $remoteHead = (git rev-parse origin/main).Trim()
    if ($localHead -ne $remoteHead) {
        throw "main differs from origin/main. Push main before publishing."
    }

    if (-not (git tag --list $Version)) {
        Invoke-Native { git tag -a $Version -m "SmallTV Custom $Version" } "git tag"
    } else {
        Write-Host "  Tag $Version already exists locally."
    }

    Invoke-Native { git push origin $Version } "git push of tag $Version"

    Invoke-Native {
        gh release create $Version -R $Repo --title "SmallTV Custom $Version" `
            --notes-file $NotesFile $fwTarget $fsTarget $zipPath
    } "gh release create"
}
finally {
    Pop-Location
}

Write-Step "Published"
Write-Host "  https://github.com/$Repo/releases/tag/$Version"
