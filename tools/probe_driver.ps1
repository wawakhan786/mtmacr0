<#
    probe_driver.ps1
    -------------------------------------------------------------------------
    Fala DIRETO com o driver, sem passar pelo cheat.

    POR QUE ISTO EXISTE

    Testar o IOCTL pelo menu tem tres elos no meio que podem falhar em
    silencio: a deteccao da tecla (o teste do ALT usava GetAsyncKeyState, que
    devolve 0 neste processo), o checkbox, e a thread. Quando nao aparece
    linha nenhuma no PkmSafe.txt, voce nao sabe QUAL elo quebrou.

    Este script remove os tres. Ele abre o device, pergunta o PING (que so' o
    driver NOVO responde), e manda um MOUSE_MOVE de verdade.

    COMO LER O RESULTADO

      CreateFile falha (err=2)
          Nenhum driver carregado. Rode o cheat primeiro.

      PING responde 0x504B4D53
          O driver NOVO esta' carregado. Se o MOUSE falhar depois disto, o
          problema e' a descoberta do callback do Mouclass.

      PING falha (err=1 ou 87)
          O driver VELHO esta' carregado (o dispatcher nao conhece 0x8FF).
          O kdmapper nao descarrega, entao cargas antigas ficam vivas. O
          caminho e' reiniciar a maquina e carregar de novo.

      MOUSE responde OK
          A cadeia inteira funciona. O cursor anda 10 px para a direita --
          e' de proposito, e' a prova visual.

      MOUSE falha com err=0
          O IOCTL chegou e o driver devolveu um status de erro (o
          STATUS_NOT_FOUND = callback nao achado). Veja o PkmSafe.txt: a
          linha "-- mouse:" diz exatamente em que passo parou.

    Uso (PRECISA SER ADMINISTRADOR):
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_driver.ps1

    ---------------------------------------------------------------------------
    O SEED (mudanca da versao 1.2)

    O nome do device deixou de ser um literal. Era "\\.\Ndu2" — e esse literal
    ficava DENTRO do .sys, que vai embutido no .exe, achavel com um `strings`.

    Agora e' "\\.\<16 hex>" derivado do seed da sessao. Sem -Seed, este script
    le o ULTIMO seed do logs.txt do cheat (que e' a sessao que tem driver
    carregado). Com -Seed, voce aponta pra uma sessao especifica.

        -Seed 0123456789abcdef     aponta para o device dessa seed
    ---------------------------------------------------------------------------
#>

[CmdletBinding()]
param(
    [int]    $Dx = 10,
    [int]    $Dy = 0,
    [int]    $UnitId = 1,
    [int]    $Flags  = 0,
    [string] $Seed = '',

    # Roda o mtmacr0.exe em modo map-only ANTES de sondar, para obter um seed
    # de verdade (o cheat loga o seed e o nome do device). Sem isto, o script
    # depende de uma sessao ja' ter rodado.
    [switch] $MapOnly,

    # Manda DEACTIVATE: o driver para de responder e sai do namespace, sem
    # liberar as paginas. E' a descarga reversivel — o proximo map-only carrega
    # de novo sem reboot.
    [switch] $Deactivate,

    # Manda SELFDESTRUCT: libera as paginas do driver. IRREVERSIVEL na sessao.
    # Numa VM com snapshot e' a prova final de que a descarga funciona.
    [switch] $SelfDestruct,

    # Imprime SO' o nome do device derivado do ultimo seed e sai.
    #
    # Existe para o RunSafeTest.cmd: o parsing de hex no batch e' fragil (o
    # `for /f` corta em ':' e trata o espaco depois da virgula), e o
    # DeviceName.ps1 ja' faz a derivacao certa. Entao o batch delega em vez de
    # reimplementar.
    [switch] $PrintDevice
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'DeviceName.ps1')

