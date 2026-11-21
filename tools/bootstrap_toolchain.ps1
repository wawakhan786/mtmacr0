<#
    bootstrap_toolchain.ps1
    -------------------------------------------------------------------------
    Verifica (e opcionalmente instala) o toolchain do PubgExt.

      * Visual Studio 2022/2026 com toolset C++ nativo
      * Windows SDK (headers, libs, rc.exe, signtool.exe)
      * Windows Driver Kit (ntifs.h, ntoskrnl.lib, WindowsDriver.props/targets)
      * git

    Nao instala dependencias de RUNTIME (vmm.dll, leechcore.dll, dbghelp.dll) —
    veja Include\README.md.

    Uso:
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\bootstrap_toolchain.ps1
        powershell ... -File tools\bootstrap_toolchain.ps1 -VerifyOnly
        powershell ... -File tools\bootstrap_toolchain.ps1 -Install
#>

[CmdletBinding()]
param(
    [switch] $VerifyOnly,
    [switch] $Install,
    [string] $SdkVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$KitsRoots = @("${env:ProgramFiles(x86)}\Windows Kits\10", "${env:ProgramFiles}\Windows Kits\10")

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  [ok]   $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Bad  { param([string]$m) Write-Host "  [miss] $m" -ForegroundColor Red }

$script:Fails = 0
function Fail { $script:Fails++ }

# ---------------------------------------------------------------------------
# Visual Studio
# ---------------------------------------------------------------------------
Write-Step 'Visual Studio'

$VsPath = $null
if (Test-Path $vswhere) {
    $VsPath = & $vswhere -latest -products * -property installationPath 2>$null
}

if (-not $VsPath) {
    Write-Bad 'nenhuma instancia do Visual Studio encontrada'
    Write-Host '         https://aka.ms/vs/17/release/vs_community.exe'
    Fail
} else {
    $ver = & $vswhere -latest -products * -property installationVersion 2>$null
    Write-Ok "instancia: $VsPath  ($ver)"

    $MSBuild = Join-Path $VsPath 'MSBuild\Current\Bin\MSBuild.exe'
    $VcVars  = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

    if (Test-Path $MSBuild) { Write-Ok 'MSBuild.exe'  } else { Write-Bad "MSBuild.exe -> $MSBuild"; Fail }
    if (Test-Path $VcVars)  { Write-Ok 'vcvars64.bat' } else { Write-Bad "vcvars64.bat -> $VcVars"; Fail }

    $MsvcRoot = Join-Path $VsPath 'VC\Tools\MSVC'
    $toolsets = @()
    if (Test-Path $MsvcRoot) {
        $toolsets = Get-ChildItem $MsvcRoot -Directory | Sort-Object Name -Descending
    }
    if (-not $toolsets) { Write-Bad 'nenhum toolset MSVC'; Fail }
    foreach ($t in $toolsets) {
        $cl    = Join-Path $t.FullName 'bin\Hostx64\x64\cl.exe'
        $ml64  = Join-Path $t.FullName 'bin\Hostx64\x64\ml64.exe'
        $atlmfc= Join-Path $t.FullName 'atlmfc\include\atlbase.h'
        $line  = "MSVC $($t.Name): cl=$([bool](Test-Path $cl))  ml64=$([bool](Test-Path $ml64))  atl=$([bool](Test-Path $atlmfc))"
        if ((Test-Path $cl) -and (Test-Path $ml64)) { Write-Ok $line } else { Write-Warn $line }
    }

    # MASM build customizations (necessario para driver\syscall.asm)
    $masmHits = Get-ChildItem (Join-Path $VsPath 'MSBuild\Microsoft\VC') -Recurse -Filter 'masm.props' -ErrorAction SilentlyContinue
    if ($masmHits) { Write-Ok "MASM customizations: $($masmHits[0].DirectoryName)" }
    else { Write-Bad 'MASM BuildCustomizations ausente (driver\syscall.asm nao compila)'; Fail }
}

# ---------------------------------------------------------------------------
# Windows SDK
# ---------------------------------------------------------------------------
Write-Step 'Windows SDK'

$kitsRoot = $KitsRoots | Where-Object { Test-Path (Join-Path $_ 'Include') } | Select-Object -First 1
if (-not $kitsRoot) {
    Write-Bad 'nenhum Windows Kits\10 encontrado'
    Write-Host '         Sem o SDK nao existe windows.h / kernel32.lib / rc.exe.'
    Write-Host '         Instale: tools\install_vs_components.ps1'
    Fail
} else {
    Write-Ok "kits root: $kitsRoot"
    $sdks = Get-ChildItem (Join-Path $kitsRoot 'Include') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+' } |
            Sort-Object Name -Descending
    if (-not $sdks) { Write-Bad 'nenhum SDK em Include\'; Fail }
    foreach ($s in $sdks) {
        $hdr = Join-Path $kitsRoot "Include\$($s.Name)\um\windows.h"
        $lib = Join-Path $kitsRoot "Lib\$($s.Name)\um\x64\kernel32.lib"
        $rc  = Join-Path $kitsRoot "bin\$($s.Name)\x64\rc.exe"
        $st  = Join-Path $kitsRoot "bin\$($s.Name)\x64\signtool.exe"
        $tag = if ((Test-Path $hdr) -and (Test-Path $lib)) { 'ok  ' } else { 'part' }
        Write-Host ("  [{0}] SDK {1}  rc={2} signtool={3}" -f $tag, $s.Name, [bool](Test-Path $rc), [bool](Test-Path $st))
    }
    if (-not (Test-Path (Join-Path $kitsRoot "Include\$SdkVersion\um\windows.h"))) {
        Write-Warn "SDK $SdkVersion (o que o driver_stub espera) nao esta presente"
    }
    # Debugging tools (dbghelp/symsrv para FixCr3)
    $dbg = Join-Path $kitsRoot 'Debuggers\x64\dbghelp.dll'
    if (Test-Path $dbg) { Write-Ok "Debugging Tools: $dbg" }
    else { Write-Warn 'Debugging Tools for Windows ausente (FixCr3 precisa de dbghelp.dll + symsrv.dll)' }
}

# ---------------------------------------------------------------------------
# WDK
# ---------------------------------------------------------------------------
Write-Step 'Windows Driver Kit'

$wdkFound = $false
foreach ($kr in $KitsRoots) {
    $buildDir = Join-Path $kr 'build'
    if (-not (Test-Path $buildDir)) { continue }
    Get-ChildItem $buildDir -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $props = Join-Path $_.FullName 'WindowsDriver.Default.props'
        $ntifs = Join-Path $kr "Include\$($_.Name)\km\ntifs.h"
        if (Test-Path $props) {
            $script:wdkFound = $true
            Write-Ok "WDK $($_.Name)  props=ok  ntifs=$([bool](Test-Path $ntifs))"
        }
    }
}
if (-not $wdkFound) {
    Write-Bad "WDK ausente (nenhum WindowsDriver.Default.props)"
    Write-Host '         driver_stub.vcxproj nao compila sem WDK.'
    Write-Host '         Instale: tools\install_vs_components.ps1'
    Fail
}

# ---------------------------------------------------------------------------
# git
# ---------------------------------------------------------------------------
Write-Step 'git'
if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Ok "git: $((Get-Command git).Source)"
} else {
    Write-Warn 'git fora do PATH (o repo .git existe, falta o binario)'
}

