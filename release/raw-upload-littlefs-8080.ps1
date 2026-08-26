param(
    [Parameter(Mandatory=$true)][string]$LittleFsBin,
    [string]$DeviceIp = "192.168.10.72"
)

$ErrorActionPreference = "Stop"
$Image = Resolve-Path $LittleFsBin

Write-Host "Uploading LittleFS image through independent raw server :8080/rawfs"
Get-Item -LiteralPath $Image | Format-List FullName,Length
Get-FileHash -LiteralPath $Image -Algorithm SHA256 | Format-List

curl.exe --fail --http1.0 --connect-timeout 5 --max-time 180 `
    --data-binary "@$Image" `
    "http://$DeviceIp:8080/rawfs"

Start-Sleep -Seconds 15
curl.exe --fail --max-time 10 "http://$DeviceIp/status"
