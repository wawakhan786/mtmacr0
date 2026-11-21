<#
    gen_driver_bytes.ps1
    -------------------------------------------------------------------------
    Regenera PubgExt\Mapper\driver_bytes.h a partir do PUBG_KM_safe.sys.

    POR QUE ISTO E' UM SCRIPT, E NAO UM COMANDO SOLTO

    O driver embutido e' o que o cheat mapeia em runtime. Se o .sys for
    recompilado e este header NAO for regenerado, o comportamento fica
    silenciosamente errado: voce compila o driver novo, o app builda, tudo
    "passa", e em runtime o cheat mapeia o driver VELHO. Nao da' erro em lugar
    nenhum — a unica pista seria o IOCTL novo devolvendo
    STATUS_INVALID_DEVICE_REQUEST.

    Foi exatamente o que quase aconteceu ao adicionar o IOCTL de mouse (0x805):
    o driver compilou com 18432 bytes, mas o header ainda tinha os 16896 do
    build anterior.

    Por isso isto e' script com verificacao no fim: ele CONFIRMA que o tamanho
    do array bate com o tamanho do arquivo.

    Uso:
        powershell -ExecutionPolicy Bypass -File tools\gen_driver_bytes.ps1
#>

[CmdletBinding()]
param(
    [string] $Sys  = '',
    [string] $Out  = '',
    [switch] $Check,          # so' verifica se o header esta' atualizado
    [switch] $Force           # embute mesmo sendo build ARMADA (assuma o risco)
)

$ErrorActionPreference = 'Stop'

if (-not $Sys) {
    $raiz = Split-Path -Parent $PSScriptRoot
    $Sys  = Join-Path $raiz 'Build\Release\PubgExt\PUBG_KM_safe.sys'
    $Out  = Join-Path $raiz 'PubgExt\Mapper\driver_bytes.h'
}

if (-not (Test-Path $Sys)) { throw "nao achei o .sys: $Sys" }
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Sys) 'driver_bytes.h' }

$bytes = [System.IO.File]::ReadAllBytes($Sys)

# ---------------------------------------------------------------------------
# O GUARD DE BUILD ARMADA SAIU DAQUI - e e' importante dizer por que.
#
# Ele recusava embutir um driver que contivesse a string KM_MOUSE_CALL_ARMED,
# escrita pelo bloco armado. Fazia sentido enquanto a chamada ao callback era
# EXPERIMENTO: o caminho natural era compilar armado pra VM, rodar este
# gerador e buildar o app - e o HOST passava a carregar o driver de teste.
#
# Duas coisas mudaram:
#   1. A injecao por kernel virou COMPORTAMENTO NORMAL do cheat.
#      KM_MOUSE_CALL_ENABLED agora e' 1 por padrao.
#   2. A string KM_MOUSE_CALL_ARMED foi removida do driver.
#
# Ou seja: o guard nao tinha mais como disparar. E um guard que NUNCA dispara
# e' pior que guard nenhum - quem le acredita que existe uma protecao ali.
#
# O que entra no lugar e' uma checagem que continua valendo de verdade: o .sys
# precisa ser MAIS NOVO que o fonte. Sem ela, editar km_safe_source.c e
# esquecer de rodar build_safe_driver.ps1 embute em silencio o driver antigo -
# e o .exe builda normal, com o header parecendo certo.
# ---------------------------------------------------------------------------
$fonteC = Join-Path $PSScriptRoot '..\km_safe_source.c'
if (Test-Path $fonteC) {
    $tSys   = (Get-Item $Sys).LastWriteTime
    $tFonte = (Get-Item $fonteC).LastWriteTime
    if ($tFonte -gt $tSys) {
        Write-Host ""
        Write-Host "  [RECUSADO] o .sys esta' MAIS VELHO que o fonte." -ForegroundColor Red
        Write-Host ("             fonte : {0}" -f $tFonte) -ForegroundColor Yellow
        Write-Host ("             .sys  : {0}" -f $tSys) -ForegroundColor Yellow
        Write-Host ""
        Write-Host "             Rode antes:" -ForegroundColor Cyan
        Write-Host "               tools\build_safe_driver.ps1" -ForegroundColor Cyan
        Write-Host ""
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Monta o array. StringBuilder em vez de concatenacao de string: com ~18 mil
# elementos, `$s += ...` seria quadratico e levaria minutos.
# ---------------------------------------------------------------------------
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('const unsigned char driver_bytes[] = {')
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($i -gt 0) { [void]$sb.Append(',') }
    [void]$sb.Append('0x')
    [void]$sb.Append($bytes[$i].ToString('X2'))
}
[void]$sb.Append('};')
$novo = $sb.ToString()

# ---------------------------------------------------------------------------
# -Check: compara o TAMANHO declarado no header atual com o do .sys.
# Nao compara byte a byte (seria lento e ruidoso); o tamanho ja' pega o caso
# que importa, que e' "esqueci de regenerar".
# ---------------------------------------------------------------------------
if ($Check) {
    if (-not (Test-Path $Out)) { Write-Host "  [err] header nao existe: $Out"; exit 1 }
    $atual = [System.IO.File]::ReadAllText($Out, [System.Text.Encoding]::UTF8)
    $n = ([regex]::Matches($atual, '0x[0-9A-Fa-f]{2}')).Count
    if ($n -eq $bytes.Length) {
        Write-Host ("  [ok] driver_bytes.h atualizado ({0} bytes)" -f $n) -ForegroundColor Green
        exit 0
    }
    Write-Host ("  [ERR] driver_bytes.h esta' com {0} bytes, mas o .sys tem {1}." -f $n, $bytes.Length) -ForegroundColor Red
    Write-Host "        Rode tools\gen_driver_bytes.ps1 e rebuilde o app." -ForegroundColor Red
    exit 1
}

$enc = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($Out, $novo, $enc)

# ---------------------------------------------------------------------------
# CONFERENCIA — o script prova o proprio trabalho.
# ---------------------------------------------------------------------------
$gravado = [System.IO.File]::ReadAllText($Out, [System.Text.Encoding]::UTF8)
$n = ([regex]::Matches($gravado, '0x[0-9A-Fa-f]{2}')).Count
Write-Host ""
Write-Host "  .sys        : $Sys"
Write-Host ("  tamanho     : {0} bytes" -f $bytes.Length)
Write-Host "  header      : $Out"
Write-Host ("  no header   : {0} bytes" -f $n)
if ($n -ne $bytes.Length) {
    Write-Host "  [ERR] o array no header NAO bate com o arquivo." -ForegroundColor Red
    exit 1
}
Write-Host "  [ok] driver_bytes.h regenerado" -ForegroundColor Green
