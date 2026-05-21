param(
    [string]$Port = "",
    [string]$Environment = "waveshare_lcd_349"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$FirmwareDir = Join-Path $Root "firmware"

$Pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (-not (Test-Path -LiteralPath $Pio)) {
    $PioCmd = Get-Command pio -ErrorAction SilentlyContinue
    if (-not $PioCmd) {
        throw "PlatformIO CLI was not found. Install PlatformIO or add pio to PATH."
    }
    $Pio = $PioCmd.Source
}

$UploadArgs = @("run", "-e", $Environment, "-t", "upload")
if ($Port) {
    $UploadArgs += @("--upload-port", $Port)
}

Write-Host "Flashing CodexMeter"
Write-Host "Firmware: $FirmwareDir"
Write-Host "Port:     $(if ($Port) { $Port } else { 'PlatformIO auto' })"
Write-Host ""

Push-Location $FirmwareDir
try {
    & $Pio @UploadArgs
} finally {
    Pop-Location
}
