param([string]$Capture = '')
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
if (-not $Capture) {
    $Capture = (Get-ChildItem -LiteralPath (Join-Path $Root 'out\reflection') -Directory |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $Capture -or -not (Test-Path -LiteralPath $Capture)) {
    throw 'A capture directory is required.'
}
$Marker = Join-Path $Capture 'aes_hotkey_state.json'
if (-not (Test-Path -LiteralPath $Marker)) {
    throw 'AES hotkey listener is not armed in this capture; check aes_hunt/aes_hotkey in the profile.'
}
$State = Get-Content -LiteralPath $Marker -Raw | ConvertFrom-Json
if ($State.running) { throw 'An AES key hunt is already running; wait for its terminal state.' }
[System.IO.File]::WriteAllText((Join-Path $Capture 'aes_hunt.request'), '')
Write-Host "Requested AES key hunt in $Capture (same path as pressing F9 in the dumper terminal)."
Write-Host 'Results appear in aes\aes_state.json, then aes_2, aes_3 for repeat requests.'
