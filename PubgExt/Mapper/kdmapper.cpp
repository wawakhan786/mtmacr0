#include "kdmapper.hpp"
#include <Windows.h>
#include <iostream>

#include "utils.hpp"
#include "intel_driver.hpp"
#include "nt.hpp"
#include "portable_executable.hpp"

void RelocateImageByDelta(portable_executable::vec_relocs relocs, const ULONG64 delta) {
	for (const auto& current_reloc : relocs) {
		for (auto i = 0u; i < current_reloc.count; ++i) {
			const uint16_t type = current_reloc.item[i] >> 12;
			const uint16_t offset = current_reloc.item[i] & 0xFFF;

			if (type == IMAGE_REL_BASED_DIR64)
				*reinterpret_cast<ULONG64*>(current_reloc.address + offset) += delta;
		}
	}
}

// Fix cookie by @Jerem584
bool FixSecurityCookie(void* local_image, ULONG64 kernel_image_base)
{
	auto headers = portable_executable::GetNtHeaders(local_image);
	if (!headers)
		return false;

	auto load_config_directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress;
	if (!load_config_directory)
	{
		kdmLog(L"[+] Load config directory wasn't found, probably StackCookie not defined, fix cookie skipped" << std::endl);
		return true;
	}

	auto load_config_struct = (PIMAGE_LOAD_CONFIG_DIRECTORY)((uintptr_t)local_image + load_config_directory);
	auto stack_cookie = load_config_struct->SecurityCookie;
	if (!stack_cookie)
	{
		kdmLog(L"[+] StackCookie not defined, fix cookie skipped" << std::endl);
		return true; // as I said, it is not an error and we should allow that behavior
	}

	stack_cookie = stack_cookie - (uintptr_t)kernel_image_base + (uintptr_t)local_image; //since our local image is already relocated the base returned will be kernel address

	if (*(uintptr_t*)(stack_cookie) != 0x2B992DDFA232) {
		kdmLog(L"[-] StackCookie already fixed!? this probably wrong" << std::endl);
		return false;
	}

	kdmLog(L"[+] Fixing stack cookie" << std::endl);

	auto new_cookie = 0x2B992DDFA232 ^ GetCurrentProcessId() ^ GetCurrentThreadId(); // here we don't really care about the value of stack cookie, it will still works and produce nice result
	if (new_cookie == 0x2B992DDFA232)
		new_cookie = 0x2B992DDFA233;

	*(uintptr_t*)(stack_cookie) = new_cookie; // the _security_cookie_complement will be init by the driver itself if they use crt
	return true;
}

bool ResolveImports(portable_executable::vec_imports imports) {
	for (const auto& current_import : imports) {
		ULONG64 Module = kdmUtils::GetKernelModuleAddress(current_import.module_name);
		if (!Module) {
#if !defined(DISABLE_OUTPUT)
			std::cout << "[-] Dependency " << current_import.module_name << " wasn't found" << std::endl;
#endif
			return false;
		}

		for (auto& current_function_data : current_import.function_datas) {
			ULONG64 function_address = intel_driver::GetKernelModuleExport(Module, current_function_data.name);

			if (!function_address) {
				//Lets try with ntoskrnl
				if (Module != intel_driver::ntoskrnlAddr) {
					function_address = intel_driver::GetKernelModuleExport(intel_driver::ntoskrnlAddr, current_function_data.name);
					if (!function_address) {
#if !defined(DISABLE_OUTPUT)
						std::cout << "[-] Failed to resolve import " << current_function_data.name << " (" << current_import.module_name << ")" << std::endl;
#endif
						return false;
					}
				}
			}

			*current_function_data.address = function_address;
		}
	}

	return true;
}

// O que foi REALMENTE alocado na ultima chamada de MapDriver:
//
// `realBase` e o endereco devolvido por MmAllocateIndependentPagesEx, e
// `image_size` ja' vem reduzido em SizeOfHeaders (o mapper nao mapeia os
// headers quando destroyHeader=true). Guardar os dois aqui e' o que
// permite ao unload liberar exatamente o que foi alocado.
static ULONG64 g_baseAlocada = 0;
static ULONG   g_tamanhoAlocado = 0;

ULONG64 kdmapper::GetUltimaBaseAlocada()     { return g_baseAlocada; }
ULONG   kdmapper::GetUltimoTamanhoAlocado() { return g_tamanhoAlocado; }

