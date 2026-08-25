param(
    [string]$Version = "v1.0.0"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "firmware\sdpro-clock-weather"
$release = Join-Path $root "release"

Push-Location $project
try {
    py -m platformio run
    py -m platformio run --target buildfs
}
finally {
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $release | Out-Null

$fwSource = Join-Path $project ".pio\build\sdpro-clock-weather\firmware.bin"
$fsSource = Join-Path $project ".pio\build\sdpro-clock-weather\littlefs.bin"
$fwTarget = Join-Path $release "SDP_ClockWeather_$Version.bin"
$fsTarget = Join-Path $release "littlefs-clock-weather-$Version.bin"

Copy-Item -LiteralPath $fwSource -Destination $fwTarget -Force
Copy-Item -LiteralPath $fsSource -Destination $fsTarget -Force

Get-FileHash -Algorithm SHA256 $fwTarget
Get-FileHash -Algorithm SHA256 $fsTarget
