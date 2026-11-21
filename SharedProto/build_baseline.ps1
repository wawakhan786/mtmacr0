<#
    build_driver.ps1 — compila o driver do PROTÓTIPO (SharedProto)
    -------------------------------------------------------------------------
    Mesma receita de tools\build_safe_driver.ps1: cl + link direto, WDK 28000,
    /NODEFAULTLIB /SUBSYSTEM:NATIVE. Não toca em Build\, não assina nada, não
    entra na PubgExt.sln.

    Saída: SharedProto\out\km_ipc_proto.sys

    Uso:
        powershell -ExecutionPolicy Bypass -File SharedProto\build_driver.ps1
#>

param(
    [string] $OutDir = ''
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot          # ...\mtmacr0
$proto = $PSScriptRoot

if (-not $OutDir) { $OutDir = Join-Path $proto 'out' }
$objDir = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force -Path $OutDir, $objDir | Out-Null

$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere ausente: $vswhere" }
$VsPath = & $vswhere -latest -products * -property installationPath
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat ausente: $VcVars" }

# WDK: maior versao instalada de km\ntifs.h
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$wdkVer = Get-ChildItem (Join-Path $kits 'Include') -Directory |
          Where-Object { Test-Path (Join-Path $_.FullName 'km\ntifs.h') } |
          Sort-Object Name -Descending | Select-Object -First 1
if (-not $wdkVer) { throw "WDK nao encontrado em $kits\Include" }

$incKm  = Join-Path $wdkVer.FullName 'km'
$incSh  = Join-Path $wdkVer.FullName 'shared'
$libKm  = Join-Path $kits "Lib\$($wdkVer.Name)\km\x64"
$src    = Join-Path $proto 'km_ioctl_baseline.c'
$def    = Join-Path $objDir 'km_baseline.def'
$out    = Join-Path $OutDir 'km_baseline.sys'

if (-not (Test-Path $src)) { throw "fonte ausente: $src" }

@('LIBRARY km_baseline', 'EXPORTS', '    DriverEntry') |
    Set-Content -Path $def -Encoding ASCII

$bat = Join-Path $OutDir 'build_driver.bat'
@(
    '@echo off'
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /c /O2 /W3 /GS- /Gy /Z7 /D_AMD64_ /DAMD64 " +
      "/I`"$incKm`" /I`"$incSh`" /I`"$proto`" /Fo`"$objDir\\`" `"$src`""
    "if errorlevel 1 exit /b 1"
    "link.exe /nologo /MANIFEST:NO /DRIVER /OPT:REF /OPT:ICF /ENTRY:DriverEntry " +
      "/NODEFAULTLIB /SUBSYSTEM:NATIVE /DEF:`"$def`" `"$objDir\km_ioctl_baseline.obj`" " +
      "ntoskrnl.lib /LIBPATH:`"$libKm`" /OUT:`"$out`""
    'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ASCII

Write-Host "  WDK  : $($wdkVer.Name)" -ForegroundColor DarkGray
Write-Host "  src  : $src" -ForegroundColor DarkGray
Write-Host "  out  : $out" -ForegroundColor DarkGray

& cmd /d /c "`"$bat`""
$code = [int]$LASTEXITCODE

if ($code -ne 0 -or -not (Test-Path $out)) {
    Write-Host "  [err] compilacao falhou (exit $code)" -ForegroundColor Red
    exit 1
}

$sz = (Get-Item $out).Length
Write-Host ("  [ok]   {0}  ({1:N0} bytes)" -f $out, $sz) -ForegroundColor Green
exit 0
