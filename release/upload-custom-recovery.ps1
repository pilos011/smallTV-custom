param([string]$DeviceIp = "192.168.10.72")
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Firmware = Join-Path $ScriptDir "SDP_CustomRecovery_20260824_1.bin"
Write-Host "Uploading SDP Custom Recovery to http://$DeviceIp/update_ota"
Get-Item -LiteralPath $Firmware | Format-List FullName,Length
Get-FileHash -LiteralPath $Firmware -Algorithm SHA256 | Format-List
curl.exe --fail --http1.0 --connect-timeout 5 --max-time 120 -F "update=@$Firmware;filename=SDP_CustomRecovery_20260824_1.bin" "http://$DeviceIp/update_ota"
Start-Sleep -Seconds 15
curl.exe --fail --max-time 10 "http://$DeviceIp/status"
