# SPDX-License-Identifier: MIT
# test infrastructure only. The same runs as run.sh, natively on Windows, from a folder holding
# steam-standin-test.exe and the stand-in steam_api64.dll:
#   powershell -NoProfile -ExecutionPolicy Bypass -File run-windows.ps1 -Dir <folder>
# Exit code 0 = every check passed.
param([Parameter(Mandatory = $true)][string]$Dir)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Dir 'steam-standin-test.exe'
if (-not (Test-Path $exe) -or -not (Test-Path (Join-Path $Dir 'steam_api64.dll'))) { Write-Output 'missing test files'; exit 2 }
# below Windows' dynamic port range (49152+), any base: the processes use base + SteamID % 1024 (unit: + 2048 and up)
$base = 20000 + (Get-Random -Maximum 9600)
$sync = Join-Path $Dir 'sync'
Remove-Item -Recurse -Force $sync -ErrorAction SilentlyContinue
New-Item -ItemType Directory $sync | Out-Null
$result = 0
& $exe unit ($base + 2048)
Write-Output "unit exit $LASTEXITCODE"
if ($LASTEXITCODE -ne 0) { $result = 1 }
$outA = Join-Path $Dir 'fabric-a.txt'
$a = Start-Process -FilePath $exe -ArgumentList @('fabric', 'a', $base, $sync) -NoNewWindow -PassThru -RedirectStandardOutput $outA
$null = $a.Handle  # Windows PowerShell 5.1 reports no ExitCode unless the handle is opened while it runs
& $exe fabric b $base $sync
Write-Output "fabric B exit $LASTEXITCODE"
if ($LASTEXITCODE -ne 0) { $result = 1 }
if (-not $a.WaitForExit(90000)) { Stop-Process -Id $a.Id -Force; Write-Output 'fabric A timed out'; $result = 1 }
elseif ($a.ExitCode -ne 0) { $result = 1 }
Get-Content $outA
Write-Output "fabric A exit $($a.ExitCode)"
exit $result
