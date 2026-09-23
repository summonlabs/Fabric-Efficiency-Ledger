# Fabric Efficiency Ledger - canonical build driver.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Usage:
#   pwsh -File scripts/build.ps1 -Config Release -Build -Test
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release')]
  [string]$Config = 'Release',
  [switch]$Build,
  [switch]$Test,
  [switch]$Asan,
  [string]$SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

# Paths are written with forward slashes: the Windows toolchain accepts them and
# it keeps this script free of escape sequences that vary between shells.
function Find-VcVars {
  $vswhere = Join-Path `${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($found) {
      $path = (Join-Path $found.Trim() 'VC/Auxiliary/Build/vcvars64.bat')
      if (Test-Path $path) { return $path }
    }
  }
  $candidates = @(
    'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat',
    'C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat',
    'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
  )
  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) { return $candidate }
  }
  throw 'No Visual C++ toolchain was found.'
}

$vcvars = Find-VcVars
$buildDir = (Join-Path $SourceDir "build/$Config")
$asanFlag = if ($Asan) { 'ON' } else { 'OFF' }

$configure = 'call "' + $vcvars + '" >nul && cmake -S "' + $SourceDir + '" -B "' + $buildDir +
             '" -G Ninja -DCMAKE_BUILD_TYPE=' + $Config + ' -DFEL_ENABLE_ASAN=' + $asanFlag
Write-Host "== configure ($Config)"
cmd /c $configure
if ($LASTEXITCODE -ne 0) { throw "configure failed with exit code $LASTEXITCODE" }

if ($Build -or $Test) {
  Write-Host "== build ($Config)"
  cmd /c ('call "' + $vcvars + '" >nul && cmake --build "' + $buildDir + '"')
  if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
}

if ($Test) {
  Write-Host "== test ($Config)"
  cmd /c ('call "' + $vcvars + '" >nul && ctest --test-dir "' + $buildDir + '" --output-on-failure')
  if ($LASTEXITCODE -ne 0) { throw "tests failed with exit code $LASTEXITCODE" }
}

Write-Host "build tree: $buildDir"
