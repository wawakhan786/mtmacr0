#pragma once
// ---------------------------------------------------------------------------
// km_boot_patch.h — a ponte entre o CLIENTE e o LOADER, sem registro.
//
// O cliente sabe os handles (secao, eventos) e o proprio Pid. O loader tem os
// BYTES do driver. Ninguem precisa de HKLM para os dois se encontrarem: o
// cliente preenche esta estrutura, e o loader grava esses valores dentro do
// campo `g_BootSlot` da imagem, antes de mapear.
//
// POR QUE UM SINGLETON EM FUNCAO E NAO UMA GLOBAL:
//   `inline int x;` em header exige C++17. Funcao com `static` local funciona em
//   qualquer padrao e continua havendo UMA instancia no programa inteiro.
// ---------------------------------------------------------------------------
#include <cstdint>

struct KmpBootPatch {
    bool     ativo = false;
    uint32_t pid   = 0;
    uint64_t sig   = 0;
    uint64_t hSec  = 0;
    uint64_t hReq  = 0;
    uint64_t hRsp  = 0;
};

inline KmpBootPatch& BootPatch()
{
    static KmpBootPatch p;
    return p;
}