# ---------------------------------------------------------------------------
# -PrintDevice: modo "so' me diga o nome", para consumo por outros scripts.
# Nao imprime nada alem do nome — o RunSafeTest.cmd captura esta saida.
# ---------------------------------------------------------------------------
if ($PrintDevice) {
    $s = Get-KmSeedFromLog
    if ($null -eq $s) { exit 1 }
    Write-Output (Get-KmDeviceName -Seed $s)
    exit 0
}

# ---------------------------------------------------------------------------
# O SEED: do parametro, do logs.txt, ou de uma sessao map-only nova.
#
# A ordem importa: se o He passou -Seed, e' aquela sessao que ele quer. Se
# passou -MapOnly, a gente CARREGA o driver agora e le o seed que apareceu.
# Sem nenhum dos dois, le' o ultimo seed do log.
# ---------------------------------------------------------------------------
if ($MapOnly) {
    $bin = Join-Path (Split-Path -Parent $PSScriptRoot) 'Build\Release\PubgExt'
    $exe = Join-Path $bin 'mtmacr0.exe'
    if (-not (Test-Path $exe)) { throw "map-only: $exe nao existe" }

    Write-Host ""
    Write-Host "  rodando o cheat em modo map-only (mapeia e sai)..." -ForegroundColor DarkGray
    $env:PUBGEXT_MAP_ONLY = '1'
    try {
        & $exe | Out-Null
    } finally {
        Remove-Item Env:\PUBGEXT_MAP_ONLY -ErrorAction SilentlyContinue
    }
}

if (-not $Seed) {
    $s = Get-KmSeedFromLog
    if ($null -eq $s) {
        Write-Host ""
        Write-Host "  [FALHA] nenhum seed disponivel." -ForegroundColor Red
        Write-Host "          O nome do device NAO e' mais um literal - ele sai do seed" -ForegroundColor Yellow
        Write-Host "          da sessao. Opcoes:" -ForegroundColor Yellow
        Write-Host "            -MapOnly                      carrega o driver agora e usa o seed novo" -ForegroundColor Yellow
        Write-Host "            -Seed 0123456789abcdef        usa um seed especifico (logs.txt)" -ForegroundColor Yellow
        exit 1
    }
    $seedVal = $s
    Write-Host ("  seed lido do logs.txt: 0x{0:X16}" -f $seedVal) -ForegroundColor DarkGray
} else {
    $seedVal = [uint64]::Parse($Seed, [System.Globalization.NumberStyles]::HexNumber)
    Write-Host ("  seed do parametro    : 0x{0:X16}" -f $seedVal) -ForegroundColor DarkGray
}

$devicePath = Get-KmDeviceName -Seed $seedVal

$src = @'
using System;
using System.Runtime.InteropServices;
public static class KmProbe {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
    public static extern IntPtr CreateFileA(string n, uint a, uint s, IntPtr sec, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool DeviceIoControl(IntPtr h, uint c, byte[] i, uint isz, byte[] o, uint osz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr h);
}
'@
if (-not ('KmProbe' -as [type])) { Add-Type -TypeDefinition $src }

# CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, func, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
#   = (0x22 << 16) | (func << 2)
function Ctl([int]$func) { return [uint32]((0x22 -shl 16) -bor ($func -shl 2)) }

$GENERIC_RW = [uint32]3221225472   # 0xC0000000
$PING  = Ctl 0x8FF
$MOUSE = Ctl 0x805
$VERSION = Ctl 0x806
$DEACTIVATE = Ctl 0x807

Write-Host ""
Write-Host "=== probe do driver ===" -ForegroundColor Cyan
Write-Host ("  device = {0}" -f $devicePath)
Write-Host ("  PING  = 0x{0:X8}" -f $PING)
Write-Host ("  MOUSE = 0x{0:X8}" -f $MOUSE)
Write-Host ("  VERSAO= 0x{0:X8}" -f $VERSION)
Write-Host ("  DEACT = 0x{0:X8}" -f $DEACTIVATE)
Write-Host ""

# --- 1) abre o device ------------------------------------------------------
$h = [KmProbe]::CreateFileA($devicePath, $GENERIC_RW, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr]::new(-1)) {
    $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host ("  [FALHA] nao abri {0}   err={1}" -f $devicePath, $e) -ForegroundColor Red
    if ($e -eq 2)  { Write-Host "          nenhum driver carregado - rode o mtmacr0.exe primeiro" -ForegroundColor Yellow }
    if ($e -eq 3)  { Write-Host "          caminho nao encontrado - o seed nao e' o da sessao que mapeou" -ForegroundColor Yellow }
    if ($e -eq 5)  { Write-Host "          acesso negado - precisa rodar como Administrador" -ForegroundColor Yellow }
    if ($e -eq 1167) { Write-Host "          device desconectado - ele ja' passou por DEACTIVATE" -ForegroundColor Yellow }
    exit 1
}
Write-Host ("  [ok] device {0} aberto" -f $devicePath) -ForegroundColor Green

