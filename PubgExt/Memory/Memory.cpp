#include "pch.h"
#include "Memory.h"

#include <thread>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <vector>
#include <TlHelp32.h>
#include <Globals.h>
Memory::Memory()
{
	LOG("loading driver...\n");
}

Memory::~Memory()
{
	driver.Cleanup();
	PROCESS_INITIALIZED = false;
}

bool Memory::Init(std::string process_name, bool memMap, bool debug)
{
	(void)memMap; (void)debug;

	if (!driver.Initialize())
	{
		LOG("[!] Initialization failed! Is the driver loaded?\n");
		return false;
	}

	// ---------------------------------------------------------------------
	// UMA VEZ SO'.
	//
	// Este `Init` e' chamado a cada SEGUNDO pelo laco de espera do TslGame, e
	// esta linha saia toda vez — o log ficava com "[+] Driver initialized
	// successfully." repetido ao lado de "Processo nao encontrado,
	// aguardando...", parecendo que o driver subia de novo a cada segundo.
	//
	// Era so' a mensagem: o `driver.Initialize()` ja' virou IDEMPOTENTE (nao
	// reabre o device). Mas mensagem repetida esconde a informacao — quem le
	// o log nao sabe se foram 3 tentativas ou 300.
	// ---------------------------------------------------------------------
	static bool jaLogouDriverOk = false;
	if (!jaLogouDriverOk) {
		jaLogouDriverOk = true;
		LOG("[+] Driver initialized successfully.\n");
	}

	if (process_name == "TslGame.exe") {
		// -----------------------------------------------------------------
		// Resolve o PID CERTO.
		//
		// O PUBG tem MAIS DE UM TslGame.exe (launcher + jogo). O FindWindowA
		// pode falhar (titulo/classe mudaram no update) e o Toolhelp32 pega o
		// PRIMEIRO — que e o launcher, com UWorld = 0.
		//
		// O jeito certo: enumera TODOS e le o UWorld de cada um. O processo do
		// jogo tem UWorld != 0.
		// -----------------------------------------------------------------
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap != INVALID_HANDLE_VALUE) {
			PROCESSENTRY32W pe{};
			pe.dwSize = sizeof(pe);
			int melhorPid = 0;
			int fallbackPid = 0;
			uint64_t melhorUWorld = 0;

			if (Process32FirstW(snap, &pe)) {
				do {
					if (_wcsicmp(pe.szExeFile, L"TslGame.exe") != 0) continue;
					const int pid = static_cast<int>(pe.th32ProcessID);
					LOG("[MB] laco: pid=%d exe=%ws\n", pid, pe.szExeFile);
					const uintptr_t base = driver.GetModuleBase(pid, L"TslGame.exe");
					LOG("[MB] loop: base=0x%llX\n", (unsigned long long)base);
					if (!base) continue;
					if (!fallbackPid) fallbackPid = pid;
					uint64_t raw = 0;
					driver.ReadMemory(pid, base + SDK.UWorld, &raw, sizeof(raw), "probe_UWorld");
					LOG("[+] TslGame.exe pid=%d base=0x%llX UWorld=0x%llX%s\n",
					    pid, (unsigned long long)base, (unsigned long long)raw,
					    raw ? "  <-- JOGO" : "");
					if (raw != 0 && !melhorPid) {
						melhorPid = pid;
						melhorUWorld = raw;
					}
				} while (Process32NextW(snap, &pe));
			}
			CloseHandle(snap);

			if (melhorPid) {
				this->current_process.PID = melhorPid;
				LOG("[+] real game: pid=%d (UWorld=0x%llX)\n",
				    melhorPid, (unsigned long long)melhorUWorld);
			} else if (fallbackPid) {
				// ---------------------------------------------------------
				// NAO COMMITAR UM PROCESSO COM UWorld = 0
				//
				// Aqui a gente aceitava o fallback e seguia. O sintoma:
				// abrir o cheat ANTES de o jogo carregar o mundo (ou estando
				// no lobby) prendia tudo em "aguardando Lobby" PARA SEMPRE —
				// o Init lia UWorld=0, gravava aquele PID e ninguem reavaliava.
				//
				// Abrir o cheat JA' DENTRO da partida funcionava, porque aí o
				// UWorld ja' tinha valor. Era essa a diferenca entre os dois
				// logs: com DOIS TslGame.exe (launcher + jogo) a escolha
				// acertava pelo working set; com UM so', aceitava zerado.
				//
				// Devolvendo false, o chamador REPETE a enumeracao a cada
				// poucos segundos ("Processo nao encontrado, aguardando...")
				// e passa a funcionar sozinho assim que o jogo carregar.
				// ---------------------------------------------------------
				LOG("[MB] TslGame.exe exists (pid=%d) but UWorld=0 (lobby/loading) -> waiting to be ready...\n", fallbackPid);
				return false;
			}
		}
	}

	if (!this->current_process.PID) {
		// A interface do driver (PUBG_KM) usa nome WIDE (wchar_t). Converte.
		std::wstring wideName(process_name.begin(), process_name.end());
		this->current_process.PID = driver.GetProcessId(wideName.c_str());
	}

	if (!this->current_process.PID)
	{
		LOG("[!] Could not get PID from [%s]!\n", process_name.c_str());
		return false;
	}
	LOG("[+] Found PID: %i\n", this->current_process.PID);
    driver.SetCurrentPid(this->current_process.PID);

	this->current_process.process_name = process_name;
	{
		std::wstring wideName(process_name.begin(), process_name.end());
		LOG("[MB] final: PID=%d nome=%ws\n", this->current_process.PID, wideName.c_str());
		this->current_process.base_address = driver.GetModuleBase(this->current_process.PID, wideName.c_str());
	}
	LOG("[+] Found Base Address: 0x%p\n", reinterpret_cast<void*>(this->current_process.base_address));
	if (!this->current_process.base_address)
	{
		LOG("[!] Could not get base address!\n");
		return false;
	}
	driver.SetBaseAddress(this->current_process.base_address);
	this->current_process.base_size = 0x5000000;

	LOG("Process information of %s\n", process_name.c_str());
	LOG("PID: %i\n", this->current_process.PID);
	LOG("Base Address: 0x%p\n", reinterpret_cast<void*>(this->current_process.base_address));

	PROCESS_INITIALIZED = TRUE;

	return true;
}

