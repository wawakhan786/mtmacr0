<#
    build_safe_driver.ps1
    -------------------------------------------------------------------------
    Compila km_safe_source.c -> Build\Release\PubgExt\PUBG_KM_safe.sys

    Saida com nome DIFERENTE de proposito: o driver atual (PUBG_KM.sys) fica
    intacto como fallback ate' o novo passar nos testes.

    Mesmo toolchain do build_kernel_driver.ps1 (cl + link + WDK), mas com
    /W4 e sem mascarar imports — aqui a gente quer o binario mais chato de
    errar possivel, nao o mais discreto.
#>
[CmdletBinding()]
param(
    [string] $Config  = 'Release',
    [string] $SrcName = 'km_safe_source.c',
    [string] $OutName = 'PUBG_KM_safe.sys',

    # -----------------------------------------------------------------------
    # A CHAMADA AO CALLBACK E' COMPORTAMENTO NORMAL. Este parametro e' legado.
    #
    # ===================== O QUE A VERDADE ACABOU SENDO =====================
    #
    # Durante muito tempo este comentario dizia que a descoberta do callback
    # "por varredura NAO e' confiavel: o DeviceExtension tem varios ponteiros
    # para .text do mouclass e nada em runtime diz qual e' o
    # MouseClassServiceCallback".
    #
    # ISSO ESTAVA ERRADO, e o erro custou caro. O alvo errado no comeco nao era
    # questao de confiabilidade da varredura: era o DRIVER ERRADO. A varredura
    # estava no \Driver\Mouclass, e o CONNECT_DATA vive no \Driver\Mouhid - o
    # driver de BAIXO, que e' quem chama o callback.
    #
    # Corrigido o alvo, a varredura encontra o par exato, e foi confirmado
    # contra o simbolo real:
    #
    #   dbh -s:srv*C:\symbols*https://msdl.microsoft.com/download/symbols \
    #       C:\Windows\System32\drivers\mouclass.sys
    #   > n MouseClassServiceCallback
    #     addr : 1004be0     ->  RVA 0x4BE0
    #
    # E o nosso scan acha ClassService em RVA 0x4BE0. O mesmo numero.
    #
    # Havia tambem a hipotese de que o valor viesse do PDB, resolvido em
    # user-mode e passado pronto pro driver. E' o que outros projetos fazem, e
    # e' mais deterministico - mas nao foi necessario: em Win10 22H2 o
    # CONNECT_DATA existe e a validacao de dois campos o identifica.
    # ========================================================================
    #
    # -NoMouseCall : compila SEM a chamada. So' serve para DEPURAR (isolar se
    #                um problema e' da injecao ou do resto do driver). Nunca
    #                use em producao: o mouse cai para o SendInput, que marca
    #                o pacote com LLMHF_INJECTED - exatamente o que a injecao
    #                por kernel existe para evitar.
    [switch] $NoMouseCall,

    # Aceito e IGNORADO: o bundle de teste da VM ainda o passa. A chamada ja'
    # e' o padrao, entao nao ha nada a ligar.
    [switch] $MouseCall
)

$ErrorActionPreference = 'Stop'

if ($NoMouseCall) {
    Write-Host ""
    Write-Host "  *** BUILD SEM A CHAMADA (KM_MOUSE_CALL_ENABLED=0) ***" -ForegroundColor Yellow
    Write-Host "  So' para depurar. O mouse cai para SendInput (com LLMHF_INJECTED)." -ForegroundColor Yellow
    Write-Host ""
}

$Root      = Split-Path -Parent $PSScriptRoot
$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'
$VsPath    = & $vswhere -latest -products * -property installationPath
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

$KitsRoot = @("${env:ProgramFiles(x86)}\Windows Kits\10", "${env:ProgramFiles}\Windows Kits\10") |
            Where-Object { Test-Path (Join-Path $_ 'Include') } | Select-Object -First 1
if (-not $KitsRoot) { throw 'Windows Kits nao encontrado.' }

$WdkVer = (Get-ChildItem (Join-Path $KitsRoot 'Include') -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+' -and (Test-Path (Join-Path $_.FullName 'km\ntifs.h')) } |
           Sort-Object Name -Descending | Select-Object -First 1).Name
if (-not $WdkVer) { throw 'Nenhum WDK com km\ntifs.h encontrado.' }

