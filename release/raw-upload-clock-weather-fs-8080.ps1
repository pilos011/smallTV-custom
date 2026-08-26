$ErrorActionPreference = "Stop"

$Device = "192.168.10.72"
$Bin = Join-Path $PSScriptRoot "littlefs-clock-weather-20260825-1.bin"

if (!(Test-Path $Bin)) {
    throw "LittleFS image not found: $Bin"
}

Write-Host "Uploading LittleFS raw stream to http://$Device`:8080/rawfs"
Write-Host "Use this only after SDP Clock Weather firmware is running."
curl.exe -v --fail --http1.0 --connect-timeout 5 --max-time 180 `
    --data-binary "@$Bin" `
    "http://$Device`:8080/rawfs"

Write-Host ""
Write-Host "Wait 15 seconds, then check:"
Write-Host "  http://$Device/status"
Write-Host "  http://$Device/"
