<#
    build_bench.ps1 — compila o cliente/benchmark do protótipo (user mode)
    -------------------------------------------------------------------------
    Saída: SharedProto\out\km_ipc_bench.exe

    Uso:
        powershell -ExecutionPolicy Bypass -File SharedProto\build_bench.ps1
#>

param([string] $OutDir = '')

$ErrorActionPreference = 'Stop'
$proto = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $proto 'out' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$VsPath    = & $vswhere -latest -products * -property installationPath
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat ausente: $VcVars" }

$src = Join-Path $proto 'km_ipc_bench.cpp'
$out = Join-Path $OutDir 'km_ipc_bench.exe'
$obj = Join-Path $OutDir 'obj_bench'
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$bat = Join-Path $OutDir 'build_bench.bat'
@(
    '@echo off'
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /O2 /W3 /std:c++20 /EHsc /utf-8 /I`"$proto`" " +
      "`"$src`" /Fe:`"$out`" /Fo:`"$obj\\`" " +
      "advapi32.lib"
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

& cmd /d /c "`"$bat`""
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) {
    Write-Host "  [err] falha ao compilar o benchmark" -ForegroundColor Red
    exit 1
}
Write-Host ("  [ok]   {0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length) -ForegroundColor Green
exit 0