ULONG64 kdmapper::MapDriver(BYTE* data, ULONG64 param1, ULONG64 param2, bool free, bool destroyHeader, AllocationMode mode, bool PassAllocationAddressAsFirstParam, mapCallback callback, NTSTATUS* exitCode) {

	const PIMAGE_NT_HEADERS64 nt_headers = portable_executable::GetNtHeaders(data);

	if (!nt_headers) {
		kdmLog(L"[-] Invalid format of PE image" << std::endl);
		return 0;
	}

	if (nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		kdmLog(L"[-] Image is not 64 bit" << std::endl);
		return 0;
	}

	ULONG32 image_size = nt_headers->OptionalHeader.SizeOfImage;

	void* local_image_base = VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!local_image_base)
		return 0;

	DWORD TotalVirtualHeaderSize = (IMAGE_FIRST_SECTION(nt_headers))->VirtualAddress;
	image_size = image_size - (destroyHeader ? TotalVirtualHeaderSize : 0);

	ULONG64 kernel_image_base = 0;
	if (mode == AllocationMode::AllocateIndependentPages)
	{
		kernel_image_base = intel_driver::MmAllocateIndependentPagesEx(image_size);
	}
	else { // AllocatePool by default
		kernel_image_base = intel_driver::AllocatePool(nt::POOL_TYPE::NonPagedPool, image_size);
	}

	if (!kernel_image_base) {
		kdmLog(L"[-] Failed to allocate remote image in kernel" << std::endl);

		VirtualFree(local_image_base, 0, MEM_RELEASE);
		return 0;
	}

	do {
		kdmLog(L"[+] Image base has been allocated at 0x" << reinterpret_cast<void*>(kernel_image_base) << std::endl);

		// Copy image headers

		memcpy(local_image_base, data, nt_headers->OptionalHeader.SizeOfHeaders);

		// Copy image sections

		const PIMAGE_SECTION_HEADER current_image_section = IMAGE_FIRST_SECTION(nt_headers);

		for (auto i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i) {
			if ((current_image_section[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) > 0)
				continue;
			auto local_section = reinterpret_cast<void*>(reinterpret_cast<ULONG64>(local_image_base) + current_image_section[i].VirtualAddress);
			memcpy(local_section, reinterpret_cast<void*>(reinterpret_cast<ULONG64>(data) + current_image_section[i].PointerToRawData), current_image_section[i].SizeOfRawData);
		}

		ULONG64 realBase = kernel_image_base;
		if (destroyHeader) {
			kernel_image_base -= TotalVirtualHeaderSize;
			kdmLog(L"[+] Skipped 0x" << std::hex << TotalVirtualHeaderSize << L" bytes of PE Header" << std::endl);
		}

		// Registra a alocacao real ANTES de qualquer falha posterior: se o
		// mapeamento abortar no meio, quem chamou ainda sabe o que liberar.
		g_baseAlocada    = realBase;
		g_tamanhoAlocado = (uint32_t)image_size;

		// Resolve relocs and imports

		RelocateImageByDelta(portable_executable::GetRelocs(local_image_base), kernel_image_base - nt_headers->OptionalHeader.ImageBase);

		if (!FixSecurityCookie(local_image_base, kernel_image_base ))
		{
			kdmLog(L"[-] Failed to fix cookie" << std::endl);
			return 0;
		}

		if (!ResolveImports(portable_executable::GetImports(local_image_base))) {
			kdmLog(L"[-] Failed to resolve imports" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		// Write fixed image to kernel

		if (!intel_driver::WriteMemory(realBase, (PVOID)((uintptr_t)local_image_base + (destroyHeader ? TotalVirtualHeaderSize : 0)), image_size)) {
			kdmLog(L"[-] Failed to write local image to remote image" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		if (mode == AllocationMode::AllocateIndependentPages)
		{
			auto ProtectionToString = [](ULONG prot) -> const char* {
				switch (prot)
				{
				case PAGE_NOACCESS: return "NOACCESS";
				case PAGE_READONLY: return "READONLY";
				case PAGE_READWRITE: return "READWRITE";
				case PAGE_EXECUTE: return "EXECUTE";
				case PAGE_EXECUTE_READ: return "EXECUTE_READ";
				case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
				default: return "UNKNOWN";
				}
				};

			// -------------------------------------------------------------------
			// A PROTECAO NAO E' OPCIONAL — e isto era um bug de verdade.
			//
			// O `MmAllocateIndependentPages` devolve as paginas como
			// PAGE_READONLY. Quem torna `.data` gravavel e' o
			// `MmSetPageProtection` no laco abaixo.
			//
			// Ate' aqui, uma FALHA nessa chamada era logada e o codigo SEGUIA:
			//
			//     if (!MmSetPageProtection(...))
			//         kdmLog("[-] Failed to set protection...");   // <- e continua
			//
			// Resultado: as secoes ficam como o alocador deixou. `.data` fica
			// SOMENTE LEITURA, e a primeira escrita do driver numa global vira
			// ATTEMPTED_WRITE_TO_READONLY_MEMORY (0xBE).
			//
			// E' o bugcheck que o He levou, e o encadeamento e' exato:
			//   varredura de assinatura acha MmSetPageProtection errado
			//   -> a chamada falha
			//   -> o aviso vai pro log e o mapeamento CONTINUA
			//   -> o driver roda por um tempo (o dispatch le' tudo)
			//   -> e cai na primeira escrita que realmente persiste
			//
			// A LICAO E' A MESMA DO SELF-UNMAP, e vale escrever de novo: uma
			// varredura de assinatura que devolve LIXO nao da' erro — da' um
			// endereco plausivel. Todo consumidor dela precisa tratar o
			// resultado como NAO CONFIAVEL, e falhar ALTO quando nao der.
			//
			// Agora a falha e' FATAL: sem protecao, o mapeamento e' ABORTADO e
			// as paginas sao liberadas. Melhor nao ter driver do que ter um
			// driver que derruba a maquina na primeira escrita.
			// -------------------------------------------------------------------
			bool protecaoOk = true;

			for (int i = 0; i < nt_headers->FileHeader.NumberOfSections; i++) {
				auto sec = &IMAGE_FIRST_SECTION(nt_headers)[i];
				uintptr_t secAddr = kernel_image_base + sec->VirtualAddress;
				uint32_t secSize = sec->Misc.VirtualSize;

				if (secSize <= 0) {
					kdmLog(L"[*] Skipping empty section: " << (char*)sec->Name << std::endl);
					continue;
				}

				ULONG prot = PAGE_READONLY;

				if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
					prot = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) ?
						PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
				}
				else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
					prot = PAGE_READWRITE;
				}
				else if (sec->Characteristics & IMAGE_SCN_MEM_READ) {
					prot = PAGE_READONLY;
				}

				kdmLog(L"[+] Setting protection for section: "
					<< (char*)sec->Name
					<< L" Base: 0x" << std::hex << secAddr
					<< L" Size: 0x" << secSize
					<< L" Prot: " << ProtectionToString(prot)
					<< std::dec << std::endl);

				if (!intel_driver::MmSetPageProtection(secAddr, secSize, prot)) {
					// A secao gravavel e' a que IMPORTA: sem ela o driver nao
					// consegue tocar as proprias globais. Falhar aqui e' fatal.
					const bool gravavel = (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE);

					kdmLog(L"[-] MmSetPageProtection FALHOU na secao " << (char*)sec->Name
						<< (gravavel ? L" (GRAVAVEL — FATAL)" : L" (somente leitura)") << std::endl);

					if (gravavel) {
						protecaoOk = false;
						break;
					}
				}
			}

			if (!protecaoOk) {
				kdmLog(L"[-] ABORTANDO o mapeamento: sem a secao gravavel, o driver" << std::endl);
				kdmLog(L"[-] cai na PRIMEIRA escrita numa global (0xBE). Liberando as paginas." << std::endl);

				// `realBase` e' a alocacao de verdade: `kernel_image_base` ja'
				// foi deslocado para tras pelo destroyHeader.
				intel_driver::MmFreeIndependentPages(realBase, image_size);
				break;
			}
		}

		// Call driver entry point

		const ULONG64 address_of_entry_point = kernel_image_base + nt_headers->OptionalHeader.AddressOfEntryPoint;

		kdmLog(L"[<] Calling DriverEntry 0x" << reinterpret_cast<void*>(address_of_entry_point) << std::endl);

		if (callback) {
			if (!callback(&param1, &param2, realBase, image_size)) {
				kdmLog(L"[-] Callback returns false, failed!" << std::endl);
				kernel_image_base = realBase;
				break;
			}
		}

		NTSTATUS status = 0;
		if (!intel_driver::CallKernelFunction(&status, address_of_entry_point, (PassAllocationAddressAsFirstParam ? realBase : param1), param2)) {
			kdmLog(L"[-] Failed to call driver entry" << std::endl);
			kernel_image_base = realBase;
			break;
		}

		if (exitCode)
			*exitCode = status;

		kdmLog(L"[+] DriverEntry returned 0x" << std::hex << status << std::endl);

		// Free memory
		if (free) {
			kdmLog(L"[+] Freeing memory" << std::endl);
			bool free_status = false;

			if (mode == AllocationMode::AllocateIndependentPages)
			{
				free_status = intel_driver::MmFreeIndependentPages(realBase, image_size);
			}
			else {
				free_status = intel_driver::FreePool(realBase);
			}

			if (free_status) {
				kdmLog(L"[+] Memory has been released" << std::endl);
			}
			else {
				kdmLog(L"[-] WARNING: Failed to free memory!" << std::endl);
			}
		}



		VirtualFree(local_image_base, 0, MEM_RELEASE);
		return realBase;

	} while (false);


	VirtualFree(local_image_base, 0, MEM_RELEASE);

	kdmLog(L"[+] Freeing memory" << std::endl);
	bool free_status = false;

	if (mode == AllocationMode::AllocateIndependentPages)
	{
		free_status = intel_driver::MmFreeIndependentPages(kernel_image_base, image_size);
	}
	else {
		free_status = intel_driver::FreePool(kernel_image_base);
	}

	if (free_status) {
		kdmLog(L"[+] Memory has been released" << std::endl);
	}
	else {
		kdmLog(L"[-] WARNING: Failed to free memory!" << std::endl);
	}

	return 0;
}


