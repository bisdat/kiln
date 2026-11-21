# SPDX-License-Identifier: MIT
# the two-DLL startup integration, native Windows runner.
# Runs the cases run.py prepared (run.py payload: cases\, cases.txt, this file) exactly as run.py's Wine runner does: each
# repeat on a fresh copy of the case's game folder, each step one start of the host with the step's environment ({game}
# and {run} replaced), a snapshot of what the checks read after each start, result.json. A step with hold_lock starts
# lockholder.exe first (another process holding the install lock); one with watch_sf puts a deny-all ACL on
# <game>\shroudforge for the start (ShroudForge's folder must never be opened in kiln mode) and hashes the folder before
# and after. Only touches $Root (under C:\kiln-runner). Processes are stopped by exact PID.
param([Parameter(Mandatory = $true)][string]$Root, [string[]]$Only = @(), [int]$Repeat = 1)
$ErrorActionPreference = 'Stop'
if (-not $Root.StartsWith('C:\kiln-runner\')) { throw "refusing root outside C:\kiln-runner: $Root" }
(Get-Process -Id $PID).PriorityClass = 'Normal'
$summary = Join-Path $Root 'summary.txt'
"priority $((Get-Process -Id $PID).PriorityClass)" | Out-File -FilePath $summary -Encoding ascii
$cases = @(Get-Content (Join-Path $Root 'cases.txt') | Where-Object { $_ -ne '' })
if ($Only.Count -gt 0) { $cases = @($cases | Where-Object { $Only -contains $_ }) }
$archive = @('enshrouded.kfc', 'enshrouded.kfc_resources', 'enshrouded.kfc.bak', 'enshrouded.kfc_resources.bak', 'enshrouded.kfc.bak.integrity')

function Snapshot([string]$game, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Get-ChildItem -File $game | Where-Object { $archive -contains $_.Name -or ($_.Name -like 'enshrouded_*.dat') } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $dest $_.Name) }
    foreach ($sub in @('kiln\state', 'kiln\logs', 'kiln\eml', 'shroudforge')) {
        $from = Join-Path $game $sub
        if (Test-Path $from -PathType Container) {
            $to = Join-Path $dest $sub
            New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
            Copy-Item -Recurse $from $to
        }
    }
}
function TreeHashes([string]$folder) {
    @(Get-ChildItem -Recurse -File $folder | Sort-Object FullName | ForEach-Object {
        "$($_.FullName.Substring($folder.Length)) $($_.Length) $((Get-FileHash $_.FullName -Algorithm SHA256).Hash) $($_.LastWriteTimeUtc.Ticks)"
    })
}

foreach ($name in $cases) {
    $folder = Join-Path $Root "cases\$name"
    $case = Get-Content (Join-Path $folder 'case.json') -Raw | ConvertFrom-Json
    foreach ($r in 1..$Repeat) {
        $run = Join-Path $Root "runs\$name\$r"
        if (Test-Path $run) { Remove-Item -Recurse -Force $run }
        New-Item -ItemType Directory -Force -Path $run | Out-Null
        $game = Join-Path $run 'game'
        Copy-Item -Recurse (Join-Path $folder 'template\game') $game
        $plugins = Join-Path $game 'kiln\plugins'
        New-Item -ItemType Directory -Force -Path $plugins | Out-Null
        foreach ($dll in $case.plugins) { Copy-Item (Join-Path $Root "shared\$dll") (Join-Path $plugins $dll) }
        $n = 0
        foreach ($step in $case.steps) {
            $n++
            foreach ($prop in $step.env.PSObject.Properties) {
                [Environment]::SetEnvironmentVariable($prop.Name, $prop.Value.Replace('{game}', $game).Replace('{run}', $run), 'Process')
            }
            $holder = $null
            if ($step.hold_lock) {
                $ready = Join-Path $run "lock-ready-$n"; $stop = Join-Path $run "lock-stop-$n"
                $holder = Start-Process -FilePath (Join-Path $Root 'shared\lockholder.exe') -ArgumentList @('--hold-lock', (Join-Path $game '.kiln-eml.lock'), $ready, $stop) -NoNewWindow -PassThru
                $null = $holder.Handle
                for ($i = 0; $i -lt 600 -and -not (Test-Path $ready) -and -not $holder.HasExited; $i++) { Start-Sleep -Milliseconds 50 }
                if (-not (Test-Path $ready)) { throw "$name : the lock holder did not take the lock" }
            }
            $acl = $null
            if ($step.watch_sf) {
                $sf = Join-Path $game 'shroudforge'
                $acl = @{ before = TreeHashes $sf; folderWrite = (Get-Item $sf).LastWriteTimeUtc.Ticks }
                icacls $sf /deny '*S-1-1-0:(OI)(CI)(F)' | Out-Null
                # in force: a file below it cannot be read (this elevated runner's own directory listing passes the
                # ACE through its backup privilege; the host removes that privilege from its own token)
                # (at least one file read: an empty folder would prove nothing)
                $denied = $true; $tried = 0
                foreach ($f in @(Get-ChildItem -Recurse -File $sf)) {
                    $tried++
                    try { Get-Content -LiteralPath $f.FullName -ErrorAction Stop | Out-Null; $denied = $false } catch { }
                }
                $acl.filesTried = $tried
                $acl.effective = $denied -and $tried -gt 0
            }
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $outFile = Join-Path $run "native-$n.out"; $errFile = Join-Path $run "native-$n.err"
            $proc = Start-Process -FilePath (Join-Path $game 'enshrouded.exe') -WorkingDirectory $game -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
            $null = $proc.Handle  # PowerShell 5.1 reports ExitCode only when the handle was taken while it ran
            $timedOut = $false
            if (-not $proc.WaitForExit([int]$step.timeout * 1000)) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit(); $timedOut = $true }
            $elapsed = $sw.Elapsed.TotalSeconds
            $rc = $proc.ExitCode
            if ($holder) {
                Set-Content -Path $stop -Value 'stop'
                if (-not $holder.WaitForExit(60000)) { Stop-Process -Id $holder.Id -Force }
            }
            if ($acl) {
                $sf = Join-Path $game 'shroudforge'
                icacls $sf /remove:d '*S-1-1-0' | Out-Null
                $acl.after = TreeHashes $sf
                $acl.folderWriteAfter = (Get-Item $sf).LastWriteTimeUtc.Ticks
            }
            foreach ($prop in $step.env.PSObject.Properties) { [Environment]::SetEnvironmentVariable($prop.Name, $null, 'Process') }
            $dest = Join-Path $run "step-$n"
            Snapshot $game $dest
            $result = [ordered]@{ rc = $rc; elapsed = [math]::Round($elapsed, 3); timed_out = $timedOut; runner = 'native' }
            if ($acl) { $result.acl = $acl }
            $result | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $dest 'result.json') -Encoding ascii
            $line = "$name run $r step $n rc=$rc elapsed=$([math]::Round($elapsed, 1))s timeout=$timedOut"
            Write-Output $line
            Add-Content -Path $summary -Value $line -Encoding ascii
        }
    }
}
