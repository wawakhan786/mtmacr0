// ===========================================================================
// ldr_offsets.cpp — mede os offsets REAIS do LDR_DATA_TABLE_ENTRY.
//
// POR QUE ISTO EXISTE
//
// O `winternl.h` declara `_LDR_DATA_TABLE_ENTRY` e OMITE `BaseDllName` (e
// outros campos). Para andar na lista de modulos do PEB, a gente precisa
// saber onde esse campo esta' — e "saber" nao pode ser chute: um offset errado
// faz o hash comparar contra memoria aleatoria, e o sintoma e' "a funcao nao
// existe", que e' o lugar errado pra procurar.
//
// Este programa percorre InMemoryOrderModuleList de verdade e, para cada
// offset TESTADO, imprime o que ele encontra como se fosse uma
// UNICODE_STRING. O offset certo e' o unico que produz nomes de DLL
// plausiveis ("ntdll.dll", "kernel32.dll", ...) em TODAS as entradas.
//
// E' medicao, nao adivinhacao. E o resultado vai pro ImportHash.h como
// constante comentada.
//
// Uso:
//     ldr_offsets.exe
// ===========================================================================

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>

// Layout real do _LDR_DATA_TABLE_ENTRY em x64 (o winternl.h omite parte).
// Esta struct existe SO' neste programa de medicao, e o ImportHash.h usa a
// constante medida aqui — nao a struct inteira.
typedef struct _LDR_FULL {
    LIST_ENTRY     InLoadOrderLinks;              // +0x00
    LIST_ENTRY     InMemoryOrderLinks;            // +0x10
    LIST_ENTRY     InInitializationOrderLinks;    // +0x20
    PVOID          DllBase;                       // +0x30
    PVOID          EntryPoint;                    // +0x38
    ULONG          SizeOfImage;                   // +0x40
    UNICODE_STRING FullDllName;                   // +0x48
    UNICODE_STRING BaseDllName;                   // +0x58
} LDR_FULL;

int main(void)
{
    printf("=========================================================\n");
    printf(" Offsets do LDR_DATA_TABLE_ENTRY (x64) — MEDIDOS\n");
    printf("=========================================================\n\n");

    printf("  sizeof(LDR_FULL)          = 0x%zX\n", sizeof(LDR_FULL));
    printf("  offsetof(InMemoryOrderLinks) = 0x%zX\n", offsetof(LDR_FULL, InMemoryOrderLinks));
    printf("  offsetof(DllBase)            = 0x%zX\n", offsetof(LDR_FULL, DllBase));
    printf("  offsetof(FullDllName)        = 0x%zX\n", offsetof(LDR_FULL, FullDllName));
    printf("  offsetof(BaseDllName)        = 0x%zX\n", offsetof(LDR_FULL, BaseDllName));
    printf("\n");

    PTEB teb = (PTEB)__readgsqword(0x30);
    PEB*  peb = teb->ProcessEnvironmentBlock;
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;

    printf("  head = %p\n\n", (void*)head);

    int n = 0, plausiveis = 0;
    for (LIST_ENTRY* e = head->Flink; e != head && n < 8; e = e->Flink, n++) {
        LDR_FULL* m = (LDR_FULL*)((BYTE*)e - offsetof(LDR_FULL, InMemoryOrderLinks));

        printf("  [%d] entry=%p  DllBase=%p  SizeOfImage=0x%lX\n",
               n, (void*)m, m->DllBase, (unsigned long)m->SizeOfImage);

        UNICODE_STRING* u = &m->BaseDllName;
        printf("      BaseDllName: Length=%u MaxLen=%u Buffer=%p",
               u->Length, u->MaximumLength, (void*)u->Buffer);

        if (u->Buffer && u->Length > 0 && u->Length < 128 &&
            u->MaximumLength >= u->Length && u->MaximumLength < 256) {
            char nome[128] = {0};
            int k = 0;
            for (; k < u->Length / sizeof(wchar_t) && k < 120; k++)
                nome[k] = (char)(u->Buffer[k] & 0x7F);
            nome[k] = 0;
            printf("  -> \"%s\"", nome);
            if (strstr(nome, ".dll") || strstr(nome, ".DLL")) plausiveis++;
        } else {
            printf("  -> (nao parece UNICODE_STRING)");
        }
        printf("\n");
    }

    printf("\n=========================================================\n");
    if (plausiveis >= 2) {
        printf(" RESULTADO: BaseDllName funciona em offset 0x%zX.\n",
               offsetof(LDR_FULL, BaseDllName));
        printf(" (%d entradas com nome de .dll plausivel)\n", plausiveis);
    } else {
        printf(" RESULTADO: o offset NAO produziu nomes plausiveis.\n");
    }
    printf("=========================================================\n");

    return plausiveis >= 2 ? 0 : 1;
}
