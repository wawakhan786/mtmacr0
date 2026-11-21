<#
    install_vs_components.ps1
    -------------------------------------------------------------------------
    Estado atual desta maquina (auditado):

      VS 2026 Community 18.10.3     C:\Program Files\Microsoft Visual Studio\18\Community
        - MSBuild.exe               OK
        - MSVC 14.51 (v145)         OK  (cl.exe, link.exe, lib.exe, ml64.exe)
        - ATL                       OK
        - MASM BuildCustomizations  OK  (v180)
        - Windows SDK               AUSENTE  <-- bloqueia TODO build C/C++
        - WDK                       AUSENTE  <-- bloqueia driver_stub
        - rc.exe                    AUSENTE  (vem no SDK)
        - signtool.exe              AUSENTE  (vem no SDK)

    Esse script adiciona os componentes que faltam na instancia existente.
    NAO instala uma segunda copia do Visual Studio.

    O que ele adiciona:
      Microsoft.VisualStudio.Component.Windows11SDK.26100
          -> Windows SDK 10.0.26100.0 (headers, libs, rc.exe, signtool)
          -> e o SDK que o driver_stub.vcxproj espera
      Microsoft.VisualStudio.Component.Windows11SDK.28000
          -> SDK mais novo, fallback / util para toolchains futuras
      Microsoft.VisualStudio.ComponentGroup.VC.Tools.143.x86.x64
          -> toolset v143 (o .sln e os .vcxproj estao em PlatformToolset v143)
      Component.Microsoft.Windows.DriverKit
      Component.Microsoft.Windows.DriverKit.BuildTools
          -> WDK (ntifs.h, ntoskrnl.lib, WindowsDriver.props/targets, extensao VS)

    Debugging Tools for Windows (dbghelp.dll + symsrv.dll para FixCr3)
    sao um componente separado do SDK. Se precisar:
        - adicione 'Microsoft.VisualStudio.Component.Windows10SDK.IpOverUsb'? nao.
        - o jeito e instalar "Debugging Tools for Windows" pelo Windows SDK
          installer separado, ou winget install Microsoft.WinDbg
          e copiar dbghelp.dll/symsrv.dll da pasta do WinDbg.

    Uso:
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_vs_components.ps1
        powershell ... -File tools\install_vs_components.ps1 -WhatIf
#>

[CmdletBinding()]
param(
    [switch] $WhatIf,
    [switch] $SkipVerify
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$setup     = Join-Path $Installer 'setup.exe'
$vswhere   = Join-Path $Installer 'vswhere.exe'

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  [ok]   $m" -ForegroundColor Green }
function Write-Warn { param([string]$m) Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Bad  { param([string]$m) Write-Host "  [err]  $m" -ForegroundColor Red }

if (-not (Test-Path $setup)) { throw "Visual Studio Installer ausente: $setup" }

$instances = & $vswhere -all -products * -format json | ConvertFrom-Json
if (-not $instances) { throw "Nenhuma instancia do Visual Studio encontrada." }

$vs = $instances |
      Sort-Object { [version]$_.installationVersion } -Descending |
      Select-Object -First 1

Write-Step 'Instancia alvo'
Write-Host "  $($vs.displayName)  $($vs.installationVersion)"
Write-Host "  $($vs.installationPath)"

$Components = @(
    'Microsoft.VisualStudio.Component.Windows11SDK.26100'
    'Microsoft.VisualStudio.Component.Windows11SDK.28000'
    'Microsoft.VisualStudio.ComponentGroup.VC.Tools.143.x86.x64'
    'Component.Microsoft.Windows.DriverKit'
    'Component.Microsoft.Windows.DriverKit.BuildTools'
)

Write-Step 'Componentes a adicionar'
foreach ($c in $Components) { Write-Host "  + $c" }

$addArgs = ($Components | ForEach-Object { "--add $_" }) -join ' '

# --installPath tem espaco. Passar array para Start-Process faz o caminho ser
# cortado em "C:\Program". Tem que entrar como uma unica string citada.
$cmdLine = 'modify --installPath "{0}" --quiet --norestart --nocache {1}' -f $vs.installationPath, $addArgs

if ($WhatIf) {
    Write-Step 'WhatIf'
    Write-Host "  `"$setup`" $cmdLine"
    exit 0
}

Write-Step 'Rodando o Visual Studio Installer'
Write-Warn 'Vai aparecer UAC. Aceite para continuar.'
Write-Warn 'Isso baixa ~2-4 GB e pode levar de 10 a 40 minutos.'

$p = Start-Process -FilePath $setup -ArgumentList $cmdLine -Wait -PassThru -Verb RunAs
switch ($p.ExitCode) {
    0      { Write-Ok 'instalacao concluida' }
    3010   { Write-Warn 'concluido, reboot necessario' }
    1602   { Write-Bad 'cancelado pelo usuario' ; exit 1602 }
    default{ Write-Bad "setup.exe retornou $($p.ExitCode)" ; exit $p.ExitCode }
}

if (-not $SkipVerify) {
    Write-Step 'Verificacao pos-instalacao'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'bootstrap_toolchain.ps1') -VerifyOnly
}
