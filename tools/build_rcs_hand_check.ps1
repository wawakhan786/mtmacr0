<#
    build_rcs_hand_check.ps1
    -------------------------------------------------------------------------
    Compila tools\rcs_hand_check.cpp contra o HEADER REAL do projeto
    (PubgExt\SDK\RcsNoise.h).

    Mesmo padrao de build_config_check.ps1: vswhere -> vcvars64 -> cl direto,
    saida no %TEMP%. Nao toca em Build\, nao assina nada, nao builda a
    solution, nao precisa do jogo aberto.

    Uso:
        powershell -ExecutionPolicy Bypass -File tools\build_rcs_hand_check.ps1
        %TEMP%\rcs_hand_check.exe
#>

param(
    [string] $Out = ''
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot

if (-not $Out) { $Out = Join-Path $env:TEMP 'rcs_hand_check.exe' }

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe nao encontrado: $vswhere" }

$VsPath = & $vswhere -latest -products * -property installationPath
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $VcVars)) { throw "vcvars64.bat nao encontrado: $VcVars" }
if (-not (Test-Path $raiz))   { throw "raiz nao encontrada: $raiz" }

$src    = Join-Path $PSScriptRoot 'rcs_hand_check.cpp'
$objDir = Join-Path $env:TEMP 'rcs_hand_check_obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

if (-not (Test-Path $src)) { throw "fonte nao encontrado: $src" }

# So' o include do SDK basta: RcsNoise.h inclui <cmath> e <random> da CRT e nao
# puxa pch, config nem driver. E' de proposito — o teste roda em qualquer
# maquina, sem o resto do projeto.
$incArg = "/I`"$(Join-Path $raiz 'PubgExt\SDK')`""

$bat = Join-Path $env:TEMP 'build_rcs_hand_check.bat'
@(
    '@echo off'
    "chcp 65001 >nul"
    "call `"$VcVars`" >nul 2>&1"
    # /std:c++20 e' o mesmo da solution. /utf-8 porque os rotulos tem acento.
    "cl.exe /nologo /O2 /std:c++20 /utf-8 /EHsc /W4 $incArg /Fe:`"$Out`" /Fo`"$objDir\\`" /Fd:`"$objDir\vc.pdb`" `"$src`""
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

& cmd /d /c "`"$bat`""
$code = [int]$LASTEXITCODE

if ($code -ne 0 -or -not (Test-Path $Out)) {
    Write-Host "  [err] falha ao compilar rcs_hand_check.cpp (exit $code)" -ForegroundColor Red
    exit 1
}

Write-Host "  [ok] $Out" -ForegroundColor Green
Write-Host "       rode: $Out" -ForegroundColor Gray
exit 0
