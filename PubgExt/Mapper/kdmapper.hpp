#pragma once

#include <Windows.h>

namespace kdmapper
{
	enum class AllocationMode
	{
		AllocatePool,
		AllocateIndependentPages
	};

	typedef bool (*mapCallback)(ULONG64* param1, ULONG64* param2, ULONG64 allocationPtr, ULONG64 allocationSize);

	//Note: if you set PassAllocationAddressAsFirstParam as true, param1 will be ignored
	// ---------------------------------------------------------------------------
// A ALOCACAO DE VERDADE, PARA QUEM VAI LIBERAR.
//
// `MapDriver` reduz `image_size` em SizeOfHeaders quando destroyHeader=true
// (imagem sem headers) e publica `realBase` como base. Quem chama o
// MmFreeIndependentPages depois PRECISA desses dois numeros exatos: a soma
// "SizeOfImage" e' MAIOR que o que foi alocado, e liberar pagina a mais
// corrompe o banco de PFNs — bugcheck PFN_LIST_CORRUPT (0x4E), medido em
// 20/10/2026.
//
// Devolver o que foi alocado, em vez de recalcular do PE do lado de fora,
// elimina a chance de os dois numeros divergirem de novo.
// ---------------------------------------------------------------------------
ULONG64 GetUltimaBaseAlocada();
ULONG   GetUltimoTamanhoAlocado();

ULONG64 MapDriver(BYTE* data, ULONG64 param1 = 0, ULONG64 param2 = 0, bool free = false, bool destroyHeader = true, AllocationMode mode = AllocationMode::AllocatePool, bool PassAllocationAddressAsFirstParam = false, mapCallback callback = nullptr, NTSTATUS* exitCode = nullptr);
}