# ---------------------------------------------------------------------------
# Runtime (nao bloqueia build)
# ---------------------------------------------------------------------------
Write-Step 'Runtime (nao bloqueia build)'
foreach ($f in 'vmm.dll','leechcore.dll','vmm.lib','leechcore.lib','dbghelp.dll','symsrv.dll','kdmapper.exe','PUBG_KM.sys') {
    $p = Join-Path $Root "Build\Release\PubgExt\$f"
    if (Test-Path $p) { Write-Host "    [ok]   $f" -ForegroundColor DarkGreen }
    else              { Write-Host "    [miss] $f" -ForegroundColor DarkYellow }
}

# ---------------------------------------------------------------------------
# Instalacao opcional
# ---------------------------------------------------------------------------
if ($Install) {
    Write-Step 'Instalando componentes faltantes'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'install_vs_components.ps1')
}

# ---------------------------------------------------------------------------
Write-Step 'Resultado'
if ($script:Fails -eq 0) {
    Write-Host '  toolchain completo.' -ForegroundColor Green
    Write-Host '  build: powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_all.ps1'
} else {
    Write-Host "  $($script:Fails) bloqueio(s) de toolchain." -ForegroundColor Yellow
    Write-Host '  corrija com: powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_vs_components.ps1'
    exit 2
}
