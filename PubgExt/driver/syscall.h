#pragma once
#include <windows.h>

// ===========================================================================
// syscall.h — stubs de syscall direta (user mode)
//
// Ver o bloco de comentario em syscall.asm para o motivo, o que foi removido na
// auditoria (`DoSyscall`) e por que os indices deixaram de ser immediate.
// ===========================================================================

// Indices usados pelos stubs do asm.
//
// NAO chame os stubs antes de `EnsureNtSyscallIndices()` (Engine.cpp) ter
// rodado, ou eles usam o fallback de build (0x18 / 0x1E, Win10 1803+ / Win11).
// Nos builds que nao sao esse, o fallback esta' errado — e' exatamente o
// problema que a resolucao em runtime conserta.
extern "C" unsigned int g_NtAllocateVirtualMemoryIndex;
extern "C" unsigned int g_NtFreeVirtualMemoryIndex;

extern "C" NTSTATUS SyscallNtAllocateVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect);

extern "C" NTSTATUS SyscallNtFreeVirtualMemory(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType);
