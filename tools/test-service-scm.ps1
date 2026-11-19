[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build\root\Release',
    [switch]$Execute,
    [switch]$AllowMachineChanges
)

$ErrorActionPreference = 'Stop'
$ruleNames = @('chast host', 'chast host tcp', 'chast tunnel', 'chast tunnel discovery')
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $buildPath = [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    $buildPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}
$serviceExe = Join-Path $buildPath 'chast-service.exe'
$hostExe = Join-Path $buildPath 'chast-host.exe'
$tunnelExe = Join-Path $buildPath 'chast-tunnel.exe'
$machineSettings = Join-Path $env:ProgramData 'chast\host.ini'

function Get-ScmSnapshot {
    $service = Get-CimInstance Win32_Service -Filter "Name='chast-host'" -ErrorAction Stop
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='chast-service.exe'" -ErrorAction Stop)
    $hostProcesses = @(Get-CimInstance Win32_Process -Filter "Name='chast-host.exe'" -ErrorAction Stop)
    $tunnelProcesses = @(Get-CimInstance Win32_Process -Filter "Name='chast-tunnel.exe'" -ErrorAction Stop)
    $rules = @(Get-NetFirewallRule -All -ErrorAction Stop | Where-Object {
        $ruleNames -contains $_.Name -or $ruleNames -contains $_.DisplayName
    })
    [pscustomobject]@{
        Service = $service
        Processes = $processes
        HostProcesses = $hostProcesses
        TunnelProcesses = $tunnelProcesses
        Rules = $rules
        MachineSettingsExists = Test-Path -LiteralPath $machineSettings
    }
}

function Assert-CleanBaseline([string]$Phase) {
    $state = Get-ScmSnapshot
    if ($null -ne $state.Service) {
        throw "$Phase refused: the chast-host SCM service already exists."
    }
    if ($state.Processes.Count -gt 0) {
        throw "$Phase refused: chast-service.exe processes already exist."
    }
    if ($state.HostProcesses.Count -gt 0 -or $state.TunnelProcesses.Count -gt 0) {
        throw "$Phase refused: a chast-host.exe or chast-tunnel.exe process is already running."
    }
    if ($state.MachineSettingsExists) {
        throw "$Phase refused: machine settings already exist at $machineSettings."
    }
    if ($state.Rules.Count -gt 0) {
        $names = ($state.Rules | ForEach-Object { $_.Name }) -join ', '
        throw "$Phase refused: product firewall rule names already exist: $names"
    }
    return $state
}

function Get-WorkerProcesses {
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='chast-service.exe'" -ErrorAction Stop)
    return @($processes | Where-Object {
        $_.CommandLine -match '(?i)(?:^|\s)--worker(?:\s|$)'
    })
}

function Wait-ServiceState([string]$State, [int]$TimeoutSeconds) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $service = Get-CimInstance Win32_Service -Filter "Name='chast-host'" -ErrorAction Stop
        if ($null -ne $service -and $service.State -eq $State) {
            return $service
        }
        Start-Sleep -Milliseconds 400
    }
    throw "chast-host did not reach state '$State' within $TimeoutSeconds seconds."
}

function Wait-WorkerPresence([bool]$Present, [int]$TimeoutSeconds) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $workers = @(Get-WorkerProcesses)
        if ((($workers.Count -gt 0) -eq $Present)) {
            return $true
        }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

function Assert-SystemWorker {
    $workers = @(Get-WorkerProcesses)
    if ($workers.Count -ne 1) {
        throw "Expected exactly one chast-service.exe --worker process; found $($workers.Count)."
    }
    $worker = $workers[0]
    if (-not [string]::Equals([string]$worker.ExecutablePath, $serviceExe,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The worker executable is '$($worker.ExecutablePath)', expected '$serviceExe'."
    }
    if ($worker.SessionId -eq 0) {
        throw 'The worker is in session 0; an interactive-session worker was expected.'
    }
    $owner = Invoke-CimMethod -InputObject $worker -MethodName GetOwner -ErrorAction Stop
    if ($owner.ReturnValue -ne 0 -or $owner.User -ne 'SYSTEM') {
        throw "The worker is not running as SYSTEM (owner=$($owner.Domain)\$($owner.User))."
    }
    return [pscustomobject]@{
        ProcessId = [uint32]$worker.ProcessId
        SessionId = [uint32]$worker.SessionId
        Owner = "$($owner.Domain)\$($owner.User)"
    }
}

function Get-ProductRule([string]$Name) {
    $state = Get-ScmSnapshot
    $matches = @($state.Rules | Where-Object { $_.Name -eq $Name -or $_.DisplayName -eq $Name })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one firewall rule named '$Name'; found $($matches.Count)."
    }
    return $matches[0]
}

