<#
    build_all.ps1
    -------------------------------------------------------------------------
    Build unificado do PubgExt. Dois alvos:

      1. mtmacr0.exe        -> PubgExt.sln (MSVC, x64, Release)
                               o nome do .exe vem do <TargetName> do vcxproj
                               (PubgExt\PubgExt.vcxproj -> "mtmacr0").
                               "RuntimeBroker" e' so' o ROTULO do alvo aqui
                               (-Target RuntimeBroker), mantido para nao
                               quebrar o comando que ja' e' usado.
                               contem MASM (driver\syscall.asm) e ImGui +
                               dependencia de import lib vmm.lib / leechcore.lib
      2. HardenStub.sys     -> driver_stub\stub_driver.vcxproj (MSBuild + WDK)
                               ou cl.exe + link.exe (fallback).
                               OBSOLETO: driver_stub\ nao existe mais (foi
                               para _lixo\) — o -Target Driver falha. Use
                               sempre -Target RuntimeBroker.

    O driver que RODA hoje e' o PUBG_KM_safe.sys: compilado por
    tools\build_safe_driver.ps1 e EMBUTIDO no cheat
    (PubgExt\Mapper\driver_bytes.h, mapeado por Mapper\AutoLoader.h).
    Ele nao sai deste script.

    HISTORICO: Loader.exe e DwmRenderer.dll eram os alvos 2 e 3 daqui ate o
    overlay passar a rodar no proprio processo. Nao sao mais buildados, e
    nada aqui mexe em .pfx, assinatura ou carregamento de driver.

    Uso:
        pwsh -File tools\build_all.ps1 -Target RuntimeBroker
        pwsh -File tools\build_all.ps1 -Target RuntimeBroker -Clean
#>

