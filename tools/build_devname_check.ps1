param(
    [string] $Out = "$env:TEMP\devname_check.exe"
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$VsPath    = & $vswhere -latest -products * -property installationPath
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not (Test-Path $VcVars)) { throw "vcvars64.bat nao encontrado: $VcVars" }

$src = Join-Path $PSScriptRoot 'devname_check.cpp'
$objDir = Join-Path $env:TEMP 'devname_obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

# ---------------------------------------------------------------------------
# Comparacao de texto precisa do console: a saida mistura '\\' e acentos, e o
# cmd.exe usa cp850 por padrao. Sem isto o redirecionamento corrompe o log.
# ---------------------------------------------------------------------------
$bat = Join-Path $env:TEMP 'build_devname_check.bat'
@(
    '@echo off'
    "chcp 65001 >nul"
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /O2 /EHsc /Fe:`"$Out`" /Fo`"$objDir\\`" `"$src`""
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

& cmd /d /c "`"$bat`""
$code = [int]$LASTEXITCODE

if ($code -ne 0 -or -not (Test-Path $Out)) {
    Write-Host "  [err] falha ao compilar devname_check.cpp (exit $code)" -ForegroundColor Red
    exit 1
}

Write-Host "  [ok] $Out" -ForegroundColor Green
exit 0
