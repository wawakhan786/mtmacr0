<#
    build_config_check.ps1
    -------------------------------------------------------------------------
    Compila tools\config_check.cpp contra os HEADERS DO PROJETO
    (Config\PlayerConfig.h -> pch -> json).

    Mesmo padrao de build_importhash_check.ps1: vcvars64 + cl direto, saida
    no %TEMP%. Nao toca em Build\, nao assina nada, nao builda a solution.

    Uso:
        powershell -ExecutionPolicy Bypass -File tools\build_config_check.ps1
        %TEMP%\config_check.exe
#>

param(
    [string] $Out = ''
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot

if (-not $Out) { $Out = Join-Path $env:TEMP 'config_check.exe' }

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$VsPath    = & $vswhere -latest -products * -property installationPath
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $VcVars)) { throw "vcvars64.bat nao encontrado: $VcVars" }
if (-not (Test-Path $raiz))   { throw "raiz nao encontrada: $raiz" }

$src    = Join-Path $PSScriptRoot 'config_check.cpp'
$objDir = Join-Path $env:TEMP 'config_check_obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

if (-not (Test-Path $src)) { throw "fonte nao encontrado: $src" }

$p          = Join-Path $raiz 'PubgExt'
$includes = @(
    (Join-Path $p 'Config'),
    (Join-Path $p 'ESP'),
    (Join-Path $p 'SDK'),
    (Join-Path $p 'Security'),
    (Join-Path $p 'Memory'),
    (Join-Path $p 'Misc'),
    (Join-Path $p 'Misc\Pch'),
    (Join-Path $p 'json'),
    $p
) | ForEach-Object { "/I`"$_`"" }

$incArg = $includes -join ' '

$bat = Join-Path $env:TEMP 'build_config_check.bat'
@(
    '@echo off'
    "chcp 65001 >nul"
    "call `"$VcVars`" >nul 2>&1"
    # /std:c++20 e' o mesmo da solution; /utf-8 porque o PlayerConfig.h e o
    # Lang.h tem acento em comentario e em rotulo.
    "cl.exe /nologo /O2 /Zi /DEBUG /std:c++20 /utf-8 /EHsc /W3 $incArg /Fe:`"$Out`" /Fo`"$objDir\\`" /Fd:`"$objDir\vc.pdb`" `"$src`""
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

& cmd /d /c "`"$bat`""
$code = [int]$LASTEXITCODE

if ($code -ne 0 -or -not (Test-Path $Out)) {
    Write-Host "  [err] falha ao compilar config_check.cpp (exit $code)" -ForegroundColor Red
    exit 1
}

Write-Host "  [ok] $Out" -ForegroundColor Green
exit 0

