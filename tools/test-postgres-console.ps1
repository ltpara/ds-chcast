# Run from any directory. Requires Go and a complete PostgreSQL distribution
# (bin, lib and share); the server build must support ICU collations.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PostgresBin,
    [ValidateRange(0, 65535)][int]$Port = 0
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$pgBin = (Resolve-Path -LiteralPath $PostgresBin).Path
foreach ($tool in @('initdb', 'pg_ctl', 'createdb', 'pg_dump', 'pg_restore', 'psql')) {
    if (-not (Test-Path -LiteralPath (Join-Path $pgBin "$tool.exe"))) {
        throw "Missing PostgreSQL tool: $tool.exe"
    }
}
$null = Get-Command go -ErrorAction Stop

# Every invocation owns a new directory. No existing cluster is reused,
# reconfigured, or deleted. Logs and the stopped cluster stay under build/.
$runName = 'postgres-console-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runRoot = Join-Path $repoRoot "build\$runName"
$dataDir = Join-Path $runRoot 'data'
New-Item -ItemType Directory -Path $runRoot | Out-Null
$testLog = Join-Path $runRoot 'integration.jsonl'
$oldDSN = [Environment]::GetEnvironmentVariable('CHAST_TEST_POSTGRES_DSN', 'Process')
$oldBin = [Environment]::GetEnvironmentVariable('CHAST_TEST_POSTGRES_BIN', 'Process')
$clusterInitialized = $false

function Invoke-PgTool([string]$Name, [string[]]$ToolArgs) {
    & (Join-Path $pgBin "$Name.exe") @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

# Start-Process -Wait can wait for pg_ctl's server descendants on Windows.
# Wait on the pg_ctl process itself so successful start returns immediately.
function Invoke-PgControl([string]$Action) {
    $controlArgs = @('-D', ('"' + $dataDir + '"'), '-w', '-t', '30')
    if ($Action -eq 'start') {
        $controlArgs += @('-l', ('"' + (Join-Path $runRoot 'postgres.log') + '"'), 'start')
    } else {
        $controlArgs += @('-m', 'fast', 'stop')
    }
    $process = Start-Process -FilePath (Join-Path $pgBin 'pg_ctl.exe') `
        -ArgumentList $controlArgs -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $runRoot "pgctl-$Action.stdout.log") `
        -RedirectStandardError (Join-Path $runRoot "pgctl-$Action.stderr.log")
    try {
        # Windows PowerShell 5.1 needs the process handle retained before
        # waiting, otherwise ExitCode can be null even after success.
        $processHandle = $process.Handle
        if (-not $process.WaitForExit(45000)) { throw "pg_ctl $Action did not exit within 45 seconds" }
        if ($process.ExitCode -ne 0) { throw "pg_ctl $Action failed; see $runRoot" }
    } finally {
        $process.Dispose()
    }
}

Push-Location (Join-Path $repoRoot 'server')
try {
    # Reserve/check a loopback port before initdb. A competing bind after
    # release causes pg_ctl to fail; it never falls back to another cluster.
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    try { $Port = $listener.LocalEndpoint.Port } finally { $listener.Stop() }
    Invoke-PgTool 'initdb' @('-D', $dataDir, '-U', 'postgres', '--auth=trust', '--encoding=UTF8', '--locale=C') `
        > (Join-Path $runRoot 'initdb.log')
    $clusterInitialized = $true
    Add-Content -LiteralPath (Join-Path $dataDir 'postgresql.conf') -Encoding ASCII -Value @(
        "listen_addresses = '127.0.0.1'",
        "port = $Port"
    )
    Invoke-PgControl 'start'
    Invoke-PgTool 'createdb' @('--no-password', '-h', '127.0.0.1', '-p', "$Port", '-U', 'postgres', 'chast_console_acceptance')
    $env:CHAST_TEST_POSTGRES_DSN = "postgres://postgres@127.0.0.1:$Port/chast_console_acceptance?sslmode=disable"
    $env:CHAST_TEST_POSTGRES_BIN = $pgBin
    Invoke-PgTool 'psql' @('--no-password', '-X', '--dbname', $env:CHAST_TEST_POSTGRES_DSN, '-c', 'SELECT version();') `
        > (Join-Path $runRoot 'version.txt')
    Write-Host "Running PostgreSQL acceptance on 127.0.0.1:$Port; logs: $runRoot"
    & go test ./internal/account ./internal/registry ./internal/api -run 'Test(Postgres|PermissionRegression)' -count=1 -timeout 5m -json > $testLog
    $testExit = $LASTEXITCODE
    $events = @(Get-Content -LiteralPath $testLog | ForEach-Object { $_ | ConvertFrom-Json })
    $passed = @($events | Where-Object { $_.Action -eq 'pass' -and $_.Test -and $_.Test -notmatch '/' })
    $skipped = @($events | Where-Object { $_.Action -eq 'skip' })
    $failed = @($events | Where-Object { $_.Action -eq 'fail' })
    if ($testExit -ne 0 -or $failed.Count -gt 0 -or $skipped.Count -gt 0 -or $passed.Count -eq 0) {
        $events | Where-Object { $_.Action -in @('fail', 'skip') } | ForEach-Object { Write-Host "$($_.Action): $($_.Package) $($_.Test)" }
        throw "PostgreSQL acceptance failed or skipped tests; inspect $testLog"
    }
    # Successful tests must leave no isolated schemas behind.
    $remaining = & (Join-Path $pgBin 'psql.exe') --no-password -X --dbname $env:CHAST_TEST_POSTGRES_DSN -Atc "SELECT count(*) FROM pg_namespace WHERE nspname LIKE 'chast_test_%'"
    if ($LASTEXITCODE -ne 0 -or "$remaining".Trim() -ne '0') { throw 'Test schemas were not fully cleaned up' }
    "$($passed.Count) top-level tests passed; 0 failed; 0 skipped; 0 remaining test schemas" | Set-Content -LiteralPath (Join-Path $runRoot 'result.txt')
    Write-Host (Get-Content -LiteralPath (Join-Path $runRoot 'result.txt'))
} finally {
    [Environment]::SetEnvironmentVariable('CHAST_TEST_POSTGRES_DSN', $oldDSN, 'Process')
    [Environment]::SetEnvironmentVariable('CHAST_TEST_POSTGRES_BIN', $oldBin, 'Process')
    try {
        if ($clusterInitialized -and (Test-Path -LiteralPath (Join-Path $dataDir 'postmaster.pid'))) {
            Invoke-PgControl 'stop'
        }
    } finally {
        Pop-Location
        Write-Host "Acceptance artifacts retained at $runRoot"
    }
}