[CmdletBinding()]
param(
    [string[]] $Target   = 'All',
    [string] $Config   = 'Release',
    [switch] $Clean,
    [switch] $NoRestore,

    # Variante do MENU (Config\MenuVariant.h -> MENU_DEBUG_TABS):
    #   dev    -> mtmacr0.exe         (com as abas DEBUG e LOGS)   [default]
    #   public -> mtmacr0_public.exe  (menu limpo, sem as duas abas)
    #   both   -> os dois
    [ValidateSet('dev','public','both')]
    [string] $Variant = 'dev'
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere   = Join-Path $Installer 'vswhere.exe'

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  [ok]   $m" -ForegroundColor Green }
function Write-Bad  { param([string]$m) Write-Host "  [err]  $m" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# VARIANTE DO MENU — alterna Config\MenuVariant.h entre build dev e publico.
#
# POR QUE AQUI E NAO NO .vcxproj: uma macro de PROJETO entra na linha de
# comando de TODOS os .cpp, entao cada troca de variante invalidaria o projeto
# inteiro (~60s). Esse header e' incluido SO' pelo Main.cpp: trocar o valor
# custa UM arquivo recompilado.
#
# A linha do define e' reescrita por regex e o resto do arquivo fica intacto.
# A funcao DEVOLVE o valor que ficou de fato no header, para o chamador poder
# conferir em vez de confiar.
# ---------------------------------------------------------------------------
$MenuVariantHeader = Join-Path $Root 'PubgExt\Config\MenuVariant.h'

function Set-MenuVariantValue {
    param([int] $Value)

    if (-not (Test-Path -LiteralPath $MenuVariantHeader)) {
        throw "MenuVariant.h ausente: $MenuVariantHeader"
    }

    $txt = Get-Content -LiteralPath $MenuVariantHeader -Raw
    if ($txt -notmatch '(?m)^#define\s+MENU_DEBUG_TABS\s+\d+') {
        throw "nao achei a linha do define MENU_DEBUG_TABS em $MenuVariantHeader"
    }

    $novo = [regex]::Replace($txt, '(?m)^#define\s+MENU_DEBUG_TABS\s+\d+', "#define MENU_DEBUG_TABS $Value")
    if ($novo -ne $txt) {
        # ASCII: o header e' ASCII puro de proposito (MSVC + sem BOM)
        Set-Content -LiteralPath $MenuVariantHeader -Value $novo -Encoding ASCII -NoNewline
    }

    $m = [regex]::Match((Get-Content -LiteralPath $MenuVariantHeader -Raw), '(?m)^#define\s+MENU_DEBUG_TABS\s+(\d+)')
    return [int]$m.Groups[1].Value
}

if (-not (Test-Path $vswhere)) { throw "vswhere.exe ausente. Rode tools\bootstrap_toolchain.ps1" }
$VsPath = & $vswhere -latest -products * -property installationPath
if (-not $VsPath) { throw "Nenhum Visual Studio instalado. Rode tools\bootstrap_toolchain.ps1" }

$MSBuild   = Join-Path $VsPath 'MSBuild\Current\Bin\MSBuild.exe'
$VcVars    = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$MsvcRoot  = Join-Path $VsPath 'VC\Tools\MSVC'
$MsvcVer   = (Get-ChildItem $MsvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$MsvcBin   = Join-Path $MsvcRoot "$MsvcVer\bin\Hostx64\x64"

Write-Host "  VS   : $VsPath ($MsvcVer)" -ForegroundColor DarkGray
$kitsProbe = @("${env:ProgramFiles(x86)}\Windows Kits\10", "${env:ProgramFiles}\Windows Kits\10") |
             Where-Object { Test-Path (Join-Path $_ 'Include') } | Select-Object -First 1
if (-not $kitsProbe) { throw "Windows SDK nao encontrado. Veja tools\bootstrap_toolchain.ps1" }
$KitsRoot = $kitsProbe
$SdkVer   = (Get-ChildItem (Join-Path $KitsRoot 'Include') -Directory |
             Where-Object { $_.Name -match '^\d+\.\d+' } |
             Sort-Object Name -Descending | Select-Object -First 1).Name
Write-Host "  SDK  : $SdkVer" -ForegroundColor DarkGray

# o driver_stub.vcxproj fixa o WDK que traz Microsoft.DriverKit.Build.Tasks.18.0.dll
$WdkPin = '10.0.28000.0'
$WdkVer = if (Test-Path (Join-Path $KitsRoot "build\$WdkPin\WindowsDriver.Default.props")) {
    $WdkPin
} else {
    (Get-ChildItem (Join-Path $KitsRoot 'build') -Directory -ErrorAction SilentlyContinue |
     Where-Object { $_.Name -match '^\d+\.\d+' -and
                    (Test-Path (Join-Path $_.FullName 'WindowsDriver.Default.props')) } |
     Sort-Object Name -Descending | Select-Object -First 1).Name
}
if ($WdkVer) { Write-Host "  WDK  : $WdkVer" -ForegroundColor DarkGray }
else         { Write-Host "  WDK  : ausente" -ForegroundColor Yellow }

if ($Clean) {
    Write-Step 'Clean'
    # ---------------------------------------------------------------------------
# ATENCAO: so' INTERMEDIARIO.
#
# "$Root\Build\$Config" (= Build\Release) NAO entra aqui. Ele nao e' lixo de
# build — e' onde ficam kdmapper.exe, o .sys, TestHarness.exe, logs.txt e
# imgui.ini. Apagar isso ja' custou o kdmapper e o historico de log do usuario
# numa sessao inteira.
#
# Se voce quer "rebuild do zero", apagar Build\obj ja' forca recompilar tudo —
# Build\Release e' SOBRESCRITO pelo link, nao precisa ser apagado.
# ---------------------------------------------------------------------------
foreach ($d in @("$Root\Build\obj", "$Root\driver_stub\Build")) {
        if (Test-Path $d) { Remove-Item $d -Recurse -Force; Write-Ok "removido $d" }
    }
}

# ---------------------------------------------------------------------------
# helper: roda um .bat que faz 'call vcvars64' e depois os comandos passados.
# Captura stdout/stderr em arquivo e devolve o exit code real do cmd.exe.
# ---------------------------------------------------------------------------
function Invoke-InVsEnv {
    param([string[]] $Commands, [string] $Label = 'build', [string] $WorkDir = $Root)

    $logDir = Join-Path $Root 'Build\logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $stamp  = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
    $tmpBat = Join-Path $logDir "$Label`_$stamp.bat"
    $logOut = Join-Path $logDir "$Label`_$stamp.log"

    # o .bat joga tudo no log; nao usamos pipe para nao esbarrar no sandbox
    $lines = @('@echo off', "call `"$VcVars`" >nul 2>&1") + $Commands + @('exit /b %ERRORLEVEL%')
    Set-Content -Path $tmpBat -Value ($lines -join "`r`n") -Encoding ASCII

    & cmd.exe /d /c "`"$tmpBat`" > `"$logOut`" 2>&1"
    $code = $LASTEXITCODE

    if (Test-Path $logOut) {
        Get-Content $logOut | Where-Object { $_ -match 'error|Error|ERROR|warning C|fatal|LNK|Aviso' } |
            Select-Object -First 40 | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkYellow }
    }
    Write-Host "      exit=$code  log=$logOut" -ForegroundColor DarkGray
    return $code
}

# ---------------------------------------------------------------------------
# 1. mtmacr0.exe (projeto RuntimeBroker)
# ---------------------------------------------------------------------------
function Build-RuntimeBroker {
    param([string] $MenuVariant = 'dev')

    Write-Step "mtmacr0.exe  [menu: $MenuVariant]  (PubgExt.sln, projeto RuntimeBroker)"
    if (-not (Test-Path $MSBuild)) { Write-Bad "MSBuild ausente: $MSBuild"; return 1 }

    # -----------------------------------------------------------------------
    # DRIVER EMBUTIDO: SO' O DE KEVENT (full KEVENT, 20/10/2026).
    #
    # O bloco do `PUBG_KM_safe.sys` -> `driver_bytes.h` saiu junto com o
    # transporte IOCTL: o .exe nao embute mais dois drivers, e sim um.
    # -----------------------------------------------------------------------
    # -----------------------------------------------------------------------
    # DRIVER DO TRANSPORTE KEVENT — mesma regra, mesmo motivo.
    #
    # O `driver_kevent_bytes.h` era um blob colado a mao, sem gerador. Medido:
    # o header embutia 12.288 bytes de uma build antiga enquanto o fonte
    # `SharedProto\km_ipc_driver.c` compilava 17.408. O app "buildava" e mapeava
    # o driver errado, sem aviso nenhum.
    #
    # Agora o pipeline e' completo e automatico:
    #
    #   km_ipc_driver.c --(SharedProto\build_driver.ps1)--> km_ipc_proto.sys
    #                   --(gen_driver_kevent_bytes.ps1)---> driver_kevent_bytes.h
    #
    # O .sys so' e' recompilado quando o fonte fica mais novo, e o gerador
    # RECUSA embutir um .sys mais velho que o fonte.
    # -----------------------------------------------------------------------
    $srcKevent   = Join-Path $Root 'SharedProto\km_ipc_driver.c'
    $sysKevent   = Join-Path $Root 'SharedProto\out\km_ipc_proto.sys'
    $genKevent   = Join-Path $PSScriptRoot 'gen_driver_kevent_bytes.ps1'
    $buildKevent = Join-Path $Root 'SharedProto\build_driver.ps1'

    if ((Test-Path $srcKevent) -and (Test-Path $buildKevent)) {
        $precisaCompilar = $true
        if (Test-Path $sysKevent) {
            $precisaCompilar = ((Get-Item $srcKevent).LastWriteTime -gt (Get-Item $sysKevent).LastWriteTime)
        }
        if ($precisaCompilar) {
            Write-Host "      driver KEVENT: fonte mais novo, recompilando..." -ForegroundColor DarkGray
            $saidaKevent  = & powershell -NoProfile -ExecutionPolicy Bypass -File $buildKevent 2>&1
            $falhouKevent = @($saidaKevent | Where-Object { $_ -match '\[err\]|error C' })
            if ($falhouKevent.Count -gt 0) {
                Write-Warn ("build do driver KEVENT falhou: " + ($falhouKevent -join ' | '))
            }
        }
        if ((Test-Path $sysKevent) -and (Test-Path $genKevent)) {
            $saida = & powershell -NoProfile -ExecutionPolicy Bypass -File $genKevent 2>&1
            $linha = (@($saida | Where-Object { $_ -match 'no header|regenerado|RECUSADO|err' }) -join ' | ')
            Write-Host ("      driver KEVENT embutido: $linha") -ForegroundColor DarkGray
        } elseif (-not (Test-Path $genKevent)) {
            Write-Warn "gen_driver_kevent_bytes.ps1 ausente: o .exe embute o KEVENT do header ATUAL"
        }
    } else {
        Write-Warn "SharedProto\km_ipc_driver.c ausente: o .exe embute o KEVENT do header ATUAL"
    }

    # Alterna a variante ANTES do build. 0 = publico (sem abas DEBUG/LOGS).
    $valor    = if ($MenuVariant -eq 'public') { 0 } else { 1 }
    $aplicado = Set-MenuVariantValue -Value $valor
    $aviso    = if ($aplicado -ne $valor) { '  <-- NAO APLICOU, o header nao mudou!' } else { '' }
    Write-Host ("      MenuVariant.h -> MENU_DEBUG_TABS $aplicado$aviso") -ForegroundColor DarkGray

    # o vcxproj importa $(VCTargetsPath)\BuildCustomizations\masm.props -> ml64
    $log = Join-Path $Root "Build\msbuild_$Config.log"
    New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null

    # -----------------------------------------------------------------------
    # POR QUE -m:1 E CL_MPCount=1 — e isto e' uma MEDICAO, nao um palpite.
    #
    # O pch.h deste projeto inclui o `json.hpp` (907 KB de fonte). O
    # precompilado resultante tem **148 MB**. Com o build PARALELO (o `-m`
    # sozinho), dois `cl.exe` disputam esse arquivo e um deles morre SEM
    # IMPRIMIR NADA:
    #
    #     ConfigUtilities.cpp
    #     <nada>
    #     FALHA da compilacao   (exit 1)
    #
    # Medido:
    #     -m (paralelo)          -> exit 1, sempre, nos DOIS targets
    #     -m:1 -p:CL_MPCount=1   -> exit 0, com os dois .exe gerados
    #
    # O sintoma enganava em duas camadas: (a) o build as vezes PASSAVA quando
    # o .pch tinha acabado de ser gerado, entao parecia intermitente; (b) o
    # "build_all" reportava [FAIL] em mtmacr0 com o exe ja' correto em disco —
    # o que e' pior que falhar, porque ensina a ignorar o resumo.
    #
    # O custo e' tempo: o build serial demora mais. A alternativa seria tirar o
    # json.hpp do pch.h, o que e' uma mudanca maior e nao e' o que esta' em
    # jogo agora.
    # -----------------------------------------------------------------------
    $msbArgs = @(
        "`"$Root\PubgExt.sln`""
        '-t:Rebuild'
        "-p:Configuration=$Config"
        '-p:Platform=x64'
        '-m:1'
        '-p:CL_MPCount=1'
        '-nologo'
        '-verbosity:minimal'
        '-fl'
        "-flp:logfile=`"$log`";verbosity=normal"
    )

    & $MSBuild @msbArgs 2>&1 | Tee-Object -Variable out | Out-Host
    $code = [int]$LASTEXITCODE
    if ($code -ne 0) {
        Write-Bad "MSBuild falhou (exit $code). Log: $log"
        $out | Select-String -Pattern 'error|Error' | Select-Object -First 40 | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkRed }
        return $code
    }

    # O .vcxproj sempre linka 'mtmacr0.exe' (TargetName fixo). Para as duas
    # variantes coexistirem, a publica e' COPIADA com sufixo — assim o
    # mtmacr0.exe continua sendo o de dev, que e' o que o resto do repo
    # (analisador de log, atalhos, guias) assume.
    $dirExe  = Join-Path $Root "Build\$Config\PubgExt"
    $origem  = Join-Path $dirExe 'mtmacr0.exe'
    if (-not (Test-Path -LiteralPath $origem)) {
        Write-Bad "build disse ok mas $origem nao existe"
        return 1
    }

    # -----------------------------------------------------------------------
    # NOME COM A VERSAO (21/10/2026)
    #
    # Antes o build produzia `mtmacr0.exe` e `mtmacr0_public.exe`, sem versao em
    # lugar nenhum do nome. Resultado pratico: tres arquivos de 1,4 MB na pasta,
    # todos chamados igual, e nenhuma forma de saber qual build o usuario esta
    # rodando quando manda um print ou um log.
    #
    # A versao vem de `PubgExt\Config\Version.h` — FONTE UNICA. Mudar o chip do
    # menu e o nome do arquivo em dois lugares era como a versao ja' tinha
    # divergido antes (chip v2.0, guia v2.1).
    # -----------------------------------------------------------------------
    $ver = '0.0'
    $verHeader = Join-Path $Root 'PubgExt\Config\Version.h'
    if (Test-Path $verHeader) {
        $m = Select-String -Path $verHeader -Pattern '#define\s+MTM_VERSAO\s+"([^"]+)"'
        if ($m) { $ver = $m.Matches[0].Groups[1].Value }
    }

    if ($MenuVariant -eq 'public') {
        $destino = Join-Path $dirExe "mtmacr0_public_v$ver.exe"
        Copy-Item -LiteralPath $origem -Destination $destino -Force
        # copia sem versao: e' o que o loader/publicacao antiga referencia
        Copy-Item -LiteralPath $origem -Destination (Join-Path $dirExe 'mtmacr0_public.exe') -Force
    } else {
        $destino = Join-Path $dirExe "mtmacr0_v$ver.exe"
        Copy-Item -LiteralPath $origem -Destination $destino -Force
    }
    Write-Ok "Build ok -> $(Split-Path $destino -Leaf)  (versao $ver)"

    # -----------------------------------------------------------------------
    # COPIA COM NOME SORTEADO — SO' NA VARIANTE PUBLICA.
    #
    # `mtmacr0.exe` e um nome fixo: todo usuario do build compartilha o mesmo
    # nome de arquivo e de processo, e isso correlaciona sem heuristica nenhuma.
    # A copia abaixo tem nome novo a cada build.
    #
    # SO' A PUBLICA: e' ela que se entrega. A variante dev existe para teste
    # local e nao vai para ninguem — antes disto a copia neutra era gerada
    # tambem para a dev, e como a dev buildava DEPOIS, a limpeza apagava a
    # copia publica e o "arquivo para publicar" acabava sendo o build errado
    # (com todos os menus, inclusive). Bug silencioso: o nome estava certo.
    # -----------------------------------------------------------------------
    if ($MenuVariant -eq 'public') {
        $tag = -join ((48..57) + (97..102) | Get-Random -Count 8 | ForEach-Object { [char]$_ })
        $nomeNeutro = "sv" + $tag + ".exe"
        $copia = Join-Path $dirExe $nomeNeutro

        # Limpa copias publicas anteriores: a pasta acumularia um .exe novo a
        # cada build. A copia e' descartavel — quem ja' recebeu nao depende dela.
        Get-ChildItem -LiteralPath $dirExe -Filter 'sv*.exe' -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }

        Copy-Item -LiteralPath $destino -Destination $copia -Force
        Write-Ok "ARQUIVO PARA PUBLICAR -> $nomeNeutro  (copia da publica)"
    }
}



# ---------------------------------------------------------------------------
# 2. HardenStub.sys
# ---------------------------------------------------------------------------
function Build-Driver {
    Write-Step 'HardenStub.sys'

    # -----------------------------------------------------------------------
    # OBSOLETO — e agora ele SABE disso.
    #
    # O driver_stub\ foi removido do projeto (§12 do ARQUITETURA.md), mas esta
    # funcao continuava tentando compilar "driver_stub\stub_driver.c". O
    # resultado era MSB1009 / C1083 e um "[FAIL] HardenStub.sys" em todo build —
    # um alarme que nunca correspondia a nada, do lado de um [FAIL] que as vezes
    # correspondia (ver o -t:Rebuild em Build-RuntimeBroker).
    #
    # Um passo que sempre falha e' pior que um passo ausente: ele ensina a
    # ignorar a linha do resumo.
    # -----------------------------------------------------------------------
    if (-not (Test-Path (Join-Path $Root 'driver_stub'))) {
        Write-Host '  [--] driver_stub\ nao existe mais (removido do projeto). Passo pulado.' -ForegroundColor DarkGray
        return 0
    }

    $wdkProps = if ($WdkVer) { Join-Path $KitsRoot "build\$WdkVer\WindowsDriver.Default.props" } else { $null }
    if ($wdkProps -and (Test-Path $wdkProps)) {
        Write-Host "  rota WDK/MSBuild (WDK $WdkVer)"
        $proj = "$Root\driver_stub\stub_driver.vcxproj"
        & $MSBuild $proj -t:Build -p:Configuration=$Config -p:Platform=x64 `
            -nologo -verbosity:minimal 2>&1 | Tee-Object -Variable out | Out-Host
        if ([int]$LASTEXITCODE -eq 0) {
            Write-Ok "Build ok -> driver_stub\Build\driver_stub\$Config\HardenStub.sys"
            return 0
        }
        Write-Bad "MSBuild/WDK falhou. Caindo para cl+link manual."
        $out | Select-String 'error' | Select-Object -First 20 | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkRed }
    } else {
        Write-Host '  WDK ausente -> rota cl+link manual' -ForegroundColor Yellow
    }

    # --- fallback: compila com os headers km do WDK -------------------------
    if (-not $WdkVer) { Write-Bad 'WDK nao instalado. Rode tools\install_wdk.ps1'; return 1 }
    $kmInc = Join-Path $KitsRoot "Include\$WdkVer"
    $kmLib = Join-Path $KitsRoot "Lib\$WdkVer\km\x64"
    if (-not (Test-Path (Join-Path $kmInc 'km\ntifs.h'))) {
        Write-Bad "Header km\ntifs.h ausente em $kmInc. WDK $WdkVer incompleto."
        return 1
    }

    $outDir = "$Root\Build\driver_stub\$Config"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $cmd = @(
        "cl.exe /nologo /c /O2 /W3 /Z7 /GS- /Gy /D_AMD64_ /DAMD64 " +
        "/DBUILD_ID=0x00000001 /DBUILD_VER=\`"p0-stub\`" " +
        "/I`"$kmInc\km`" /I`"$kmInc\shared`" " +
        "`"$Root\driver_stub\stub_driver.c`" /Fo`"$outDir\stub_driver.obj`"",
        "link.exe /nologo /MANIFEST:NO /DRIVER /OPT:REF /OPT:ICF /ENTRY:DriverEntry " +
        "/NODEFAULTLIB /SUBSYSTEM:NATIVE /INTEGRITYCHECK " +
        "`"$outDir\stub_driver.obj`" ntoskrnl.lib /LIBPATH:`"$kmLib`" " +
        "/OUT:`"$outDir\HardenStub.sys`""
    )
    $code = Invoke-InVsEnv -Commands $cmd -Label 'driver'
    if ($code -eq 0) { Write-Ok "Build ok -> Build\driver_stub\$Config\HardenStub.sys" } else { Write-Bad "Driver falhou (exit $code)" }
    return $code
}

# ---------------------------------------------------------------------------
# runner
# ---------------------------------------------------------------------------
$Known = @('All','RuntimeBroker','Driver')
$flat  = @()
foreach ($t in $Target) { $flat += ($t -split ',') }
$flat = $flat | ForEach-Object { $_.Trim() } | Where-Object { $_ }
foreach ($t in $flat) {
    if ($Known -notcontains $t) { throw "Target invalido: '$t'. Validos: $($Known -join ', ')" }
}

$results = [ordered]@{}
$run = if ($flat -contains 'All') { @('RuntimeBroker','Driver') } else { $flat }

# A ORDEM importa no 'both': o PUBLICO builda primeiro e e' copiado com sufixo;
# o DEV builda por ultimo, para o mtmacr0.exe terminar sendo o de dev.
$variantes = switch ($Variant) {
    'both'   { @('public','dev') }
    'public' { @('public') }
    default  { @('dev') }
}

try {
    foreach ($t in $run) {
        switch ($t) {
            'RuntimeBroker' {
                $rc = 0
                foreach ($v in $variantes) {
                    $rc = [int](Build-RuntimeBroker -MenuVariant $v)
                    if ($rc -ne 0) { break }
                }
                $results.RuntimeBroker = $rc
            }
            'Driver'        { $results.Driver        = [int](Build-Driver) }
        }
    }
} finally {
    # -----------------------------------------------------------------------
    # RESTAURA O HEADER.
    #
    # MenuVariant.h e' arquivo do repo e o valor de REPOUSO e' dev (1). Sem
    # isto, buildar a variante publica deixaria um "publico" silencioso no
    # working tree — e a proxima sessao de debug sem as abas, sem ninguem
    # entender por que. Roda mesmo se o build estourar no meio.
    # -----------------------------------------------------------------------
    $repouso = Set-MenuVariantValue -Value 1
    if ($Variant -ne 'dev') {
        Write-Host "  [i] MenuVariant.h devolvido para MENU_DEBUG_TABS $repouso (dev)" -ForegroundColor DarkGray
    }
}

Write-Step 'Resumo'
$bad = 0
# o alvo do script continua 'RuntimeBroker' — e' rotulo, nao o nome do projeto
# no .sln (esse e' PubgExt); o binario que sai do build e' o mtmacr0.exe
$Labels = @{ RuntimeBroker = 'mtmacr0 (PubgExt.sln)'; Driver = 'HardenStub.sys' }
foreach ($k in $results.Keys) {
    $name = if ($Labels.ContainsKey($k)) { $Labels[$k] } else { $k }
    if ($results[$k] -eq 0) { Write-Host "  [ok]   $name" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $name (exit $($results[$k]))" -ForegroundColor Red; $bad++ }
}

Write-Host ''
Write-Host '  Artefatos de runtime esperados em Build\Release\PubgExt\:' -ForegroundColor DarkGray
# Ndu.sys e kdmapper.exe NAO entram mais nesta lista: o driver atual e' o
# PUBG_KM_safe.sys e o cheat o embute/mapeia sozinho
# (PubgExt\Mapper\AutoLoader.h + Mapper\driver_bytes.h) — o kdmapper.exe
# standalone nao e' necessario. Cobrar os dois so' imprimia [miss] em todo build.
$artefatos = @('mtmacr0.exe')
# so' cobra o _public se ele existir: um build -Variant dev (default) nao
# produz essa variante, e imprimir [miss] nela seria ruido.
if (Test-Path (Join-Path $Root "Build\Release\PubgExt\mtmacr0_public.exe")) {
    $artefatos += 'mtmacr0_public.exe'
}
$artefatos += 'PUBG_KM_safe.sys'
foreach ($f in $artefatos) {
    $p = Join-Path $Root "Build\Release\PubgExt\$f"
    $mark = if (Test-Path $p) { '[ok]  ' } else { '[miss]' }
    Write-Host "    $mark $f" -ForegroundColor DarkGray
}

# "Outros artefatos" saiu daqui. Ele listava o HardenStub.sys em
# driver_stub\Build\driver_stub\$Config\, mas o driver_stub\ nao existe mais (foi
# para _lixo\) e a secao so' imprimia [ok] quando o arquivo existia — ou seja,
# nunca imprimia nada. O driver atual e' o PUBG_KM_safe.sys, ja' listado acima.

Write-Host ''
Write-Host '  Verificacoes (rode depois do build):' -ForegroundColor DarkGray
Write-Host '    python tools\dump_driver\contract_check.py            # contrato user<->kernel' -ForegroundColor DarkGray
Write-Host '    python tools\dump_driver\contract_check.py --selftest # prova que o checker funciona' -ForegroundColor DarkGray
Write-Host '    python tools\dump_driver\offset_audit.py              # offsets vs dump de referencia' -ForegroundColor DarkGray

exit $bad
