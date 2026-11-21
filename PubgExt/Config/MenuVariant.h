#pragma once
// ===========================================================================
// VARIANTE DO MENU ??? build "dev" (com diagnostico) x build "publico".
//
// MENU_DEBUG_TABS
//     1 = menu COMPLETO  -> mostra as abas DEBUG e LOGS.
//     0 = menu PUBLICO   -> as duas abas NAO existem: nem o botao na sidebar,
//                           nem o conteudo, nem o buffer de log vivo que
//                           alimentava a aba LOGS.
//
// ---------------------------------------------------------------------------
// POR QUE UM HEADER, E NAO UM `#define` NO .vcxproj
//
// Uma macro de projeto entra na linha de comando de TODOS os .cpp. Alternar
// entre as variantes invalidaria o projeto inteiro e custaria um rebuild
// completo (~60s) a cada troca.
//
// Este header e' incluido SO' pelo Main.cpp. Trocar o valor da linha abaixo
// recompila UM arquivo e re-linka ??? poucos segundos.
//
// ---------------------------------------------------------------------------
// QUEM TROCA ISTO
//
//   tools\build_all.ps1 -Variant dev      (default)  -> mtmacr0.exe
//   tools\build_all.ps1 -Variant public              -> mtmacr0_public.exe
//   tools\build_all.ps1 -Variant both                -> os dois
//
// O script reescreve a linha do define, builda e devolve o arquivo ao valor
// de `dev` no fim (mesmo se o build falhar). Editar a mao tambem funciona ???
// e' literalmente so' esta linha.
// ===========================================================================

#define MENU_DEBUG_TABS 1

// ---------------------------------------------------------------------------
// MENU_SO_ESP ? o build PUBLICO entrega SO' o ESP.
//
// AIMBOT e RCS ficam fora do binario publico:
//   - a aba AIMBOT nao aparece na sidebar;
//   - `Aimbot::Tick` e `RecoilControl` nao sao chamados na MemoryLoop;
//   - as duas flags sao forcadas para false no boot (config salvo nao religa).
//
// Derivado de MENU_DEBUG_TABS de proposito: o build_all.ps1 ja' reescreve
// aquela linha, entao nao existe uma SEGUNDA linha para alguem esquecer de
// trocar ? que e' como as variantes divergem.
// ---------------------------------------------------------------------------
#define MENU_SO_ESP (MENU_DEBUG_TABS == 0)
