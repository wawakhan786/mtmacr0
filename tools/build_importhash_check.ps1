param(
    [string] $Out = ''
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot

if (-not $Out) { $Out = Join-Path $env:TEMP 'importhash_check.exe' }

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$VsPath    = & $vswhere -latest -products * -property installationPath
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $VcVars)) { throw "vcvars64.bat nao encontrado: $VcVars" }

$src    = Join-Path $PSScriptRoot 'importhash_check.cpp'
$objDir = Join-Path $env:TEMP 'importhash_obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

if (-not (Test-Path $src)) { throw "fonte nao encontrado: $src" }

$bat = Join-Path $env:TEMP 'build_importhash_check.bat'
@(
    '@echo off'
    "chcp 65001 >nul"
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /O2 /EHsc /W3 /Fe:`"$Out`" /Fo`"$objDir\\`" /Fd:`"$objDir\vc.pdb`" `"$src`""
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

& cmd /d /c "`"$bat`""
$code = [int]$LASTEXITCODE

if ($code -ne 0 -or -not (Test-Path $Out)) {
    Write-Host "  [err] falha ao compilar importhash_check.cpp (exit $code)" -ForegroundColor Red
    exit 1
}

Write-Host "  [ok] $Out" -ForegroundColor Green
exit 0
