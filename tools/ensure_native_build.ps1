param([string]$OutputDirectory = 'build-aes')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$OutputPath = Join-Path $ProjectRoot $OutputDirectory
$Targets = @('EbonDumper.dll', 'EbonLogConsole.exe', 'EbonDebugInject.exe',
             'EbonAesFinder.exe', 'EbonAesScan.exe', 'EbonAesSelfTest.exe', 'EbonOodleDecode.exe')
$Sources = @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'native') -Recurse -File |
    Where-Object {
        # SDK fixture executables are not inputs to any of the capture targets.
        # Their edits must not invalidate the independently built AES programs.
        $_.Extension -in '.cpp', '.hpp', '.h', '.inl' -and
        -not ($_.Extension -eq '.cpp' -and $_.DirectoryName -eq (Join-Path $ProjectRoot 'native\sdk'))
    })
$Sources += Get-Item -LiteralPath (Join-Path $ProjectRoot 'build_native.bat')
$Sources += Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'third_party\ooz') -File |
    Where-Object { $_.Extension -in '.cpp','.h' }
$NewestSource = ($Sources | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
$NeedsBuild = $false
foreach ($Target in $Targets) {
    $TargetPath = Join-Path $OutputPath $Target
    if (-not (Test-Path -LiteralPath $TargetPath -PathType Leaf) -or
        (Get-Item -LiteralPath $TargetPath).LastWriteTimeUtc -lt $NewestSource) {
        $NeedsBuild = $true
        break
    }
}
if (-not $NeedsBuild) { exit 0 }
Write-Host "Native AES capture tools are missing or older than the source; rebuilding $OutputDirectory."
Push-Location $ProjectRoot
try {
    & (Join-Path $ProjectRoot 'build_native.bat') $OutputDirectory
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
