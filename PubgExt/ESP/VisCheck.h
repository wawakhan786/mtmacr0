#pragma once
#include <cmath>
#include <cstdint>

// ===========================================================================
// VEREDITO DE VISIBILIDADE — `mesh + 0x75C` (LastRenderTimeOnScreen)
//
// A REGRA, EM UM LUGAR SO': o Engine (SDK/Engine.cpp) chama esta funcao e o
// teste (tools/config_check.cpp) chama a MESMA funcao.
//
// ===========================================================================
// POR QUE A REGRA MUDOU (18/10/2026, depois de DUAS tentativas falhas)
//
// Versao 1: `atraso = agora - campo`, com `agora` = TimeSeconds do GameState.
//   Falhou quando o GameState nao estava publicado (lobby, logo apos mapear):
//   virava fail-open e TODO MUNDO ficava verde.
//
// Versao 2: mesma conta, com `agora` = o campo do PAWN LOCAL.
//   Falhou em FPP: o corpo do local nao e' renderizado, o campo dele vale
//   -1000, e comparar contra -1000 da' atraso NEGATIVO gigante -> todo mundo
//   "visivel" de novo.
//
// O ERRO COMUM AS DUAS: depender de uma base de tempo. O campo nao precisa de
// base nenhuma — ele mesmo carrega a informacao, porque so' AVANCA quando o
// pawn e' desenhado:
//
//     o valor mudou nos ultimos N ms?   -> o motor desenhou agora   -> VISIVEL
//     ficou parado mais que N ms?       -> nao esta' sendo desenhado -> OCLUIDO
//
// Nada externo entra na conta. Nao existe fuso, nao existe escala, nao existe
// "e se o GameState ainda nao veio".
//
// ---------------------------------------------------------------------------
// O QUE AINDA PODE DAR ERRADO (e o que a tela mostra quando da')
// ---------------------------------------------------------------------------
//   - o campo nao mudar NUNCA, nem com o inimigo na frente: entao o 0x75C
//     nao e' o componente renderizado nesta build. A tela mostra "parado ha
//     XXXX ms" subindo sem parar -> desligue o VisCheck e me diga esse numero.
//   - o campo mudar mesmo atras de parede: nao e' oclusao (seria submit time).
//     A tela mostra "parado ha 3 ms" com o cara atras do muro.
//
// `toleranciaMs` e' a janela de silencio que ainda conta como "desenhando".
// Faixa util entre 100 e 250 ms: abaixo de ~100 o rotulo pisca (o campo pode
// atualizar a cada 2-3 frames conforme a occlusion query responde), acima de
// ~400 demora a acusar oculto.
// ===========================================================================
namespace VisCheck {

// `msDesdeMudanca` = ha' quanto tempo o campo esta' parado.
inline bool VereditoRecente(uint64_t msDesdeMudanca, float toleranciaMs)
{
	if (!std::isfinite(toleranciaMs) || toleranciaMs <= 0.0f) return true;  // fail-open
	return (float)msDesdeMudanca <= toleranciaMs;
}

} // namespace VisCheck
