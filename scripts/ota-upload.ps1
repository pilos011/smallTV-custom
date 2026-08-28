<#
.SYNOPSIS
    Upload firmware or a LittleFS image over OTA, with the checks that keep a
    half-written image from reaching the flash.

.DESCRIPTION
    The device cannot tell a truncated upload from a complete one unless it is
    told what to expect: without a length, Update.end() has to accept whatever
    arrived, and a device that reboots into half an image is recovered only
    through the UART pads inside the case. This script measures the file and
    sends the length and the MD5 with the request, so the device refuses
    anything short or altered before it reboots.

    It also refuses to send a firmware image that does not start with 0xE9,
    which catches a bad download on this side of the wire.

.PARAMETER Device
    Device address, e.g. <device-ip> or http://<device-ip>.

.PARAMETER Bin
    Path to the image.

.PARAMETER Fs
    Send as a LittleFS image (/api/ota/fs) instead of firmware (/api/ota/fw).
    NOTE: a filesystem upload replaces settings, web files and album photos.

.EXAMPLE
    .\scripts\ota-upload.ps1 -Device <device-ip> -Bin release\SDP_ClockWeather_v1.0.21.bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Device,
    [Parameter(Mandatory = $true)][string]$Bin,
    [switch]$Fs
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Bin)) { throw "Image not found: $Bin" }

$base = $Device
if ($base -notmatch '^https?://') { $base = "http://$base" }
$base = $base.TrimEnd('/')

$item = Get-Item -LiteralPath $Bin
$size = $item.Length
$md5 = (Get-FileHash -LiteralPath $Bin -Algorithm MD5).Hash.ToLower()

if ($size -lt 1) { throw "Image is empty: $Bin" }

# An ESP8266 sketch image always opens with 0xE9. A filesystem image does not,
# so this check only applies to firmware.
if (-not $Fs) {
    $head = Get-Content -LiteralPath $Bin -Encoding Byte -TotalCount 1
    if ($head[0] -ne 0xE9) {
        throw ("Not an ESP8266 firmware image (first byte 0x{0:X2}, expected 0xE9): {1}" -f $head[0], $Bin)
    }
    if ($size -lt 200000) {
        throw "Firmware image is only $size bytes; the device will refuse it as too small."
    }
}

$route = if ($Fs) { "/api/ota/fs" } else { "/api/ota/fw" }
$field = if ($Fs) { "fs" } else { "update" }
$url = "$base$route" + "?size=$size&md5=$md5"

Write-Host "Device : $base"
Write-Host "Image  : $Bin"
Write-Host "Size   : $size bytes"
Write-Host "MD5    : $md5"
if ($Fs) {
    Write-Host ""
    Write-Host "WARNING: a filesystem upload replaces settings, web files and album photos."
}
Write-Host ""
Write-Host "Uploading to $route ..."

$reply = & curl.exe -s --max-time 300 -F "$field=@$Bin" $url
if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE" }

Write-Host "Device replied: $reply"

if ($reply -notmatch '^OK') {
    throw "Device refused the update. Nothing was flashed; the device did not reboot."
}
if ($reply -match 'unverified') {
    Write-Warning "The device could not verify this image. Check that it accepted the size parameter."
}

Write-Host ""
Write-Host "Waiting for the device to come back..."

# An OK means the image was accepted and verified, not that it boots. Nothing
# is confirmed until the device answers /status again.
$deadline = (Get-Date).AddSeconds(60)
$status = $null
while ((Get-Date) -lt $deadline) {
    try {
        $status = Invoke-RestMethod -Uri "$base/status" -TimeoutSec 5
        break
    } catch {
        Start-Sleep -Seconds 2
    }
}

if ($null -eq $status) {
    Write-Host ""
    Write-Warning "The device has not answered /status within 60 seconds."
    Write-Warning "Check it before uploading anything else. See docs/DEVICE_RECOVERY.md."
    exit 1
}

Write-Host ""
Write-Host "Back up: version $($status.version) at $($status.ip)"
Write-Host "  free heap $($status.free_heap)  fw $($status.fw_used)/$($status.fw_total)"
