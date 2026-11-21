; ===========================================================================
; syscall.asm — stubs de syscall direta (USER MODE)
;
; POR QUE ISTO EXISTE: chamar ntdll!Nt* pelo caminho normal passa pelo hook que
; qualquer coisa instalou no ntdll. A syscall vai direto.
;
; ---------------------------------------------------------------------------
; O QUE MUDOU (auditoria)
;
; 1. `DoSyscall` FOI REMOVIDO. Estava morto (nenhum chamador) e, pior, errado:
;    recebia o indice em `ecx` e fazia
;
;        mov r10, rcx     ; r10 = INDICE
;        mov eax, ecx     ; eax = INDICE
;        mov rcx, rdx     ; so' entao rcx = 1o argumento
;
;    Em syscall x64 o `r10` tem que levar o PRIMEIRO ARGUMENTO (o `syscall`
;    grava o endereco de retorno em rcx, que e' onde o 1o argumento estava).
;    Ali ele levava o indice. Qualquer uso com argumento chamaria a funcao
;    certa com lixo no 1o parametro. Nao explodia porque ninguem chamava — e' o
;    tipo de coisa que alguem religa seis meses depois.
;
;    (O relatorio da auditoria externa tambem citou `SyscallNtFreeVirtualMemory`
;    como morto. NAO E': ele e' chamado no destrutor de Engine, Engine.cpp:98.
;    Mantido, com a mesma correcao de indice.)
;
; 2. O INDICE DEIXOU DE SER IMMEDIATE. Era `mov eax, 18h` / `mov eax, 1Eh`
;    fixos. Esses sao os indices nas builds Win10 1803+ .. Win11; em Win10
;    anterior o mesmo servico tem OUTRO numero, e o stub chamaria uma funcao
;    qualquer do SSDT com a assinatura errada. O sintoma nao seria "erro":
;    seria comportamento aleatorio ou bugcheck.
;
;    Agora cada stub le' o indice de uma variavel, preenchida em runtime pelo
;    `EnsureNtSyscallIndices()` (Engine.cpp) a partir do proprio stub do
;    ntdll — le' o imm32 do `mov eax, imm32` que a Microsoft emitiu naquela
;    build. Se a leitura falhar (stub desconhecido), os valores de build
;    continuam valendo, que e' o comportamento antigo.
;
;    Zero API nova, zero offset de estrutura: so' leitura do codigo do ntdll
;    que ja' esta' carregado no processo.
; ===========================================================================

.data

; Fallbacks = Win10 1803+ / Win11. Sobrescritos por EnsureNtSyscallIndices().
PUBLIC g_NtAllocateVirtualMemoryIndex
g_NtAllocateVirtualMemoryIndex dd 18h

PUBLIC g_NtFreeVirtualMemoryIndex
g_NtFreeVirtualMemoryIndex     dd 1Eh

.code

; NTSTATUS SyscallNtAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T,
;                                         ULONG AllocationType, ULONG Protect)
;
; O corpo NAO mexe nos argumentos de proposito: rcx/rdx/r8/r9 ja' chegam nos
; lugares certos e o 5o/6o (AllocationType/Protect) ja' estao no home space do
; chamador, que e' exatamente onde o kernel os espera. So' duas coisas sao
; necessarias: o `r10 = rcx` (convencao do `syscall`) e o `eax = indice`.
SyscallNtAllocateVirtualMemory PROC
    mov  r10, rcx
    mov  eax, DWORD PTR [g_NtAllocateVirtualMemoryIndex]
    syscall
    ret
SyscallNtAllocateVirtualMemory ENDP

; NTSTATUS SyscallNtFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG FreeType)
SyscallNtFreeVirtualMemory PROC
    mov  r10, rcx
    mov  eax, DWORD PTR [g_NtFreeVirtualMemoryIndex]
    syscall
    ret
SyscallNtFreeVirtualMemory ENDP

END
