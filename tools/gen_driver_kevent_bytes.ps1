<#
    gen_driver_kevent_bytes.ps1
    -------------------------------------------------------------------------
    Regenera PubgExt\Mapper\driver_kevent_bytes.h a partir do
    SharedProto\out\km_ipc_proto.sys.

    POR QUE ISTO PRECISOU EXISTIR (medido em 20/10/2026)

    O `driver_bytes.h` (driver IOCTL) tinha gerador e o build chamava. O
    `driver_kevent_bytes.h` NAO tinha: era um blob colado a mao. Resultado
    medido no repo:

        km_ipc_driver.c  (fonte, editado)      -> compila 17.408 bytes
        driver_kevent_bytes.h (embutido)       -> 12.288 bytes de uma build ANTIGA

    Ou seja: dava para editar o driver de transporte, compilar o cheat, tudo
    "passar", e o .exe sair mapeando o driver VELHO. Mesma classe de bug que o
    comentario do build_all.ps1 descreve para o driver IOCTL — so' que aqui sem
    nenhuma rede de protecao.

    O QUE ESTE SCRIPT FAZ

      1. recusa embutir se o .sys estiver mais VELHO que o fonte (mesma regra
         do gen_driver_bytes.ps1: sem isso, editar e esquecer de compilar passa
         em silencio);
      2. converte o .sys em array C, no mesmo formato do outro header;
      3. CONFIRMA o tamanho do array contra o do arquivo antes de sair com 0.

    Uso:
        powershell -ExecutionPolicy Bypass -File tools\gen_driver_kevent_bytes.ps1
        powershell -ExecutionPolicy Bypass -File tools\gen_driver_kevent_bytes.ps1 -Check
#>

[CmdletBinding()]
param(
    [string] $Sys = '',
    [string] $Out = '',
    [switch] $Check
)

$ErrorActionPreference = 'Stop'

$raiz = Split-Path -Parent $PSScriptRoot

if (-not $Sys) {
    $Sys = Join-Path $raiz 'SharedProto\out\km_ipc_proto.sys'
    $Out = Join-Path $raiz 'PubgExt\Mapper\driver_kevent_bytes.h'
}
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Sys) 'driver_kevent_bytes.h' }

if (-not (Test-Path $Sys)) {
    Write-Host "  [err] nao achei o .sys do transporte KEVENT: $Sys" -ForegroundColor Red
    Write-Host "        rode antes: SharedProto\\build_driver.ps1" -ForegroundColor Yellow
    exit 1
}

# ---------------------------------------------------------------------------
# FRESCOR: o .sys tem que ser MAIS NOVO que o fonte do driver e que o
# protocolo. Sem esta checagem, "compilei o driver" e "compilei o app" viram
# duas coisas independentes — e o binario sai com o transporte errado.
# ---------------------------------------------------------------------------
$fontes = @(
    (Join-Path $raiz 'SharedProto\km_ipc_driver.c'),
    (Join-Path $raiz 'SharedProto\km_ipc_proto.h')
)
$tSys = (Get-Item $Sys).LastWriteTime
foreach ($f in $fontes) {
    if (-not (Test-Path $f)) { continue }
    if ((Get-Item $f).LastWriteTime -gt $tSys) {
        Write-Host ""
        Write-Host "  [RECUSADO] o .sys do KEVENT esta' MAIS VELHO que o fonte." -ForegroundColor Red
        Write-Host ("             fonte : {0}  ({1})" -f (Split-Path -Leaf $f), (Get-Item $f).LastWriteTime) -ForegroundColor Yellow
        Write-Host ("             .sys  : {0}  {1}" -f $Sys, $tSys) -ForegroundColor Yellow
        Write-Host ""
        Write-Host "             Rode antes:" -ForegroundColor Cyan
        Write-Host "               powershell -File SharedProto\\build_driver.ps1" -ForegroundColor Cyan
        Write-Host ""
        exit 1
    }
}

$bytes = [System.IO.File]::ReadAllBytes($Sys)

if ($Check) {
    if (-not (Test-Path $Out)) { Write-Host "  [err] header nao existe: $Out"; exit 1 }
    $atual = [System.IO.File]::ReadAllText($Out, [System.Text.Encoding]::UTF8)
    $n = ([regex]::Matches($atual, '0x[0-9A-Fa-f]{2}')).Count
    if ($n -eq $bytes.Length) {
        Write-Host ("  [ok] driver_kevent_bytes.h atualizado ({0} bytes)" -f $n) -ForegroundColor Green
        exit 0
    }
    Write-Host ("  [ERR] driver_kevent_bytes.h tem {0} bytes, o .sys tem {1}." -f $n, $bytes.Length) -ForegroundColor Red
    Write-Host "        Rode tools\\gen_driver_kevent_bytes.ps1 e rebuilde o app." -ForegroundColor Red
    exit 1
}

# StringBuilder: com ~17 mil elementos, `$s += ...` seria quadratico.
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append('const unsigned char driver_kevent_bytes[] = {')
for ($i = 0; $i -lt $bytes.Length; $i++) {
    if ($i -gt 0) { [void]$sb.Append(',') }
    [void]$sb.Append('0x')
    [void]$sb.Append($bytes[$i].ToString('X2'))
}
[void]$sb.Append('};')
$novo = $sb.ToString()

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
Write-Host "  [ok] driver_kevent_bytes.h regenerado" -ForegroundColor Green