DWORD Memory::GetPidFromName(std::string process_name)
{
	std::wstring wide(process_name.begin(), process_name.end());
	return driver.GetProcessId(wide.c_str());
}

std::vector<int> Memory::GetPidListFromName(std::string name)
{
	std::vector<int> list;
	list.push_back(GetPidFromName(name));
	return list;
}

std::vector<std::string> Memory::GetModuleList(std::string process_name)
{
	return std::vector<std::string>(); 
}

size_t Memory::GetBaseAddress(std::string module_name)
{
	std::wstring key(module_name.begin(), module_name.end());
	if (!Modules.contains(key))
	{
		size_t base = driver.GetModuleBase(this->current_process.PID, key.c_str());
		if (!base)
		{
			LOG("[!] Couldn't find Base Address for %s\n", module_name.c_str());
			return 0;
		}

		LOG("[+] Found Base Address for %s at 0x%p\n", module_name.c_str(), reinterpret_cast<void*>(base));
		Modules[key] = base;
		return base;
	}
	return Modules[key];
}

int Memory::ReturnPid()
{
	return this->current_process.PID;
}

size_t Memory::GetBaseSize(std::string module_name)
{
	// Le o SizeOfImage do PE em vez de devolver constante. A versao anterior
	// devolvia 0x500000 fixo, e `current_process.base_size` era setado em
	// 0x5000000 em outro lugar — dois valores diferentes para a mesma coisa.
	(void)module_name;
	if (current_process.base_size) return current_process.base_size;
	if (!current_process.base_address) return 0;

	IMAGE_DOS_HEADER dos{};
	if (!Read(current_process.base_address, &dos, sizeof(dos))) return 0;
	if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;

	IMAGE_NT_HEADERS64 nt{};
	if (!Read(current_process.base_address + dos.e_lfanew, &nt, sizeof(nt))) return 0;
	if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;

	current_process.base_size = nt.OptionalHeader.SizeOfImage;
	LOG("[+] SizeOfImage de %s: 0x%zX\n", module_name.c_str(), current_process.base_size);
	return current_process.base_size;
}

