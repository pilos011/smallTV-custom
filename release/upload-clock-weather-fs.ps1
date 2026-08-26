$ErrorActionPreference = "Stop"

$Device = "192.168.10.72"
$Bin = Join-Path $PSScriptRoot "littlefs-clock-weather-20260825-1.bin"

if (!(Test-Path $Bin)) {
    throw "LittleFS image not found: $Bin"
}

Write-Host "Uploading LittleFS to http://$Device/api/ota/fs"
Write-Host "File: $Bin"
curl.exe -v --http1.0 --connect-timeout 5 --max-time 180 `
    -F "fs=@$Bin;filename=littlefs-clock-weather-20260825-1.bin" `
    "http://$Device/api/ota/fs"

Write-Host ""
Write-Host "Wait 15 seconds, then check:"
Write-Host "  http://$Device/status"
Write-Host "  http://$Device/"