function Assert-FirewallRule {
    param(
        [string]$Name,
        [string]$ExpectedProgram,
        [string]$ExpectedProtocol
    )

    $rule = Get-ProductRule $Name
    if (-not $rule.Enabled -or $rule.Direction -ne 'Inbound' -or $rule.Action -ne 'Allow') {
        throw "Firewall rule '$($rule.Name)' is not enabled inbound allow."
    }
    $application = Get-NetFirewallApplicationFilter -AssociatedNetFirewallRule $rule -ErrorAction Stop
    if (-not [string]::Equals([string]$application.Program, $ExpectedProgram,
                              [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Firewall rule '$($rule.Name)' targets '$($application.Program)', expected '$ExpectedProgram'."
    }
    $portFilter = Get-NetFirewallPortFilter -AssociatedNetFirewallRule $rule -ErrorAction Stop
    $protocol = [string]$portFilter.Protocol
    $validProtocols = if ($ExpectedProtocol -eq 'UDP') { @('UDP', '17') } else { @('TCP', '6') }
    if ($protocol -notin $validProtocols) {
        throw "Firewall rule '$($rule.Name)' has unexpected protocol value '$protocol'."
    }
    $port = 0
    if (-not [int]::TryParse([string]$portFilter.LocalPort, [ref]$port)) {
        throw "Firewall rule '$($rule.Name)' has a non-numeric local port '$($portFilter.LocalPort)'."
    }
    return $port
}

function Assert-FirewallRules {
    param([string]$ExpectedHostProgram, [string]$ExpectedTunnelProgram)

    $hostUdpPort = Assert-FirewallRule 'chast host' $ExpectedHostProgram 'UDP'
    $hostTcpPort = Assert-FirewallRule 'chast host tcp' $ExpectedHostProgram 'TCP'
    $tunnelPort = Assert-FirewallRule 'chast tunnel' $ExpectedTunnelProgram 'UDP'
    $discoveryPort = Assert-FirewallRule 'chast tunnel discovery' $ExpectedTunnelProgram 'UDP'
    if ($hostTcpPort -ne ($hostUdpPort + 1)) {
        throw 'Host TCP fallback port is not the successor of the host UDP port.'
    }
    if ($discoveryPort -ne 21129) {
        throw "Tunnel discovery port is $discoveryPort, expected 21129."
    }
    return [pscustomobject]@{
        HostUdpPort = $hostUdpPort
        HostTcpPort = $hostTcpPort
        TunnelUdpPort = $tunnelPort
        TunnelDiscoveryPort = $discoveryPort
    }
}

if (-not (Test-Path -LiteralPath $serviceExe -PathType Leaf)) {
    throw "Missing service executable: $serviceExe"
}
if (-not (Test-Path -LiteralPath $hostExe -PathType Leaf)) {
    throw "Missing required sibling host executable: $hostExe"
}
if (-not (Test-Path -LiteralPath $tunnelExe -PathType Leaf)) {
    throw "Missing required sibling tunnel executable: $tunnelExe"
}

$baseline = Assert-CleanBaseline 'Preflight'
Write-Host 'Preflight PASS: service and product processes absent; host.ini absent; product firewall rules absent.'
Write-Host "Build directory: $buildPath"
if (-not $Execute) {
    Write-Host 'Read-only mode. No service, configuration, or firewall changes were made.'
    Write-Host 'To run the lifecycle test, pass -Execute -AllowMachineChanges and enter the confirmation phrase.'
    return
}
if (-not $AllowMachineChanges) {
    throw 'Execution requires both -Execute and -AllowMachineChanges.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Execution requires an elevated PowerShell session.'
}
$sessionId = (Get-Process -Id $PID).SessionId
if ($sessionId -eq 0) {
    throw 'Execution must run from an interactive Windows session so the worker can be verified.'
}
$confirmation = Read-Host 'This installs/removes a system service and firewall rules, retaining host.ini. Type RUN CHAST SCM TEST to continue'
if ($confirmation -cne 'RUN CHAST SCM TEST') {
    throw 'Confirmation phrase did not match; no machine changes were made.'
}

$report = [ordered]@{
    startedUtc = [DateTime]::UtcNow.ToString('o')
    buildDirectory = $buildPath
    checks = [ordered]@{
        cleanBaseline = $true
        installAndRunning = $false
        serviceIdentity = $false
        workerIdentity = $false
        stopAndWorkerExit = $false
        restartAndWorkerRecovery = $false
        firewallRules = $false
        uninstallAndFirewallCleanup = $false
        machineSettingsRetained = $false
    }
    worker = $null
    firewall = $null
    failure = $null
    cleanupFailure = $null
    manualFollowUp = @('Lock/unlock behavior', 'User and RDP session switching', 'Secure desktop behavior', 'Second-machine and cross-network behavior')
}
$installAttempted = $false
$installSucceeded = $false
$testFailure = $null
$uninstallExit = $null

try {
    $null = Assert-CleanBaseline 'Pre-install'
    $installAttempted = $true
    $installOutput = @(& $serviceExe --install 2>&1)
    $installExit = $LASTEXITCODE
    if ($installExit -ne 0) {
        throw "--install exited with code $installExit. Output: $($installOutput -join ' ')"
    }
    $installSucceeded = $true
    $service = Wait-ServiceState 'Running' 30
    $report.checks.installAndRunning = $true

    $expectedPath = '"' + $serviceExe + '" --run'
    if (-not [string]::Equals([string]$service.PathName, $expectedPath,
                              [System.StringComparison]::OrdinalIgnoreCase) -or
        $service.StartName -notin @('LocalSystem', 'NT AUTHORITY\SYSTEM')) {
        throw "Unexpected SCM configuration (path='$($service.PathName)', account='$($service.StartName)')."
    }
    $report.checks.serviceIdentity = $true

    if (-not (Wait-WorkerPresence $true 30)) {
        throw 'The service did not start a worker in an interactive session.'
    }
    $report.worker = Assert-SystemWorker
    $report.checks.workerIdentity = $true

    $report.firewall = Assert-FirewallRules $serviceExe $tunnelExe
    $report.checks.firewallRules = $true
    if (-not (Test-Path -LiteralPath $machineSettings -PathType Leaf)) {
        throw "--install succeeded but did not create $machineSettings."
    }

    Stop-Service -Name 'chast-host' -ErrorAction Stop
    $null = Wait-ServiceState 'Stopped' 30
    if (-not (Wait-WorkerPresence $false 30)) {
        throw 'The worker remained after the service stopped.'
    }
    $report.checks.stopAndWorkerExit = $true

    Start-Service -Name 'chast-host' -ErrorAction Stop
    $service = Wait-ServiceState 'Running' 30
    if (-not (Wait-WorkerPresence $true 30)) {
        throw 'The worker did not return after the service restarted.'
    }
    $report.worker = Assert-SystemWorker
    $report.checks.restartAndWorkerRecovery = $true
} catch {
    $testFailure = $_.Exception.Message
} finally {
    if ($installAttempted) {
        try {
            $uninstallOutput = @(& $serviceExe --uninstall 2>&1)
            $uninstallExit = $LASTEXITCODE
            if ($uninstallExit -ne 0) {
                throw "--uninstall exited with code $uninstallExit. Output: $($uninstallOutput -join ' ')"
            }
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            do {
                $current = Get-CimInstance Win32_Service -Filter "Name='chast-host'" -ErrorAction Stop
                if ($null -eq $current) { break }
                Start-Sleep -Milliseconds 400
            } while ([DateTime]::UtcNow -lt $deadline)
            if ($null -ne $current) {
                throw 'The SCM service still exists after --uninstall.'
            }
            if (-not (Wait-WorkerPresence $false 10)) {
                throw 'A worker process remained after --uninstall.'
            }
            $finalState = Get-ScmSnapshot
            if ($finalState.Processes.Count -gt 0) {
                throw 'A chast-service.exe process remained after --uninstall.'
            }
            if ($finalState.Rules.Count -gt 0) {
                throw 'Product firewall rules remain after --uninstall.'
            }
            if ($installSucceeded -and -not $finalState.MachineSettingsExists) {
                throw 'Machine settings were not retained after --uninstall.'
            }
            $report.checks.uninstallAndFirewallCleanup = $true
            $report.checks.machineSettingsRetained = $finalState.MachineSettingsExists
        } catch {
            $report.cleanupFailure = $_.Exception.Message
        }
    }
}

$report.failure = $testFailure
$report.finishedUtc = [DateTime]::UtcNow.ToString('o')
$report.result = if ($null -eq $testFailure -and $null -eq $report.cleanupFailure) { 'PASS' } else { 'FAIL' }
$reportPath = Join-Path $buildPath ("service-scm-acceptance-{0}.json" -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host "Result: $($report.result)"
Write-Host "Evidence: $reportPath"
Write-Host "Manual follow-up remains: $($report.manualFollowUp -join '; ')"
if ($report.result -ne 'PASS') {
    if ($testFailure) { Write-Error "Acceptance failed: $testFailure" }
    if ($report.cleanupFailure) { Write-Error "Cleanup failed: $($report.cleanupFailure)" }
    exit 1
}
