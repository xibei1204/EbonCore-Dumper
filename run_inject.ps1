param(
    [string]$Game = 'HTGame.exe',
    [string]$Dumper = 'build\EbonDumper.dll',
    [string]$Profile = 'profiles\nte-ue5.ini',
    [string]$Out = '',
    [int]$MinModules = 24,
    [int]$Seconds = 900
)

$Root = $PSScriptRoot
Set-Location $Root

$InjectExe = Join-Path $Root 'build\EbonDebugInject.exe'
if (-not (Test-Path $InjectExe)) {
    Write-Host "[!] $InjectExe not found. Running build_native.bat..." -ForegroundColor Yellow
    cmd /c "$Root\build_native.bat"
}

$DumperPath = [System.IO.Path]::GetFullPath((Join-Path $Root $Dumper))
$ProfilePath = [System.IO.Path]::GetFullPath((Join-Path $Root $Profile))

$argsList = @(
    '--game', $Game,
    '--dumper', $DumperPath,
    '--profile', $ProfilePath,
    '--min-modules', $MinModules,
    '--seconds', $Seconds
)

if ($Out) {
    $argsList += @('--out', $Out)
}

& python (Join-Path $Root 'tools\ebon_cli.py') inject-and-dump --injector $InjectExe @argsList
exit $LASTEXITCODE
