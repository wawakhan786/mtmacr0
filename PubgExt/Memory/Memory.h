#pragma once
#include "pch.h"
#include "../driver/driver_interface_v3.h"

// Map the VMM Scatter handle to an array list of driver read requests
typedef std::vector<DriverInterfaceV3::BatchReadEntry>* VMMDLL_SCATTER_HANDLE;

class Memory
{
private:
	struct CurrentProcessInformation
	{
		int PID = 0;
		size_t base_address = 0;
		size_t base_size = 0;
		std::string process_name = "";
	};
	std::unordered_map<std::wstring, ULONG64> Modules;


	static inline BOOLEAN PROCESS_INITIALIZED = FALSE;

	/*this->registry_ptr = std::make_shared<c_registry>(*this);
	this->key_ptr = std::make_shared<c_keys>(*this);*/

public:
	static inline CurrentProcessInformation current_process{ };

	/**
	 * brief Constructor takes a wide string of the process.
	 * Expects that all the libraries are in the root dir
	 */
	Memory();
	~Memory();

	/**
	* brief Initializes the DMA
	* This is required before any DMA operations can be done.
	* @param process_name the name of the process
	* @param memMap if true, will dump the memory map to a file	& make the DMA use it.
	* @return true if successful, false if not.
	*/
	bool Init(std::string process_name, bool memMap = true, bool debug = false);
	/*This part here is things related to the process information such as Base daddy, Size ect.*/

	/**
	* brief Gets the process id of the process
	* @param process_name the name of the process
	* @return the process id of the process
	*/
	DWORD GetPidFromName(std::string process_name);

	/**
	* brief Gets all the processes id(s) of the process
	* @param process_name the name of the process
	* @returns all the processes id(s) of the process
	*/
	std::vector<int> GetPidListFromName(std::string process_name);

	/**
	* \brief Gets the module list of the process
	* \param process_name the name of the process
	* \return all the module names of the process
	*/
	std::vector<std::string> GetModuleList(std::string process_name);


	/**
	* brief Gets the base address of the process
	* @param module_name the name of the module
	* @return the base address of the process
	*/
	size_t GetBaseAddress(std::string module_name);


	int ReturnPid();
	/**
	* brief Gets the base size of the process
	* @param module_name the name of the module
	* @return the base size of the process
	*/
	size_t GetBaseSize(std::string module_name);

	/**
	* brief Gets the export table address of the process
	* @param import the name of the export
	* @param process the name of the process
	* @param module the name of the module that you wanna find the export in
	* @return the export table address of the export
	*/
	uintptr_t GetExportTableAddress(std::string import, std::string process, std::string module);

	/**
	* brief Gets the import table address of the process
	* @param import the name of the import
	* @param process the name of the process
	* @param module the name of the module that you wanna find the import in
	* @return the import table address of the import
	*/
	uintptr_t GetImportTableAddress(std::string import, std::string process, std::string module);

	/**
	 * \brief Testa se da para ler no base address (confere a assinatura MZ).
	 *
	 * ATENCAO ao nome: isto NAO corrige CR3. O nome antigo era `FixCr3`, mas a
	 * funcao so le alguns bytes e devolve se passou — o comentario acima
	 * descrevia uma logica de DTB que nunca existiu aqui. O CR3 de verdade e
	 * resolvido pelo driver (`IOCTL_GET_CR3` -> `_EPROCESS->DirectoryTableBase`).
	 *
	 * Chamado em loop de retry no boot. `silencioso` existe porque a versao
	 * anterior logava TAMBEM NO SUCESSO: o loop saia na primeira iteracao, mas
	 * ja tinha escrito "leitura ok" no log — e como a funcao e chamada de mais
	 * de um lugar, o logs.txt enchia de centenas de linhas identicas.
	 *
	 * \param silencioso nao loga quando da certo (a falha sempre loga)
	 */
	bool ProbeReadable(bool silencioso = false);

	/**
	 * \brief Dumps the process memory at address (requires to be a valid PE Header) to the path
	 * \param address the address to the PE Header(BaseAddress)
	 * \param path the path where you wanna save dump to
	 */
	bool DumpMemory(uintptr_t address, std::string path);

	/*This part is where all memory operations are done, such as read, write.*/

