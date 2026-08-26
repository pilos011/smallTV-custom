param([string]$DeviceIp = "192.168.10.72")

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Official = Resolve-Path (Join-Path $ScriptDir "..\..\sdpro-ota-safe\SDPro_V1.0.6_20260525_174828.bin")

Write-Host "Restoring official SD_PRO V1.0.6 through custom recovery /update_ota"
Get-Item -LiteralPath $Official | Format-List FullName,Length
Get-FileHash -LiteralPath $Official -Algorithm SHA256 | Format-List

curl.exe --fail --http1.0 --connect-timeout 5 --max-time 120 `
    -F "update=@$Official;filename=SDPro_V1.0.6_20260525_174828.bin" `
    "http://$DeviceIp/update_ota"

Start-Sleep -Seconds 15
curl.exe --fail --max-time 10 "http://$DeviceIp/config"