# --- 2) PING: qual driver esta' carregado? ---------------------------------
$out = New-Object byte[] 4
$ret = [uint32]0
$ok = [KmProbe]::DeviceIoControl($h, $PING, $null, [uint32]0, $out, [uint32]4, [ref]$ret, [IntPtr]::Zero)
if ($ok) {
    $magic = [BitConverter]::ToUInt32($out, 0)
    if ($magic -eq 0x504B4D53) {
        # ATENCAO: o PING existe nas DUAS versoes do km_safe. Ele prova que ha'
        # um driver nosso carregado - NAO que e' o desta build. Quem responde
        # isso e' o MOUSE abaixo. (Eu escrevi "driver NOVO" aqui na primeira
        # versao e isso mandou o diagnostico pro lado errado por uma rodada.)
        Write-Host "  [ok] PING respondeu 0x504B4D53  -> ha' um km_safe carregado" -ForegroundColor Green
        Write-Host "       (isto NAO diz a versao - o PING existe em todas)" -ForegroundColor DarkGray
    } else {
        Write-Host ("  [?]  PING respondeu 0x{0:X8} (magic inesperado)" -f $magic) -ForegroundColor Yellow
    }
} else {
    $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host ("  [err] PING falhou  err={0}  -> driver VELHO (nao conhece 0x8FF)" -f $e) -ForegroundColor Red
    [void][KmProbe]::CloseHandle($h)
    exit 2
}

# --- 2b) VERSAO: QUAL driver esta' carregado? -------------------------------
#
# Este e' o unico teste que responde "qual", e nao "se". O PING existe em toda
# build; o GET_VERSION nao — um driver 1.0 devolve err=1 aqui.
#
# E' o que resolve o "driver fantasma": como o kdmapper nao descarrega, o device
# de uma sessao anterior continua vivo, o CreateFile tem sucesso, e sem esta
# pergunta o cheat roda contra a versao antiga sem saber.
$outV = New-Object byte[] 4
$retV = [uint32]0
$okV = [KmProbe]::DeviceIoControl($h, $VERSION, $null, [uint32]0, $outV, [uint32]4, [ref]$retV, [IntPtr]::Zero)
$ESPERADA = 0x00010002
if ($okV) {
    $ver = [BitConverter]::ToUInt32($outV, 0)
    if ($ver -eq $ESPERADA) {
        Write-Host ("  [ok] VERSAO = 0x{0:X8}  -> e' o driver DESTA build" -f $ver) -ForegroundColor Green
    } else {
        Write-Host ("  [!]  VERSAO = 0x{0:X8}  esperada 0x{1:X8}" -f $ver, $ESPERADA) -ForegroundColor Yellow
        Write-Host "       -> driver de OUTRA build. Se for anterior a 1.2 ele NAO" -ForegroundColor Yellow
        Write-Host "          conhece DEACTIVATE/SELFDESTRUCT, e a recarga sem reboot" -ForegroundColor Yellow
        Write-Host "          nao vai funcionar com ele (o nome dele tambem e outro)." -ForegroundColor Yellow
    }
} else {
    $eV = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host ("  [!]  VERSAO: err={0}  -> o driver NAO conhece GET_VERSION (0x806)" -f $eV) -ForegroundColor Yellow
    Write-Host "       e' build ANTERIOR a 1.1. O PING respondeu, o resto pode nao bater." -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 2c) DEACTIVATE / SELFDESTRUCT (1.2) — a descarga
