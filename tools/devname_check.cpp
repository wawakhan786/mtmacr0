// ===========================================================================
// devname_check.cpp — prova que as derivacoes de nome CONCORDAM.
//
// POR QUE ISTO EXISTE
//
// A partir da 1.2 o nome do device nao e' mais um literal: ele sai de um seed,
// por uma mistura implementada em TRES lugares:
//
//   1. km_safe_source.c        (KmMixSeed + KmHex16)  -> \Driver\ e \DosDevices\
//   2. PubgExt\driver\driver_interface_v3.cpp          -> \\.\<hex>
//   3. tools\DeviceName.ps1                            -> \\.\<hex> (testes)
//
// Se (1) e (2) divergirem, o CreateFile do cheat nao acha o device e o sintoma
// e' "o driver nao carregou". Se (2) e (3) divergirem, o probe e o harness nao
// acham o device e o sintoma e' "o driver nao esta' carregado" — de novo, o
// lugar errado pra procurar um bug de nome.
//
// Este programa imprime o que a implementacao (2) produz. Comparar com a saida
// do Get-KmDeviceName do DeviceName.ps1 para os mesmos seeds e' a verificacao.
//
// Uso:
//     devname_check.exe seeds.txt     (uma seed hex por linha, '#' comenta)
//     devname_check.exe 0123456789abcdef
// ===========================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static const uint64_t KM_NAME_MIX = 0x9E3779B97F4A7C15ULL;

static uint64_t KmMixSeedCpp(uint64_t seed, uint32_t kind)
{
    uint64_t k = seed ^ (KM_NAME_MIX * (uint64_t)(kind + 1));
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

// Copia exata de DeriveDeviceName (driver_interface_v3.cpp), sem o std::string.
static void DeriveDeviceName(uint64_t seed, char* out, size_t n)
{
    static const char hexd[] = "0123456789abcdef";
    uint64_t v = KmMixSeedCpp(seed, 1);
    size_t p = 0;

    const char* pre = "\\\\.\\";
    for (const char* s = pre; *s && p + 1 < n; s++) out[p++] = *s;

    for (int i = 15; i >= 0 && p + 1 < n; i--)
        out[p++] = hexd[(v >> (i * 4)) & 0xF];

    out[p] = 0;
}

static void Emitir(const char* hex)
{
    uint64_t seed = strtoull(hex, nullptr, 16);
    char nome[64];
    DeriveDeviceName(seed, nome, sizeof(nome));
    printf("%016llx %s\n", (unsigned long long)seed, nome);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("uso: devname_check.exe <seeds.txt | seed-hex>\n");
        return 1;
    }

    // Um argumento que existe como arquivo = lista. Senao, trata como seed.
    FILE* f = fopen(argv[1], "r");
    if (f) {
        char linha[128];
        while (fgets(linha, sizeof(linha), f)) {
            char* p = linha;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;

            char* fim = p;
            while (*fim && *fim != '\n' && *fim != '\r' && *fim != ' ') fim++;
            *fim = 0;
            Emitir(p);
        }
        fclose(f);
        return 0;
    }

    for (int i = 1; i < argc; i++) Emitir(argv[i]);
    return 0;
}
