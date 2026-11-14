param([string]$Capture = '')
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
if (-not $Capture) {
    $Capture = (Get-ChildItem (Join-Path $Root 'out\reflection') -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $Capture -or -not (Test-Path -LiteralPath (Join-Path $Capture 'dump_state.json'))) {
    throw 'A completed capture directory is required.'
}
$State = Get-Content -LiteralPath (Join-Path $Capture 'dump_state.json') -Raw | ConvertFrom-Json
if (-not $State.success) { throw 'Wait for the initial reflection capture to complete first.' }
New-Item -ItemType File -Path (Join-Path $Capture 'network_capture.request') -Force | Out-Null
Write-Host "Requested another network snapshot in $Capture. Keep the game running."
