param([string]$Capture = '')
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
if (-not $Capture) {
    $Capture = (Get-ChildItem -LiteralPath (Join-Path $Root 'out\reflection') -Directory |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $Capture -or -not (Test-Path -LiteralPath (Join-Path $Capture 'dump_state.json'))) {
    throw 'A completed capture directory is required.'
}
$State = Get-Content -LiteralPath (Join-Path $Capture 'dump_state.json') -Raw | ConvertFrom-Json
if (-not $State.success) { throw 'Wait for the initial reflection capture to complete first.' }
$RequestPath = Join-Path $Capture 'resource_capture.request'
[System.IO.File]::WriteAllText($RequestPath, '')
Write-Host "Requested resource capture in $Capture. Keep the game running with the new Dumper DLL."
Write-Host 'Results appear in resources\res_N\resource_state.json.'