#
# Estes dois IOCTLs sao o que a 1.2 trouxe de funcional, e eles rodam AQUI, no
# fim, porque destroem o objeto de teste: depois do DEACTIVATE o driver nao
# responde mais, e depois do SELFDESTRUCT as paginas dele vao embora.
#
#   DEACTIVATE    revertevel: o proximo map-only carrega de novo, sem reboot.
#   SELFDESTRUCT  irrevertevel nesta sessao: libera as paginas do driver.
# ---------------------------------------------------------------------------
if ($Deactivate -or $SelfDestruct) {
    Write-Host ""

    $DEACT  = Ctl 0x807
    $DESTR  = Ctl 0x808
    $MAGIC  = [uint32]0x4B4D5344          # tem que bater com KM_DESTRUCT_MAGIC
    $UNMAP  = 2

    if ($SelfDestruct) {
        Write-Host "  [!!] SELFDESTRUCT: o driver vai liberar as paginas dele." -ForegroundColor Red
        Write-Host "       Numa VM com snapshot isso e' o teste final da descarga." -ForegroundColor Red
        Write-Host "       Numa maquina sem snapshot, use -Deactivate." -ForegroundColor Red
    }

    $reqD = New-Object byte[] 40
    if ($SelfDestruct) {
        [Array]::Copy([BitConverter]::GetBytes([int]$UNMAP),  0, $reqD, 0,  4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]$MAGIC), 0, $reqD, 16, 4)
        $code = $DESTR
        $nome = 'SELFDESTRUCT'
    } else {
        $code = $DEACT
        $nome = 'DEACTIVATE'
    }

    $retD = [uint32]0
    $okD  = [KmProbe]::DeviceIoControl($h, $code, $reqD, [uint32]40, $null, [uint32]0, [ref]$retD, [IntPtr]::Zero)
    if ($okD) {
        Write-Host ("  [ok] {0} aceito" -f $nome) -ForegroundColor Green
    } else {
        $eD = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Host ("  [err] {0} falhou  err={1}" -f $nome, $eD) -ForegroundColor Red
        if ($eD -eq 1 -or $eD -eq 87) {
            Write-Host "        -> o driver NAO conhece este IOCTL: e' anterior a 1.2." -ForegroundColor Yellow
        }
    }
    [void][KmProbe]::CloseHandle($h)

    # Depois do DEACTIVATE o symlink some: um CreateFile novo tem que falhar.
    Start-Sleep -Milliseconds 400
    $h2 = [KmProbe]::CreateFileA($devicePath, $GENERIC_RW, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
    if ($h2 -eq [IntPtr]::Zero -or $h2 -eq [IntPtr]::new(-1)) {
        Write-Host "  [ok] o device NAO abre mais -> fora do namespace" -ForegroundColor Green
    } else {
        [void][KmProbe]::CloseHandle($h2)
        Write-Host "  [!] o device AINDA abre. O deactivate nao tirou o symlink." -ForegroundColor Yellow
    }

    Write-Host ""
    Write-Host "  Para carregar de novo (sem reboot):" -ForegroundColor Cyan
    Write-Host "    mtmacr0.exe            (com PUBGEXT_MAP_ONLY=1)" -ForegroundColor Cyan
    Write-Host "    ou: tools\probe_driver.ps1 -MapOnly" -ForegroundColor Cyan
    Write-Host ""
    exit 0
}

# --- 3) MOUSE_MOVE: a cadeia toda ------------------------------------------
# DRIVER_REQUEST (40 bytes): pid(0) pad(4) address(8) buffer(16) size(24) name(32)
$req = New-Object byte[] 40
[Array]::Copy([BitConverter]::GetBytes([int64]$Dx), 0, $req, 8, 8)
[Array]::Copy([BitConverter]::GetBytes([int64]$Dy), 0, $req, 24, 8)