	/**
	 * \brief Scan de padrao estilo IDA.
	 *
	 * Sintaxe: "48 8B 05 ?? ?? ?? ?? 48 85 C0"
	 *   "48"  byte exato
	 *   "?"   wildcard (nibble inteiro)
	 *   "4?"  nibble alto fixo, baixo livre
	 *
	 * Le em blocos de 1 MiB (o teto do IOCTL no driver) com sobreposicao do
	 * tamanho do padrao, entao um padrao na fronteira do bloco nao se perde.
	 * Bloco nao mapeado e pulado em vez de abortar o scan.
	 *
	 * E o que resolve o problema de verdade a cada patch: em vez de atualizar
	 * constante na mao, procura-se a instrucao.
	 *
	 * \param signature o padrao
	 * \param range_start inicio do range
	 * \param range_end fim do range
	 * \param PID alvo (0 = processo atual)
	 * \return endereco do match, ou 0
	 */
	/**
	 * \brief Quebra um padrao textual em bytes + mascaras.
	 *
	 * "48 8B ?? 4?" -> bytes [0x48,0x8B,0x00,0x40], masks [0xFF,0xFF,0x00,0xF0]
	 * Retorna false se o padrao tiver sintaxe invalida.
	 */
	static bool ParsePattern(const char* pattern, std::vector<uint8_t>& bytes,
	                         std::vector<uint8_t>& masks);

		uint64_t FindSignature(const char* signature, uint64_t range_start, uint64_t range_end, int PID = 0);

	/**
	 * \brief Writes memory to the process
	 * \param address The address to write to
	 * \param buffer The buffer to writeze of the buffer
	 * \return
	 * \param size The si
	 */
	bool Write(uintptr_t address, void* buffer, size_t size) const;
	bool Write(uintptr_t address, void* buffer, size_t size, int pid) const;

	/**
	 * \brief Writes memory to the process using a template
	 * \param address to write to
	 * \param value the value you'll write to the address
	 */
	template <typename T>
	bool Write(void* address, T value)
	{
		return Write(address, &value, sizeof(T));
	}

	template <typename T>
	bool Write(uintptr_t address, T value)
	{
		return Write(address, &value, sizeof(T));
	}

	/**
	* brief Reads memory from the process
	* @param address The address to read from
	* @param buffer The buffer to read to
	* @param size The size of the buffer
	* @return true if successful, false if not.
	*/
	bool Read(uintptr_t address, void* buffer, size_t size, const char* debugName = nullptr) const;
	bool Read(uintptr_t address, void* buffer, size_t size, int pid, const char* debugName = nullptr) const;

	/**
	* brief Batch reads multiple addresses in a single driver call for extreme performance
	* @param entries List of batch read requests
	* @param count Number of entries
	* @return true if successful
	*/
	bool BatchRead(DriverInterfaceV3::BatchReadEntry* entries, size_t count) const;

	/**
	* brief Reads memory from the process using a template
	* @param address The address to read from
	* @return the value read from the process
	*/
	template <typename T>
	T Read(void* address, const char* debugName = nullptr)
	{
		T buffer{ };
		memset(&buffer, 0, sizeof(T));
		Read(reinterpret_cast<uint64_t>(address), reinterpret_cast<void*>(&buffer), sizeof(T), current_process.PID, debugName);

		return buffer;
	}

	template <typename T>
	T Read(uint64_t address, const char* debugName = nullptr)
	{
		return Read<T>(reinterpret_cast<void*>(address), debugName);
	}

	/**
	* brief Reads memory from the process using a template and pid
	* @param address The address to read from
	* @param pid The process id of the process
	* @return the value read from the process
	*/
	template <typename T>
	T Read(void* address, int pid, const char* debugName = nullptr)
	{
		T buffer{ };
		memset(&buffer, 0, sizeof(T));
		Read(reinterpret_cast<uint64_t>(address), reinterpret_cast<void*>(&buffer), sizeof(T), pid, debugName);

		return buffer;
	}

	template <typename T>
	T Read(uint64_t address, int pid, const char* debugName = nullptr)
	{
		return Read<T>(reinterpret_cast<void*>(address), pid, debugName);
	}




	/**
	 * \brief O driver por baixo do Memory.
	 *
	 * Publico de proposito: o banco de teste (DriverSelfTest.cpp) precisa
	 * chamar GetProcessCr3 / ReadMemoryViaPhysical direto para comparar os
	 * caminhos de leitura sem passar pela camada de alto nivel.
	 */
	DriverInterfaceV3 driver;
};

inline Memory TargetProcess;