uintptr_t Memory::GetExportTableAddress(std::string import, std::string process, std::string module)
{
	(void)import; (void)process; (void)module;
	LOG("[!] GetExportTableAddress nao implementado — retornando 0\n");
	return 0;
}

uintptr_t Memory::GetImportTableAddress(std::string import, std::string process, std::string module)
{
	(void)import; (void)process; (void)module;
	LOG("[!] GetImportTableAddress nao implementado — retornando 0\n");
	return 0;
}

// ---------------------------------------------------------------------------
// ProbeReadable — NAO e FixCr3
//
// O nome antigo era `FixCr3`, mas a funcao nunca corrigiu CR3: ela le 2 bytes
// no base address e devolve true se a leitura passou. O CR3 de verdade e
// resolvido pelo driver (`IOCTL_GET_CR3` -> `_EPROCESS->DirectoryTableBase`).
//
// O nome mentiroso custava caro: `Main.cpp` faz
//
//     while (GIsRunning && !TargetProcess.ProbeReadable()) sleep(3s);
//
// e quem lesse isso ia procurar logica de CR3 que nao existe aqui.
//
// Le a assinatura MZ do cabecalho. `silencioso=true` nao loga no sucesso,
// para o retry do boot nao encher o logs.txt.
// ---------------------------------------------------------------------------
bool Memory::ProbeReadable(bool silencioso)
{
	if (!current_process.base_address) {
		LOG("[-] ProbeReadable: sem base address\n");
		return false;
	}

	uint16_t magic = 0;
	if (!driver.ReadMemory(current_process.PID, current_process.base_address,
	                       &magic, sizeof(magic))) {
		LOG("[-] ProbeReadable: leitura falhou — o driver esta bloqueado?\n");
		return false;
	}

	if (magic != IMAGE_DOS_SIGNATURE) {
		LOG("[-] ProbeReadable: leu 0x%04X, esperava MZ (0x5A4D) — base errado\n", magic);
		return false;
	}

	// Silencioso no SUCESSO. A versao anterior logava sempre, e como a funcao
	// e chamada em retry de varios lugares, o logs.txt enchia de centenas de
	// linhas identicas — escondendo o que importava.
	if (!silencioso) {
		LOG("[+] ProbeReadable: leitura ok, base tem assinatura MZ\n");
	}
	return true;
}

bool Memory::DumpMemory(uintptr_t address, std::string path)
{
	(void)address; (void)path;
	LOG("[!] DumpMemory nao implementado — retornando false\n");
	return false;
}

// ---------------------------------------------------------------------------
// FindSignature — scan de padrao estilo IDA
//
// Sintaxe: "48 8B 05 ?? ?? ?? ?? 48 85 C0"
//   "48"  byte exato
//   "?"   wildcard (nibble inteiro)
//   "4?"  nibble alto fixo, baixo livre
//
// O scan le em blocos de 1 MiB (o teto do IOCTL) e mantem uma sobreposicao do
// tamanho do padrao entre blocos, para um padrao que cai na fronteira nao ser
// perdido. Sem isso o scan falha silenciosamente em ~1 endereco a cada MiB.
//
// E o que resolve o problema de verdade a cada patch do jogo: em vez de
// atualizar constante na mao, procura-se a instrucao.
// ---------------------------------------------------------------------------
bool Memory::ParsePattern(const char* pattern, std::vector<uint8_t>& bytes,
                          std::vector<uint8_t>& masks)
{
	bytes.clear();
	masks.clear();

	if (!pattern) return false;

	const char* p = pattern;
	while (*p) {
		while (*p == ' ') ++p;
		if (!*p) break;

		const char hi = *p++;
		if (!*p) return false;              // nibble solto no fim
		const char lo = *p++;

		// valida que o proximo caractere e separador ou fim
		if (*p && *p != ' ') return false;

		auto nib = [](char c, uint8_t& out) -> bool {
			if (c >= '0' && c <= '9') { out = (uint8_t)(c - '0');        return true; }
			if (c >= 'a' && c <= 'f') { out = (uint8_t)(c - 'a' + 10);   return true; }
			if (c >= 'A' && c <= 'F') { out = (uint8_t)(c - 'A' + 10);   return true; }
			if (c == '?')             { out = 0;                          return true; }
			return false;
		};

		uint8_t h = 0, l = 0;
		if (!nib(hi, h) || !nib(lo, l)) return false;

		const bool hiAny = (hi == '?');
		const bool loAny = (lo == '?');

		bytes.push_back((uint8_t)((h << 4) | l));
		masks.push_back((uint8_t)((hiAny ? 0x00 : 0xF0) | (loAny ? 0x00 : 0x0F)));
	}

	return !bytes.empty();
}

