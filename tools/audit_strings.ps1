# ===========================================================================
# audit_strings.ps1 — mede o que o NOSSO binario entrega em texto claro.
#
# ---------------------------------------------------------------------------
# POR QUE ISTO EXISTE
#
# "Esta ofuscado" nao e' uma opiniao, e' uma MEDICAO. A primeira versao do
# MakeClassName() neste projeto tinha a intencao certa (montar o nome da janela
# em runtime) e o resultado errado: as dez strings de prefixo apareciam em
# texto claro no .rdata, porque um array de `const char*` com literais vai pro
# .rdata junto com os literais. A intencao nao aparecia em lugar nenhum.
#
# Foi este script que mostrou isso. Rode depois de qualquer mudanca que mexa em
# nome de device, classe de janela, chave de registry ou mensagem de log.
#
# ---------------------------------------------------------------------------
# O QUE ELE MEDE
#
# Le' o .exe como bytes, decodifica como ASCII E como UTF-16, e procura um
# conjunto de strings que identificam o binario. Zero ocorrencia = limpo.
#
# O QUE ELE NAO MEDE
#
#   - NAO mede o que aparece na HEAP em runtime (o LIT() materializa o texto no
#     stack e o std::string copia — com o processo rodando, `strings` na memoria
#     acha). Isto mede o ARQUIVO EM REPOUSO, que e' o que um scan de disco ve';
#   - NAO mede assinatura de codigo (bytes de funcao);
#   - NAO mede imports da IAT;
#   - NAO mede o PDB path do driver doador EMBUTIDO — veja a secao 4.
#
# Uso:
#     powershell -ExecutionPolicy Bypass -File tools\audit_strings.ps1
#     powershell -ExecutionPolicy Bypass -File tools\audit_strings.ps1 -Exe caminho.exe
# ===========================================================================
[CmdletBinding()]
param(
    [string] $Exe = ''
)

$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $raiz 'Build\Release\PubgExt\mtmacr0.exe' }

if (-not (Test-Path $Exe)) { throw "nao achei o .exe: $Exe" }

$bytes  = [System.IO.File]::ReadAllBytes($Exe)
$ascii  = [System.Text.Encoding]::ASCII.GetString($bytes)
$wide   = [System.Text.Encoding]::Unicode.GetString($bytes)

# ---------------------------------------------------------------------------
# A FRONTEIRA DO DRIVER DOADOR EMBUTIDO.
#
# O iqvw64e.sys vai inteiro dentro de intel_driver_resource.hpp, como array de
# bytes. Dentro dele ha' um PDB path de build que carrega o nome — e ISSO NAO
# TEM CONSERTO: os bytes do driver SAO o payload que o NtLoadDriver carrega.
# Cifra-los quebraria o carregamento.
#
# Sem esta fronteira, o auditor acusa o mesmo "iqvw64e" duas vezes por motivos
# diferentes (string de codigo vs. byte de payload), e um relatorio que mistura
# as duas coisas ensina a ignorar o alerta.
#
# Como achar a fronteira: a assinatura MZ do driver embutido. Pega a ULTIMA
# ocorrencia de "MZ" seguida de um PE valido — o .exe tem a sua no offset 0, e
# a do driver vem depois.
# ---------------------------------------------------------------------------
$inicioDoador = $bytes.Length
for ($i = $bytes.Length - 2; $i -gt 0x1000; $i--) {
    if ($bytes[$i] -ne 0x4D -or $bytes[$i + 1] -ne 0x5A) { continue }
    # e_lfanew em +0x3C
    $lf = [BitConverter]::ToInt32($bytes, $i + 0x3C)
    if ($lf -le 0 -or ($i + $lf + 4) -ge $bytes.Length) { continue }
    if ([BitConverter]::ToUInt32($bytes, $i + $lf) -ne 0x00004550) { continue }   # "PE\0\0"
    $inicioDoador = $i
    break
}