# DRIVER_REQUEST (40 bytes): pid(0) pad(4) address(8) buffer(16) size(24) name(32)
#   pid     = UnitId   (o callback roteia por este campo)
#   address = dx
#   buffer  = Flags    (0 = relativo, 3 = absoluto|virtual-desktop)
#   size    = dy
[Array]::Copy([BitConverter]::GetBytes([int]$UnitId),   0, $req, 0,  4)
[Array]::Copy([BitConverter]::GetBytes([int64]$Flags),  0, $req, 16, 8)

Write-Host ("  ... injetando MOUSE_MOVE UnitId={0} Flags={1} dx={2} dy={3}" -f $UnitId, $Flags, $Dx, $Dy)
$ret2 = [uint32]0
$ok2 = [KmProbe]::DeviceIoControl($h, $MOUSE, $req, [uint32]40, $null, [uint32]0, [ref]$ret2, [IntPtr]::Zero)
if ($ok2) {
    Write-Host "  [ok] MOUSE_MOVE aceito -> a cadeia de kernel funciona" -ForegroundColor Green
    Write-Host "       (o cursor deve ter andado -- se nao andou, veja o PkmSafe.txt)"
} else {
    $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host ("  [err] MOUSE_MOVE falhou  err={0}" -f $e) -ForegroundColor Red
    if ($e -eq 1 -or $e -eq 87) {
        Write-Host "        -> DRIVER ANTIGO no kernel." -ForegroundColor Red
        Write-Host "           O kdmapper nao descarrega: uma carga anterior ficou" -ForegroundColor Yellow
        Write-Host "           com o symlink e o Initialize() do cheat aceitou ela." -ForegroundColor Yellow
        Write-Host "           SOLUCAO: REINICIAR a maquina e abrir o cheat de novo." -ForegroundColor Yellow
    } else {
        Write-Host "        olhe '-- mouse:' no C:\Windows\PkmSafe.txt para o passo exato" -ForegroundColor Yellow
    }
}
[void][KmProbe]::CloseHandle($h)

Write-Host ""
Write-Host "=== log do driver ===" -ForegroundColor Cyan
# ---------------------------------------------------------------------------
# ATENCAO: o C:\Windows\PkmSafe.txt mencionado nas mensagens acima NAO EXISTE
# numa build normal. O log do kernel esta' DESLIGADO por padrao (KM_LOG_HABILITADO
# = 0 no km_safe_source.c), porque um arquivo texto em C:\Windows descrevendo o
# driver em portugues era o artefato mais facil de achar da lista inteira.
#
# O que existe e' o log do USER-MODE, que cobre o que interessa: se o driver
# respondeu, qual versao, e o estado do IOCTL de mouse.
# ---------------------------------------------------------------------------
$logCheat = Join-Path (Split-Path -Parent $PSScriptRoot) 'Build\Release\PubgExt\logs.txt'
if (Test-Path $logCheat) {
    $m = Select-String -Path $logCheat -Pattern 'driver|device|seed|mouse|MAP_ONLY' -ErrorAction SilentlyContinue
    if ($m) { $m | Select-Object -Last 12 | ForEach-Object { $_.Line } }
    else    { Write-Host "  (nada no logs.txt)" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host ("  log completo: {0}" -f $logCheat) -ForegroundColor DarkGray
} else {
    Write-Host ("  (logs.txt nao existe em {0})" -f $logCheat) -ForegroundColor Yellow
}
Write-Host ""
Write-Host "  Para o log do KERNEL (so' em investigacao, em VM):" -ForegroundColor DarkGray
Write-Host "    troque KM_LOG_HABILITADO para 1 no km_safe_source.c," -ForegroundColor DarkGray
Write-Host "    recompile, e APAGUE C:\Windows\PkmSafe.txt depois." -ForegroundColor DarkGray
