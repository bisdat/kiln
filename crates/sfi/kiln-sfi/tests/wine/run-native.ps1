# SPDX-License-Identifier: MIT
# Native Windows run of kiln-sfi's synthetic loader cases, laid out by run.py --prepare-only (copy cases/, probe/ and cases.txt to $Root).
# Only touches $Root (under C:\kiln-runner). One harness process per case, 180 s each, stopped by exact PID on timeout.
param([Parameter(Mandatory=$true)][string]$Root, [string[]]$Only = @(), [int]$Repeat = 1)
$ErrorActionPreference = 'Stop'
if (-not $Root.StartsWith('C:\kiln-runner\')) { throw "refusing root outside C:\kiln-runner: $Root" }
$summary = Join-Path $Root 'summary.txt'
function Run-One([string]$exe, [string[]]$arguments, [string]$dir, [string]$log) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $dir -NoNewWindow -PassThru `
        -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
    $null = $p.Handle  # PowerShell 5.1 reports ExitCode only when the handle was taken while it ran
    if (-not $p.WaitForExit(180000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(); return 'timeout' }
    return [string]$p.ExitCode
}
$probe = Join-Path $Root 'probe'
$rc = Run-One (Join-Path $probe 'mapping-probe.exe') @((Join-Path $probe 'loaded.dll'), (Join-Path $probe 'mapped-b.dll'), (Join-Path $probe 'renamed-a.dll')) $probe (Join-Path $probe 'result')
"probe rc=$rc" | Tee-Object -FilePath $summary -Append
$cases = Get-Content (Join-Path $Root 'cases.txt') | Where-Object { $_ -ne '' }
if ($Only.Count -gt 0) { $cases = $cases | Where-Object { $Only -contains $_ } }
foreach ($i in 1..$Repeat) {
    foreach ($case in $cases) {
        $folder = Join-Path $Root "cases\$case"
        $env:SFI_WINE_CASE = $case
        $env:SFI_WINE_FIXTURES = $folder
        $env:SFI_TOUCH_FILE = Join-Path $folder 'game\witness\input.bin'
        $rc = Run-One (Join-Path $folder 'harness.exe') @('synthetic_wine_native_bindings', '--exact', '--nocapture', '--test-threads=1') $folder (Join-Path $folder "result-$i")
        "run $i $case rc=$rc" | Tee-Object -FilePath $summary -Append
    }
}
