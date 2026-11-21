<#
    install_wdk.ps1
    -------------------------------------------------------------------------
    O WDK NAO vem pelo instalador do Visual Studio de forma confiavel.
    O componente 'Component.Microsoft.Windows.DriverKit' instala a extensao
    do VS (ProjectTemplates + PlatformToolsets), mas NAO instala os headers
    de kernel, libs e os .props do MSBuild. Isso vem do wdksetup.exe.

    Esse script baixa e instala o WDK 10.0.26100 (o que o stub_driver.vcxproj
    espera). Para outra versao, veja a tabela de WDKs suportados:
    https://learn.microsoft.com/windows-hardware/drivers/other-wdk-downloads

    Uso:
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_wdk.ps1
        powershell ... -File tools\install_wdk.ps1 -WdkUrl <url> -Label 28000
#>

[CmdletBinding()]
param(
    [string] $WdkUrl = 'https://go.microsoft.com/fwlink/?linkid=2335869',  # WDK 10.0.26100.6584
    [string] $Label  = '26100',
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$Root     = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $Root '_tools'
$LogDir   = Join-Path $Root 'Build\logs'
$Kits     = "${env:ProgramFiles(x86)}\Windows Kits\10"
$exe      = Join-Path $ToolsDir "wdksetup_$Label.exe"
$log      = Join-Path $LogDir   "wdksetup_$Label.log"

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  [ok]   $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Bad  { param([string]$m) Write-Host "  [err]  $m" -ForegroundColor Red }

New-Item -ItemType Directory -Force -Path $ToolsDir, $LogDir | Out-Null

# ---------------------------------------------------------------------------
# Ja instalado?
# ---------------------------------------------------------------------------
if (-not $Force) {
    $haveBuild = Test-Path (Join-Path $Kits 'build')
    if ($haveBuild) {
        $wdks = Get-ChildItem (Join-Path $Kits 'build') -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName 'WindowsDriver.Default.props') }
        if ($wdks) {
            foreach ($w in $wdks) {
                $ntifs = Join-Path $Kits "Include\$($w.Name)\km\ntifs.h"
                Write-Ok "WDK $($w.Name)  props=ok  ntifs=$([bool](Test-Path $ntifs))"
            }
            Write-Host '  use -Force para reinstalar.' -ForegroundColor DarkGray
            exit 0
        }
    }
}

# ---------------------------------------------------------------------------
# Download
# ---------------------------------------------------------------------------
Write-Step "Baixando WDK ($Label)"
if (Test-Path $exe) {
    Write-Ok "ja em cache: $exe"
} else {
    Invoke-WebRequest -Uri $WdkUrl -OutFile $exe -MaximumRedirection 10 -TimeoutSec 600
    $v = (Get-Item $exe).VersionInfo
    Write-Ok "$exe  ($($v.ProductName))"
}

# ---------------------------------------------------------------------------
# Instalar
# ---------------------------------------------------------------------------
Write-Step 'Instalando'
Write-Warn 'UAC vai aparecer. Aceite.'

$p = Start-Process -FilePath $exe `
                   -ArgumentList @('/quiet','/norestart','/log', "`"$log`"") `
                   -Wait -PassThru -Verb RunAs

switch ($p.ExitCode) {
    0     { Write-Ok 'instalacao concluida' }
    3010  { Write-Warn 'concluido, reboot necessario' }
    default { Write-Bad "wdksetup retornou $($p.ExitCode). Log: $log"; exit $p.ExitCode }
}

# ---------------------------------------------------------------------------
# Verificar
# ---------------------------------------------------------------------------
Write-Step 'Verificacao'
$ok = $false
Get-ChildItem (Join-Path $Kits 'build') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'WindowsDriver.Default.props') } |
    ForEach-Object {
        $ntifs = Join-Path $Kits "Include\$($_.Name)\km\ntifs.h"
        $ntos  = Join-Path $Kits "Lib\$($_.Name)\km\x64\ntoskrnl.lib"
        if ((Test-Path $ntifs) -and (Test-Path $ntos)) {
            $script:ok = $true
            Write-Ok "WDK $($_.Name): props + ntifs.h + ntoskrnl.lib"
        } else {
            Write-Warn "WDK $($_.Name) incompleto (ntifs=$([bool](Test-Path $ntifs)) ntoskrnl=$([bool](Test-Path $ntos)))"
        }
    }

if (-not $ok) { Write-Bad 'nenhum WDK completo encontrado'; exit 2 }
Write-Host '  build do driver: powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_all.ps1 -Target Driver' -ForegroundColor DarkGray
