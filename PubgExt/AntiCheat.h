#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ===========================================================================
// AntiCheat — deteccao de BattlEye / Zakynthos / Xenuine ANTES de mapear.
//
// ---------------------------------------------------------------------------
// O QUE ESTE ARQUIVO NAO E'
//
// Nao e' um detector de anti-cheat de verdade. E' uma checagem de PRESENCA, com
// quatro fontes que erram de formas diferentes. Ela existe para responder uma
// pergunta so':
//
//     "vale a pena mapear o driver AGORA?"
//
// E a resposta honesta e' heuristica: se ela diz LIMPO, o anti-cheat pode
// estar ativo de um jeito que ela nao pega. Se ela diz ATIVO, quase sempre
// esta' — falso positivo custa uma espera, falso negativo custa o mapeamento
// exposto.
//
// ---------------------------------------------------------------------------
// POR QUE QUATRO FONTES, E NAO SO' O PROCESSO
//
// A versao anterior (Main.cpp, ChecarBattlEye) olhava duas coisas: o processo
// BEService.exe na lista de processos e o device \Device\BEDaisy abrindo.
//
// As duas sao fracas:
//
//   - O NOME DO PROCESSO muda entre builds do BE (BEService.exe,
//     BEService_x64.exe, BEService_pubg.exe). Medido nesta maquina: os dois
//     executaveis em disco sao BEService.exe e BEService_pubg.exe.
//
//   - O DEVICE QUASE NUNCA ABRE. O BEDaisy fecha o device para user-mode de
//     proposito; um CreateFile devolvendo ERROR_ACCESS_DENIED nao distingue
//     "nao existe" de "existe e me barrou". Falso negativo garantido.
//
// O que realmente decide e' o MODULO DE KERNEL carregado: enquanto o BEDaisy
// esta' na lista de modulos, o anti-cheat esta' ATIVO, independente de nome de
// processo, de device, de servico parado ou de como ele se chama no disco.
//
// ---------------------------------------------------------------------------
// ORDEM DAS FONTES, DA MAIS FORTE PARA A MAIS FRACA
//
//   1. MODULO DE KERNEL (NtQuerySystemInformation/SystemModuleInformation)
//      Prova que o driver esta' carregado AGORA. E' a unica que responde a
//      pergunta de verdade.
//
//   2. CHAVE DE SERVICO (C:\Windows\System32\drivers\BEDaisy.sys no registry)
//      So' prova que o anti-cheat esta' INSTALADO — ele pode estar parado.
//      Nesta maquina, BEDaisy.sys NAO tem chave propria: quem carrega e' o
//      BEService.exe em runtime, e o arquivo e' apagado depois.
//
//   3. PROCESSO (Toolhelp32)
//      Prova que o servico subiu. O servico roda ANTES do driver na maioria
//      dos casos, entao isto e' um AVISO ANTECIPADO: se aparece, o modulo
//      costuma vir em seguida.
//
//   4. DEVICE (\\.\BEDaisy)
//      O mais fraco dos quatro, pelo motivo explicado acima. Fica como
//      confirmacao, nunca como decisao.
// ===========================================================================

namespace AntiCheat {

// O que foi encontrado. Cada campo e' uma fonte independente.
struct Estado {
    bool moduloKernel  = false;   // mais forte: driver carregado agora
    bool servico       = false;   // instalado (pode estar parado)
    bool processo      = false;   // servico rodando
    bool device        = false;   // device respondeu (raro)

    // Nomes exatos do que foi encontrado, para o log dizer O QUE bateu em vez
    // de so' "tem anti-cheat".
    std::vector<std::string> modulosBateram;
    std::vector<std::string> processosBateram;
    std::vector<std::string> servicosBateram;

    // true se ha' QUALQUER sinal. Para decidir carga, use `Ativo()`, que
    // pondera as fontes.
    bool AlgumSinal() const {
        return moduloKernel || servico || processo || device;
    }

    // A decisao que importa: o anti-cheat pode estar operando AGORA?
    //
    // Modulo de kernel -> sim, sem discussao.
    // Processo          -> sim (o servico pode carregar o modulo a qualquer
    //                      instante; mapear nesta janela e' apostar corrida).
    // Servico instalado -> NAO sozinho. BEService existe em toda maquina que
    //                      ja' jogou PUBG, mesmo com o jogo fechado. Se isto
    //                      bloqueasse, o driver nunca mapearia.
    // Device            -> sim, e' sinal de driver vivo.
    bool Ativo() const {
        return moduloKernel || processo || device;
    }

    // Texto curto para o log: "BEDaisy.sys (modulo), BEService.exe (processo)".
    std::string Resumo() const;
};

// Faz a varredura completa.
Estado Checar();

// ---------------------------------------------------------------------------
// ChecarComRelatorio — a MESMA varredura do `Checar()`, escrevendo no log o que
// ela viu, passo a passo. E' o que o botao MAP NOW do menu chama.
//
// POR QUE ISTO EXISTE
//
// O gate bloqueou acusando `modulo: BEDaisy.sys` com o jogo fechado. Ficaram
// DUAS hipoteses e nao dava' para decidir lendo o codigo:
//
//   (a) o BEDaisy.sys estava carregado de verdade;
//   (b) o matcher tem falso positivo.
//
// A (b) tinha um suspeito concreto: o padrao `"zak"` — TRES letras, casando por
// SUBSTRING num alvo que e' o CAMINHO COMPLETO do modulo (`FullPathName`), nao
// o nome do arquivo. Tres letras casam em qualquer coisa. (Ja' removido.)
//
// Este relatorio responde com DADO em vez de discussao.
//
// IMPRIME: quantos modulos foram lidos (0 = a leitura falhou), cada modulo que
// casou com o padrao que casou, e o veredito por fonte.
// ---------------------------------------------------------------------------
Estado ChecarComRelatorio();

// Modulos de kernel cujo nome indica anti-cheat. Comparados por SUBSTRING,
// case-insensitive, sobre o nome do modulo (ex: "BEDaisy.sys").
const std::vector<std::string>& PadroesAntiCheat();

// Exposto para diagnostico e para a VM: devolve TODOS os modulos de kernel
// carregados. Nao serve so' pra anti-cheat — serve pra ver o que esta' na
// maquina, incluindo um driver mapeado por nos numa execucao anterior.
std::vector<std::string> ModulosDeKernel();

} // namespace AntiCheat
