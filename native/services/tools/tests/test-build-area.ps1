# SPDX-License-Identifier: MIT
# Run on the native Windows runner under one shared runner lock. No game process or installation is used.
param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..\..\..").Path
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$out = (Resolve-Path $OutputDirectory).Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw 'No installed MSVC toolchain' }
$cmd = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "$out"
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /fp:precise /I"$root\native\core\abi" "$here\build_area_test.cpp" "$here\..\hooks\build_area.cpp" "$here\..\policy.cpp" /Fe:"$out\build-area-math.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /fp:precise /I"$root\native\core\abi" "$here\build_area_win_test.cpp" "$here\..\hooks\build_area.cpp" "$here\..\policy.cpp" /Fe:"$out\build-area-windows.exe"
exit /b %errorlevel%
"@
[System.IO.File]::WriteAllText("$out\build.cmd", $cmd)
& cmd /d /c "$out\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Native building-area test compilation failed' }
foreach ($exe in 'build-area-math', 'build-area-windows') {
    $process = Start-Process -FilePath "$out\$exe.exe" -WorkingDirectory $out -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0) { throw "$exe failed with exit $($process.ExitCode)" }
}
