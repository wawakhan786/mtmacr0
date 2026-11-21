#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

// ---------------------------------------------------------------------------
// kdmLog — o log do kdmapper.
//
// POR QUE ISSO VIROU UM SINK (auditoria de 2026)
//
// Era `std::wcout << content`. O `mtmacr0.exe` e' um app de JANELA: nao tem
// console, e o console de diagnostico so' existe com PUBGEXT_CONSOLE=1. Ou
// seja: TODA a instrumentacao do mapper — "Failed to load the vulnerable
// driver", "Can't exploit intel driver, is there any antivirus...", o status
// exato de cada etapa — ia para um stream que ninguem le'.
//
// O sintoma que isso produzia: o loader dizia "falha no mapeamento - ver
// logs.txt" e o logs.txt nao tinha UMA linha do mapper. O diagnostico existia,
// so' nao chegava em lugar nenhum.
//
// Agora quem embute o mapper (o cheat) registra um destino:
//
//     kdmUtils::g_logSink = [](const wchar_t* s) { LOG("[kdm] %ls", s); };
//
// Sem sink, cai no `std::wcout` como antes — o OffsetDumper, que tem console,
// continua funcionando igual.
// ---------------------------------------------------------------------------
namespace kdmUtils
{
	typedef void (*LogSink)(const wchar_t* texto);

	// `inline` de proposito: o header entra em mais de uma unidade de traducao
	// (cheat e OffsetDumper) e o `inline` garante UMA instancia so'.
	inline LogSink g_logSink = nullptr;

	std::wstring GetFullTempPath();
	bool ReadFileToMemory(const std::wstring& file_path, std::vector<BYTE>* out_buffer);
	bool CreateFileFromMemory(const std::wstring& desired_file_path, const char* address, size_t size);
	uint64_t GetKernelModuleAddress(const std::string& module_name);
	BOOLEAN bDataCompare(const BYTE* pData, const BYTE* bMask, const char* szMask);
	uintptr_t FindPattern(uintptr_t dwAddress, uintptr_t dwLen, BYTE* bMask, const char* szMask);
	PVOID FindSection(const char* sectionName, uintptr_t modulePtr, PULONG size);
	std::wstring GetCurrentAppFolder();
}

#if defined(DISABLE_OUTPUT)
#define kdmLog(content)
#else
// Formata primeiro, entrega depois: um `wostringstream` local resolve o
// `<<` de qualquer tipo e da' uma string unica para o sink (que escreve UMA
// linha no logs.txt em vez de uma por pedaco).
#define kdmLog(content)                                                       \
	do {                                                                      \
		std::wostringstream _kdmS;                                            \
		_kdmS << content;                                                     \
		if (kdmUtils::g_logSink) kdmUtils::g_logSink(_kdmS.str().c_str());    \
		else std::wcout << _kdmS.str();                                       \
	} while (0)
#endif
