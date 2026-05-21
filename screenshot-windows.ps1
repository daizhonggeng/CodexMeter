param(
    [string]$Output = "screenshot.png",
    [string]$Port = "auto"
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

& $Python.Source @PythonArgs (Join-Path $Root "tools\screenshot_serial.py") $Output $Port