Write-Host ""
Write-Host "  auditando: $Exe" -ForegroundColor Cyan
Write-Host ("  tamanho  : {0:N0} bytes" -f $bytes.Length) -ForegroundColor DarkGray
if ($inicioDoador -lt $bytes.Length) {
    Write-Host ("  doador   : MZ+PE em 0x{0:X} - abaixo disso e' o NOSSO codigo" -f $inicioDoador) -ForegroundColor DarkGray
} else {
    Write-Host "  doador   : nao encontrei MZ+PE embutido (auditoria cobre tudo)" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# O conjunto. Cada linha e' uma coisa que NAO pode estar em claro, e o motivo.
#
# Adicionar aqui e' barato; o custo e' lembrar de rodar depois de mexer.
# ---------------------------------------------------------------------------
$alvos = @(
    # --- alvo do cheat: diz CONTRA QUEM o binario existe ---
    @{ s = 'BEDaisy';                     porque = 'BattlEye (modulo de kernel)' }
    @{ s = 'Zakynthos';                   porque = 'anti-cheat interno do PUBG' }
    @{ s = 'BEService';                   porque = 'servico do BattlEye' }
    @{ s = 'bekernel';                    porque = 'BattlEye (variante antiga)' }
    @{ s = 'xenuine';                     porque = 'Xenuine' }
    @{ s = 'easyanti';                    porque = 'EasyAntiCheat' }

    # --- como entra no kernel: o vestigio mais autoincriminador ---
    @{ s = 'kdmapper';                    porque = 'a tecnica de mapeamento' }
    @{ s = 'iqvw64e';                     porque = 'o driver doador (BYOVD)' }

    # --- o spoof de janela: o vocabulario do disfarce ---
    @{ s = 'CoreWindow';                  porque = 'prefixo de classe de janela' }
    @{ s = 'Shell_TrayWnd';               porque = 'prefixo de classe de janela' }
    @{ s = 'ApplicationFrameWindow';      porque = 'prefixo de classe de janela' }
    @{ s = 'TaskManagerWindow';           porque = 'prefixo de classe de janela' }
    @{ s = 'XamlExplorerHostIsland';      porque = 'prefixo de classe de janela' }
    @{ s = 'DesktopChildSiteBridge';      porque = 'prefixo de classe de janela' }
    @{ s = 'Windows Shell Experience';    porque = 'titulo de janela' }
    @{ s = 'Microsoft Text Input';        porque = 'titulo de janela' }
    @{ s = 'Desktop Window Manager';      porque = 'titulo de janela' }

    # --- artefato em disco ---
    @{ s = 'RenderCache';                 porque = 'pasta de artefatos em %LOCALAPPDATA%' }

    # --- nome de API sensivel, resolvida por hash (1.3) ---
    #
    # Este NAO e' um identificador de anti-cheat: e' o nome de uma API que
    # denuncia overlay. `SetWindowDisplayAffinity` existe para dizer ao DWM "nao
    # me capture" — e' usada por OBS e por overlay de cheat.
    #
    # Ele tem DOIS lugares de onde some, e os dois importam:
    #   1. a IAT (dumpbin /imports) — resolvido por hash, ver ImportHash.h;
    #   2. o .rdata — o texto do log que a citava, cifrado com LIT().
    #
    # A licao: remover a CHAMADA nao remove a STRING. Foi preciso fazer os dois.
    @{ s = 'SetWindowDisplayAffinity';    porque = 'API que denuncia overlay (streamproof)' }
)

Write-Host ""
# Os parenteses em volta da string de formato NAO sao estilo: sem eles, o -f e
# o -ForegroundColor ficam no mesmo nivel de parsing e o PowerShell tenta ligar
# o TEXTO do formato ao parametro de cor ("nao e' possivel converter
# 'STRING,ACHOU,POR QUE...' para System.ConsoleColor"). O -f tem que ser
# resolvido ANTES do binding do parametro.
Write-Host ("  {0,-26} {1,-6} {2}" -f 'STRING', 'ACHOU', 'POR QUE NAO PODE ESTAR EM CLARO') -ForegroundColor DarkGray
Write-Host ("  {0,-26} {1,-6} {2}" -f ('-' * 26), ('-' * 6), ('-' * 40)) -ForegroundColor DarkGray

$limpos = 0
$sujos  = 0
$achados = @()
$soNoDoador = @()

foreach ($a in $alvos) {
    # Conta SO' na parte que e' o NOSSO codigo — antes do driver embutido.
    $nA = ([regex]::Matches($ascii.Substring(0, $inicioDoador), [regex]::Escape($a.s))).Count
    $fimWide = [Math]::Min($wide.Length, $inicioDoador)
    $nW = ([regex]::Matches($wide.Substring(0, $fimWide), [regex]::Escape($a.s))).Count
    $n  = $nA + $nW

    # Quanto aparece APENAS no payload do doador — o relatorio separa os dois
    # casos, porque um tem conserto e o outro nao.
    $nDoador = ([regex]::Matches($ascii, [regex]::Escape($a.s))).Count - $nA

    if ($n -eq 0) {
        $limpos++
        if ($nDoador -gt 0) { $soNoDoador += $a.s }
        Write-Host ("  {0,-26} {1,-6} {2}" -f $a.s, '-', $a.porque) -ForegroundColor DarkGray
    } else {
        $sujos++
        $achados += $a.s
        Write-Host ("  {0,-26} {1,-6} {2}" -f $a.s, $n, $a.porque) -ForegroundColor Red
    }
}

Write-Host ""
if ($sujos -eq 0) {
    Write-Host ("  [ok] {0}/{0} limpos. Nenhum identificador em texto claro." -f $limpos) -ForegroundColor Green
} else {
    Write-Host ("  [{0} sujo(s)] {1} limpos de {2}. Envolva os que sobraram em LIT()" -f $sujos, $limpos, $alvos.Count) -ForegroundColor Red
    Write-Host "               (ver PubgExt\Security\XorStr.h) e rode de novo." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Note: um literal em COMENTARIO nao entra no binario. Se ainda" -ForegroundColor DarkGray
    Write-Host "        aparece, e' string de verdade em algum .cpp/.h." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# O PDB path do driver doador embutido — este NAO tem conserto.
#
# Os bytes do iqvw64e.sys estao em intel_driver_resource.hpp, e SAO o payload
# que o NtLoadDriver carrega: nao da' para cifrar sem quebrar o carregamento.
# Dentro desses bytes vai o caminho do PDB de build do driver, que carrega o
# nome. E' limitacao estrutural da tecnica, nao bug.
# ---------------------------------------------------------------------------
$nPdb = ([regex]::Matches($ascii, 'iqvw64e\.pdb')).Count
if ($nPdb -gt 0) {
    Write-Host ""
    Write-Host ("  [--] {0} ocorrencia(s) de iqvw64e.pdb - PDB path DENTRO dos" -f $nPdb) -ForegroundColor Yellow
    Write-Host "       bytes do driver doador embutido (intel_driver_resource.hpp)." -ForegroundColor Yellow
    Write-Host "       Nao tem conserto por cifra: os bytes SAO o payload carregado." -ForegroundColor Yellow
}

if ($soNoDoador.Count -gt 0) {
    Write-Host ""
    Write-Host ("  [i] {0} alvo(s) so' aparecem no PAYLOAD do doador: {1}" -f $soNoDoador.Count, ($soNoDoador -join ', ')) -ForegroundColor DarkGray
    Write-Host "      Contam como limpos: nao estao no nosso codigo." -ForegroundColor DarkGray
}

Write-Host ""
exit $(if ($sujos -eq 0) { 0 } else { 1 })