$kmInc  = Join-Path $KitsRoot "Include\$WdkVer"
$kmLib  = Join-Path $KitsRoot "Lib\$WdkVer\km\x64"
$outDir = Join-Path $Root "Build\$Config\PubgExt"
$objDir = Join-Path $Root 'Build\obj\kernel_driver'
$logDir = Join-Path $Root 'Build\logs'

New-Item -ItemType Directory -Force -Path $outDir, $objDir, $logDir | Out-Null

$src = Join-Path $Root $SrcName
if (-not (Test-Path $src)) { throw "fonte nao encontrado: $src" }

Write-Host "  WDK  : $WdkVer" -ForegroundColor DarkGray
Write-Host "  src  : $SrcName" -ForegroundColor DarkGray
Write-Host "  out  : $outDir\$OutName" -ForegroundColor DarkGray

$def = Join-Path $objDir 'pkm_safe.def'
@'
LIBRARY KmSafe
; Nome INTERNO do modulo. Era "PkmSafe" -- que aparecia como string
; ASCII dentro do .sys E dentro do mtmacr0.exe (o driver vai embutido em
; driver_bytes.h). Um nome que remete ao cheat, visivel numa varredura
; de strings do binario.
EXPORTS
    DriverEntry
'@ | Set-Content -Path $def -Encoding ASCII

$obj    = Join-Path $objDir 'km_safe_source.obj'
$bat    = Join-Path $logDir 'kernel_safe.bat'
$logOut = Join-Path $logDir "kernel_safe_$(Get-Date -Format yyyyMMdd_HHmmss_fff).log"

# O define que liga a chamada perigosa. Vem do parametro, e o default e' NAO
# definir (o `#ifndef` no fonte assume 0).
$defineMouse = if ($NoMouseCall) { '/DKM_MOUSE_CALL_ENABLED=0 ' } else { '' }

# ---------------------------------------------------------------------------
# SEM /TP. O driver continua compilando como C, e isto e' uma decisao REVERTIDA.
#
# Em algum momento desta sessao eu liguei `/TP` aqui para poder usar
# `std::index_sequence` no KM_S() (a string cifrada em tempo de compilacao do
# km_safe_source.c). Funcionava, e quebrou outra coisa bem maior:
#
#   - C++ MANGLING em todo simbolo. O `DriverEntry` virou
#     `?DriverEntry@@YAJPEAU_DRIVER_OBJECT@@...`, e as oito funcoes de kernel
#     declaradas a mao (IoCreateDriver, MmCopyVirtualMemory, ZwQuerySystem...)
#     viraram LNK2019. O ntoskrnl exporta esses nomes SEM mangling;
#   - `ObReferenceObjectByName` e `IoDriverObjectType` continuaram sem resolver
#     mesmo com `extern "C"`, e o motivo nao era obvio pelo erro.
#
# Passar o driver INTEIRO para C++ por causa de uma string e' trocar um
# problema grande por um problema menor. O KM_S() foi reescrito SEM template
# (KM_S16/KM_CC no km_safe_source.c): cifra em tempo de compilacao, em C puro.
#
# SE ALGUEM FOR LIGAR /TP DE NOVO: leia o bloco do KM_S no fonte e o
# `extern "C"` das declaracoes de kernel antes. Os dois erros voltam juntos.
# ---------------------------------------------------------------------------
$cmds = @(
    '@echo off'
    "call `"$VcVars`" >nul 2>&1"
    "cl.exe /nologo /c /O2 /W3 /GS- /Gy /Z7 /D_AMD64_ /DAMD64 $defineMouse" +
      "/I`"$kmInc\km`" /I`"$kmInc\shared`" " +
      "`"$src`" /Fo`"$obj`""
    "link.exe /nologo /MANIFEST:NO /DRIVER /OPT:REF /OPT:ICF /ENTRY:DriverEntry " +
      "/NODEFAULTLIB /SUBSYSTEM:NATIVE " +
      "/DEF:`"$def`" " +
      "`"$obj`" " +
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
    Write-Host "  [err]  $($compileErrors.Count) erro(s):" -ForegroundColor Red
    $compileErrors | Select-Object -First 15 | ForEach-Object { Write-Host "         $_" -ForegroundColor DarkRed }
} else {
    Write-Host "  [err]  build falhou (exit $code). Log: $logOut" -ForegroundColor Red
}
exit 1
