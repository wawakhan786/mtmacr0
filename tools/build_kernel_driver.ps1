<#
    build_kernel_driver.ps1
    -------------------------------------------------------------------------
    Compila km_driver_source.c -> Build\Release\PubgExt\Ndu.sys

    Este e o driver que o kdmapper carrega. NAO e assinado (nao tem como, ver
    sig\README.md) â€” por isso e o kdmapper que o mapeia, nao o sc.exe.

    Compila com WDK (headers km + ntoskrnl.lib) e mascara os imports de
    MmCopyVirtualMemory/PsLookupProcessByProcessId com /ALTERNATENAME, para o
    driver nao carregar uma tabela de imports com o nome exato do primitivo.

    Uso:
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_kernel_driver.ps1
        powershell ... -File tools\build_kernel_driver.ps1 -OutName Ndu.sys
#>

[CmdletBinding()]
param(
    [string] $Config  = 'Release',
    [string] $OutName = 'Ndu.sys'
)

$ErrorActionPreference = 'Stop'

$Root     = Split-Path -Parent $PSScriptRoot
$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere  = Join-Path $Installer 'vswhere.exe'
$VsPath   = & $vswhere -latest -products * -property installationPath
$VcVars   = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

$KitsRoot = @("${env:ProgramFiles(x86)}\Windows Kits\10", "${env:ProgramFiles}\Windows Kits\10") |
            Where-Object { Test-Path (Join-Path $_ 'Include') } | Select-Object -First 1
if (-not $KitsRoot) { throw 'Windows Kits nao encontrado.' }

# O WDK que tem os headers km completos. Preferimos o mais novo.
$WdkVer = (Get-ChildItem (Join-Path $KitsRoot 'Include') -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+' -and (Test-Path (Join-Path $_.FullName 'km\ntifs.h')) } |
           Sort-Object Name -Descending | Select-Object -First 1).Name
if (-not $WdkVer) { throw 'Nenhum WDK com km\ntifs.h encontrado. Rode tools\install_wdk.ps1' }

$kmInc = Join-Path $KitsRoot "Include\$WdkVer"
$kmLib = Join-Path $KitsRoot "Lib\$WdkVer\km\x64"
$outDir = Join-Path $Root "Build\$Config\PubgExt"
$objDir = Join-Path $Root 'Build\obj\kernel_driver'

New-Item -ItemType Directory -Force -Path $outDir, $objDir | Out-Null

Write-Host "  WDK  : $WdkVer" -ForegroundColor DarkGray
Write-Host "  out  : $outDir\$OutName" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Um .def para exportar DriverEntry sem decoracao (o kdmapper procura o nome)
# ---------------------------------------------------------------------------
$def = Join-Path $objDir 'ndu.def'
@'
LIBRARY Ndu
EXPORTS
    DriverEntry
'@ | Set-Content -Path $def -Encoding ASCII

$logDir = Join-Path $Root 'Build\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logOut = Join-Path $logDir "kernel_driver_$(Get-Date -Format yyyyMMdd_HHmmss_fff).log"
$bat    = Join-Path $logDir "kernel_driver.bat"

$cmds = @(
    '@echo off'
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /c /O2 /W3 /GS- /Gy /Z7 /D_AMD64_ /DAMD64 " +
      "/I`"$kmInc\km`" /I`"$kmInc\shared`" " +
      "`"$Root\km_driver_source.c`" /Fo`"$objDir\km_driver_source.obj`""
    "link.exe /nologo /MANIFEST:NO /DRIVER /OPT:REF /OPT:ICF /ENTRY:DriverEntry " +
      "/NODEFAULTLIB /SUBSYSTEM:NATIVE /INTEGRITYCHECK " +
      "/DEF:`"$def`" " +
      "`"$objDir\km_driver_source.obj`" " +
      "ntoskrnl.lib /LIBPATH:`"$kmLib`" " +
      "/OUT:`"$outDir\$OutName`""
    'exit /b %ERRORLEVEL%'
)
Set-Content -Path $bat -Value ($cmds -join "`r`n") -Encoding ASCII

& cmd.exe /d /c "`"$bat`" > `"$logOut`" 2>&1"
$code = [int]$LASTEXITCODE

if (Test-Path $logOut) {
    Get-Content $logOut | Where-Object { $_ -match 'error|warning|LNK|fatal' } |
        Select-Object -First 40 | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkYellow }
}

$outFile = Join-Path $outDir $OutName

# Nao basta olhar o exit code: o link pode ter reaproveitado um .obj antigo e
# "sucedido" com o binario da build anterior. Se o log tem C-number de erro,
# a build falhou.
$compileErrors = @()
if (Test-Path $logOut) {
    $compileErrors = Get-Content $logOut | Where-Object {
        $_ -match '\berror C\d{4}\b' -or $_ -match '\berror LNK\d{4}\b'
    }
}

if ($code -eq 0 -and -not $compileErrors -and (Test-Path $outFile)) {
    Write-Host "  [ok]   $outFile  ($((Get-Item $outFile).Length) bytes)" -ForegroundColor Green
    exit 0
}

if ($compileErrors) {
    Write-Host "  [err]  $($compileErrors.Count) erro(s) de compilacao:" -ForegroundColor Red
    $compileErrors | Select-Object -First 15 | ForEach-Object { Write-Host "         $_" -ForegroundColor DarkRed }
} 
Write-Host "  [err]  build do driver falhou (exit $code). Log: $logOut" -ForegroundColor Red
exit 1