uint64_t Memory::FindSignature(const char* signature, uint64_t range_start,
                               uint64_t range_end, int PID)
{
	if (PID == 0) PID = current_process.PID;
	if (!PID || !range_start || range_end <= range_start) return 0;

	std::vector<uint8_t> pat, mask;
	if (!ParsePattern(signature, pat, mask)) {
		LOG("[!] FindSignature: padrao invalido: %s\n", signature ? signature : "(null)");
		return 0;
	}

	constexpr size_t kChunk    = 1024 * 1024;      // teto do IOCTL no driver
	const size_t       patLen  = pat.size();

	std::vector<uint8_t> buf(kChunk + patLen);
	uint64_t offset = 0;

	while (range_start + offset < range_end) {
		const uint64_t addr = range_start + offset;
		size_t want = kChunk;
		if (addr + want > range_end) want = (size_t)(range_end - addr);

		if (!driver.ReadMemory(PID, (uintptr_t)addr, buf.data(), want, "FindSignature")) {
			// regiao nao mapeada: pula o bloco em vez de abortar o scan
			offset += want;
			continue;
		}

		// o bloco tem patLen-1 bytes de sobreposicao da leitura anterior
		const size_t searchLen = want + (offset ? patLen - 1 : 0);
		const size_t searchStart = offset ? 0 : 0;

		if (searchLen >= patLen) {
			for (size_t i = searchStart; i + patLen <= searchLen; ++i) {
				bool hit = true;
				for (size_t j = 0; j < patLen; ++j) {
					if ((buf[i + j] & mask[j]) != (pat[j] & mask[j])) { hit = false; break; }
				}
				if (hit) {
					const uint64_t found = addr - (offset ? patLen - 1 : 0) + i;
					LOG("[+] FindSignature: %s -> 0x%llX\n", signature,
					    (unsigned long long)found);
					return found;
				}
			}
		}

		offset += want;
	}

	LOG("[-] FindSignature: %s not found in 0x%llX..0x%llX\n", signature,
	    (unsigned long long)range_start, (unsigned long long)range_end);
	return 0;
}

bool Memory::Write(uintptr_t address, void* buffer, size_t size) const
{
	// O PUBG_KM.sys nao implementa escrita (so READ 0x803 e GET_MODULE_BASE 0x802).
	// O ESP e read-only; write fica como no-op ate termos um driver que grave.
	(void)address; (void)buffer; (void)size;
	return false;
}

bool Memory::Write(uintptr_t address, void* buffer, size_t size, int pid) const
{
	(void)address; (void)buffer; (void)size; (void)pid;
	return false;
}

bool Memory::Read(uintptr_t address, void* buffer, size_t size, const char* debugName) const
{
	return const_cast<Memory*>(this)->driver.ReadMemory(this->current_process.PID, address, buffer, size, debugName);
}

bool Memory::Read(uintptr_t address, void* buffer, size_t size, int pid, const char* debugName) const
{
	return const_cast<Memory*>(this)->driver.ReadMemory(pid, address, buffer, size, debugName);
}

bool Memory::BatchRead(DriverInterfaceV3::BatchReadEntry* entries, size_t count) const
{
	return const_cast<Memory*>(this)->driver.BatchReadMemory(this->current_process.PID, entries, count);
}


