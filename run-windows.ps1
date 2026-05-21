param(
    [switch]$InstallDeps,
    [switch]$NoBle,
    [string]$SerialPort = "auto",
    [string]$CodexBin = "",
    [int]$ControlPort = 3490
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$Python = Get-Command python -ErrorAction SilentlyContinue
$PythonArgs = @()
if (-not $Python) {
    $Python = Get-Command py -ErrorAction SilentlyContinue
    $PythonArgs = @("-3")
}
if (-not $Python) {
    throw "Python 3 was not found. Install Python 3 or add it to PATH."
}

if ($InstallDeps) {
    & $Python.Source @PythonArgs -m pip install -r (Join-Path $Root "daemon\requirements.txt")
}

$DaemonArgs = @(
    (Join-Path $Root "daemon\codexmeter_daemon.py"),
    "--watch",
    "--sync-pet-sprite",
    "--cwd", $Root,
    "--auto-serial-port", $SerialPort,
    "--control-port", "$ControlPort"
)

if (-not $NoBle) {
    $DaemonArgs += "--ble"
}
if ($CodexBin) {
    $DaemonArgs += @("--codex-bin", $CodexBin)
}

Write-Host "CodexMeter Windows runner"
Write-Host "Project: $Root"
Write-Host "Serial:  $SerialPort"
Write-Host "Control: http://127.0.0.1:$ControlPort"
Write-Host ""

& $Python.Source @PythonArgs @DaemonArgs
