$ErrorActionPreference = "Stop"

$Device = "192.168.10.72"
$Bin = Join-Path $PSScriptRoot "SDP_ClockWeather_20260825_1.bin"

if (!(Test-Path $Bin)) {
    throw "Firmware not found: $Bin"
}

Write-Host "Uploading firmware to http://$Device/update_ota"
Write-Host "File: $Bin"
curl.exe -v --http1.0 --connect-timeout 5 --max-time 120 `
    -F "update=@$Bin;filename=SDP_ClockWeather_20260825_1.bin" `
    "http://$Device/update_ota"

Write-Host ""
Write-Host "Wait 15 seconds, then check:"
Write-Host "  http://$Device/status"
