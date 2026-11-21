# =============================================================================
# perf_ler.ps1 ? le as linhas de medicao da auditoria de desempenho
# =============================================================================
#
# POR QUE ELE FILTRA
#
# A primeira tentativa de medir saiu com um bug: o contador de frames nunca era
# incrementado, entao `render=0 fps` e todo numero "por frame" saiu zero. Essas
# linhas ficaram no log e NAO servem para nada.
#
# Este script descarta elas e mostra so' o que tem medida de verdade ? para
# ninguem tirar conclusao de linha morta.
#
# Uso:
#     $env:MTMACR0_PERF = "1"
#     .\Build\Release\PubgExt\mtmacr0.exe        (joga uma partida)
#     .\tools\perf_ler.ps1
# =============================================================================

$lg = Join-Path $env:LOCALAPPDATA 'RenderCache\logs.txt'

if (-not (Test-Path $lg)) {
    Write-Host ""
    Write-Host "logs.txt nao encontrado em $lg" -ForegroundColor Red
    exit 1
}

$todas = Select-String -Path $lg -Pattern '\[PERF\]' -EA SilentlyContinue

if (-not $todas) {
    Write-Host ""
    Write-Host "Nenhuma linha [PERF] no log." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "A variavel precisa estar no MESMO terminal que abre o exe:" -ForegroundColor Cyan
    Write-Host "    `$env:MTMACR0_PERF = '1'" -ForegroundColor Cyan
    Write-Host "    .\Build\Release\PubgExt\mtmacr0.exe" -ForegroundColor Cyan
    exit 0
}

# As linhas com medida real tem `render=` diferente de zero.
$boas = $todas | Where-Object { $_.Line -match '\[PERF\] render=' -and $_.Line -notmatch 'render=0 fps' }

if (-not $boas) {
    Write-Host ""
    Write-Host ("So' existem {0} linhas [PERF], e TODAS tem render=0." -f @($todas).Count) -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Isso e' a build ANTIGA (com o contador de frames quebrado)." -ForegroundColor Yellow
    Write-Host "Feche o mtmacr0, abra de novo pelo exe recem-compilado, e va' para" -ForegroundColor Cyan
    Write-Host "uma PARTIDA DE VERDADE -- no lobby, Actors=0 e nao mede nada." -ForegroundColor Cyan
    exit 0
}

Write-Host ""
Write-Host ("{0} amostras com medida real - mostrando as 24 ultimas:" -f @($boas).Count) -ForegroundColor Green
Write-Host ""
$boas | Select-Object -Last 24 | ForEach-Object { $_.Line }

Write-Host ""
Write-Host "-----------------------------------------------------------------" -ForegroundColor DarkGray
Write-Host "Cole o bloco acima na conversa." -ForegroundColor Cyan
Write-Host ""
Write-Host "O que olhar primeiro:" -ForegroundColor DarkGray
Write-Host "  render=      perto do refresh do seu monitor? (senao o contador falhou)" -ForegroundColor DarkGray
Write-Host "  cross-check  os dois contadores de frame batem?" -ForegroundColor DarkGray
Write-Host "  oclusoes     > 0 significa loop girando sem freio (P0 real)" -ForegroundColor DarkGray
Write-Host "  us cada      x IOCTL/s = quanto de um nucleo a memoria consome" -ForegroundColor DarkGray
