# Build build\version.dll (x64, static CRT) with the Visual Studio toolchain.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat',
                        'C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat' -ErrorAction SilentlyContinue |
          Select-Object -First 1
# The device wrapper is generated from the SDK's own d3d9.h (never committed).
$d3d9h = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Recurse -Filter d3d9.h -ErrorAction SilentlyContinue |
         Sort-Object FullName | Select-Object -Last 1
python -I (Join-Path $root 'tools\gen_d3d9_wrapper.py') $d3d9h.FullName (Join-Path $out 'rt_device_gen.h')
if ($LASTEXITCODE -ne 0) { throw 'wrapper generation failed' }

$mh = Join-Path $root 'third_party\minhook'
$files = @(Get-ChildItem $PSScriptRoot -Filter *.cpp) + @(Get-ChildItem "$mh\src" -Filter *.c) + @(Get-Item "$mh\src\hde\hde64.c")
$src = ($files | ForEach-Object { "`"$($_.FullName)`"" }) -join ' '

# A batch file sidesteps PowerShell 5.1 quoting rules for cmd.exe.
$bat = Join-Path $out 'build.cmd'
@"
@echo off
call "$($vcvars.FullName)" >nul 2>nul
cl /nologo /std:c++17 /O2 /Zi /MT /EHsc /W4 /LD /I"$mh\include" /I"$mh\src" /I"$out" /Fo"$out\\" /Fd"$out\\" /Fe"$out\version.dll" $src /link /NOLOGO /DEBUG /OPT:REF /OPT:ICF /PDB:"$out\version.pdb" /PDBALTPATH:version.pdb user32.lib kernel32.lib
"@ | Set-Content -Encoding ascii $bat

$ErrorActionPreference = 'Continue'
cmd /c $bat 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built $out\version.dll"
