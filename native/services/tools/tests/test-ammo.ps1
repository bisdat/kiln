# SPDX-License-Identifier: MIT
# Native runner only, under one shared runner flock. Never launches a game or installs files.
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..\..\..").Path
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$out = (Resolve-Path $OutputDirectory).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'No installed MSVC toolchain' }
$tests = @('ammo_abi','ammo_list','ammo_sources','ammo_hook','presentation','presentation_api','presentation_admission','brush_frame','held_state','held_observer','service_presentation_lifecycle')
$lines = @('@echo off', "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$out`"")
foreach ($test in $tests) {
    $extra = if ($test -eq 'service_presentation_lifecycle') { "`"$here\..\hooks\presentation.cpp`"" } else { '' }
    $lines += "cl /nologo /std:c++20 /EHsc /W3 /O2 /I`"$root\native\core\abi`" /I`"$here\..\include`" `"$here\$test.cpp`" `"$here\..\policy.cpp`" $extra /Fe:`"$out\$test.exe`""
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += 'exit /b 0'
[System.IO.File]::WriteAllLines("$out\build.cmd", $lines)
& cmd /d /c "$out\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Native ammunition fixture compilation failed' }
foreach ($test in $tests) {
    $process = Start-Process -FilePath "$out\$test.exe" -WorkingDirectory $out -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0) { throw "$test failed with exit $($process.ExitCode)" }
}
"Native ammunition/presentation suites: $($tests.Count) passed"
