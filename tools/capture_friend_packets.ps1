param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateRange(5, 120)][int]$Seconds = 45
)

# Run separately in an elevated terminal. Never requests elevation itself,
# changes an existing PktMon session, or marks packets as decoded Friend RPCs.
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'PktMon requires an Administrator PowerShell terminal.'
}
$process = Get-Process -Id $ProcessId
$image = $process.Path
if (-not $image -or [IO.Path]::GetFileName($image) -ne 'HTGame.exe') {
    throw 'The selected process is not a readable HTGame.exe.'
}
$expected = '64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec'
if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Client SHA-256 differs from the verified build.'
}
$output = [IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $output) { throw 'Output directory must be new.' }
$clientDirectory = [IO.Path]::GetDirectoryName($image).TrimEnd('\') + '\'
if ($output.StartsWith($clientDirectory, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be outside the installed executable directory.'
}

$status = (& pktmon.exe status 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "Cannot read PktMon status: $status" }
if ($status -notmatch '(?i)not running|\u672a\u8fd0\u884c|\u6ca1\u6709\u8fd0\u884c') {
    throw "PktMon is running or its status is unrecognized; no changes made: $status"
}
$existing = (& pktmon.exe filter list 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "Cannot read PktMon filters: $existing" }
if ($existing -notmatch '(?i)no (packet )?filters|\u672a\u6307\u5b9a.*\u7b5b\u9009\u5668|\u6ca1\u6709.*\u7b5b\u9009\u5668|\u65e0.*\u7b5b\u9009\u5668') {
    throw "Existing filters or unrecognized filter state; no changes made: $existing"
}

$allUdp = @(Get-NetUDPEndpoint -ErrorAction Stop)
$allTcp = @(Get-NetTCPConnection -ErrorAction Stop)
$ports = @()
foreach ($spec in @(@{ Protocol = 'UDP'; Endpoints = $allUdp }, @{ Protocol = 'TCP'; Endpoints = $allTcp })) {
    $owned = @($spec.Endpoints | Where-Object OwningProcess -eq $ProcessId | Select-Object -ExpandProperty LocalPort -Unique)
    foreach ($port in $owned) {
        if ($port -eq 0) { continue }
        if (@($spec.Endpoints | Where-Object { $_.LocalPort -eq $port -and $_.OwningProcess -ne $ProcessId }).Count) {
            throw "Port $port is shared with another process; refusing a broader capture."
        }
        $ports += @{ protocol = $spec.Protocol; local_port = [int]$port }
    }
}
if (-not $ports.Count -or $ports.Count -gt 32) { throw 'Expected 1..32 game-owned transport ports.' }
New-Item -ItemType Directory -Path $output | Out-Null
$log = Join-Path $output 'pktmon.log'
$ownedFilters = [Collections.Generic.List[string]]::new()
$started = $false
$record = [ordered]@{
    format = 'EbonFriendPacketCapture-v1'; process_id = $ProcessId; module_sha256 = $expected
    process_start_utc = $process.StartTime.ToUniversalTime().ToString('o')
    ports = $ports; packet_mapping_verified = $false; friend_rpc_correlation_verified = $false
    complete = $false; requested_seconds = $Seconds
    note = 'Raw NIC packets filtered by game-owned local ports; not decoded RPCs. New ports after start are outside capture coverage.'
}
try {
    foreach ($port in $ports) {
        $name = 'EbonFriend-{0}-{1}-{2}' -f $ProcessId, $port.protocol, $port.local_port
        & pktmon.exe filter add $name -t $port.protocol -p $port.local_port 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        if ($LASTEXITCODE -ne 0) { throw "Cannot install filter $name" }
        $ownedFilters.Add($name)
    }
    $record.start_utc = [DateTime]::UtcNow.ToString('o')
    $record.start_tick32_ms = [BitConverter]::ToUInt32([BitConverter]::GetBytes([Environment]::TickCount), 0)
    & pktmon.exe start --capture --comp nics --pkt-size 0 --file-size 64 --log-mode circular --file-name (Join-Path $output 'friend.etl') 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'PktMon capture did not start.' }
    $started = $true
    Write-Host "Capturing for $Seconds seconds. Open and refresh the friend list now."
    Start-Sleep -Seconds $Seconds
} catch {
    $record.error = $_.Exception.Message
    throw
} finally {
    if ($started) {
        & pktmon.exe stop 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        $record.stop_succeeded = $LASTEXITCODE -eq 0
        $record.end_utc = [DateTime]::UtcNow.ToString('o')
    }
    foreach ($name in $ownedFilters) {
        & pktmon.exe filter remove $name 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not remove owned filter $name; see $log" }
    }
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'capture.json') -Encoding utf8
}
if (-not $record.stop_succeeded) { throw 'PktMon stop failed; inspect pktmon.log.' }
& pktmon.exe etl2pcap (Join-Path $output 'friend.etl') --out (Join-Path $output 'friend.pcapng') 2>&1 | Out-File -LiteralPath $log -Append -Encoding utf8
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $output 'friend.pcapng'))) {
    throw 'Packet conversion failed; original ETL retained.'
}
$record.complete = $true
$record.packet_file_sha256 = (Get-FileHash -LiteralPath (Join-Path $output 'friend.pcapng') -Algorithm SHA256).Hash.ToLowerInvariant()
$record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'capture.json') -Encoding utf8
Write-Host "Saved raw packet capture to $output"
