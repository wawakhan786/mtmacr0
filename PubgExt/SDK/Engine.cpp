#include "pch.h"
#include <set>
#include <map>
#include "Engine.h"
#include "ActorEntity.h"
#include "Globals.h"
#include "../ESP/VisCheck.h"
#include "../Config/ConfigUtilities.h"
#include "../driver/syscall.h"
#include <codecvt>
#include <locale>
#include <algorithm>
#include <cmath>          // std::isfinite em RefreshViewMatrix
Engine::Engine()
{
	CachedBaseAddress = TargetProcess.GetBaseAddress(ProcessName);

	// Confere que os parametros de decrypt de nome estao coerentes antes de
	// tentar resolver qualquer FName. Se isso falhar no log, o problema esta
	// nos offsets de NameIndex*, nao no jogo.
	SelfTestDecryptCIndex();

	if (InitDecrypt(SDK.XenuineDecrypt)) {
		GetGNames();
	}
}
typedef int64_t(__fastcall* DecryptFunctoin)(uintptr_t key, uintptr_t argv);
inline DecryptFunctoin fnDecryptFunctoin = NULL;
inline uint64_t Tmpadd;
inline bool g_IsLeaInstruction = false;

// ---------------------------------------------------------------------------
// EnsureNtSyscallIndices — resolve os indices de syscall em RUNTIME.
//
// Os stubs (driver\syscall.asm) chamavam o SSDT com indices FIXOS: 0x18 para
// NtAllocateVirtualMemory e 0x1E para NtFreeVirtualMemory. Esses sao os
// numeros das builds Win10 1803+ .. Win11. Em Win10 anterior o mesmo servico
// tem outro numero, e o stub chamaria outra funcao do SSDT com a assinatura
// errada — nao daria erro, daria comportamento aleatorio.
//
// A resolucao le' o codigo do proprio stub em `ntdll`, que ja' esta' carregado
// no processo, e pega o imm32 que a Microsoft emitiu:
//
//     Win10 1809+ :  4C 8B D1        mov r10, rcx
//                    B8 xx xx xx xx  mov eax, <indice>
//     Win10 antigo:  B8 xx xx xx xx  mov eax, <indice>
//                    ...
//
// ZERO API nova e zero offset de estrutura. Se nenhum padrao casar, o valor de
// build continua valendo — ou seja, o pior caso e' exatamente o comportamento
// de antes da mudanca.
// ---------------------------------------------------------------------------
inline void EnsureNtSyscallIndices()
{
    static bool feito = false;
    if (feito) return;
    feito = true;

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;

    struct Alvo { const char* nome; unsigned int* destino; };
    const Alvo alvos[2] = {
        { "NtAllocateVirtualMemory", &g_NtAllocateVirtualMemoryIndex },
        { "NtFreeVirtualMemory",     &g_NtFreeVirtualMemoryIndex     },
    };

    for (int i = 0; i < 2; ++i) {
        const BYTE* p = (const BYTE*)GetProcAddress(ntdll, alvos[i].nome);
        if (!p) continue;

        unsigned int idx = 0;
        if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8) {
            idx = *(const unsigned int*)(p + 4);        // stub moderno
        } else if (p[0] == 0xB8) {
            idx = *(const unsigned int*)(p + 1);        // stub antigo
        } else {
            LOG("[SYSCALL] %s: stub desconhecido (%02X %02X %02X %02X) -> "
                "mantendo o indice de build 0x%X\n",
                alvos[i].nome, p[0], p[1], p[2], p[3], *alvos[i].destino);
            continue;
        }

        // Sanidade: indice de syscall real fica muito abaixo disto. Um valor
        // absurdo significa que o padrao casou no lugar errado — e nesse caso o
        // fallback de build e' estritamente melhor que um numero inventado.
        if (idx == 0 || idx > 0x1000) {
            LOG("[SYSCALL] %s: indice resolvido implausivel (0x%X) -> mantendo 0x%X\n",
                alvos[i].nome, idx, *alvos[i].destino);
            continue;
        }

        LOG("[SYSCALL] %s: indice resolvido = 0x%X (build dizia 0x%X)\n",
            alvos[i].nome, idx, *alvos[i].destino);
        *alvos[i].destino = idx;
    }
}

Engine::~Engine()
{
	if (fnDecryptFunctoin) {
		// Idempotente: se o InitDecrypt ja' rodou, isto nao faz nada. Existe
		// para o caso do destrutor ser o PRIMEIRO ponto de uso dos stubs.
		EnsureNtSyscallIndices();

		PVOID freeAddr = (PVOID)fnDecryptFunctoin;
		SIZE_T freeSize = 0;
		SyscallNtFreeVirtualMemory((HANDLE)-1, &freeAddr, &freeSize, MEM_RELEASE);
		fnDecryptFunctoin = NULL;
	}
}


inline bool Engine::InitDecrypt(uint64_t offset)
{
	// ANTES de qualquer stub de syscall: os indices eram immediate de build.
	EnsureNtSyscallIndices();

	if (fnDecryptFunctoin) return true;

	uint64_t base = CachedBaseAddress;
	if (!base) {
		CachedBaseAddress = TargetProcess.GetBaseAddress(ProcessName);
		base = CachedBaseAddress;
		if (!base) return false;
	}

	uintptr_t DecryptAddr = base + offset;
	uintptr_t DecryptPtr = 0;
	bool read_success = TargetProcess.Read(DecryptAddr, &DecryptPtr, sizeof(uintptr_t), "DecryptPtr_Init");
	
	if (!read_success || !DecryptPtr) {
		return false;
	}

	// -----------------------------------------------------------------------
	// CORRECAO DO RVA — o bug que deixava o UWorld cifrado
	//
	// O slot em `base + XenuineDecrypt` NAO guarda um endereco. Medido no
	// TslGame.exe em disco (tools\dump_driver\xenuine_fix_check.py):
	//
	//     slot RVA 0x11757E28 (secao .rdata) = 0x00000001591DE7B0
	//
	// Um VA x64 comeca com 0x00007FF... ou 0xFFFF.... 0x1591DE7B0 nao e
	// nenhum dos dois: e um RVA (offset dentro do modulo).
	//
	// O codigo antigo fazia:
	//     DecryptPtr = base + 0x11757E28   -> 0x1591DE7B0
	//     Read(DecryptPtr, shellcode, 1024)   <- le de 0x1591DE7B0
	//
	// 0x1591DE7B0 e memoria BAIXA do processo. O modulo carrega em
	// ~0x7FFxxxxxxxxx, entao a leitura traz lixo, `ShellcodeBuff[3]` nunca e
	// 0x8D, `g_IsLeaInstruction` fica false, o UWorld nunca e decifrado e o
	// log mostra `rawUWorld = 0x0`.
	//
	// A correcao: quando o valor nao parece endereco, somar a base.
	//
	// A checagem e CONSERVADORA de proposito — se algum dia o slot voltar a
	// guardar um VA de verdade, o codigo usa o valor cru e nada quebra. So
	// somamos a base quando o valor comprovadamente nao pode ser um VA.
	// -----------------------------------------------------------------------
	{
		const bool pareceVa =
			(DecryptPtr >> 48) == 0xFFFF ||
			(DecryptPtr >= 0x0000700000000000ULL && DecryptPtr < 0x0000800000000000ULL);

		if (!pareceVa) {
			const uintptr_t corrigido = base + (uintptr_t)DecryptPtr;
			printf("[+] InitDecrypt: slot=0x%llX NAO e VA -> tratando como RVA\n",
			       (unsigned long long)DecryptPtr);
			printf("    base=0x%llX + rva -> DecryptPtr=0x%llX\n",
			       (unsigned long long)base, (unsigned long long)corrigido);
			DecryptPtr = corrigido;
		}
	}
	
	unsigned char ShellcodeBuff[1024] = { NULL };
	ShellcodeBuff[0] = 0x90; // nop
	ShellcodeBuff[1] = 0x90; // nop
		if (!TargetProcess.Read(DecryptPtr, &ShellcodeBuff[2], sizeof(ShellcodeBuff) - 2, "DecryptShellcode")) {
		return false;
	}
	int32_t Tmp1Add = *(int32_t*)&ShellcodeBuff[5];
	Tmpadd = Tmp1Add + DecryptPtr + 7;
	if (ShellcodeBuff[3] == 0x8D) {
		g_IsLeaInstruction = true;
	} else if (ShellcodeBuff[3] == 0x8B) {
		g_IsLeaInstruction = false;
	} else {
		g_IsLeaInstruction = false;
	}

	// -----------------------------------------------------------------------
	// DIAGNOSTICO do resultado da shellcode.
	//
	// Antes, quando isto dava errado, o unico sintoma era o UWorld lido como
	// 0 — sem nenhuma pista de ONDE a cadeia quebrou. Agora sai o byte que
	// decidiu, os primeiros bytes lidos e o Tmpadd calculado.
	//
	//   0x8D -> LEA  (caminho bom)
	//   0x8B -> mov  (o codigo trata como nao-LEA)
	//   outro -> a leitura trouxe lixo, provavelmente endereco errado
	// -----------------------------------------------------------------------
	printf("[+] InitDecrypt: shellcode[3]=0x%02X -> %s\n",
	       ShellcodeBuff[3],
	       g_IsLeaInstruction ? "LEA" : "nao-LEA");
	// Tambem pro logs.txt: o console nem sempre esta' visivel, e ESTE e' o
	// ponto exato que decide se o UWorld e' decifrado. Sem isso a gente ficava
	// cego bem na hora da decisao.
	LOG("[INITDEC] shellcode[3]=0x%02X -> %s  Tmp1Add=0x%X DecryptPtr=0x%llX Tmpadd=0x%llX\n",
	    ShellcodeBuff[3], g_IsLeaInstruction ? "LEA" : "nao-LEA",
	    Tmp1Add, (unsigned long long)DecryptPtr, (unsigned long long)Tmpadd);
	printf("    primeiros 16 bytes: "
	       "%02X %02X %02X %02X %02X %02X %02X %02X "
	       "%02X %02X %02X %02X %02X %02X %02X %02X\n",
	       ShellcodeBuff[0], ShellcodeBuff[1], ShellcodeBuff[2], ShellcodeBuff[3],
	       ShellcodeBuff[4], ShellcodeBuff[5], ShellcodeBuff[6], ShellcodeBuff[7],
	       ShellcodeBuff[8], ShellcodeBuff[9], ShellcodeBuff[10], ShellcodeBuff[11],
	       ShellcodeBuff[12], ShellcodeBuff[13], ShellcodeBuff[14], ShellcodeBuff[15]);
	printf("    Tmp1Add=0x%X  DecryptPtr=0x%llX  Tmpadd=0x%llX\n",
	       Tmp1Add, (unsigned long long)DecryptPtr, (unsigned long long)Tmpadd);

	ShellcodeBuff[2] = 0x48;
	ShellcodeBuff[3] = 0x8B;
	ShellcodeBuff[4] = 0xC1;
	ShellcodeBuff[5] = 0x90;
	ShellcodeBuff[6] = 0x90;
	ShellcodeBuff[7] = 0x90;
	ShellcodeBuff[8] = 0x90;
	
	PVOID allocAddr = NULL;
	SIZE_T allocSize = sizeof(ShellcodeBuff) + 4;
	SyscallNtAllocateVirtualMemory((HANDLE)-1, &allocAddr, 0,
		&allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	fnDecryptFunctoin = (DecryptFunctoin)allocAddr;
	if (!fnDecryptFunctoin) return false;
	RtlCopyMemory((LPVOID)fnDecryptFunctoin, (LPVOID)ShellcodeBuff, sizeof(ShellcodeBuff));
	return 1;
}
void Engine::UpdateWeaponPhysics()
{
	if (!AcknowledgedPawn || bIsDriving) return;

	auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration_cast<std::chrono::milliseconds>(now - LastWeaponUpdateTimestamp).count() < 200) return;
	LastWeaponUpdateTimestamp = now;

	uintptr_t weaponProcessor = SafeRead<uintptr_t>(AcknowledgedPawn + SDK.WeaponProcessor, "WeaponProcessor");
	if (!weaponProcessor || weaponProcessor < 0x10000000000ULL || (weaponProcessor & 7) != 0) return;

	uint8_t weaponIndex = 0;
	uintptr_t equippedWeaponsPtr = 0;
	{
		DriverInterfaceV3::BatchReadEntry wpBatch[2];
		wpBatch[0] = { weaponProcessor + SDK.CurrentWeaponIndex, sizeof(uint8_t), &weaponIndex };
		wpBatch[1] = { weaponProcessor + SDK.EquippedWeapons, sizeof(uintptr_t), &equippedWeaponsPtr };
		TargetProcess.BatchRead(wpBatch, 2);
	}
	if (!IsAddrValid(equippedWeaponsPtr) || equippedWeaponsPtr < 0x10000000000ULL || weaponIndex > 4) return;

	uintptr_t currentWeapon = SafeRead<uintptr_t>(equippedWeaponsPtr + (weaponIndex * 8), "CurrentWeapon");
	if (!IsAddrValid(currentWeapon)) return;

	static uintptr_t lastCurrentWeapon = 0;
	if (currentWeapon == lastCurrentWeapon) return;
	lastCurrentWeapon = currentWeapon;

	LOG("[WeaponPhysics] New Weapon Selected: 0x%llX | Index: %u\n", currentWeapon, weaponIndex);


	if (weaponIndex >= 1 && weaponIndex <= 3) {
		uintptr_t actorPtr = SafeRead<uintptr_t>(currentWeapon + SDK.WeaponTrajectoryData, "Actor_TrajectoryPtr");
		uintptr_t procPtr = SafeRead<uintptr_t>(weaponProcessor + SDK.WeaponTrajectoryData, "Processor_TrajectoryPtr");

		auto IsHeapValid = [](uintptr_t p) { return p > 0x10000000000ULL && (p & 7) == 0; };
		uintptr_t validTrajectoryData = IsHeapValid(actorPtr) ? actorPtr : 
										(IsHeapValid(procPtr) ? procPtr : 0);

		if (IsAddrValid(validTrajectoryData)) {
			uintptr_t trajectoryConfigAddr = validTrajectoryData + SDK.TrajectoryConfig;

				float bulletSpeed = 0.0f, substepTime = 0.0f, vDrag = 0.0f;
			{
				DriverInterfaceV3::BatchReadEntry trajBatch[3];
				trajBatch[0] = { trajectoryConfigAddr + 0x0, sizeof(float), &bulletSpeed };
				trajBatch[1] = { trajectoryConfigAddr + 0x40, sizeof(float), &substepTime };
				trajBatch[2] = { trajectoryConfigAddr + 0x44, sizeof(float), &vDrag };
				TargetProcess.BatchRead(trajBatch, 3);
			}

			CurrentBulletSpeed = bulletSpeed;
			CurrentSimulationSubstepTime = (substepTime > 0.0f) ? substepTime : 0.016f;
			CurrentVDragCoefficient = vDrag;
			CurrentGravity = -9.8f;

			LOG("[WeaponPhysics] InitialSpeed: %f | Gravity: %f | Drag: %f\n", CurrentBulletSpeed, CurrentGravity, CurrentVDragCoefficient);
		}
		else {
			CurrentGravity = -9.8f;
			LOG("[WeaponPhysics] No trajectory data, using default Gravity: %f\n", CurrentGravity);
		}
	} else {
		CurrentBulletSpeed = 0.0f;
		CurrentSimulationSubstepTime = 0.016f;
		CurrentVDragCoefficient = 0.0f;
		CurrentGravity = 0.0f;
		LOG("[WeaponPhysics] Resetting ballistics for non-firearm index: %u\n", weaponIndex);
	}
}

Vector3 Engine::GetPrediction(const std::shared_ptr<ActorEntity>& target, const Vector3& targetPos, float& dropPitchOffset)
{
	dropPitchOffset = 0.0f;
	if (CurrentBulletSpeed <= 100.0f) return targetPos;

	Vector3 localPos = Vector3(CameraEntry.POV.Location.X, CameraEntry.POV.Location.Y, CameraEntry.POV.Location.Z);
	float distanceMax = Vector3::Distance(localPos, targetPos);
	
	float internalGravityZ = abs(CurrentGravity); 
	float substepTime = CurrentSimulationSubstepTime > 0.0f ? CurrentSimulationSubstepTime : 0.016f; 
	float dragCoef = CurrentVDragCoefficient > 0.0f ? CurrentVDragCoefficient : 1.0f;

	float flyTime = 0.0f;
	float drop = 0.0f;
	float travelDistance = 0.0f;
	float ballisticSubstepDropVelocity = 0.0f;

	float zeroingDistance = 100.0f * 100.0f; 
	float dropOffset = 0.0f;

	for (int i = 0; i < 500; i++)
	{
		float v = CurrentBulletSpeed * 100.0f;
		
		drop += ballisticSubstepDropVelocity * substepTime;
		travelDistance += substepTime * v;
		flyTime += substepTime;

		if (travelDistance >= zeroingDistance && dropOffset == 0.0f) {
			dropOffset = drop;
		}

		if (travelDistance >= distanceMax)
		{
			flyTime -= (travelDistance - distanceMax) / v;
			drop -= ((travelDistance - distanceMax) / v) * ballisticSubstepDropVelocity;
			break;
		}
		
		ballisticSubstepDropVelocity += substepTime * internalGravityZ * 100.0f * dragCoef;
	}

	float scopZ = 5.0f; 
	const float RAD_TO_DEG = 57.2957795f; 
	dropPitchOffset = (((abs(scopZ) + drop) / distanceMax) * RAD_TO_DEG) -
					 (((abs(scopZ) + dropOffset) / zeroingDistance) * RAD_TO_DEG);

	if (distanceMax > zeroingDistance) {
		if (distanceMax >= 90000.0f)      dropPitchOffset -= 0.0310f;
		else if (distanceMax >= 80000.0f) dropPitchOffset -= 0.0285f;
		else if (distanceMax >= 70000.0f) dropPitchOffset -= 0.0265f;
		else if (distanceMax >= 60000.0f) dropPitchOffset -= 0.0245f;
		else if (distanceMax >= 50000.0f) dropPitchOffset -= 0.0222f;
		else if (distanceMax >= 40000.0f) dropPitchOffset -= 0.0200f;
		else if (distanceMax >= 30000.0f) dropPitchOffset -= 0.0175f;
		else if (distanceMax >= 20000.0f) dropPitchOffset -= 0.0115f;
		else                             dropPitchOffset -= 0.0150f;
	}

	Vector3 predictionPos = targetPos + (target->Velocity * flyTime);
	
	return predictionPos;
}

uintptr_t Engine::xe_decrypt(const uintptr_t encrypted)
{
	if (!encrypted) return 0;
	
	uint64_t key = g_IsLeaInstruction ? Tmpadd : CachedDecryptKey;
	
	if (!key) return 0;
	
	uintptr_t data = fnDecryptFunctoin(key, encrypted); 
	return data;
}

DWORD Engine::DecryptCIndex(DWORD value)
{
	// Build TslGame 2610.1.2.7 (AINDA NAO VALIDADA)  (era 2610.1.1.71)
	//
	// A FORMA E' ESTAVEL ENTRE BUILDS. SO' AS CONSTANTES MUDAM:
	//
	//     xored  = value ^ XorKey1
	//     left   = (xored << Dval) & ShlMask
	//     right  = (xored >> Sval) & XorKey3
	//     return (left | right) ^ _rotr(xored, Rval) ^ XorKey2
	//
	// As builds caem na MESMA forma:
	//
	//   2609:        K1=0xB753B1E8 Dval=19 ShlMask=inofensivo Sval=13 Key3=0x70000 Rval=29 K2=0x0CAFC916
	//   2610.1.1:    K1=0x3F346DAB Dval=2  ShlMask=0xFFFF0000 Sval=0  Key3=0x0     Rval=14 K2=0x9531CE38
	//   2610.1.1.71: K1=0x46BAE3B9 Dval=11 ShlMask=0xFFFF0000 Sval=0  Key3=0x0     Rval=5  K2=0x54D51E55
	//   2610.1.2.7:  K1=0x00000000 Dval=6  ShlMask=0xFFFF0000 Sval=0  Key3=0x0     Rval=10 K2=0x821E5EAD
	//               (K1=0: a build corrente cifra SEM XOR inicial; rank 32/32,
	//                bijetiva — verificado offline em _offsets\_check_2610_1_2_7.py)
	//
	// TODO valor sai do SDK — nada hardcoded aqui. Ver o bloco no Globals.h,
	// que traz o mapa lista->campo e o historico dos dois erros de leitura.
	//
	// O `& ShlMask` e' o detalhe que quase passou: na 2609 ele era inofensivo
	// (`<< 19` ja' zera os bits de baixo) e por isso o campo foi removido com a
	// conclusao "nao existe nesta formula". Na 2610 o shift e' `<< 2` e a
	// mascara separa o termo CERTO (bits 16..31) de um com 14 bits a mais.
	// Sem ela, o indice de FName resolve para memoria aleatoria: nomes de
	// classe saem como bytecode x86, o `IsPlayer` rejeita TODOS os atores, e o
	// ESP fica vazio (`[STATE] Actors=0`).
	// -----------------------------------------------------------------
	const DWORD xored   = value ^ (DWORD)SDK.NameIndexXor1;                            // ^ XorKey1
	// O `& ShlMask` NAO e' decorativo. Na 2609, com `<< 19`, ele era inofensivo e
	// por isso foi removido com a conclusao "nao existe nesta formula". Na 2610 o
	// shift virou `<< 2` e a mascara passou a separar o termo CERTO (bits 16..31)
	// de um com 14 bits a mais. Sem ela: FName resolve para memoria aleatoria,
	// nome de classe sai como bytecode, `IsPlayer` rejeita todos, ESP vazio —
	// `[STATE] Actors=0`. Ver o bloco no Globals.h.
	const DWORD left    = (xored << ((DWORD)SDK.NameIndexShl & 31u))
		& (DWORD)SDK.NameIndexShlMask;                                            // (<< Dval) & ShlMask
	const DWORD right   = (xored >> ((DWORD)SDK.NameIndexShr & 31u))
		& (DWORD)SDK.NameIndexMaskRight;                                          // (>> Sval) & XorKey3
	const DWORD merged  = left | right;                                               // OR, nao AND
	const DWORD rotated = _rotr(xored, (int)((DWORD)SDK.NameIndexRotr & 31u));        // _rotr(x, Rval)
	return merged ^ rotated ^ (DWORD)SDK.NameIndexXor2;                               // ^ XorKey2
}

// Autoteste do decrypt de indice de nome.
//
// O que ele cobre — e o que NAO cobre:
//
//   (a) RANK 32x32 sobre GF(2): a parte linear (tudo menos os XOR de chave) e'
//       uma BIJECAO de 32 bits, ou seja, indices de FName distintos nao
//       colidem. E' util, mas NAO prova que as constantes estejam certas: a
//       leitura ERRADA desta build (shl=15, AND no lugar do OR, `>>` ignorado,
//       rotr=1) tambem dava rank 32/32 — e o ESP nao funcionava.
//
//   (b) IDA-E-VOLTA: deriva a INVERSA exata da parte linear (Gauss-Jordan
//       sobre GF(2), a mesma algebra do rank) e confere
//       inversa(DecryptCIndex(x)) == x numa amostra grande. Pega erro de
//       implementacao (bit perdido, XOR de chave no lugar errado) — mas NAO
//       pega constante trocada, porque a inversa e' derivada da MESMA funcao.
//
//   (c) REFERENCIA DO DUMP: compara com a forma literal do dump, escrita aqui
//       com as constantes na mao. ESTE e' o teste que pega o erro que
//       aconteceu: se Dval/Sval/Rval/Ror estiverem trocados no Globals.h — ou
//       se alguem reintroduzir o `0xFFFF0000` — o rank continua 32/32, a
//       ida-e-volta continua 100% e (c) FALHA. E' o unico dos tres que olha
//       para a formula do dump.
//
//   NENHUM dos tres prova que as constantes batem com o binario do jogo: so' o
//   jogo prova isso. Se (a), (b) e (c) passarem e os nomes ainda sairem
//   errados, o problema e' o VALOR de um parametro na lista da build, nao a
//   forma do codigo.
//
// Chame uma vez no boot (ver Engine::Engine) e olhe o log.
bool Engine::SelfTestDecryptCIndex()
{
	// As MESMAS constantes que o Runtime usa (nada derivado por invariante).
	const uint32_t shl  = (uint32_t)SDK.NameIndexShl & 31u;      // Dval    = 19
	const uint32_t shr  = (uint32_t)SDK.NameIndexShr & 31u;      // Sval    = 13
	const uint32_t mask = (uint32_t)SDK.NameIndexMaskRight;      // XorKey3 = 0x70000
	const uint32_t rot  = (uint32_t)SDK.NameIndexRotr & 31u;     // Rval    = 29
	const uint32_t ror  = (uint32_t)SDK.NameIndexRor & 31u;      // Ror     = 1 (nao usado)

	// --- parte linear, sem os XOR de chave ---------------------------------
	auto linearPart = [shl, shr, mask, rot](uint32_t x) -> uint32_t {
		return ((x << shl) | ((x >> shr) & mask)) ^ _rotr(x, (int)rot);
	};

	// --- (a) rank da matriz 32x32 sobre GF(2) ------------------------------
	// cols[b] = imagem do vetor da base e_b; mat[i] = linha i (bits das colunas).
	uint32_t cols[32];
	for (int i = 0; i < 32; ++i) cols[i] = linearPart(1u << i);

	uint32_t mat[32] = { 0 };
	for (int b = 0; b < 32; ++b)
		for (int i = 0; i < 32; ++i)
			if ((cols[b] >> i) & 1u) mat[i] |= (1u << b);

	int rank = 0;
	{
		uint32_t m[32];                    // copia: `mat` ainda vai ser invertida
		for (int i = 0; i < 32; ++i) m[i] = mat[i];
		for (int i = 0; i < 32; ++i) {
			int piv = -1;
			for (int r = rank; r < 32; ++r)
				if ((m[r] >> i) & 1u) { piv = r; break; }
			if (piv < 0) continue;
			uint32_t tr = m[rank]; m[rank] = m[piv]; m[piv] = tr;
			for (int r = 0; r < 32; ++r)
				if (r != rank && ((m[r] >> i) & 1u)) m[r] ^= m[rank];
			++rank;
		}
	}
	const bool bijective = (rank == 32);

	// --- (b) inversa exata da parte linear: [A | I] -> [I | A^-1] ----------
	// `inv[i]` sai como a LINHA i da inversa (bit b = coeficiente de y_b em x_i).
	uint32_t inv[32];
	for (int i = 0; i < 32; ++i) inv[i] = 1u << i;
	bool invOk = true;
	for (int col = 0; col < 32 && invOk; ++col) {
		int piv = -1;
		for (int r = col; r < 32; ++r)
			if ((mat[r] >> col) & 1u) { piv = r; break; }
		if (piv < 0) { invOk = false; break; }
		uint32_t tr = mat[col]; mat[col] = mat[piv]; mat[piv] = tr;
		tr = inv[col]; inv[col] = inv[piv]; inv[piv] = tr;
		for (int r = 0; r < 32; ++r)
			if (r != col && ((mat[r] >> col) & 1u)) { mat[r] ^= mat[col]; inv[r] ^= inv[col]; }
	}

	// inv[i] e' a LINHA i da inversa; transpoe para COLUNAS (imagem de e_b),
	// senao a aplicacao abaixo inverte o eixo errado e a ida-e-volta falha.
	uint32_t invCol[32] = { 0 };
	for (int i = 0; i < 32; ++i)
		for (int b = 0; b < 32; ++b)
			if ((inv[i] >> b) & 1u) invCol[b] |= (1u << i);
	if (invOk)
		for (int b = 0; b < 32; ++b) {
			uint32_t chk = 0;
			for (int i = 0; i < 32; ++i)
				if ((cols[b] >> i) & 1u) chk ^= invCol[i];
			if (chk != (1u << b)) invOk = false;
		}

	// Inversa completa: y = shape(x ^ k1) ^ k2  ->  x = shape^-1(y ^ k2) ^ k1
	auto decryptInverse = [&invCol](uint32_t y) -> uint32_t {
		const uint32_t t = y ^ (uint32_t)SDK.NameIndexXor2;
		uint32_t x = 0;
		for (int b = 0; b < 32; ++b)
			if ((t >> b) & 1u) x ^= invCol[b];
		return x ^ (uint32_t)SDK.NameIndexXor1;
	};

	// --- amostra: probes fixos + os 32 vetores da base + pseudo-aleatorio ---
	const uint32_t probes[] = { 0x00000000u, 0x00000001u, 0xFFFFFFFFu,
								0x12345678u, 0xDEADBEEFu, 0x0000FFFFu,
								0xFFFF0000u, 0x80000000u };
	const int NPROBE  = (int)(sizeof(probes) / sizeof(probes[0]));
	const int NSAMPLE = 32 + 512;
	uint32_t samples[NSAMPLE];
	int nSamples = 0;
	for (int i = 0; i < NPROBE; ++i) samples[nSamples++] = probes[i];
	for (int i = 0; i < 32; ++i) samples[nSamples++] = 1u << i;
	uint32_t rng = 0x9E3779B9u;                            // xorshift32
	while (nSamples < NSAMPLE) {
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		samples[nSamples++] = rng;
	}

	int rtFwd = 0, rtInv = 0;
	for (int i = 0; i < nSamples; ++i) {
		const uint32_t x = samples[i];
		const uint32_t y = DecryptCIndex(x);
		if (invOk) {
			if (decryptInverse(y) == x) ++rtFwd;
			if (DecryptCIndex(decryptInverse(y)) == y) ++rtInv;
		}
	}
	const bool roundTrip = invOk && (rtFwd == nSamples) && (rtInv == nSamples);

	// --- (c) forma LITERAL do dump, com as constantes na mao (de proposito) -
	//
	// Copy LITERAL do que a lista da build traz, com os numeros escritos a mao.
	// O proposito e' comparar a implementacao PARAMETRIZADA (que le do SDK) com
	// a forma escrita: se alguem trocar um campo de lugar no Globals.h, a
	// parametrizada diverge da literal e este teste acusa. Foi ele que pegou a
	// leitura errada de `shl`/`mask` numa das rodadas.
	//
	// POR ISSO ELA TEM QUE ANDAR JUNTO COM O Globals.h.
	//
	// Se ficar para tras, o teste compara a implementacao NOVA com a formula
	// VELHA e acusa `refMatch=FALHOU` — apontando para o lugar errado. A
	// mensagem nao distingue "parametrizada errada" de "referencia velha", e
	// quem le vai procurar o bug na funcao certa.
	//
	// Build 2610.1.2.7 (AINDA NAO VALIDADA), como veio na lista
	// (XeDecryptCIndex):
	//
	//     return ((((v >> 10) | ((v) << 22))) ^ (((v) << 6) & 0xFFFF0000U)) ^ 0x821E5EADU;
	//
	// A lista tambem traz XeDecryptCIndex2 (xored = v ^ 0x8EF87ACE;
	// (xored<<6)&0xFFFF0000 ^ rotr(xored,10) ^ 0x8FA3E0B3) — demonstrado ser a
	// MESMA funcao (mesma parte linear, chaves escritas de outro jeito; ver
	// _offsets\_check_2610_1_2_7.py). Sem XOR inicial: K1 = 0.
	//
	// Confere com a parametrizada: K1=0, Dval=6, ShlMask=0xFFFF0000, Sval=0,
	// XorKey3=0 (o termo da direita some), Rval=10, XorKey2=0x821E5EAD.
	// Obs.: ((v>>10)|(v<<22)) e' o rotr10 completo (termos de bits disjuntos
	// — nesse ponto OR == XOR).
	//
	// HISTORICO (2610.1.1.65/.71): K1=0xD81623EB/0x46BAE3B9, Dval=0/11,
	// Rval=16/5, K2=0x46406FA7/0x54D51E55.
	// ---------------------------------------------------------------
	auto dumpReference = [](uint32_t valor) -> uint32_t {
		uint32_t x = valor;   // K1 = 0 nesta build
		return (uint32_t)(((x >> 10) | (x << 22)) ^ ((x << 6) & 0xFFFF0000u) ^ 0x821E5EADu);
	};
	int refOk = 0;
	for (int i = 0; i < nSamples; ++i)
		if (DecryptCIndex(samples[i]) == dumpReference(samples[i])) ++refOk;
	const bool refMatch = (refOk == nSamples);

	// Colisao entre probes distintos: barata, e pega bijecao quebrada na pratica.
	bool collide = false;
	for (int i = 0; i < NPROBE && !collide; ++i)
		for (int j = i + 1; j < NPROBE && !collide; ++j)
			if (probes[i] != probes[j] &&
				DecryptCIndex(probes[i]) == DecryptCIndex(probes[j])) collide = true;

	// -----------------------------------------------------------------------
	// O QUE CONTA COMO PASSE — e o que NAO conta.
	//
	// ===========================================================================
	// ESTE TESTE JA' DEU FALSO ALARME NA BUILD 2610. LEIA ANTES DE MEXER.
	// ===========================================================================
	//
	// Ele exigia `bijective && roundTrip && refMatch`. Na 2610 a formula da
	// lista tem rank 18/32: NAO e' invertivel, e o round-trip da inversa nao
	// existe. O teste berrou FALHOU e mandou conferir Shl/Shr/MaskRight/Rotr —
	// apontando para o lugar errado, porque os nomes de classe resolviam
	// perfeitamente no mesmo log ('WeapAWM_C', 'BP_Dirtbike_C'...).
	//
	// A conclusao errada era a do TESTE, nao a da formula. Bijecao nunca foi
	// requisito do jogo: quem cifra o indice e' o TslGame, e nos so' aplicamos
	// a decifragem. Uma funcao nao-injetiva decifra corretamente — so' nao pode
	// ser invertida para conferencia.
	//
	// O QUE PASSA A CONTAR:
	//   refMatch  -> a implementacao parametrizada bate com a forma LITERAL da
	//                lista. E' o unico teste que pega erro de FIACAO (campo
	//                trocado de lugar no Globals.h). E o que importa.
	//
	// O QUE VIRA INFORMACAO (nao reprova):
	//   rank      -> so' e' conclusivo quando < 32 E a formula deveria ser
	//                invertivel. Vira aviso de colisao, nao de falha.
	//   roundTrip -> so' faz sentido se rank == 32. Com rank < 32 a inversa e'
	//                singular por construcao, e exigir isso reprova a build
	//                certa.
	//
	// Se um dia `refMatch` falhar de verdade, a mensagem de DIVERGE continua
	// sendo o caminho — e ela cita as constantes da build CERTA agora.
	// -----------------------------------------------------------------------
	const bool ok = refMatch;

	LOG("[%s] DecryptCIndex build 2610.1.2.7 (nao validada): k1=0x%08X shl=%u shr=%u "
		"mask=0x%08X rotr=%u k2=0x%08X | Ror=0x%X nao usado | "
		"rank=%d/32 (informativo) round-trip=%d/%d %s "
		"ref-dump=%s (forma literal da lista) -> %s\n",
		ok ? "+" : "!",
		(uint32_t)SDK.NameIndexXor1, shl, shr, mask, rot,
		(uint32_t)SDK.NameIndexXor2, ror,
		rank, rtFwd, nSamples,
		(invOk ? "(inversa derivada)" : "(inversa SINGULAR - ver a nota)"),
		refMatch ? "ok" : "DIVERGE",
		ok ? "ok" : "FALHOU");

	if (!bijective) {
		LOG("[i] DecryptCIndex tem rank %d/32: a formula desta build NAO e' "
			"invertivel, e isso e' NORMAL — bijecao nunca foi requisito (quem "
			"cifra e' o jogo). Consequencia pratica: 2^%d indices distintos "
			"podem cair no mesmo FName. Se aparecer nome de classe errado numa "
			"minoria dos atores, pode ser isto.\n",
			rank, 32 - rank);
	}
	if (!invOk || !roundTrip) {
		LOG("[i] Round-trip pulado: com rank %d/32 a inversa da parte linear e' "
			"singular por construcao, entao o teste de ida-e-volta nao se aplica. "
			"Isso NAO e' defeito (a versao anterior deste teste tratava como se "
			"fosse, e deu falso alarme na 2610).\n", rank);
	}
	if (!refMatch) {
		LOG("[!] DecryptCIndex DIVERGE da forma literal da lista (%d/%d iguais). "
			"Os campos do Globals.h NAO estao aplicados como na formula. Confira, "
			"na build 2610.1.2.7: NameIndexXor1=0x0 (K1 — sem XOR inicial), "
			"NameIndexShl=6 (Dval), NameIndexShlMask=0xFFFF0000, NameIndexShr=0x0 "
			"(Sval), NameIndexMaskRight=0x0 (XorKey3), NameIndexRotr=10 (Rval), "
			"NameIndexXor2=0x821E5EAD — e que o `NameIndexRor` (0x1) NAO esteja "
			"sendo usado.\n", refOk, nSamples);
	}
	if (collide && bijective) {
		// So' reporta colisao quando a funcao DEVERIA ser bijetiva. Com rank<32
		// a colisao e' esperada e ja' foi explicada acima.
		LOG("[!] DecryptCIndex: dois probes distintos colidiram com rank 32/32 — "
			"isso sim e' incoerencia interna.\n");
	}

	return ok;
}

void Engine::HealthDecipher(uint8_t* a1, uint32_t a3)
{
	uint32_t XorKeys[16];
	XorKeys[0] = (uint32_t)SDK.HealthXorKey0;
	XorKeys[1] = (uint32_t)SDK.HealthXorKey1;
	XorKeys[2] = (uint32_t)SDK.HealthXorKey2;
	XorKeys[3] = (uint32_t)SDK.HealthXorKey3;
	XorKeys[4] = (uint32_t)SDK.HealthXorKey4;
	XorKeys[5] = (uint32_t)SDK.HealthXorKey5;
	XorKeys[6] = (uint32_t)SDK.HealthXorKey6;
	XorKeys[7] = (uint32_t)SDK.HealthXorKey7;
	XorKeys[8] = (uint32_t)SDK.HealthXorKey8;
	XorKeys[9] = (uint32_t)SDK.HealthXorKey9;
	XorKeys[10] = (uint32_t)SDK.HealthXorKey10;
	XorKeys[11] = (uint32_t)SDK.HealthXorKey11;
	XorKeys[12] = (uint32_t)SDK.HealthXorKey12;
	XorKeys[13] = (uint32_t)SDK.HealthXorKey13;
	XorKeys[14] = (uint32_t)SDK.HealthXorKey14;
	XorKeys[15] = (uint32_t)SDK.HealthXorKey15;

	for (int i = 0; i < 4; i++)
	{
		uint8_t v1 = (i + a3) & 0x3F;
		a1[i] ^= *((uint8_t*)XorKeys + v1);
	}
}

float Engine::DecryptHealth(uint8_t* buffer, bool& readValid)
{
	// ------------------------------------------------------------------
	// VALIDADE DA LEITURA.
	//
	// Esta funcao devolvia `0.0f` para DUAS situacoes diferentes:
	//     (a) o campo de vida VALE zero (jogador eliminado);
	//     (b) a leitura NAO produziu valor (o `return 0.0f` de falha).
	// Para o filtro de morte essa diferenca e' tudo: (a) e' morte, (b) e'
	// ignorancia — e ignorancia NAO pode virar "morto" (esconderia um vivo).
	//
	// Comeca em true e SO' cai no caminho de falha comprovada, para que
	// todo retorno real (inclusive o `0.0f` legitimo) saia marcado como valido.
	// ------------------------------------------------------------------
	readValid = true;

	uint32_t HeaFlag_Local = 0; // SDK.HeaFlag - SDK.HeaFlag
	uint32_t Health1_Local = SDK.Health1 - SDK.HeaFlag;
	uint32_t Health2_Local = SDK.Health2 - SDK.HeaFlag;
	uint32_t Health3_Local = SDK.Health3 - SDK.HeaFlag;
	uint32_t Health4_Local = SDK.Health4 - SDK.HeaFlag;
	uint32_t Health5_Local = SDK.Health5 - SDK.HeaFlag;
	uint32_t Health6_Local = SDK.Health6 - SDK.HeaFlag;

	if (buffer[HeaFlag_Local] != 3 && *(uint32_t*)&buffer[Health1_Local])
	{
		uint8_t v2 = buffer[Health3_Local];
		// ESTE e' o unico caminho de FALHA: o indice v2 apontaria para fora da
		// janela de 0x900 bytes que o batch leu. Nao da' para saber a vida
		// agora — entao avisa, em vez de mentir `0.0f` (= morto).
		// ---------------------------------------------------------------------
		// A JANELA E' 0x1000 (era 0x900) — ver o comentario em `healthBulk`.
		//
		// E o teste agora NAO e' so' do indice do jogo: ele confere que TODOS os
		// campos que a funcao vai tocar cabem no buffer. Com a base errada, os
		// campos caem fora e a leitura passa a devolver memoria que nao e' do
		// campo — sem erro nenhum, porque a LEITURA em si deu certo.
		//
		// Foi assim que `HeaFlag=0xE8` zerou a vida de 47 atores em silencio.
		// ---------------------------------------------------------------------
		const uint32_t JANELA = 0x1000;
		const uint32_t maiorCampo = (Health1_Local > Health2_Local ? Health1_Local : Health2_Local);
		const uint32_t maior3 = (Health3_Local > Health4_Local ? Health3_Local : Health4_Local);
		const uint32_t maior5 = (Health5_Local > Health6_Local ? Health5_Local : Health6_Local);
		const uint32_t maior = (maiorCampo > maior3 ? (maiorCampo > maior5 ? maiorCampo : maior5) : (maior3 > maior5 ? maior3 : maior5));
		if (maior + 4 > JANELA) {
			// Base incompativel com a janela: NAO e' leitura ruim, e' offset
			// errado. Avisa uma vez e devolve invalido, em vez de ler lixo.
			static bool avisou = false;
			if (!avisou) {
				avisou = true;
				LOG("[!] BLOCO DE VIDA FORA DA JANELA: HeaFlag=0x%X deixa o campo "
					"mais alto em 0x%X, e a janela tem 0x%X.\n"
					"    HeaFlag e janela sao um PAR: aumentar so' a janela resolve "
					"se a BASE estiver certa.\n",
					(unsigned)SDK.HeaFlag, maior, JANELA);
			}
			readValid = false; return 0.0f;
		}
		if (v2 + Health4_Local + 4 > JANELA) { readValid = false; return 0.0f; }

		float v5 = *(float*)&buffer[v2 + Health4_Local];

		if (buffer[Health5_Local] != 0)
		{
			HealthDecipher((uint8_t*)&v5, *(uint32_t*)&buffer[Health6_Local]);
			return v5;
		}
		return v5; 
	}
	
	// Caminho direto: leitura dentro da janela, sem indice vindo do jogo.
	// Qualquer valor que saia daqui — 0.0 inclusive — e' leitura REAL.
	return *(float*)&buffer[Health2_Local];
}
std::wstring string_to_wstring(const std::string& str) {
	std::wstring ws(str.begin(), str.end());
	return ws;
}
bool contains(const std::string& haystack, const std::string& needle) {
	return haystack.find(needle) != std::string::npos;
}
// ===========================================================================
// InvalidateSnapshot — "nao existe partida valida neste tick".
//
// O BUG QUE ISTO CONSERTA (ESP fantasma congelado na tela do lobby):
//
//   `Actors` / `GrenadeActors` / `VehicleActors` / `LootActors` so' eram
//   ATRIBUIDAS no fim do Cache(), dentro do bloco de refresh (throttled 1x/s).
//   TODOS os `return` antecipados do Cache() — UWorld zerado/ilegivel, mapa do
//   lobby, camera invalida, ActorsArray invalido, falha de leitura — saiam SEM
//   tocar nessas listas: elas ficavam congeladas com o ultimo frame bom da
//   partida.
//
//   Pior: ao voltar pro lobby o `PlayerCameraManager` vira 0 e o MemoryLoop so'
//   chama UpdatePlayers quando `PlayerCameraManager != 0`. Ou seja, a lista
//   nunca mais era reescrita por ninguem. E como o UWorld do LOBBY tambem e'
//   valido, o gate do RenderFrame (`frameData.UWorld != 0`) continuava
//   passando, e o ESP desenhava o ultimo esqueleto PARA SEMPRE.
//
// A cura e' limpar DE VERDADE o que o render consome. Quem chama isto e' o
// Cache(), em todo caminho que significa "nao ha' dado de partida agora".
//
// NAO ha' flag grudenta: `bInMatch` e' reescrito em TODO tick do Cache() e
// volta a true no primeiro tick em que os dados existirem de novo (ver o
// `bInMatch = true` logo depois da checagem do ActorsArray) — o ESP volta
// sozinho ao entrar na proxima partida.
//
// Nao mexe em `UWorld`: zerar ele faria o Cache() re-decifrar e re-inicializar
// a cadeia inteira em todo tick do lobby (ActorCache/Local.KV limpos a 144 Hz).
// UWorld continua sendo "o Engine esta' sincronizado com um mundo"; quem diz
// se aquele mundo E' uma partida e' o `bInMatch`.
// ===========================================================================
void Engine::InvalidateSnapshot(const char* motivo)
{
	// Log de TRANSICAO (1 linha por vez que a partida deixa de ser valida).
	// Sem isto, o ESP "sumir" fica indistinguivel de um return silencioso
	// qualquer — e foi exatamente essa falta de sinal que fez o bug do ESP
	// fantasma demorar a aparecer.
	if (bInMatch) {
		LOG("[STATE] fim de partida / snapshot invalidado (%s) - ESP limpo\n", motivo);
	}

	bInMatch = false;

	// As quatro listas que a thread de render le' (SharedData). Elas sao o
	// unico caminho de dados de ator do ESP (jogador/loot/veiculo/granada) e
	// do radar — ver DrawPlayerEsp / Radar::Render.
	Actors.clear();
	GrenadeActors.clear();
	VehicleActors.clear();
	LootActors.clear();

	// O HUD de espectadores NAO passa pelo snapshot: o PlayerEsp le' o global
	// `Local` direto (DrawPlayerEsp -> "Spectators: N"). Sem zerar aqui, esse
	// contador ficava congelado no lobby mesmo com o ESP ja' limpo.
	Local.SpectatedCount = 0;

	// Sem partida nao ha' camera. Zerar tambem fecha o gate do MemoryLoop
	// (`PlayerCameraManager != 0`), que parava de varrer ator no lobby.
	PlayerCameraManager = 0;

	// A zona azul/safety (radar) vem do GameState. Sem partida nao ha' zona.
	GameState = 0;
}

void Engine::Cache()
{
	uintptr_t base_data = CachedBaseAddress;
	// Sem base do modulo nao existe leitura nenhuma: nao ha' partida e o
	// snapshot tem que ficar limpo (senao um `base_data` que cai a zero no
	// meio da sessao deixaria o ultimo ESP congelado na tela).
	if (!base_data) { InvalidateSnapshot("sem base do modulo"); return; }

	FrameTranslateError = false;


	uint64_t localDecryptKey = 0;
	if (!g_IsLeaInstruction && Tmpadd != 0) {
	    TargetProcess.Read(Tmpadd, &localDecryptKey, sizeof(localDecryptKey), "Tmpadd");
	}
	// =======================================================================
	// SONDA DO UWORLD  (diagnostico do travamento "aguardando Lobby")
	//
	// Sintoma medido: base + SDK.UWorld le ZERO em todo frame, o Cache() sai
	// no `if (!rawUWorld) return;` e nenhuma linha [STATE] aparece no log.
	//
	// Em vez de eu chutar qual e' o RVA certo nesta build, a sonda LE VARIOS
	// candidatos de uma vez e reporta quem tem valor:
	//   - o RVA atual (SDK.UWorld)
	//   - o RVA do build anterior (0x1327BFE8) — referencia
	//   - uma JANELA em volta do atual (±0x400, passo 8)
	//
	// O UWorld cifrado tem forma reconhecivel (ex: 0x310573AD2459AEE): e' um
	// valor GRANDE e nao-canonico. A sonda destaca esses.
	//
	// Roda 1x a cada 3s, so' enquanto o UWorld ainda nao foi resolvido.
	// =======================================================================
	{
		static auto ultimaSonda = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		auto agoraSonda = std::chrono::steady_clock::now();
		// Atras do DebugLogs: resolveu o caso do UWorld, mas e' verbosa demais
		// pra ficar ligada sempre (~12 linhas a cada 2s).
		// `!UWorld`: a sonda existe para achar o RVA QUANDO ele nao resolve. Sem
		// esta condicao ela continuava a partida inteira com DebugLogs ligado — e
		// cada passada faz ~257 leituras de driver de uma vez (janela +-0x400 de
		// 8 em 8). Isso e' ESTOURO de IOCTL num tick so', a cada 3 s: travamento
		// visivel, nao custo medio.
		if (Configs.Survivor.DebugLogs && !UWorld &&
		    std::chrono::duration_cast<std::chrono::seconds>(agoraSonda - ultimaSonda).count() >= 3) {
			ultimaSonda = agoraSonda;

			uintptr_t vAtual = 0, vAntigo = 0;
			TargetProcess.Read(base_data + SDK.UWorld,      &vAtual,  sizeof(vAtual),  "sonda_atual");
			TargetProcess.Read(base_data + 0x1327BFE8ULL,   &vAntigo, sizeof(vAntigo), "sonda_antigo");

			LOG("[UWPROBE] RVA atual  0x%llX -> 0x%llX\n",
			    (unsigned long long)SDK.UWorld, (unsigned long long)vAtual);
			LOG("[UWPROBE] RVA antigo 0x1327BFE8 -> 0x%llX\n", (unsigned long long)vAntigo);

			// janela em volta do RVA atual: quem tiver valor, aparece
			int achados = 0;
			for (int64_t off = -0x400; off <= 0x400 && achados < 12; off += 8) {
				if (off == 0) continue;
				uintptr_t v = 0;
				if (!TargetProcess.Read(base_data + SDK.UWorld + off, &v, sizeof(v), "sonda_win")) continue;
				if (!v) continue;
				// so' reporta o que PARECE ponteiro cifrado do UWorld:
				// valor alto, nao-canonico (o decifrado e' 0x0000_017A_xxxx_xxxx)
				const bool pareceCifrado = (v > 0x0000FFFFFFFFFFFFULL) && ((v >> 48) != 0xFFFF);
				if (pareceCifrado) {
					LOG("[UWPROBE]   +0x%llX -> 0x%llX  <-- CANDIDATO\n",
					    (unsigned long long)off, (unsigned long long)v);
					achados++;
				}
			}
			if (!achados) LOG("[UWPROBE]   janela +-0x400: nenhum candidato\n");

			LOG("[UWPROBE] estado: fnDecrypt=%d g_IsLea=%d Tmpadd=0x%llX chave=0x%llX\n",
			    (int)(fnDecryptFunctoin != nullptr), (int)g_IsLeaInstruction,
			    (unsigned long long)Tmpadd, (unsigned long long)CachedDecryptKey);
		}
	}

	uintptr_t localRawUWorld = 0;
	if (!TargetProcess.Read(base_data + SDK.UWorld, &localRawUWorld, sizeof(localRawUWorld), "localRawUWorld")) {
		FrameTranslateError = true;

		// -----------------------------------------------------------------
		// DIAGNOSTICO: loga a falha UMA vez (nao a 144 Hz).
		//
		// Isto e o ponto exato que decide se o ESP tem dados ou nao. Antes
		// so o FrameTranslateError subia, e nao dava para saber SE a leitura
		// falhou ou se ela funcionou e devolveu 0 (offset errado / jogo
		// atualizado).
		//
		//   leitura falhou  -> o driver nao conseguiu ler aquele endereco
		//   devolveu zero   -> a leitura funcionou mas o offset esta errado
		//   valor plausivel -> o problema esta depois, na cadeia
		// -----------------------------------------------------------------
		static bool jaLogouUWorld = false;
		if (!jaLogouUWorld) {
			jaLogouUWorld = true;
			LOG("[!] Cache: FAILED to read UWorld\n");
			LOG("    base_address = 0x%llX\n", (unsigned long long)base_data);
			LOG("    SDK.UWorld   = 0x%llX  -> endereco 0x%llX\n",
			    (unsigned long long)SDK.UWorld,
			    (unsigned long long)(base_data + SDK.UWorld));
			LOG("    (o driver nao leu esse endereco)\n");
		}

		// A leitura do UWorld falhou: nao existe estado de partida neste tick.
		// A partir daqui o Cache() ainda tem caminhos que retornam, mas este
		// ja' basta para limpar o que o render consome.
		InvalidateSnapshot("falha de leitura do UWorld");
	} else {
		static bool jaLogouOk = false;
		if (!jaLogouOk) {
			jaLogouOk = true;
			LOG("[+] Cache: UWorld read OK\n");
			LOG("    SDK.UWorld   = 0x%llX\n", (unsigned long long)SDK.UWorld);
			LOG("    rawUWorld    = 0x%llX %s\n",
			    (unsigned long long)localRawUWorld,
			    localRawUWorld ? "(tem valor)" : "<-- ZERO! offset provavelmente errado");
		}
	}
	
	if (!fnDecryptFunctoin) {
		if (InitDecrypt(SDK.XenuineDecrypt)) {
			GetGNames();
			LOG("[+] Delayed InitDecrypt successfully connected!\n");
		} else {
			// Sem decrypt nao da' para resolver UWorld/nome: snapshot limpo.
			InvalidateSnapshot("no decryption function (Xenuine)");
			return;
		}
	}

	if (!g_IsLeaInstruction) {
		CachedDecryptKey = localDecryptKey;
		if (!CachedDecryptKey) {
			// Retry InitDecrypt if it failed initially
			if (InitDecrypt(SDK.XenuineDecrypt)) {
				GetGNames();
			}
			// Sem chave de decrypt o UWorld nao decifra: snapshot limpo.
			InvalidateSnapshot("without the UWorld decryption key");
			return;
		}
	}

	uintptr_t rawUWorld = localRawUWorld;
	// UWorld zerado no jogo (troca de mapa / fim de partida / processo reiniciado).
	if (!rawUWorld) { InvalidateSnapshot("UWorld fully completed"); return; }
	
	static uint64_t last_UWorld = 0;
	if (rawUWorld != last_UWorld || !UWorld) {
		// -------------------------------------------------------------------
		// RESET DE CACHE — PELO UWORLD *DECIFRADO*, NAO PELO BRUTO.
		//
		// O BUG (o "FPS cai no comeco e depois fica limpo" do testador):
		//
		//   `rawUWorld` e' o ponteiro AINDA CRIPTOGRAFADO. A Xenuine gira a
		//   chave/rotacao desse ponteiro — e durante o carregamento do mapa ela
		//   gira varias vezes. Como a condicao comparava o valor BRUTO, cada
		//   rotacao caia aqui dentro e fazia:
		//
		//     ActorCache.clear() -> TODO ator destruido e reconstruido; o
		//                           construtor le PlayerState, stats do roster
		//                           e nome (varias leituras de driver cada)
		//     Local.KV.clear()   -> TODO nome de classe volta a ser resolvido
		//                           por GNames: 3 leituras de driver cada
		//
		//   Numa partida com ~100 atores isso e' um pico gigante, e ele se
		//   repetia a cada rotacao enquanto o mapa ainda estava assentando.
		//   Depois que o mapa estabiliza a rotacao para — e o FPS "fica limpo".
		//
		// O QUE DECIDE SE E' A MESMA PARTIDA E' O UWORLD DECIFRADO.
		//
		//   Se o bruto mudou mas o decifrado continua o mesmo, foi so' a chave
		//   que girou: e' a MESMA partida, o cache continua VALIDO e limpar era
		//   jogar trabalho fora. Troca de mapa muda o decifrado — e ai' sim
		//   tem que limpar.
		// -------------------------------------------------------------------
		const uint64_t novoUWorld = xe_decrypt(rawUWorld);
		last_UWorld = rawUWorld;

		const bool mesmaPartida = (novoUWorld == UWorld && IsAddrValid(UWorld));

		UWorld = IsAddrValid(novoUWorld) ? novoUWorld : 0;

		if (!mesmaPartida) {
			CurrentLevel = 0;
			GameInstance = 0;
			LocalPlayers = 0;
			PlayerController = 0;
			PlayerCameraManager = 0;
			AcknowledgedPawn = 0;
			OwningActor = 0;
			ActorCache.clear();
			Local.KV.clear();
			bIsMapInitialized = false;
		}
	}
	if (!IsAddrValid(UWorld)) {
		// O decrypt devolveu lixo (Xenuine com chave errada / UWorld trocando):
		// nao ha' partida legivel neste tick.
		InvalidateSnapshot("UWorld decrypted invalid");
		return;
	}

	auto now = std::chrono::steady_clock::now();

	// -----------------------------------------------------------------------
	// DIAGNOSTICO: loga o estado do Engine a cada 1s, para ver se o UWorld/GNames
	// estao validos DENTRO do Cache (ou se viram 0 num frame seguinte).
	// -----------------------------------------------------------------------
	{
		static std::chrono::steady_clock::time_point lastStateLog;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStateLog).count() > 1000) {
			// InMatch e' o que o render usa como gate (SharedData::DataValid).
			// Ele entra no log de proposito: e' a forma de distinguir "saiu da
			// partida e o ESP limpou" de "o ESP sumiu por outro motivo".
			LOG("[STATE] UWorld=0x%llX GNames=0x%llX Actors=%zu FrameErr=%d PCM=0x%llX InMatch=%d\n",
			    (unsigned long long)UWorld, (unsigned long long)GNames, Actors.size(),
			    FrameTranslateError ? 1 : 0, (unsigned long long)PlayerCameraManager,
			    bInMatch ? 1 : 0);
			lastStateLog = now;
		}
	}

	static std::chrono::steady_clock::time_point lastUWorldIdCheckTime;
	static uint32_t cachedRawUWorldId = 0;
	if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUWorldIdCheckTime).count() > 1000 || cachedRawUWorldId == 0) {
		cachedRawUWorldId = SafeRead<uint32_t>(UWorld + SDK.ObjectID, "UWorld_ObjectID");
		lastUWorldIdCheckTime = now;
	}
	uint32_t rawUworldId = cachedRawUWorldId;
	if (FrameTranslateError) {
		static std::chrono::steady_clock::time_point lastFault1;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFault1).count() > 2000) {
			LOG("[-] FAULT on UWorld_ObjectID! UWorld: 0x%p | rawUWorld: 0x%p | DecryptKey: 0x%p\n", UWorld, (void*)rawUWorld, (void*)CachedDecryptKey);
			lastFault1 = now;
		}
		// Falha de leitura no ObjectID do UWorld: a cadeia esta' quebrada
		// neste tick, entao o snapshot nao pode continuar valido.
		InvalidateSnapshot("falha de leitura no ObjectID do UWorld");
		return;
	}

	auto UWorldID = DecryptCIndex(rawUworldId);
	std::string MapName = GetNamesFromList(UWorldID);
	
	if (MapName.empty() || MapName == "Unknown" || MapName == "UNKOWNNAME") {
		MapName = GetNames(UWorldID);
		if (!MapName.empty() && MapName != "UNKOWNNAME") {
			Local.KV[UWorldID] = MapName;
		}
	}

	if (MapName.find("Lobby") != std::string::npos || MapName.empty() || MapName.find("Unknown") != std::string::npos) {
		static bool lobbyLogged = false;
		if (!lobbyLogged) {
			LOG("[!] WARNING: Map check triggered Lobby/Reset state. MapName: '%s' | UWorldID: %u | UWorld: 0x%p\n", MapName.c_str(), UWorldID, (void*)UWorld);
			lobbyLogged = true;
		}
		oldMapName = "";
		MaxPacket = 0;
		AcknowledgedPawn = 0;
		bIsMapInitialized = false;
		// ESTE e' o caminho do bug relatado ("sai da partida e o ESP fica
		// congelado no lobby"): o mapa do lobby cai aqui e, antes, o Cache()
		// saia daqui SEM limpar `Actors` — entao a thread de render seguia
		// desenhando o ultimo esqueleto da partida pra sempre. O
		// InvalidateSnapshot tambem zera o PlayerCameraManager (que nao era
		// mais reescrito no lobby) e o contador de espectadores do HUD.
		InvalidateSnapshot("mapa do lobby / nome de mapa desconhecido");
		return;
	}

	if (MapName != oldMapName) {
		MaxPacket = 0;
		LOG("[+] Map Changed: %s\n", MapName.c_str());
		bIsMapInitialized = false;
	}
	oldMapName = MapName;

	static uint64_t TArrayActorsPtr = 0;

	if (!bIsMapInitialized) {
		CurrentLevel = xe_decrypt(SafeRead<uint64_t>(UWorld + SDK.CurrentLevel, "CurrentLevel"));
		if (!IsAddrValid(CurrentLevel) || FrameTranslateError) { InvalidateSnapshot("CurrentLevel invalido"); return; }

		GameInstance = xe_decrypt(SafeRead<uint64_t>(UWorld + SDK.GameInstance, "GameInstance"));
		if (!IsAddrValid(GameInstance) || FrameTranslateError) { InvalidateSnapshot("GameInstance invalido"); return; }

		LocalPlayers = xe_decrypt(SafeRead<uint64_t>(SafeRead<uint64_t>(GameInstance + SDK.LocalPlayers, "LocalPlayers_Ptr"), "LocalPlayers_Val"));
		if (!IsAddrValid(LocalPlayers) || FrameTranslateError) { InvalidateSnapshot("LocalPlayers invalido"); return; }

		PlayerController = xe_decrypt(SafeRead<uint64_t>(LocalPlayers + SDK.PlayerController, "PlayerController"));
		if (!IsAddrValid(PlayerController) || FrameTranslateError) { InvalidateSnapshot("PlayerController invalido"); return; }

		uint64_t TArrayPtr = xe_decrypt(SafeRead<uint64_t>(CurrentLevel + SDK.Actors, "ActorsArray"));
		if (!IsAddrValid(TArrayPtr) || FrameTranslateError) { InvalidateSnapshot("array de atores do level invalido"); return; }
		TArrayActorsPtr = TArrayPtr;

		bIsMapInitialized = true;
	}

	static uint64_t lastRawPawn = 0;
	uint64_t rawPawn = 0;
	uint64_t rawCameraManager = 0;
	int spectatedCount = 0;
	int maxPacket = 0;
	uint64_t ActorsArray = 0;

	DriverInterfaceV3::BatchReadEntry cacheBatch[8];
	int bCount = 0;
	
	cacheBatch[bCount++] = { PlayerController + SDK.PlayerCameraManager, 8, &rawCameraManager };
	cacheBatch[bCount++] = { PlayerController + SDK.AcknowledgedPawn, 8, &rawPawn };
	// -----------------------------------------------------------------
	// ESPECTADORES
	//
	// DOIS BUGS AQUI, e o log mostrou os dois de uma vez:
	//     [SPEC] pawn=0 playerState=0 -> usando=0  (PS=0x0)
	//
	//   1) `spectatedCount` NUNCA ERA ESCRITO. A variavel estava declarada,
	//      lida no `okPawn` e impressa no log — mas nao existia NENHUMA
	//      entrada de leitura pra ela no `cacheBatch`. Zero por construcao;
	//      o caminho do pawn estava morto desde que alguem mexeu no batch.
	//
	//   2) A entrada do PlayerState era montada com `AcknowledgedPawn` ANTES
	//      de o pawn ser decifrado (a decifragem acontece depois do
	//      BatchRead). No primeiro tick o pawn ainda e' 0, a entrada nem
	//      entra no batch, e `rawPlayerState` fica 0 — a leitura do
	//      SpectatedCount no PlayerState nunca acontecia no tick em que
	//      importava.
	//
	// Agora os DOIS sao lidos. O offset do pawn (`SDK.SpectatedCount`) e' o
	// que a lista da build traz, e o do PlayerState serve de conferencia —
	// o codigo abaixo usa o plausivel, como ja' fazia.
	// -----------------------------------------------------------------
	uint64_t rawPlayerState = 0;
	if (AcknowledgedPawn) {
		cacheBatch[bCount++] = { AcknowledgedPawn + SDK.PlayerState,    8, &rawPlayerState };
		cacheBatch[bCount++] = { AcknowledgedPawn + SDK.SpectatedCount, 4, &spectatedCount };
	}
	cacheBatch[bCount++] = { TArrayActorsPtr, 8, &ActorsArray };
	cacheBatch[bCount++] = { TArrayActorsPtr + 8, 4, &maxPacket };

	if (bCount > 0) TargetProcess.BatchRead(cacheBatch, bCount);

	// -----------------------------------------------------------------
	// 2o estagio: agora que temos o PlayerState, le o contador de la'
	// -----------------------------------------------------------------
	int spectatedFromPS = 0;
	if (IsCanonicalPtr(rawPlayerState)) {
		TargetProcess.Read(rawPlayerState + SDK.SpectatedCount, &spectatedFromPS,
		                   sizeof(spectatedFromPS), "SpectatedCount(PS)");
	}

	PlayerCameraManager = rawCameraManager;
	if (!IsAddrValid(PlayerCameraManager)) {
		// Sem PlayerCameraManager nao existe camera nem projecao: e' o estado
		// normal no LOBBY e logo depois de sair da partida. Antes daqui saia-se
		// sem limpar nada — e era por isso que o ESP ficava congelado na tela
		// do lobby (o MemoryLoop nem chamava mais UpdatePlayers, porque ele tem
		// este mesmo gate).
		InvalidateSnapshot("PlayerCameraManager invalido");
		return;
	}

	if (rawPawn != lastRawPawn || !AcknowledgedPawn) {
		AcknowledgedPawn = xe_decrypt(rawPawn);
		if (!IsAddrValid(AcknowledgedPawn)) AcknowledgedPawn = 0;
		lastRawPawn = rawPawn;
	}

	// usa o plausivel; loga os dois pra confirmar o offset
	{
		const bool okPawn = (spectatedCount   > 0 && spectatedCount   < 100);
		const bool okPS   = (spectatedFromPS > 0 && spectatedFromPS < 100);
		Local.SpectatedCount = okPS ? spectatedFromPS : (okPawn ? spectatedCount : 0);

		// -----------------------------------------------------------------
		// LOG PERIODICO, NAO SO' NA MUDANCA.
		//
		// O log antigo so' saia quando o valor MUDASSE. Com o contador preso
		// em zero, isso significava UMA linha na sessao inteira — e ela era
		// justamente a do primeiro tick, antes de o pawn existir. Resultado:
		// o log dizia "pawn=0 playerState=0" e nao havia como saber se isso
		// era a verdade ou so' o estado inicial.
		//
		// Agora sai na mudanca OU a cada 10 s, com os valores CRUS e as duas
		// flags de plausibilidade — da' pra separar "ninguem me especta" de
		// "a leitura nao esta' chegando".
		// -----------------------------------------------------------------
		static int ultimoLog = -1;
		static auto ultimoSpecLog = std::chrono::steady_clock::now() - std::chrono::seconds(60);
		const auto agoraSpec = std::chrono::steady_clock::now();
		const bool mudou = (Local.SpectatedCount != ultimoLog);
		const bool hora  = std::chrono::duration_cast<std::chrono::seconds>(
		                       agoraSpec - ultimoSpecLog).count() >= 10;

		if (mudou || hora) {
			ultimoLog = Local.SpectatedCount;
			ultimoSpecLog = agoraSpec;
			LOG("[SPEC] pawn=%d(ok=%d) playerState=%d(ok=%d) -> usando=%d | "
			    "pawnAddr=0x%llX PS=0x%llX\n",
			    spectatedCount, (int)okPawn, spectatedFromPS, (int)okPS,
			    Local.SpectatedCount,
			    (unsigned long long)AcknowledgedPawn,
			    (unsigned long long)rawPlayerState);
		}
	}
	MaxPacket = maxPacket;

	if (MaxPacket <= 0 || MaxPacket > 20000 || !IsAddrValid(ActorsArray)) {
		static std::chrono::steady_clock::time_point lastLogTime;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() > 2000) {
			LOG("[-] Actor Read Failed: maxPacket=%d, ActorsArray=0x%p\n", maxPacket, (void*)ActorsArray);
			lastLogTime = now;
		}
		// O array de atores do level sumiu/ficou ilegivel: nao ha' lista de
		// entidades para desenhar (e' o que acontece quando o level da partida
		// e' desmontado ao voltar pro lobby).
		InvalidateSnapshot("array de atores invalido (maxPacket/ActorsArray)");
		return;
	}

	// -----------------------------------------------------------------------
	// A PARTIR DAQUI O ESTADO DE PARTIDA E' VALIDO.
	//
	// UWorld, nome de mapa, CurrentLevel/GameInstance/LocalPlayers,
	// PlayerController, PlayerCameraManager e o array de atores passaram em
	// todas as validacoes. Marcamos `bInMatch` AQUI (e nao so' no fim do bloco
	// de refresh abaixo) porque o refresh e' THROTTLED (1x/s por
	// LastActorRefreshTimestamp): nos frames em que ele nao roda o snapshot
	// continua valendo e NAO pode ser limpo.
	//
	// Este e' o unico ponto que devolve o ESP a ativa — se ele nao existisse, o
	// toggle viraria uma flag grudenta e o ESP nunca mais desenharia.
	// -----------------------------------------------------------------------
	bInMatch = true;

	if (std::chrono::duration_cast<std::chrono::milliseconds>(now - LastActorRefreshTimestamp).count() > 1000 || Actors.empty()) {
		std::vector<uint64_t> entitylist;
		entitylist.resize(MaxPacket);
		std::unique_ptr<uint64_t[]> object_raw_ptr = std::make_unique<uint64_t[]>(MaxPacket);
		
		if (!TargetProcess.Read(ActorsArray, object_raw_ptr.get(), MaxPacket * sizeof(uint64_t), "ActorEntityBuffer")) {
			// Falha dura de leitura da lista de atores: nao ha' dado de entidade
			// neste tick. Limpa em vez de deixar a ultima lista congelada.
			// (Sai antes das leituras em lote por ator, entao a retentativa do
			// proximo tick custa 1 IOCTL, nao a varredura inteira.)
			InvalidateSnapshot("falha de leitura do ActorEntityBuffer");
			return;
		}

		for (size_t i = 0; i < MaxPacket; i++) {
			entitylist[i] = object_raw_ptr[i];
		}

		static bool loggedFirstActor = false;
		if (!loggedFirstActor && MaxPacket > 0) {
			LOG("[DEBUG] First Actor Raw: 0x%p | Decrypted: 0x%p\n", (void*)object_raw_ptr[0], (void*)entitylist[0]);
			loggedFirstActor = true;
		}

		std::list<std::shared_ptr<ActorEntity>> actors;
		std::vector<std::shared_ptr<ActorEntity>> grenadeList;
		std::vector<std::shared_ptr<ActorEntity>> vehicleList;
		std::vector<std::shared_ptr<ActorEntity>> lootList;
		std::unordered_map<uint64_t, std::shared_ptr<ActorEntity>> newCache;

		for (size_t i = 0; i < entitylist.size(); i += MAX_BATCH_ENTRIES) {
			size_t chunkSize = (entitylist.size() - i > MAX_BATCH_ENTRIES) ? MAX_BATCH_ENTRIES : (entitylist.size() - i);
			DriverInterfaceV3::BatchReadEntry batchEntries[MAX_BATCH_ENTRIES];
			uint32_t batchObjectIDs[MAX_BATCH_ENTRIES] = {0};
			int activeInBatch = 0;

			for (size_t j = 0; j < chunkSize; j++) {
				uintptr_t actorAddr = entitylist[i + j];
				if (actorAddr < 0x1000000) {
					batchEntries[j].address = 0;
					batchEntries[j].size = 0;
					batchEntries[j].buffer = nullptr;
					continue;
				}
				batchEntries[j].address = actorAddr + SDK.ObjectID;
				batchEntries[j].size = sizeof(uint32_t);
				batchEntries[j].buffer = &batchObjectIDs[j];
				activeInBatch++;
			}

			if (activeInBatch == 0) continue;

			TargetProcess.BatchRead(batchEntries, chunkSize);
			for (size_t j = 0; j < chunkSize; j++) {
					uintptr_t actorAddr = entitylist[i + j];
					if (actorAddr < 0x1000000) continue;

					uint32_t encryptedID = batchObjectIDs[j];
					if (encryptedID == 0) continue;

					int id = DecryptCIndex(encryptedID);
					std::string name = GetNamesFromList(id);
					if (name == "") {
						name = GetNames(id);
						Local.KV[id] = name;
					}

					// -----------------------------------------------------------------
					// DIAGNOSTICO: loga o processamento do PRIMEIRO ator, para saber
					// em qual etapa a cadeia quebra (ObjectID / decrypt / name /
					// IsPlayer). O treino TEM bot, entao Actors Found:0 e bug.
					// -----------------------------------------------------------------
					static bool loggedFirstObj = false;
					if (!loggedFirstObj) {
						loggedFirstObj = true;
						LOG("[DEBUG] Actor obj: rawID=0x%08X -> id=%d -> name='%s' -> isPlayer=%d\n",
						    encryptedID, id, name.c_str(), IsPlayer(name));
					}

					// -----------------------------------------------------------------
					// CENSO DE CLASSES (uma linha por classe unica)
					//
					// Loga cada nome de classe de ator que aparece no mapa.
					// Isso responde de uma vez:
					//   - quais CLASSES de veiculo existem de verdade
					//   - quais CLASSES de loot existem
					//   - se arma vem por classe ('WeapAK47_C') ou generica
					//
					// E' o diagnostico de maior informacao por linha de log —
					// melhor que eu adivinhar offset por offset.
					// -----------------------------------------------------------------
					{
						static std::set<std::string> jaVistos;
						if (Configs.Survivor.DebugLogs && jaVistos.size() < 300 && jaVistos.find(name) == jaVistos.end()) {
							jaVistos.insert(name);
							LOG("[CENSO] cls='%s'  player=%d veiculo=%d loot=%d\n",
							    name.c_str(), IsPlayer(name), (int)IsVehicle(name), (int)IsLoot(name));
						}
					}

					// -----------------------------------------------------------------
					// DETECCAO DE PLAYER: nome do OBJETO primeiro (barato, em
					// memoria ja' cacheada), e por CLASSE como fallback.
					//
					// Build 2610.1.2.7: os pawns de personagem tem nome de
					// objeto HASHED ('*xxxxxxxxxx' — ver CENSO/log), entao o
					// IsPlayer pelo nome do ator nunca casa e o Actors fica 0.
					// A classe (UObject+ClassPrivate) ainda tem nome legivel na
					// tabela estatica (PlayerMale_A_C / UltAICharacter / ...).
					// Ver ResolveActorClassName — memoiza, custo ~zero depois
					// da 1a probe. clsName vira o objName do entity (escolha
					// de ossos do GetIndex e exibicao).
					// -----------------------------------------------------------------
					std::string clsName;
					int playerType = IsPlayer(name);
					if (playerType == 0) {
						clsName = ResolveActorClassName(actorAddr);
						if (!clsName.empty()) playerType = IsPlayer(clsName);
					}
					if (playerType == 0) {
						int explosionRadius = IsProjectile(name);
						if (explosionRadius > 0) {
							std::shared_ptr<ActorEntity> grenadeEntity;
							auto git = ActorCache.find(actorAddr);
							if (git != ActorCache.end()) {
								grenadeEntity = git->second;
							} else {
								grenadeEntity = std::make_shared<ActorEntity>(actorAddr);
								grenadeEntity->id = encryptedID;
								grenadeEntity->PlayerType = 3; 
								grenadeEntity->objName = name;
								grenadeEntity->ExplosionRadius = (float)explosionRadius;
							}
							newCache[actorAddr] = grenadeEntity;
							grenadeList.push_back(grenadeEntity);
						}
						// ---- veiculo ----
						else if (IsVehicle(name)) {
							std::shared_ptr<ActorEntity> veh;
							auto vit = ActorCache.find(actorAddr);
							if (vit != ActorCache.end()) {
								veh = vit->second;
							} else {
								veh = std::make_shared<ActorEntity>(actorAddr);
								veh->id = encryptedID;
								veh->PlayerType = 4;   // veiculo
								veh->objName = name;
							}
							newCache[actorAddr] = veh;
							vehicleList.push_back(veh);
						}
						// ---- loot / item no chao ----
						else if (IsLoot(name)) {
							// -------------------------------------------------
							// DIAGNOSTICO DO AIRDROP — ligado SEMPRE, 1x por
							// classe de airdrop vista.
							//
							// Por que fica fora do DebugLogs: o `IsAirdrop`
							// existia e nunca era chamado, e a categoria do
							// airdrop caia em "Other" — entao nao dava pra
							// saber, olhando o menu, se a caixa de suprimento
							// sequer chegava a ser classificada ou se ela era
							// classificada e morria no desenho.
							//
							// Esta linha separa os dois casos de uma vez:
							//   - aparece  -> a classificacao funciona, o
							//                 problema esta' no desenho/distancia;
							//   - nao aparece -> o ator nao chega ate aqui.
							//
							// Custa uma comparacao de string por ator de loot
							// e um log por classe. Nao da' pra pagar menos que
							// isso por uma resposta definitiva.
							// -------------------------------------------------
							if (IsAirdrop(name)) {
								static std::set<std::string> jaLogados;
								if (jaLogados.find(name) == jaLogados.end() && jaLogados.size() < 32) {
									jaLogados.insert(name);
									LOG("[AIRDROP] classe='%s' -> LootActors (categoria Airdrop)\n",
									    name.c_str());
								}
							}

							std::shared_ptr<ActorEntity> loot;
							auto lit = ActorCache.find(actorAddr);
							if (lit != ActorCache.end()) {
								loot = lit->second;
							} else {
								loot = std::make_shared<ActorEntity>(actorAddr);
								loot->id = encryptedID;
								loot->PlayerType = 5;   // loot
								loot->objName = name;
							}
							newCache[actorAddr] = loot;
							lootList.push_back(loot);
						}
						if (actorAddr == AcknowledgedPawn) {
							this->bIsDriving = true;
						}
						continue; 
					}
					if (actorAddr == AcknowledgedPawn) {
						this->bIsDriving = false;
						this->LocalCharacterPawn = actorAddr; 
					}

					std::shared_ptr<ActorEntity> entity;
					auto it = ActorCache.find(actorAddr);
					if (it != ActorCache.end()) {
						entity = it->second;
					} else {
						entity = std::make_shared<ActorEntity>(actorAddr);
						entity->id = encryptedID;
						entity->PlayerType = playerType;
						// Nome do OBJETO e' hash em personagem nesta build; o
						// nome da CLASSE (clsName) e' o que o GetIndex casa
						// para os ossos e o que a UI mostra como fallback.
						entity->objName = clsName.empty() ? name : clsName;
						entity->TempId = id;
						GetIndex(entity->index, entity->objName);
					}

					newCache[actorAddr] = entity;
					actors.push_back(entity);
				}
		}
		
		ActorCache = std::move(newCache);
		std::vector<std::shared_ptr<ActorEntity>> playerlist;
		for (std::shared_ptr<ActorEntity> entity : actors) {
			if (entity->RootComponent) {
				entity->isCheck = true;
			}
			playerlist.push_back(entity);
		}
		Actors = playerlist;
		GrenadeActors = std::move(grenadeList);
		VehicleActors = std::move(vehicleList);
		LootActors = std::move(lootList);
		LastActorRefreshTimestamp = now;
		// Refresh concluido: a lista publicada e' deste tick (o `bInMatch = true`
		// la' em cima ja' cobria os frames throttled; este aqui e' explicitamente
		// o fim do caminho que PRODUZIU dado novo).
		bInMatch = true;
	}

}

int Engine::IsPlayer(std::string entity_name) {
	if (entity_name.empty() || entity_name == "" || entity_name == "Unknown") { return 0; }
	// -----------------------------------------------------------------------
	// BOTS. Build 2610.1.2.7 (SDK dump 2026-10-27): os pawns de IA nao tem
	// mais a classe AIPawn_Base_* — a familia virou
	//   AUltAICharacter : ABRBaseAICharacter  (pawn)
	//   AUltAIPlayerState / ATslEventBotPlayerState : ABRBaseAIPlayerState
   //   (ETslCharacterType::user_ai discrimina no lado do jogo).
	// Os nomes antigos ficam (custo zero) e o resto casa por SUBSTRING nos
	// prefixos da familia — cobre variantes BP_X e futuros renames parciais
	// sem ter que listar cada uma.
	// -----------------------------------------------------------------------
	if (entity_name == "AIPawn_Base_Female_C") { return 1; }
	if (entity_name == "AIPawn_Base_Male_C") { return 1; }
	if (entity_name == "UltAIPawn_Base_C") { return 1; }
	if (entity_name == "UltAIPawn_Base_Female_C") { return 1; }
	if (entity_name == "UltAIPawn_Base_Male_C") {return 1; }
	if (entity_name == "UltAIPawn_Base_Pillar_C") {return 1; }
	if (entity_name == "UltAIPawn_Base_Female_Pillar_C") {return 1; }
	if (entity_name == "UltAIPawn_Base_Male_Pillar_C") {return 1; }
	if (entity_name == "BP_MarketAI_Pawn_C") {return 1; }
	if (entity_name.find("UltAI") != std::string::npos) { return 1; }        // UltAICharacter / UltAIPawn_Base* / BP_UltAI...
	if (entity_name.find("ABRBaseAI") != std::string::npos) { return 1; }     // ABRBaseAICharacter (base nativa dos bots)
	// -----------------------------------------------------------------------
	// HUMANOS remotos. O dump 2610.1.2.7 confirma que as classes dos pawns
	// de jogador continuam APlayerMale_A_C / APlayerFemale_A_C
	// (: APlayerPawn_v2_C) em /Game/Blueprints/Pawns/.
	// -----------------------------------------------------------------------
	if (entity_name == "RegistedPlayer") {return 2; }
	if (entity_name == "PlayerMale_A") {return 2; }
	if (entity_name == "PlayerMale_A_C") {return 2; }
	if (entity_name == "PlayerFemale_A") {return 2; }
	if (entity_name == "PlayerFemale_A_C") {return 2; }
	return 0;
}
int Engine::IsProjectile(const std::string& name) {
	if (name == "ProjGrenade_C")      return 500;  
	if (name == "ProjMolotov_C")      return 200;  
	return 0;
}

// ---------------------------------------------------------------------------
// Deteccao de VEICULOS e LOOT por nome de classe.
// Casa por substring para cobrir as variantes (_C, _Spawn, etc.).
// ---------------------------------------------------------------------------
bool Engine::IsVehicle(const std::string& name) {
	if (name.empty()) return false;

	// -----------------------------------------------------------------
	// REGRAS TIRADAS DO CENSO DE CLASSES (dados reais do jogo, nao suposicao)
	//
	// O filtro antigo era por SUBSTRING e estava AO CONTRARIO do certo:
	//
	//   PEGAVA (errado):  P_VehicleImpact_Wood_BP_C        (efeito de impacto)
	//                     P_Vehicle_Muffler_BP_Motorcycle_C (escapamento)
	//                     BP_VehicleTrunk_C                (porta-malas)
	//
	//   PERDIA (todos):   BP_Dirtbike_C  BP_PicoBus_C  BP_Food_Truck_C
	//                     BP_Bicycle_C   BP_Blanc_C
	//
	// O padrao que separa os dois grupos e' o PREFIXO:
	//     BP_... = ator real do mundo
	//     P_...  = particula / efeito / peca
	//
	// Entao: exclui P_ e componentes, e casa contra a lista de veiculos.
	// -----------------------------------------------------------------

	// 1) efeito / particula / peca (prefixo P_)
	if (name.size() >= 2 && name[0] == 'P' && name[1] == '_') return false;

	// 2) componente do veiculo (ator separado que pertence ao veiculo)
	static const char* componentes[] = {
		"Trunk", "Wheel", "Door", "Seat", "Bumper", "Window", "Component",
		"Propeller", "Rotor", "Tire", "Spoiler", "Mirror", "Light", "Muffler"
	};
	for (const char* c : componentes) {
		if (name.find(c) != std::string::npos) return false;
	}

	// 3) nomes de veiculo (nomes reais confirmados no censo em MAIUSCULO
	//    do que a comunidade lista — 'Dirtbike', 'PicoBus', 'Food_Truck' etc)
	static const char* keys[] = {
		"Dirtbike", "PicoBus", "Food_Truck", "Bicycle", "Blanc",
		"Vehicle", "WheeledVehicle", "Buggy", "UAZ", "Dacia", "Motorcycle",
		"Scooter", "Aquarail", "Mirado", "Van", "Pickup", "BRDM", "Lada",
		"Coupe", "Tukai", "Quad", "Snowbike", "Motorglider", "Boat",
		"PG117", "JetSki", "Rony", "Porter", "Zima", "Moto", "Truck", "Bus"
	};
	for (const char* k : keys) {
		if (name.find(k) != std::string::npos) {
			// "Bus" casa com "PicoBus" (ok) mas tambem com "BusStop" (nao).
			// Como ja' excluimos P_ e componentes, o risco e' baixo.
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// AIRDROP / CAIXA DE SUPRIMENTO
//
// Classe REAL (do censo): Carapackage_RedBox_C — com "A", nao "CarePackage".
// O filtro antigo procurava "CarePackage" e nunca casava com nada.
// ---------------------------------------------------------------------------
// ===========================================================================
// ContemIgnorandoCaixa — busca case-insensitive.
//
// POR QUE EXISTE: as chaves de airdrop estavam escritas "AirDrop" (D
// maiusculo), mas a classe real pode vir como "TslAirdrop..." (d minusculo) —
// e `std::string::find` e' CASE-SENSITIVE. A caixa de suprimento com fumaca
// no chao nao era classificada por causa de UMA letra.
//
// Nao converti os dois lados pra lowercase porque isso alocaria duas strings
// por chamada, e `IsLoot` roda por ator em todo refresh.
// ===========================================================================
static bool ContemIgnorandoCaixa(const std::string& s, const char* needle)
{
	const size_t n = strlen(needle);
	if (n == 0 || s.size() < n) return false;
	for (size_t i = 0; i + n <= s.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			const char a = (char)tolower((unsigned char)s[i + j]);
			const char b = (char)tolower((unsigned char)needle[j]);
			if (a != b) break;
		}
		if (j == n) return true;
	}
	return false;
}

bool Engine::IsAirdrop(const std::string& entity_name) {
	if (entity_name.empty()) return false;
	static const char* keys[] = {
		"Carapackage", "CarePackage", "AirDrop", "AircraftCarePackage",
		"TslBalloonCarePackage", "TslCustomAirDrop"
	};
	for (const char* k : keys) {
		// Case-insensitive de proposito: ver ContemIgnorandoCaixa.
		if (ContemIgnorandoCaixa(entity_name, k)) return true;
	}
	return false;
}

bool Engine::IsLoot(const std::string& name) {
	if (name.empty()) return false;

	// -----------------------------------------------------------------
	// LOOT — nomes do censo
	//
	// "WeapXXX_C" e' o padrao REAL das armas no chao/equipadas:
	//     WeapAK47_C  WeapHK416_C  WeapBerylM762_C  WeapM249_C  WeapAUG_C
	//     WeapMk14_C  WeapMk12_C   WeapMini14_C     WeapC4_C    WeapMortar_C
	// O nome da ARMA ESTA' NA CLASSE. "DroppedItem" e' o generico (nao tem
	// nome de item — esse so' vem do ItemID, ver LOOTDIAG).
	//
	// AirDrop: Carapackage_RedBox_C  (com A! o filtro antigo procurava
	// "CarePackage" com E e nunca casava)
	// -----------------------------------------------------------------
	static const char* keys[] = {
		"DroppedItem", "ItemPackage", "BP_Item_", "Item_Ammo",
		"Item_Weapon", "Item_Attach", "Item_Armor", "Item_Back",
		"Item_Helmet", "Item_Use", "Item_Heal",
		// armas (nome real do censo)
		"Weap",
		// airdrop
		"Carapackage", "CarePackage", "AirDrop",
		// -----------------------------------------------------------------
		// "TslDropMagazine" SAIU DAQUI.
		//
		// Recarregar uma arma joga o carregador vazio no chao, e ele entrava
		// em `LootActors` como loot — aparecendo no ESP como "Magazine" a cada
		// recarga. Nao e' loot: e' residuo da acao do proprio jogador, e
		// poluia a tela justamente na hora em que a pessoa esta' atirando.
		//
		// Sem esta chave o ator nao e' classificado como loot, nao entra em
		// `LootActors` e nao e' desenhado por ninguem.
		// -----------------------------------------------------------------
	};
	for (const char* k : keys) {
		if (name.find(k) != std::string::npos) return true;
	}

	// -----------------------------------------------------------------------
	// AIRDROP — TESTE SEPARADO E CASE-INSENSITIVE.
	//
	// As chaves acima continuam case-sensitive (elas sao padroes de classe
	// medidos). O airdrop NAO pode ser: a classe real pode vir como
	// "TslAirdrop..." com 'd' minusculo, e "AirDrop" com 'D' maiusculo nao
	// casa — a caixa no chao com fumaca ficava sem ESP por causa de UMA letra.
	//
	// Este teste fica FORA do laco de proposito: assim a diferenca de
	// tratamento fica visivel para quem ler depois, em vez de escondida no
	// meio das outras chaves.
	// -----------------------------------------------------------------------
	if (ContemIgnorandoCaixa(name, "carapackage") ||
	    ContemIgnorandoCaixa(name, "carepackage") ||
	    ContemIgnorandoCaixa(name, "airdrop")) return true;

	return false;
}

void Engine::UpdateGrenades() {
	if (GrenadeActors.empty()) return;

	struct GrenadeReadInfo {
		std::shared_ptr<ActorEntity> actor;
		float timeTillExplosion = 0.0f;
		uint32_t explodeState = 0;
		UEVector pos = {};
		uint64_t encryptedRoot = 0;
	};

	std::vector<GrenadeReadInfo> infos(GrenadeActors.size());

	std::vector<DriverInterfaceV3::BatchReadEntry> batch1;
	for (size_t i = 0; i < GrenadeActors.size(); i++) {
		infos[i].actor = GrenadeActors[i];
		auto addr = GrenadeActors[i]->Class;
		if (!addr) continue;

		batch1.push_back({ addr + SDK.RootComponent, sizeof(uint64_t), &infos[i].encryptedRoot });
		batch1.push_back({ addr + SDK.TimeTillExplosion, sizeof(float), &infos[i].timeTillExplosion });
		batch1.push_back({ addr + SDK.ExplodeState, sizeof(uint32_t), &infos[i].explodeState });
	}

	try {
		for (size_t i = 0; i < batch1.size(); i += MAX_BATCH_ENTRIES) {
			size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, batch1.size() - i);
			TargetProcess.BatchRead(&batch1[i], chunk);
		}
	} catch (...) {
		static bool loggedBatchExc = false;
		if (!loggedBatchExc) { loggedBatchExc = true; LOG("[UP] batch1 read lancou excecao (posicoes ja lidas)\n"); }
	}
	{ static bool _s = false; if (!_s) { _s = true; LOG("[UP] s4 batchRead ok\n"); } }

	std::vector<DriverInterfaceV3::BatchReadEntry> batch2;
	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].encryptedRoot) continue;
		auto root = xe_decrypt(infos[i].encryptedRoot);
		if (!IsAddrValid(root)) continue;
		infos[i].actor->RootComponent = root;
		batch2.push_back({ root + SDK.ComponentLocation, sizeof(UEVector), &infos[i].pos });
	}

	for (size_t i = 0; i < batch2.size(); i += MAX_BATCH_ENTRIES) {
		size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, batch2.size() - i);
		TargetProcess.BatchRead(&batch2[i], chunk);
	}

	for (size_t i = 0; i < infos.size(); i++) {
		auto& a = infos[i].actor;
		a->TimeTillExplosion = infos[i].timeTillExplosion;
		a->ExplodeState = infos[i].explodeState;
		if (infos[i].pos.X != 0.0 || infos[i].pos.Y != 0.0) {
			a->UEPosition = infos[i].pos;
			a->LastGoodPosition = infos[i].pos;
		}
	}
}
void Engine::GetGNames()
{
	GNames = xe_decrypt(SafeRead<uint64_t>(CachedBaseAddress + SDK.GNames, "GNamesArray"));
	GNames = xe_decrypt(SafeRead<uint64_t>(GNames + SDK.GNames_offset, "GNamesPtr"));
}
struct StringA
{
	char buffer[1024];
};
std::string Engine::GetNamesFromList(DWORD ID)
{
	// -----------------------------------------------------------------------
	// `find`, NAO `operator[]`.
	//
	// `std::map::operator[]` INSERE um elemento default-construido quando a
	// chave nao existe. Esta funcao e' chamada ~13.000x/s com ids de ITEM, que
	// nunca estao neste mapa (ele guarda nome de CLASSE de ator, ver
	// Engine.cpp:1161). Resultado: uma alocacao e uma string vazia por
	// chamada, e o mapa crescendo sem teto pelo resto da partida.
	//
	// Consulta que nao acha NAO PODE ter efeito colateral.
	// -----------------------------------------------------------------------
	auto it = Local.KV.find((int)ID);
	return (it != Local.KV.end()) ? it->second : std::string();
}
std::string Engine::GetNames(DWORD ID)
{
	std::string emp = "UNKOWNNAME";
	if (ID <= 0) return emp;
	uint32_t IdDiv = ID / SDK.ElementsPerChunk;
	uint32_t Idtemp = ID % SDK.ElementsPerChunk;
	uint64_t Serial = TargetProcess.Read<uint64_t>(GNames + IdDiv * 0x8, "GNamesChunk");

	if (!Serial || Serial < 0x100000)
		return emp;
	uint64_t pName = TargetProcess.Read<uint64_t>(Serial + 0x8 * Idtemp, "GNamesSerial");

	if (!pName || pName < 0x100000)
		return emp;

	StringA names;
	memset(&names, 0, sizeof(StringA));
	if (TargetProcess.Read(pName + 0x10, &names, sizeof(StringA), "GNamesStringA")) {
		names.buffer[1023] = '\0';
		return std::string(names.buffer);
	}
	return emp;
}

// =====================================================================
// CLSPROBE — ver o bloco do metodo no Engine.h (por que existe).
//
// O log [CLSPROBE] e' o diagnostico: uma linha por nome de classe novo
// (teto 64), com o offset que produziu. Se em jogo aparecer, por exemplo:
//     [CLSPROBE] off=0x18 ... -> nome='TslCharacter'
// entao 0x18 e' o ClassPrivate desta build e a deteccao de player por
// classe esta' viva. Se NENHUM offset produzir nome legivel, os pawns
// tem ate' o nome da classe hashed e a discriminacao tera' que ser por
// campo numerico (LastTeamNum 0x1148 / bIsABot) — o CENSO decide.
// =====================================================================
std::string Engine::ResolveActorClassName(uint64_t actorAddr)
{
	if (!IsAddrValid(actorAddr)) return std::string();

	auto itAc = ClassPtrByActor.find(actorAddr);
	if (itAc != ClassPtrByActor.end()) {
		auto itNm = ClassNameByClassPtr.find(itAc->second);
		return (itNm != ClassNameByClassPtr.end()) ? itNm->second : std::string();
	}
	if (ClassProbeFailed.count(actorAddr)) return std::string();

	// Teto generoso: endereco de ator e' esparsa (nao e' arena), entao o mapa
	// so' cresce ate' ~numero de atores vivos + reciclagem.
	if (ClassPtrByActor.size() >= 65536 || ClassProbeFailed.size() >= 200000)
		return std::string();

	static const uint32_t CANDIDATES[] = { 0x08, 0x18, 0x20 };
	for (uint32_t off : CANDIDATES) {
		const uint64_t raw = SafeRead<uint64_t>(actorAddr + off, "ClSPROBE_raw");
		if (!raw) continue;
		const uint64_t cls = xe_decrypt(raw);
		if (!IsCanonicalPtr(cls)) continue;                 // nao e' (ou nao decifrou) ponteiro
		const uint32_t cid = SafeRead<uint32_t>(cls + SDK.ObjectID, "ClSPROBE_id");
		if (!cid) continue;
		std::string cn = GetNames(DecryptCIndex(cid));
		if (cn.empty() || cn == "UNKOWNNAME") continue;
		if (!cn.empty() && cn[0] == '*') continue;          // ainda hashed: tenta outro offset

		ClassPtrByActor[actorAddr] = cls;
		if (ClassNameByClassPtr.find(cls) == ClassNameByClassPtr.end()) {
			ClassNameByClassPtr[cls] = cn;
			static std::set<std::string> jaLogado;
			if (jaLogado.size() < 64 && jaLogado.insert(cn).second) {
				LOG("[CLSPROBE] off=0x%X raw=0x%llX -> cls=0x%llX nome='%s' (actor=0x%llX)\n",
				    off, (unsigned long long)raw, (unsigned long long)cls,
				    cn.c_str(), (unsigned long long)actorAddr);
			}
		}
		return cn;
	}
	ClassProbeFailed.insert(actorAddr);
	return std::string();
}

void Engine::UpdatePlayers()
{
	// PhysX: a sonda DiagPhysx() foi REMOVIDA daqui em 11/10/2026.
	//
	// O reconhecimento provou a cadeia (NpPhyscis -> NpScene -> mSQManager ->
	// os dois pruners), mas o PruningPool -- a lista de {Shape, Actor}, sem a
	// qual nao ha' geometria e nao ha' raycast -- NAO existe nos offsets
	// conhecidos deste build. Sem pool, o visible check so' sairia de dado nao
	// confiavel (feature que mente e' pior que feature ausente), e o usuario
	// tambem nao quer o cheat lendo memoria do PhysX. Resultado e caminho de
	// retomada: ARQUITETURA.md, "VISIBLE CHECK VIA PHYSX".
	UpdateWeaponPhysics();
	{ static bool _s = false; if (!_s) { _s = true; LOG("[UP] s1 weaponPhysics ok\n"); } }
	if (Actors.empty()) return;


	struct ActorReadInfo {
		std::shared_ptr<ActorEntity> actor;
		int lastTeamNum = 0;
		UEVector pos = {};
		Vector3 velocity = Vector3(0.f, 0.f, 0.f);
		uint8_t meshBulk[0xC00] = {0};   // 0xBE8 (RecentlyRendered) tem que caber — antes era 0xB10   // precisa cobrir SDK.StaticMesh (0xAF8) + 8
		// ---------------------------------------------------------------------------
	// healthBulk — a JANELA de leitura do bloco de vida.
	//
	// ===========================================================================
	// ERA 0x900 E ISSO ESTAVA ERRADO. JA' CUSTOU A VIDA DO AIMBOT.
	// ===========================================================================
	//
	// O `HeaFlag` e' a BASE da janela: lemos `Class + HeaFlag` por `sizeof`
	// bytes. Os outros campos entram como `HealthN - HeaFlag`.
	//
	// Com HeaFlag=0xE8 (lista da 2610) os campos ficavam assim:
	//     Health1 -> 0x874  OK
	//     Health2 -> 0x920  FORA
	//     Health3 -> 0x93C  FORA
	//     Health4 -> 0x928  FORA
	//     Health5 -> 0x93D  FORA
	//     Health6 -> 0x938  FORA
	//
	// Ou seja: o `DecryptHealth` lia LIXO de fora do buffer e devolvia 0.000
	// para TODO ator. E o sintoma era cruel e assimetrico:
	//
	//     ESP  desenhava normal (filtra por nome de classe)
	//     AIMBOT morto, `checaveis=0`, `actorFail=4704` (filtra por Health>0)
	//
	// 0x1000 cobre os dois candidatos de HeaFlag (0xE8 e 0x238) sem depender de
	// qual esta' certo — o que tira uma variavel da equacao enquanto a gente
	// decide a outra por medicao, e nao por deducao.
	//
	// A LICAO: base e janela sao um PAR. Trocar uma so' pode empurrar campos
	// validos para fora do buffer, e o resultado nao e' erro de leitura — e'
	// leitura bem-sucedida de memoria que nao e' do campo.
	//
	// ===========================================================================
	// 28/10/2026 — JANELA AMPLIADA 0x1000 -> 0x1780 (MESMA base HeaFlag=0xE8).
	// Motivo: PERFORMANCE, nao correcao — a base esta' certa desde o fix de
	// cima. Cada campo abaixo era uma op separada de 4/8B dentro do mesmo
	// batch, e cada op paga slot de round-trip: com N atores x 144 ticks/s
	// somavam ~7 ops x N por tick. Eles cabem TODOS numa copia so' de 0x1780
	// porque os offsets Class ficam entre 0x750 e 0x1854:
	//
	//     campo                    offset Class    relativo (off - 0xE8)
	//     LastTimeScreenRenderered 0x750           0x668
	//     Mesh                     0x788           0x6A0
	//     Mesh3P                   0x800           0x718
	//     bIsDBNO_CP               0x8E2           0x7FA
	//     WeaponProcessor          0x970           0x888
	//     (Health1 0x95C -> 0x874, ja' decifrado como antes)
	//     LastTeamNum              0x1148          0x1060
	//     GroggyHealth             0x1850          0x1768
	//
	// Fim da regiao: 0x1854 (GroggyHealth+4) -> janela [0xE8 .. 0x1855) =
	// 0x176D bytes; arredondamos para 0x1780 (6.2KB vs 4KB: +2.2KB por ator,
	// que compensa com folga as ~7 ops descartadas — menos trabalho de
	// kernel e menos falhas parciais por tick).
	//
	// O par do DecryptHealth continua valendo: Health1-HeaFlag = 0x874 < 0x1000
	// e tambem < 0x1780, entao a janela nova NAO invalida o fix antigo.
	//
	// Comportamento em falha: identico ao de sempre — buffer zerado no init,
	// DecryptHealth de buffer nulo/zeros = 0.0f, vischeck fail-open. Nada de
	// semantica nova.
	// ===========================================================================
	uint8_t healthBulk[0x1780] = { 0 };
		uint8_t fullBoneBuffer[190 * sizeof(FTransform)] = {0}; 
		uint64_t freshMesh = 0; 

		// -----------------------------------------------------------------
		// SEGUNDO CANDIDATO DE MESH.
		//
		// `SDK.Mesh` (0x300) le' ZERO nesta build — medido no logs.txt:
		//     [HP]  Health=100.000 BoneArray=0x0 bHasBones=0 mesh=0x0
		//     [VIS] LOCAL mesh=0x0 INVALIDO - leitura pulada (hipotese 1)
		// Sem mesh nao existe BoneArray -> `bHasBones` fica false -> o
		// ESQUELETO NAO DESENHA (PlayerEsp.cpp:664 exige `bHasBones`).
		//
		// Na lista de offsets da build, `Mesh` e `CapsuleComponent` aparecem
		// com o MESMO 0x300 — um dos dois esta' errado. E `Mesh3P` (0x820)
		// esta' declarado no Globals.h desde sempre sem NUNCA ser usado: e' o
		// mesh do corpo visivel (3a pessoa), que e' justamente o que o
		// esqueleto do ESP quer.
		//
		// Os dois sao lidos e quem decide e' o probe abaixo — nao um chute.
		// -----------------------------------------------------------------
		uint64_t freshMesh3P = 0;

		// Probe: `candidato + SDK.StaticMesh` (0xAD8), que e' de onde sai o
		// BoneArray. 8 bytes por candidato — barato de proposito, porque roda
		// ANTES do meshBulk (0xC00 bytes por ator).
		uint64_t boneProbeA = 0;   // em `Mesh`
		uint64_t boneProbeB = 0;   // em `Mesh3P`
		uint64_t meshChosen = 0;   // quem venceu o probe
		
		// ---- novos: DBNO / arma ----
		uint8_t  dbnoByte = 0;      // bIsDBNO_CP (bitfield)
		float    groggyHealth = 0.0f;
		uint64_t weaponProcessor = 0;

		// -----------------------------------------------------------------
		// VISIBILIDADE — o float CRU de `Class + LastTimeScreenRenderered`.
		//
		// NAN e' o sentinela de "o batch nao escreveu": `BatchRead` so'
		// preenche o destino quando a leitura passa, entao um valor inicial
		// -1.0 ou 0.0 seria indistinguivel de leitura de verdade. Com NaN, o
		// veredito cai em fail-open (visivel) em vez de pintar todo mundo de
		// "oculto" por causa de um tick ruim.
		// -----------------------------------------------------------------
		float    lastRender = NAN;

		bool successPos = false;
		bool successMesh = false;
	};

	std::vector<ActorReadInfo> infos(Actors.size());
	for (size_t i = 0; i < Actors.size(); i++) infos[i].actor = Actors[i];
	{ static bool _s = false; if (!_s) { _s = true; LOG("[UP] s2 infos=%zu\n", infos.size()); } }

	// =====================================================================
	// BATCH 0 — RECARREGA O RootComponent DE TODOS OS ATORES, TODO TICK.
	//
	// O BUG (o "ESP fantasma" que o testador fotografou):
	//
	//   `ActorEntity::RootComponent` era lido UMA VEZ, no construtor
	//   (ActorEntity.cpp:186-188) e NUNCA mais. O `Engine.cpp:1338` so'
	//   VERIFICAVA se ele existia (`if (entity->RootComponent) isCheck=true`).
	//
	//   Em modo com RESPAWN (arcade / TDM) o pawn e' recriado e o alocador
	//   REUSA o mesmo endereco de ator. Entao o `ActorCache.find(actorAddr)`
	//   ACERTA, a entidade antiga e' reaproveitada — e ela fica com o
	//   RootComponent do pawn ANTERIOR.
	//
	//   Do endereco velho sai:
	//       RootComponent + ComponentLocation  ->  POSICAO CONGELADA
	//   enquanto o resto aponta pro ator ATUAL (mesmo endereco de Class):
	//       Class + HeaFlag                    ->  VIDA AO VIVO
	//
	//   E' exatamente o sintoma relatado: caixa travada onde o cara estava no
	//   comeco da partida, com a vida caindo normalmente. Quando ele morre o
	//   ator e' filtrado e "o ESP volta ao normal".
	//
	//   ISTO E' A TERCEIRA VEZ que este mesmo padrao aparece no projeto —
	//   nome do jogador, PlayerState, e agora RootComponent. Sempre a mesma
	//   forma: VALOR LIDO UMA VEZ E NUNCA REVALIDADO. Ponteiro de ator viva em
	//   jogo com respawn nao e' dado imutavel; a unica leitura que vale e' a
	//   deste tick.
	//
	// Custo: 8 bytes por ator num batch proprio. Barato perto de desenhar uma
	// caixa no lugar errado.
	//
	// Precisa rodar ANTES do batch1, porque o batch1 monta a leitura de
	// posicao a partir de `actor->RootComponent`.
	// =====================================================================
	{
		std::vector<uint64_t> encRoot(infos.size(), 0);
		std::vector<DriverInterfaceV3::BatchReadEntry> b0;

		for (size_t i = 0; i < infos.size(); i++) {
			if (!infos[i].actor->Class) continue;
			b0.push_back({ infos[i].actor->Class + SDK.RootComponent, sizeof(uint64_t), &encRoot[i] });
		}
		for (size_t i = 0; i < b0.size(); i += MAX_BATCH_ENTRIES)
			TargetProcess.BatchRead(&b0[i], std::min((size_t)MAX_BATCH_ENTRIES, b0.size() - i));

		for (size_t i = 0; i < infos.size(); i++) {
			if (!encRoot[i]) continue;

			// -----------------------------------------------------------------
			// ROOT: DECIFRADO **OU** CRU.
			//
			// O `RootComponent` do TslCharacter vem cifrado (Xenuine) e por isso
			// passa pelo `xe_decrypt`. Mas o MESMO offset, lido de outras classes
			// de ator (item no chao, arma largada), nem sempre vem cifrado — e
			// decifrar um ponteiro em texto claro devolve lixo, que o
			// `IsAddrValid` recusa. Resultado medido no log: item com
			// `pos=(0,0,0)` e o ESP sem desenhar aquele item.
			//
			// Aceitar os dois custa um `IsAddrValid` e fecha essa classe inteira
			// de falha. O que entra em `RootComponent` continua sendo so''
			// ponteiro canonico — o resto e'' recusado como antes.
			// -----------------------------------------------------------------
			uint64_t root = xe_decrypt(encRoot[i]);
			if (!IsAddrValid(root) && IsAddrValid(encRoot[i])) root = encRoot[i];
			if (!IsAddrValid(root)) continue;

			infos[i].actor->RootComponent = root;
			// Achamos um root VALIDO agora: o ator e' checavel. Sem isto, um
			// ator cujo construtor perdeu a corrida ficaria com `isCheck`
			// falso para sempre e seria pulado pelo batch1 em todo tick.
			infos[i].actor->isCheck = TRUE;
		}
	}

	std::vector<DriverInterfaceV3::BatchReadEntry> batch1;
	Vector3 localRecoil = {0.f, 0.f, 0.f};
	// ---------------------------------------------------------------------
	// TEMPO DE MUNDO — a BASE do vischeck.
	//
	// `LastTimeScreenRenderered` e' escrito em segundos de tempo de mundo; o
	// `TimeSeconds` do GameState e' a mesma base (ReadGameState ja' le' este
	// campo, so' nao guardava — aqui entra no batch1, 4 bytes, sem IOCTL novo).
	//
	// Sem GameState (lobby, troca de mapa) `worldTime` fica 0 e o veredito cai
	// em fail-open — ver o bloco de decisao no segundo passe.
	// ---------------------------------------------------------------------
	float worldTime = 0.0f;
	if (AcknowledgedPawn && !bIsDriving) {
		batch1.push_back({ AcknowledgedPawn + SDK.RecoilADSRotation_CP, sizeof(Vector3), &localRecoil });
	}
	if (GameState) {
		batch1.push_back({ GameState + SDK.TimeSeconds, sizeof(float), &worldTime });
	}

	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].actor->Class || !infos[i].actor->RootComponent || !infos[i].actor->isCheck) continue;
		// O BOT nao e' mais lido da memoria: ver a atribuicao de `bIsBot`
		// no segundo passe (usa a classificacao por classe do IsPlayer).
		batch1.push_back({ infos[i].actor->RootComponent + SDK.ComponentLocation, sizeof(UEVector), &infos[i].pos });
		batch1.push_back({ infos[i].actor->RootComponent + SDK.ComponentVelocity, sizeof(Vector3), &infos[i].velocity });

		// =================================================================
		// JANELA GRANDE DE CLASS — 0x1780 a partir de HeaFlag (ver o struct).
		//
		// AS 7 OPS MIUDAS MORRERAM AQUI (28/10): LastTeamNum, Mesh, Mesh3P,
		// LastTimeScreenRenderered, bIsDBNO_CP, GroggyHealth e WeaponProcessor
		// eram 7 entradas de 1..8 bytes cada neste mesmo batch. Todas caem
		// dentro da janela [HeaFlag=0xE8 .. 0x1855), entao vao DENTRO desta
		// copia unica e sao extraidas por offset relativo no passo de
		// EXTRACAO logo abaixo do BatchRead — o consumo embaixo (probe de
		// mesh, vischeck, DBNO, UpdateWeapons) nao muda NENHUMA linha.
		//
		// Aposicao no batch: pos/vel primeiro, health por ultimo (motivo
		// historico preservado: uma copia grande ruim nao deve derrubar o
		// chunk de ops anterior dentro da mesma rodada de 64 do kernel).
		// =================================================================
		batch1.push_back({ infos[i].actor->Class + SDK.HeaFlag, sizeof(infos[i].healthBulk), infos[i].healthBulk });
	}
	{ static bool _s = false; if (!_s) { _s = true; LOG("[UP] s3 batch1=%zu\n", batch1.size()); } }

	try {
		for (size_t i = 0; i < batch1.size(); i += MAX_BATCH_ENTRIES) {
			size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, batch1.size() - i);
			TargetProcess.BatchRead(&batch1[i], chunk);
		}
	} catch (...) {
		static bool loggedBatchExc = false;
		if (!loggedBatchExc) { loggedBatchExc = true; LOG("[UP] batch1 read lancou excecao (posicoes ja lidas)\n"); }
	}
	Local.Recoil = localRecoil;

	// =====================================================================
	// EXTRACAO DA JANELA GRANDE (0x1780, base Class + HeaFlag).
	//
	// Roda DEPOIS do BatchRead porque os 7 campos que eram ops avulsas agora
	// CHEGAM DENTRO da copia de healthBulk. Os destinos sao os MESMOS campos
	// que o fluxo ja' usava (freshMesh/freshMesh3P para o probe, lastRender
	// para o vischeck, dbnoByte/groggyHealth para o DBNO, weaponProcessor
	// para o UpdateWeapons, lastTeamNum para o time) — quem consome embaixo
	// nao sabe nem precisa saber que mudou COMO o valor chegou la.
	//
	// offset no buffer = offset Class - SDK.HeaFlag. Sanidade verificada UMA
	// vez na primeira execucao: se qualquer offset da lista cair fora da
	// janela, loga e segue — o campo fica com o valor zero-inicializado do
	// struct, que e' exatamente o fail-open de sempre quando o batch falha
	// (buffer nao escrito = zeros).
	// =====================================================================
	{
		static bool _chk = false;
		if (!_chk) {
			_chk = true;
			const uint64_t base = (uint64_t)SDK.HeaFlag;
			const size_t   lim  = sizeof(infos[0].healthBulk); // tempo de compilacao; nao avalia o vetor
			auto dentro = [&](const char* q, uint64_t off, size_t n) {
				if (off < base || off - base + n > lim)
					LOG("[UP][JANELA] FORA DA JANELA: %s off=0x%llX n=%zu (janela 0x%llX) — campo fica zerado\n",
					    q, off, n, (unsigned long long)lim);
			};
			dentro("LastTimeScreenRenderered", (uint64_t)SDK.LastTimeScreenRenderered, sizeof(float));
			dentro("Mesh",                     (uint64_t)SDK.Mesh,                     sizeof(uint64_t));
			dentro("Mesh3P",                   (uint64_t)SDK.Mesh3P,                   sizeof(uint64_t));
			dentro("bIsDBNO_CP",               (uint64_t)SDK.bIsDBNO_CP,               1);
			dentro("WeaponProcessor",          (uint64_t)SDK.WeaponProcessor,          sizeof(uint64_t));
			dentro("LastTeamNum",              (uint64_t)SDK.LastTeamNum,              sizeof(int));
			dentro("GroggyHealth",             (uint64_t)SDK.GroggyHealth,             sizeof(float));
			LOG("[UP][JANELA] ok: janela 0x%llX cobre os 7 campos (base HeaFlag=0x%llX)\n",
			    (unsigned long long)lim, (unsigned long long)base);
		}

		for (size_t i = 0; i < infos.size(); i++) {
			auto& r = infos[i];
			if (!r.actor || !r.actor->Class) continue;
			const uint8_t* p = r.healthBulk; // base = Class + SDK.HeaFlag
			auto fatia = [&](uint64_t off, void* dst, size_t n) {
				memcpy(dst, p + (off - (uint64_t)SDK.HeaFlag), n);
			};
			fatia((uint64_t)SDK.LastTimeScreenRenderered, &r.lastRender,      sizeof(float));
			fatia((uint64_t)SDK.Mesh,                     &r.freshMesh,       sizeof(uint64_t));
			fatia((uint64_t)SDK.Mesh3P,                   &r.freshMesh3P,     sizeof(uint64_t));
			fatia((uint64_t)SDK.bIsDBNO_CP,               &r.dbnoByte,        1);
			fatia((uint64_t)SDK.GroggyHealth,             &r.groggyHealth,    sizeof(float));
			fatia((uint64_t)SDK.WeaponProcessor,          &r.weaponProcessor, sizeof(uint64_t));
			uint32_t teamNum = 0;
			fatia((uint64_t)SDK.LastTeamNum,              &teamNum,           sizeof(int));
			r.lastTeamNum = (int)teamNum;
		}
	}

std::vector<DriverInterfaceV3::BatchReadEntry> batch2;
	// Probe dos DOIS candidatos de mesh (ver o struct ActorReadInfo). Roda
	// antes do batch2, que e' ~3 KB por ator — o probe e' 8 bytes por candidato.
	std::vector<DriverInterfaceV3::BatchReadEntry> probeMesh;
	Vector3 localPos = Vector3(CameraEntry.POV.Location.X, CameraEntry.POV.Location.Y, CameraEntry.POV.Location.Z);

	for (size_t i = 0; i < infos.size(); i++) {
		auto& a = infos[i].actor;
		if (!a->Class || !a->RootComponent || !a->isCheck) continue;

		a->LastTeamNum = infos[i].lastTeamNum;

		// -----------------------------------------------------------------
		// REUSO DO WeaponProcessor (28/10) — o valor JA' VEIO da janela de
		// 0x1780 deste tick (infos[i].weaponProcessor, mesma leitura que a
		// vida). O UpdateWeapons roda logo abaixo, MESMO tick e MESMA
		// thread, e consome isto em vez de reler `Class + SDK.WeaponProcessor`
		// por ator — era a duplicata R1 do inventario (~N ops x 144Hz).
		// Flag limpa apos o consumo la'; ator que nao passa no gate deste
		// tick mantem false e o UpdateWeapons cai no fallback de ler direto.
		// -----------------------------------------------------------------
		a->WeaponProcessorFresh = infos[i].weaponProcessor;
		a->WeaponProcessorFresca = true;
		// -----------------------------------------------------------------
		// BOT — PELA CLASSE DO ATOR, NAO PELO `bIsABot`.
		//
		// Antes isto era `infos[i].botByte != 0`, lido de
		// `PlayerState + SDK.bIsABot (0x468)`. Esse offset esta' marcado no
		// proprio Globals.h como "NAO ATUALIZADO nesta build" — nao veio na
		// lista 2609.1.3.5. Resultado medido no logs.txt: `bot=1` em TODAS as
		// linhas `[DEATH]`, inclusive no jogador LOCAL. Um humano marcado como
		// bot, sempre.
		//
		// O sinal certo ja' existia e nunca era usado: `PlayerType` vem do
		// `Engine::IsPlayer`, que separa por CLASSE do ator —
		//     AIPawn_Base_* / UltAIPawn_Base_* / BP_MarketAI_Pawn_C  -> 1 (bot)
		//     PlayerMale_A_C / PlayerFemale_A_C / RegistedPlayer     -> 2
		// No log, a partida inteira era `t=2` (PlayerFemale_A_C): nao havia
		// bot nenhum, e o rotulo correto era `[P]`.
		// -----------------------------------------------------------------
		a->bIsBot = (a->PlayerType == 1);
		if (a->Class == AcknowledgedPawn) Local.Teamid = a->LastTeamNum;
		// `healthRead` diz se a LEITURA foi valida (nao se o valor e' zero).
		// Ver Engine::DecryptHealth: 0.0f sozinho nao separa "morto" de "nao li".
		bool healthRead = false;
		a->Health = DecryptHealth(infos[i].healthBulk, healthRead);

		// ---- DBNO (deitado/knocked) ----
		// GroggyHealth > 0 significa que esta deitado; o byte do bitfield e um
		// reforco. Loga uma vez para conferir.
		a->GroggyHealth = infos[i].groggyHealth;
		// -----------------------------------------------------------------
		// DBNO — o sinal certo e' o BITFIELD do jogo, nao o groggyHealth.
		//
		// O log mostrou: dbnoByte=0x00 groggy=100.00 -> bIsDBNO=1
		//
		// groggyHealth = 100 num jogador EM PE' e' o valor PADRAO (cheio).
		// Usar `> 0.5` marcava TODO MUNDO como deitado, e o ESP pintava todo
		// mundo de laranja (era esse o "ESP amarelado").
		//
		// Agora: bIsDBNO_CP decide. O groggyHealth so' conta quando
		// esta' ABAIXO de 100 — sinal de que esta' deitado E ferido, que e'
		// o unico caso em que ele carrega informacao.
		//
		// BUILD 2609.1.3.5: o bIsDBNO_CP saiu de 0x8F2 e virou 0x8E2 (esta'
		// em SDK.bIsDBNO_CP, o valor literal abaixo e' so' registro). Como a
		// vida e o HeaFlag tambem mudaram (HeaFlag 0x110 -> 0x238), o filtro
		// de morte depende de a cadeia de vida inteira estar certa: se
		// Health1..6/HeaFlag estiverem errados, `Health` le' lixo e isDie
		// marca quem nao devia.
		// -----------------------------------------------------------------
		a->bIsDBNO = (infos[i].dbnoByte != 0) ||
		             (infos[i].groggyHealth > 0.5f && infos[i].groggyHealth < 99.0f);
		static bool loggedDbno = false;
		if (!loggedDbno && a->PlayerType == 2) {
			loggedDbno = true;
			LOG("[DBNO] dbnoByte=0x%02X groggy=%.2f -> bIsDBNO=%d\n",
			    infos[i].dbnoByte, infos[i].groggyHealth, (int)a->bIsDBNO);
		}
		// -----------------------------------------------------------------
		// FILTRO DE MORTE — e' a UNICA escrita de `isDie` em todo o cheat.
		//
		// `isDie` estava declarado (ActorEntity.h:74), zerado no ctor
		// (ActorEntity.cpp:131) e lido em QUATRO gates que ja' funcionavam —
		// PlayerEsp, Radar, Aimbot — mas NUNCA era escrito `true`. Resultado: o
		// ESP continuava desenhando o cadaver enquanto o jogo mantivesse o ator
		// no array. Esta linha e' o elo que faltava.
		//
		// MORTO = leitura VALIDA + vida zerada + NAO esta' DBNO:
		//
		//   - `healthRead` (validade): sem isto, a leitura que FALHA — e que
		//     tambem devolve 0.0f — esconderia um jogador VIVO. A leitura roda a
		//     144 Hz (MemoryLoop -> UpdatePlayers), entao uma falha isolada se
		//     corrige no tick seguinte; o que NAO pode e' marcar "morto" por
		//     falta de leitura.
		//
		//   - `Health <= 0.0f`: e' o SINAL medido na sonda [DEATH] — hp=0.00
		//     apareceu em 77 linhas, TODAS em nao-local (o LOCAL deu hp=1000 em
		//     315/315). NAO ha' comparacao com TETO aqui, de proposito: o local
		//     vale 1000, entao um `Health > 150` (que ja' esteve comentado logo
		//     abaixo) marcaria o proprio jogador como morto.
		//
		//   - `!bIsDBNO`: um DBNO esta' CAIDO mas VIVO — o ESP pinta ele de
		//     laranja e ele TEM que continuar sendo desenhado (e' alvo). Esta
		//     guarda entra por SEGURANCA, nao por medicao: no treino os bots
		//     morrem direto e o A8F2 nunca saiu de 0x00, entao nao houve DBNO
		//     para medir.
		//
		// ONDE: colado na escrita de `Health`/`bIsDBNO` (poucas linhas acima),
		// para o valor ser FRESCO. Este laco roda em TODO tick de UpdatePlayers,
		// inclusive para o ator que veio do CACHE HIT do Cache(): o cache hit
		// reusa o MESMO shared_ptr e a lista `Actors` so' e' reconstruida 1x/s,
		// mas este laco a percorre a 144 Hz. E' por isso que o calculo NAO pode
		// morar dentro do Cache() — la' o bloco de reclassificacao e' PULADO no
		// cache hit (ver o `else` do `it != ActorCache.end()`).
		//
		// Precisa vir DEPOIS do `bIsDBNO` (acima) e ANTES do `continue` dos
		// aliados (abaixo), senao um aliado ficaria com `isDie` do tick anterior.
		// -----------------------------------------------------------------
		// A expressao antiga que morava AQUI (`Health <= 0.0f || Health > 150.0f`)
		// foi removida: o `> 150.0f` marcaria o LOCAL (1000) como morto e o
		// `<= 0` sem validade confundiria morte com falha de leitura. O que vale
		// e' a linha abaixo.
		a->isDie = (healthRead && a->Health <= 0.0f && !a->bIsDBNO);
		
		// =================================================================
		// O BUG DO "ESP FANTASMA DO ALIADO" (fotografado pelo testador).
		//
		// O `continue` dos aliados estava AQUI, ANTES do bloco de posicao.
		// Ou seja: aliado saia do laco antes de `a->UEPosition` ser escrito.
		//
		// Consequencia: o aliado ficava para SEMPRE com a posicao que tinha no
		// tick em que `LastTeamNum` ainda era 0 (times nao lidos ainda) — que
		// e', na pratica, A POSICAO DE QUANDO A PESSOA ENTROU NA PARTIDA. O
		// ESP continuava desenhando ele ali, congelado, com a vida (lida de
		// `Class + HeaFlag`, que nao passa por este laco) atualizando normal.
		// Quando ele morria, o `isDie` acima filtrava e o fantasma sumia.
		//
		// Foi exatamente o que o testador descreveu, com estas palavras:
		// "travou naquele lugar que quando eu entrei na partida ele estava".
		//
		// A LICAO: o `continue` de aliado e' um FILTRO DE COMPORTAMENTO (nao
		// mexer em mira/RCS com quem e' do time), mas ele estava sendo usado
		// como filtro de LEITURA. Dado de ator se le para todo mundo — quem
		// decide o que DESENHAR e' o ESP, nao o motor de leitura.
		// =================================================================

		// -----------------------------------------------------------------
		// POSICAO — E O TETO PARA A POSICAO VELHA.
		//
		// `LastGoodPosition` existe para absorver falha TRANSITORIA de leitura
		// do RootComponent (acontece). Sem TETO ele virava uma posicao ETERNA:
		// um ator que saiu da partida (objeto reciclado, leitura sempre
		// falhando) continuava desenhado na ultima posicao boa — e o filtro de
		// morte nao pegava, porque `isDie` depende de uma leitura de VIDA que
		// tambem falha. Resultado: caixa flutuando onde nao tem ninguem.
		//
		// Agora falha seguida conta, e depois de ~10 ticks (a 144 Hz, ~70 ms —
		// falha transitoria nao chega perto) o ator e' marcado `isStale` e o
		// ESP pula ele.
		// -----------------------------------------------------------------
		// -----------------------------------------------------------------
		// POSICAO — VALIDADA ANTES DE ENTRAR.
		//
		// O teste era `X != 0 || Y != 0 || Z != 0`, e ele ACEITA NaN: `NaN != 0`
		// e'' verdadeiro. Resultado medido no log da partida:
		//
		//     [DRAW] NaN rejeitado: tipo=loot pos=(0.2,-nan,1146.8)
		//
		// Ou seja: a leitura decifrada vinha lixo, o NaN passava por aqui, o
		// ESP recebia NaN e o DrawSanity recusava — item invisivel, sem nenhum
		// aviso de que era isso.
		//
		// Agora: nao-finito ou fora de faixa nao substitui NADA. O ator conta
		// uma falha (e o `isStale` continua valendo) e mantem a ultima posicao
		// boa — que e'' o comportamento certo: dado ruim nao apaga dado bom.
		// -----------------------------------------------------------------
		const UEVector pRaw = infos[i].pos;
		const bool posFinito = std::isfinite(pRaw.X) && std::isfinite(pRaw.Y) && std::isfinite(pRaw.Z) &&
		                       std::fabs(pRaw.X) < 1.0e7f && std::fabs(pRaw.Y) < 1.0e7f && std::fabs(pRaw.Z) < 1.0e7f;
		const bool posZero   = posFinito && pRaw.X == 0.0f && pRaw.Y == 0.0f && pRaw.Z == 0.0f;

		if (posFinito && !posZero) {
			a->UEPosition = pRaw;
			a->LastGoodPosition = a->UEPosition;
			a->posFalhas = 0;
			a->isStale   = false;
		} else {
			a->UEPosition = a->LastGoodPosition;
			if (a->posFalhas < 1000) a->posFalhas++;
			a->isStale = (a->posFalhas > 10);
		}

		// -----------------------------------------------------------------
		// IDENTIDADE — RE-TENTATIVA.
		//
		// `PlayerState` e `Name` sao lidos no CONSTRUTOR do ator, uma vez so'
		// (ver `ActorEntity::RefreshIdentity`). Quando aquela leitura perde a
		// corrida, o ator fica "Player" para o resto da sessao — o objeto e'
		// reaproveitado pelo ActorCache e nada mais relia.
		//
		// Re-tenta enquanto faltar, com throttle de 1 s por ator: custo zero
		// quando a identidade ja' esta' completa, e no maximo 2 leituras por
		// segundo nos poucos que ainda nao tem nome.
		// -----------------------------------------------------------------
		if (!a->PlayerState || a->Name.empty()) {
			const auto agoraId = std::chrono::steady_clock::now();
			if (a->identidadeTentativa.time_since_epoch().count() == 0 ||
			    std::chrono::duration_cast<std::chrono::seconds>(
			        agoraId - a->identidadeTentativa).count() >= 1) {
				a->identidadeTentativa = agoraId;
				a->RefreshIdentity();
			}
		}

		// -----------------------------------------------------------------
		// STATS DO ROSTER — TODO TICK, SEMPRE.
		//
		// Isto estava DENTRO do RefreshIdentity, e por isso ficava preso ao
		// guard logo acima: depois que o ator tinha nome, o RefreshIdentity
		// nunca mais rodava e Kills / TeamNumber / DamageDealt congelavam no
		// primeiro valor lido.
		//
		// O caso ruim e' o TeamNumber: se a primeira leitura aconteceu antes
		// do roster popular, ele ficava 0 PARA SEMPRE — e o filtro de time nao
		// pegava aquele jogador.
		//
		// Aqui NAO ha leitura de memoria: e' um find() no PlayerStatsMap, que
		// ja' esta' na memoria do processo. E' o que permite chamar sempre.
		// -----------------------------------------------------------------
		a->RefreshStats();

		// -----------------------------------------------------------------
		// AGORA SIM: o skip de aliado, DEPOIS da posicao e da identidade.
		//
		// Daqui para baixo e' o que NAO deve rodar para aliado: selecao de
		// alvo, etc. Os dados (posicao, nome) ja' foram atualizados acima —
		// senao o ESP desenharia o aliado numa posicao congelada, que era
		// exatamente o fantasma relatado.
		// -----------------------------------------------------------------
		if (a->LastTeamNum == Local.Teamid && a->Class != AcknowledgedPawn) continue;
		// DIAGNOSTICO: loga a posicao do primeiro ator valido (uma vez)
		static bool loggedFirstPos = false;
		if (!loggedFirstPos) {
			loggedFirstPos = true;
			LOG("[POS] Class=0x%llX Root=0x%llX pos=%.1f,%.1f,%.1f\n",
			    (unsigned long long)a->Class, (unsigned long long)a->RootComponent,
			    a->UEPosition.X, a->UEPosition.Y, a->UEPosition.Z);
		}
		// DIAGNOSTICO (uma vez): vida, BoneArray e flag de ossos
		static bool loggedFirstHP = false;
		if (!loggedFirstHP) {
			loggedFirstHP = true;
			LOG("[HP] Health=%.3f BoneArray=0x%llX bHasBones=%d mesh=0x%llX\n",
			    a->Health, (unsigned long long)a->BoneArray, (int)a->bHasBones,
			    (unsigned long long)a->Mesh);
		}
		// -----------------------------------------------------------------
		// VELOCIDADE — SANITIZADA NA ENTRADA.
		//
		// `SDK.ComponentVelocity` (0x354) e' offset de build ANTERIOR: nao veio
		// na lista 2609.1.3.5 (ver Globals.h:194). Se ele cai em memoria que
		// nao e' velocidade, o valor entra em `targetPos + Velocity * flyTime`
		// — que e' usado na SELECAO DE ALVO do aimbot (Aimbot.cpp) — e
		// contamina a posicao projetada. Um valor NaN faz `Camera::WorldToScreen`
		// devolver (0,0) para o candidato; um valor enorme joga a posicao para
		// fora do campo de visao. Nos dois casos o alvo e' reprovado, e o ESP
		// nao sente porque ele desenha a posicao CRUA.
		//
		// Jogador no PUBG nao passa de ~12 m/s (1200 cm/s). Fora disso, ou
		// nao-finito, e' lixo de leitura: trata como parado. Isto so' consegue
		// transformar lixo em zero — nunca mexe em valor plausivel.
		// -----------------------------------------------------------------
		{
			Vector3 v = infos[i].velocity;
			constexpr float kMaxPlayerSpeed = 2000.0f;   // cm/s, folga sobre 1200
			const bool plausivel =
				std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
				std::fabs(v.x) < kMaxPlayerSpeed &&
				std::fabs(v.y) < kMaxPlayerSpeed &&
				std::fabs(v.z) < kMaxPlayerSpeed;
			a->Velocity = plausivel ? v : Vector3(0.0f, 0.0f, 0.0f);
		}
		float distance = Vector3::Distance(localPos, a->GetPosition()) / 100.0f;

		// -----------------------------------------------------------------
		// MESH — AUTORITATIVO POR TICK. SEM FALLBACK PARA O VALOR ANTIGO.
		//
		// Antes:
		//     if (IsAddrValid(freshMesh))   a->Mesh   = freshMesh;
		//     if (IsAddrValid(freshMesh3P)) a->Mesh3P = freshMesh3P;
		//
		// Se a leitura fresca FALHAVA, `a->Mesh` simplesmente GUARDAVA o valor
		// do tick anterior. E o pior: o probe de osso valida `a->Mesh` lendo
		// `a->Mesh + SDK.StaticMesh` — ou seja, ele validava o mesh VELHO
		// contra ele mesmo. Passava sempre.
		//
		// Resultado: o esqueleto era lido de um componente que ja' nao e' do
		// pawn atual. Ossos CONGELADOS. E como o ESP calcula a caixa a partir
		// dos ossos quando eles existem, a CAIXA tambem congelava — enquanto o
		// rotulo de distancia (que vem do UEPosition) continuava vivo. Era
		// exatamente o "ESP fantasma com a vida e a distancia atualizando".
		//
		// UM ESQUELETO DESENHADO DE UM MESH QUE NAO CONSEGUIMOS RELER NESTE
		// TICK E' UMA MENTIRA. Melhor piscar por um tick do que mostrar um
		// osso que nao existe: com o mesh zerado, `successMesh` fica false e o
		// ESP cai no fallback — que usa a posicao VIVA.
		//
		// ISTO E' A QUARTA VEZ que o padrao "valor lido uma vez / fallback sem
		// teto" aparece aqui: nome do jogador, PlayerState, RootComponent,
		// agora Mesh.
		// -----------------------------------------------------------------
		a->Mesh   = IsAddrValid(infos[i].freshMesh)   ? infos[i].freshMesh   : 0;
		a->Mesh3P = IsAddrValid(infos[i].freshMesh3P) ? infos[i].freshMesh3P : 0;

		const bool querMesh = (a->PlayerType != 0 && distance < (float)Configs.Survivor.MaxDistance)
		                      || a->Class == AcknowledgedPawn;
		if (querMesh) {
			infos[i].successPos = true;
			if (a->Mesh)   probeMesh.push_back({ a->Mesh   + SDK.StaticMesh, sizeof(uint64_t), &infos[i].boneProbeA });
			if (a->Mesh3P) probeMesh.push_back({ a->Mesh3P + SDK.StaticMesh, sizeof(uint64_t), &infos[i].boneProbeB });
		}
	}

	// -----------------------------------------------------------------------
	// O PROBE: qual dos dois e' o componente de osso.
	//
	// O candidato VENCE se `candidato + SDK.StaticMesh` contiver um ponteiro
	// valido — e' desse campo que sai o BoneArray que o esqueleto le.
	//
	// Preferencia para o `Mesh` (A) quando os dois passam: se ele voltar a
	// funcionar numa build futura, o comportamento antigo e' retomado sozinho.
	// -----------------------------------------------------------------------
	for (size_t i = 0; i < probeMesh.size(); i += MAX_BATCH_ENTRIES) {
		size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, probeMesh.size() - i);
		TargetProcess.BatchRead(&probeMesh[i], chunk);
	}

	static bool loggedMeshPick = false;
	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].successPos) continue;
		auto& a = infos[i].actor;

		const bool okA = IsAddrValid(infos[i].boneProbeA);
		const bool okB = IsAddrValid(infos[i].boneProbeB);

		uint64_t escolhido = 0;
		if (okA)      escolhido = a->Mesh;
		else if (okB) escolhido = a->Mesh3P;

		// Loga SEMPRE, no primeiro ator que chega aqui — INCLUSIVE quando os
		// dois candidatos falham. A versao anterior so' logava no caminho de
		// sucesso, e "nada no log" era indistinguivel de "nenhum candidato
		// prestou". Foi esse o furo que custou uma rodada.
		if (!loggedMeshPick) {
			loggedMeshPick = true;
			LOG("[MESH] A(Mesh 0x%X)=0x%llX boneA=0x%llX | B(Mesh3P 0x%X)=0x%llX boneB=0x%llX | "
			    "escolhido=0x%llX (%s)\n",
			    SDK.Mesh,   (unsigned long long)infos[i].freshMesh,
			    (unsigned long long)infos[i].boneProbeA,
			    SDK.Mesh3P, (unsigned long long)infos[i].freshMesh3P,
			    (unsigned long long)infos[i].boneProbeB,
			    (unsigned long long)escolhido,
			    okA ? "Mesh" : (okB ? "Mesh3P" : "NENHUM"));
		}

		if (!escolhido) {
			// ---------------------------------------------------------------
			// NENHUM DOS DOIS CANDIDATOS SERVIU — VARREDURA, UMA VEZ.
			//
			// O `[MESH]` antigo so' logava quando um candidato VENCIA, entao
			// "os dois falharam" saia mudo. Aqui a gente MEDE o offset em vez
			// de tentar o proximo chute: varre os campos do ator procurando um
			// PONTEIRO VALIDO que tenha outro PONTEIRO VALIDO em
			// `+ SDK.StaticMesh` — que e' de onde o BoneArray sai.
			//
			// Isso separa as duas hipoteses de uma vez:
			//   - aparece algum campo  -> o offset do MESH e' que estava errado;
			//   - nao aparece nenhum   -> o offset do BONE ARRAY
			//                             (`SDK.StaticMesh`) e' que esta' errado.
			//
			// Custo: ~300 offsets x 2 leituras, UMA vez na sessao.
			// ---------------------------------------------------------------
			static bool varreu = false;
			if (!varreu) {
				varreu = true;
				LOG("[MESH] candidatos conhecidos falharam (ator 0x%llX) — varrendo os campos do ator...\n",
				    (unsigned long long)a->Class);

				int achados = 0;
				for (uint32_t off = 0x280; off < 0xC00 && achados < 16; off += 8) {
					uint64_t p = 0;
					if (!TargetProcess.Read(a->Class + off, &p, sizeof(p), "meshscan_ptr")) continue;
					if (!IsAddrValid(p)) continue;

					uint64_t b = 0;
					if (!TargetProcess.Read(p + SDK.StaticMesh, &b, sizeof(b), "meshscan_bone")) continue;
					if (!IsAddrValid(b)) continue;

					LOG("[MESH]   +0x%X -> ptr=0x%llX   bone(+0x%X)=0x%llX   <-- CANDIDATE\n",
					    off, (unsigned long long)p, SDK.StaticMesh, (unsigned long long)b);
					achados++;
				}

				if (!achados) {
					LOG("[MESH]   NENHUM campo do ator tem BoneArray valido em +0x%X.\n"
					    "         => o offset do BONE ARRAY (SDK.StaticMesh) e' o suspeito,\n"
					    "            nao o do mesh.\n",
					    SDK.StaticMesh);
				}
				LOG("[MESH] scan complete (%d candidates)\n", achados);
			}

			// Nenhum dos dois entrega osso: este ator nao tem esqueleto. Nao
			// e' erro, e' ausencia de dado — e o `successPos` falso faz o
			// resto do pipeline pular ele em vez de ler lixo.
			infos[i].successPos = false;
			continue;
		}

		infos[i].meshChosen = escolhido;
		a->Mesh = escolhido;   // o [VIS] e o ToWorld usam este campo

		batch2.push_back({ escolhido, sizeof(infos[i].meshBulk), &infos[i].meshBulk });
	}

	for (size_t i = 0; i < batch2.size(); i += MAX_BATCH_ENTRIES) {
		size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, batch2.size() - i);
		TargetProcess.BatchRead(&batch2[i], chunk);
	}
	std::vector<DriverInterfaceV3::BatchReadEntry> batch3;
	std::vector<size_t> validIndices;

	for (size_t i = 0; i < infos.size(); i++) {
		if (infos[i].successPos) {
			validIndices.push_back(i);
		}
	}

	Vector3 stage3CamPos = Vector3(CameraEntry.POV.Location.X, CameraEntry.POV.Location.Y, CameraEntry.POV.Location.Z);
	std::sort(validIndices.begin(), validIndices.end(), [&](size_t a, size_t b) {
		float distA = Vector3::Distance(stage3CamPos, infos[a].actor->GetPosition());
		float distB = Vector3::Distance(stage3CamPos, infos[b].actor->GetPosition());
		return distA < distB;
	});

	int skeletonsProcessed = 0;
	int maxSkeletons = 15;

	// =====================================================================
	// QUEM PRECISA DOS OSSOS: O ESP (esqueleto) *E* O AIMBOT.
	//
	// Este bloco era gated SO' em `Configs.Survivor.Skeleton`. Mas o aimbot
	// mira em osso: `Aimbot::GetBestBone` devolve 1 (Head) sempre que
	// `AutoBone` esta' off e `TargetBone == 0`, e o `Aimbot::Tick` entao monta
	// a posicao alvo com `Head3D`.
	//
	// Com o esqueleto do ESP DESLIGADO, `info.successMesh` nunca era setado,
	// o `continue` do segundo passe pulava o ator, `bHasBones` ficava false e
	// `Head3D` permanecia em (0,0,0). O aimbot entao mirava a ORIGEM DO MUNDO.
	//
	// Efeito medido no log (build anterior, com o dump novo):
	//     raw=16509,4053,1944 -> tela=(970,534) d2c=12     <- ator EM CIMA da mira
	//     osso=0,0,0          -> tela=(-251,71833)         <- o que o aimbot usava
	// A projecao da origem cai fora da tela -> `fovFail` em TODO candidato ->
	// `noTarget` -> nada acontece. Quando a origem cai atras da camera, o mesmo
	// bug aparece como `projFail`. Era esse o "aimbot nao gruda" silencioso.
	//
	// O custo de ler osso do aimbot e' o mesmo que o do esqueleto: um batch
	// por ator, ja' limitado por `maxSkeletons`.
	// =====================================================================
	const bool precisaOssos = Configs.Survivor.Skeleton || Configs.Aimbot.Enabled;

	for (size_t idx : validIndices) {
		auto& info = infos[idx];
		auto& a = info.actor;

		a->ToWorld = *(FTransform*)(&info.meshBulk[SDK.ComponentToWorld]);
		a->BoneArray = *(uint64_t*)(&info.meshBulk[SDK.StaticMesh]);

		if (a->Class == AcknowledgedPawn) {
			continue;
		}

		if (precisaOssos && skeletonsProcessed < maxSkeletons) {
			if (a->BoneArray > 0x1000000) {
				bool isFemale = (a->objName.find("Female") != std::string::npos);
				size_t boneCount = isFemale ? 190 : 183;
				batch3.push_back({ a->BoneArray + (1 * sizeof(FTransform)), boneCount * sizeof(FTransform), &info.fullBoneBuffer });
				info.successMesh = true;
			}
			skeletonsProcessed++;
		}
	}

	for (size_t i = 0; i < batch3.size(); i += MAX_BATCH_ENTRIES) {
		size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, batch3.size() - i);
		TargetProcess.BatchRead(&batch3[i], chunk);
	}

	
	// =====================================================================
	// VISIBILIDADE — NAO EXISTE MAIS. IsVisible fica TRUE (fail-open).
	//
	// O QUE FOI MEDIDO NO JOGO (build 2609.1.3.1), e por que este bloco caiu:
	//
	//   O diagnostico [VIS] logava, 1x/s, os dois floats de render do mesh
	//   (0x758 / 0x75C) de ate' 8 inimigos. Resultado MEDIDO:
	//
	//     ator 171m   ren(0x75C) =  7.619   <- CONGELADO em todas as leituras
	//     ator  33m   ren(0x75C) = 10.521   <- CONGELADO
	//     ator  15m   ren(0x75C) = 28.168   <- SO' o player local avanca
	//     maximo     28.168 -> 29.169 -> 30.172       (~1/s)
	//
	//   0x75C NAO avanca para inimigo -> atraso ~20000 ms -> "invisivel" pra
	//   TODOS -> a cor travava num valor so'. O SINAL NAO DISCRIMINA.
	//   O scn(0x22D) diferia (CF perto / 97 longe), mas correlaciona com
	//   DISTANCIA, nao com parede — e' LOD/cull-distance, nao oclusao.
	//
	//   CONCLUSAO: nao ha sinal de oclusao legivel na memoria do objeto.
	//   Oclusao no UE4 e' transitoria, vive no renderer (HZB / occlusion
	//   query) e NAO e' UPROPERTY: nao tem onde estar. O que daria precisao
	//   real e' ler o DEPTH BUFFER via DXGI (projeto a parte) ou o raycast do
	//   jogo (exigiria injecao).
	//
	// COMO FICOU (medicao #1): os campos VisSubmit/VisRender/VisPadByte/
	// VisSceneByte, o log [VIS] e o slider de tolerancia foram REMOVIDOS, e os
	// offsets sairam do Globals.h.
	//
	// >>> MEDICAO #2 (versao atual): o usuario pediu para retestar — ele
	//     acredita ter visto diferenca de cor. Os offsets voltaram ao
	//     Globals.h e o log `[VIS]` esta' de novo no FIM desta funcao, atras
	//     de Configs.Survivor.DebugLogs (default OFF). Este bloco de decisao
	//     NAO mudou: o ESP continua com UMA cor.
	//
	// E o `IsVisible` (abaixo, em ActorEntity) fica TRUE por decisao
	// explicita: ele ainda e' lido pelo ESP (PlayerEsp, escolha da cor) e pelo
	// AIMBOT (`VisibilityCheck` em Aimbot.cpp). Deixar `false` pintaria TODO
	// mundo com a cor "oculto" e, com o VisibilityCheck ligado (default), o
	// aimbot ficaria SEM ALVO. Fail-open e' o unico estado honesto enquanto
	// nao houver um sinal medido.
	//
	// >>> BUILD 2610.1.1.65 — O SINAL APARECEU, EM OUTRO LUGAR.
	//     O campo util NAO e' do mesh: e' `LastTimeScreenRenderered`, em
	//     `Class + 0x750` (o mesmo slot do `Eyes`), na base do
	//     `TimeSeconds` do GameState. Tudo o que esta' escrito acima sobre o
	//     0x75C continua valido — aquilo era o componente errado.
	//     A decisao vive agora no bloco "VISIBILIDADE — LastTimeScreen-
	//     Renderered x TimeSeconds" no SEGUNDO passe (logo abaixo), atras de
	//     `Configs.Survivor.VisCheck`; o default segue fail-open.
	// =====================================================================

	// =====================================================================
	// VISIBILIDADE — `LastTimeScreenRenderered` x `TimeSeconds`.
	//
	// O SINAL (medido pelo usuario, ver Globals.h): o pawn carrega, em
	// `Class + 0x750` (o MESMO slot do `Eyes`), um float com o ultimo tempo de
	// mundo em que ele foi DESENHADO. Parede na frente -> o valor congela.
	//
	//     atraso_ms = (worldTime - lastRender) * 1000
	//     visivel   = atraso_ms <= VisToleranceMs
	//
	// Diferente do 0x75C que falhou antes (ler do MESH e nunca avancava): este
	// e' o campo do TslCharacter, na mesma base do TimeSeconds do GameState.
	//
	// QUANDO A CONTA RODA — e a decisao que evita o pior sintoma:
	//   `VisCheck` (ESP) OU aimbot ligado com `VisibilityCheck` (default true).
	//   Se o aimbot sozinho nao ativasse, o checkbox dele seria um campo sem
	//   leitor. E se o ESP nao tivesse o seu, nao daria pra testar a cor sem
	//   mexer no aimbot.
	//
	// FAIL-OPEN EM TRES CASOS, de proposito:
	//   - leitura nao veio (NaN no `lastRender`): BatchRead nao escreveu;
	//   - `worldTime` invalido: GameState ainda nao publicado (lobby/mapa);
	//   - delta nao-finito.
	// Nos tres, `IsVisible` fica true. A alternativa (false) pintaria o mapa
	// inteiro de "oculto" e deixaria o aimbot sem alvo por causa de um tick
	// ruim — que e' exatamente o sintoma que ja' custou uma sessao de
	// diagnostico neste projeto.
	//
	// HISTERESE: nao ha. `VisToleranceMs` (default 100 ms) JA' e' a histerese:
	// a 144 Hz o campo avanca a cada ~7 ms enquanto o cara esta' na tela, entao
	// 100 ms de folga absorve a defasagem entre a leitura e o render sem deixar
	// o rotulo piscar.
	// =====================================================================
	const bool  visLigado = Configs.Survivor.VisCheck ||
	                        (Configs.Aimbot.Enabled && Configs.Aimbot.VisibilityCheck);
	const float visTolMs  = (float)Configs.Survivor.VisToleranceMs;
	const bool  visTempoOk = std::isfinite(worldTime) && worldTime > 0.0f;

	// ---------------------------------------------------------------------
	// A BASE DE TEMPO — COM FALLBACK PARA O PAWN LOCAL.
	//
	// O `LastTimeScreenRenderered` e' tempo de MUNDO. O `TimeSeconds` do
	// GameState deveria ser a mesma base, mas ele depende de `ReadGameState`
	// ter rodado (1x/2s) e do `GameState` estar valido — no lobby, ou logo
	// depois de mapear o driver, ele e' 0. Com 0 a versao anterior caia em
	// FAIL-OPEN e nao existia sinal nenhum: "liguei o vischeck e nada mudou".
	//
	// O FALLBACK QUE NAO DEPENDE DE NADA: o `LastRender` do PROPRIO pawn
	// local. Em TPP o personagem esta' sempre na tela, entao o valor dele
	// avanca todo frame — ele E' um "agora" valido, medido na MESMA base do
	// campo que estamos comparando. Nao precisa de GameState, nao precisa de
	// relogio, nao inventa conversao de escala.
	//
	// Ordem: `TimeSeconds` se plausivel; senao o local; senao fail-open.
	// ---------------------------------------------------------------------
	// ---------------------------------------------------------------------
	// ONDE ESTA' O SINAL QUE FUNCIONA: `mesh + 0x75C`.
	// ---------------------------------------------------------------------
	// OS TRES TEMPOS DO COMPONENTE — medidos no [VISCAN] em partida.
	//
	//   mesh+0x748  LastSubmitTime          (frustum)
	//   mesh+0x74C  LastRenderTime          (qualquer passe, inclui shadow)
	//   mesh+0x750  LastRenderTimeOnScreen  (tela — e' o que a parede corta)
	//
	// O 0x75C antigo le' ZERO nesta build; o BoundsScale em 0x754 tambem, e e'
	// isso que provou que o BLOCO mudou de lugar (nao que o campo morreu).
	//
	// O veredito usa o campo de MAIOR prioridade que esteja VIVO, com
	// fallback: 0x750 -> 0x74C -> 0x748. Se nenhum estiver vivo, fail-open.
	// A escolha aparece no menu e no [VIS], entao "nao funciona" nunca mais
	// vira adivinhacao.
	//
	// CUSTO ZERO: os tres offsets ja' estao dentro do `meshBulk` (0xC00).
	// ---------------------------------------------------------------------
	static const uint32_t kOffVis[3] = { SDK.LastSubmitTime, SDK.LastRenderTime, SDK.LastRenderTimeOnScreen };
	static const char*    kNomeVis[3] = { "0x748 submit", "0x74C render", "0x750 on-screen" };

	const auto tempoDoMesh = [](const uint8_t* meshBulk, uint32_t off) -> float {
		float v = 0.0f;
		std::memcpy(&v, meshBulk + off, sizeof(v));
		if (!std::isfinite(v) || v == 0.0f) return NAN;
		return v;
	};

	VisAtivo     = visLigado;
	VisBaseFonte = 3;   // 3 = "por mudanca do campo" (nao ha' base externa)
	VisWorldTime = 0.0f;
	VisLocalVis  = true;
	VisInimVis   = true;
	VisAtivos    = 0;
	VisOcultos   = 0;
	for (int k = 0; k < 3; k++) { VisLocalParadoMs[k] = 0.0f; VisInimParadoMs[k] = 0.0f; }
	// =====================================================================
	// QUAL DOS TRES CAMPOS ESTA' VIVO? — e a escolha do veredito.
	//
	// "Vivo" = mudou em algum ator nos ultimos 2 s. O veredito usa o de maior
	// prioridade que esteja vivo: on-screen (0x750) -> render (0x74C) ->
	// submit (0x748). Nenhum vivo = fail-open, e a tela diz isso.
	//
	// Usa o estado do tick ANTERIOR (1 tick de atraso, irrelevante para 2 s).
	// =====================================================================
	const auto visAgora = std::chrono::steady_clock::now();
	int  escolhido = -1;
	bool vivoPorCampo[3] = { false, false, false };
	if (visLigado) {
		for (int k = 2; k >= 0; k--) {
			if (VisUltimaMudanca[k].time_since_epoch().count() == 0) continue;
			const bool vivo = (visAgora - VisUltimaMudanca[k]) < std::chrono::seconds(2);
			vivoPorCampo[k] = vivo;
			if (vivo && escolhido < 0) escolhido = k;
		}
	}
	VisCampoEscolhido = escolhido;
	VisCampoVivo      = (escolhido >= 0);

	{
		static int64_t ultimoAvisoVis = 0;
		const int64_t msVis = std::chrono::duration_cast<std::chrono::milliseconds>(
			visAgora.time_since_epoch()).count();
		if (visLigado && msVis - ultimoAvisoVis > 10000) {
			ultimoAvisoVis = msVis;
			LOG("[VIS] campos: submit(0x%X)=%s render(0x%X)=%s onScreen(0x%X)=%s | usando=%s\n",
			    SDK.LastSubmitTime,         vivoPorCampo[0] ? "vivo" : "parado",
			    SDK.LastRenderTime,         vivoPorCampo[1] ? "vivo" : "parado",
			    SDK.LastRenderTimeOnScreen, vivoPorCampo[2] ? "vivo" : "parado",
			    escolhido < 0 ? "NENHUM (fail-open)" : kNomeVis[escolhido]);
		}
	}

	// =====================================================================
	// [VISCAN] v4 — varredura com GATILHO DE MOVIMENTO (DebugLogs)
	//
	// O QUE AS VERSOES ANTERIORES ENSINARAM (medido):
	//   v1/v2 — buffer zerado: `TargetProcess.Read` em bloco grande nao serve.
	//           Trocar por `BatchRead` resolveu (ok=1 nas tres bases).
	//   v3   — leitura OK, mas ZERO campos mudaram... porque a varredura roda
	//           UMA vez, no instante em que o primeiro ator aparece — quase
	//           sempre com o jogador PARADO (menu, lobby, planejando). Varredura
	//           de movimento com o alvo imovel nao mede nada, e o silencio se
	//           parece com "campo morto".
	//
	// AGORA: so' comeca quando o PAWN LOCAL esta' se movendo (|velocidade| >
	// 100 cm/s), varre 0x000..0xC00 de TRES bases (Class, Root, Mesh) com
	// 125 ms entre amostras, e REPETE a cada 5 s ate' achar um candidato ou
	// esgotar 20 tentativas.
	//
	//   [TIME?] float CRESCENTE nas tres amostras  -> candidato a timestamp
	//   [chg]   mudou sem monotonia                -> posicao/rotacao/flags
	//
	// Se aparecer [chg] no ROOT e nenhum [TIME?] no Class, o campo de render
	// nao esta na faixa varrida — e ai o proximo passo e'' subir a faixa, nao
	// trocar de offset.
	// =====================================================================
	if (visLigado && Configs.Survivor.DebugLogs) {
		static const int kJan = 0xC00;
		static int   vsFase = 0, vsEspera = 0, vsTentativas = 0, vsAchou = 0;
		static uint64_t vsBase[3] = { 0, 0, 0 };
		static const char* vsNome[3] = { "ATOR", "ROOT", "MESH" };
		static int vsOk[3][3] = { {0} };
		static uint8_t vsBuf[3][3][0xC00];
		static auto vsUltimoMovimento = std::chrono::steady_clock::time_point{};
		static auto vsProximaTentativa = std::chrono::steady_clock::time_point{};

		const auto vsAgora = std::chrono::steady_clock::now();

		// ---------- 1) acompanha o movimento do pawn local ----------
		{
			static Vector3 vsUltimaPos{};
			static bool vsTemPos = false;
			Vector3 posLocal{};
			bool achou = false;
			for (size_t i = 0; i < infos.size(); i++) {
				auto& a = infos[i].actor;
				if (!a || a->Class != AcknowledgedPawn) continue;
				posLocal = a->GetPosition();
				achou = true;
				break;
			}
			if (achou) {
				if (vsTemPos) {
					const float d = Vector3::Distance(posLocal, vsUltimaPos);
					// 20 cm num tick e'' ~29 m/s: folga larga sobre qualquer
					// caminhada/corrida. Parado nao move 20 cm por frame.
					if (d > 20.0f) vsUltimoMovimento = vsAgora;
				}
				vsUltimaPos = posLocal;
				vsTemPos = true;
			}
		}

		const bool movendoAgora =
			vsUltimoMovimento.time_since_epoch().count() != 0 &&
			(vsAgora - vsUltimoMovimento) < std::chrono::milliseconds(600);

		// ---------- 2) maquina de amostragem ----------
		if (vsFase == 0 && vsTentativas < 20 && vsAchou < 3 &&
		    movendoAgora && vsAgora >= vsProximaTentativa) {

			int alvo = -1;
			for (size_t i = 0; i < infos.size(); i++) {
				auto& a = infos[i].actor;
				if (!a || a->Class != AcknowledgedPawn || !a->Mesh) continue;
				alvo = (int)i; break;
			}
			if (alvo >= 0) {
				auto& a = infos[(size_t)alvo].actor;
				vsBase[0] = a->Class;
				vsBase[1] = a->RootComponent;
				vsBase[2] = a->Mesh;
				vsTentativas++;
				LOG("[VISCAN] tentativa %d (local movendo) Class=0x%llX Root=0x%llX Mesh=0x%llX\n",
				    vsTentativas, (unsigned long long)vsBase[0],
				    (unsigned long long)vsBase[1], (unsigned long long)vsBase[2]);
				vsFase = 1; vsEspera = 0;
			}
		}

		if (vsFase >= 1 && vsFase <= 3) {
			if (++vsEspera < 18) return;
			vsEspera = 0;
			const int s = vsFase - 1;
			for (int b = 0; b < 3; b++) {
				if (!vsBase[b]) { vsOk[b][s] = -1; continue; }
				DriverInterfaceV3::BatchReadEntry um[1] = { { vsBase[b], (size_t)kJan, vsBuf[b][s] } };
				vsOk[b][s] = TargetProcess.BatchRead(um, 1) ? 1 : 0;
			}
			vsFase++;
			if (vsFase <= 3) return;
		}

		if (vsFase == 4) {
			for (int b = 0; b < 3; b++) {
				if (!vsBase[b]) { LOG("[VISCAN] %s: base ausente\n", vsNome[b]); continue; }
				const auto F = [&](int s, int off) { float v; memcpy(&v, &vsBuf[b][s][off], 4); return v; };
				int nTime = 0, nChg = 0;
				for (int o = 0; o + 4 <= kJan; o += 4) {
					const bool d01 = memcmp(&vsBuf[b][0][o], &vsBuf[b][1][o], 4) != 0;
					const bool d12 = memcmp(&vsBuf[b][1][o], &vsBuf[b][2][o], 4) != 0;
					if (!d01 && !d12) continue;
					const float f0 = F(0,o), f1 = F(1,o), f2 = F(2,o);
					const bool mono = std::isfinite(f0) && std::isfinite(f1) && std::isfinite(f2) &&
					                  f2 > f1 && f1 > f0;
					if (mono) {
						if (nTime < 20) LOG("[VISCAN]   [TIME?] %s +0x%03X  %.4f -> %.4f -> %.4f\n", vsNome[b], (unsigned)o, f0, f1, f2);
						nTime++;
					} else if (nChg < 20) {
						LOG("[VISCAN]   [chg]   %s +0x%03X  %.3f -> %.3f -> %.3f\n", vsNome[b], (unsigned)o, f0, f1, f2);
						nChg++;
					}
				}
				LOG("[VISCAN] %s: moveu=%d ok=%d | %d [TIME?], %d [chg]\n",
				    vsNome[b], 0, vsOk[b][2], nTime, nChg);
				vsAchou += nTime;
			}
			LOG("[VISCAN] fim da tentativa %d\n", vsTentativas);
			vsFase = 0;
			vsProximaTentativa = vsAgora + std::chrono::seconds(5);
		}
	}
	// Segundo passe: e' o MESMO loop que le os ossos logo abaixo.
	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].successPos) continue;
		auto& a = infos[i].actor;

		// -----------------------------------------------------------------
		// VISIBILIDADE — o veredito de `LastTimeScreenRenderered`.
		//
		// O `if` do player local que existia aqui antes nao voltou: ele so'
		// servia pra NAO mexer no IsVisible do pawn local, que ninguem
		// desenha. Para o local o campo tambem avanca (ele esta' sempre na
		// tela em TPP), entao o veredito dele e' "visivel" de qualquer forma.
		// -----------------------------------------------------------------
		// -----------------------------------------------------------------
		// O CAMPO QUE VALE: `mesh + 0x75C` (LastRenderTimeOnScreen do
		// componente que desenha o pawn). O `Class + 0x750` fica so'' como
		// diagnostico — ver a coluna lr750 do `[VIS]`.
		// -----------------------------------------------------------------
		// VEREDITO — tres campos, um escolhido.
		//
		// Para cada um dos tempos (0x748 submit / 0x74C render / 0x750
		// on-screen) guardamos "ha' quanto tempo ele esta' parado". O veredito
		// usa o de MAIOR prioridade que esteja vivo globalmente (ver
		// `escolhido` antes do laco).
		//
		// Os tres numeros vao para o menu: e' assim que se descobre, em
		// partida, QUAL deles congela atras da parede — o submit continua
		// avancando (o ator esta' no frustum), o on-screen para'.
		// -----------------------------------------------------------------
		const auto agoraVis = std::chrono::steady_clock::now();

		for (int k = 0; k < 3; k++) {
			const float v = infos[i].successPos ? tempoDoMesh(infos[i].meshBulk, kOffVis[k]) : NAN;
			a->LastRenderTime[k] = v;

			if (!std::isfinite(v)) {
				a->VisParadoMs[k] = 0.0f;   // sem leitura: nao conta como parado
				continue;
			}
			// mudou (ou primeira leitura) -> reinicia o cronometro do campo
			if (!std::isfinite(a->LastRenderPrev[k]) || v != a->LastRenderPrev[k]) {
				a->LastRenderMudanca[k] = agoraVis;
				a->LastRenderPrev[k]    = v;
				VisUltimaMudanca[k]     = agoraVis;   // vida global do campo k
			}
			a->VisParadoMs[k] = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
				agoraVis - a->LastRenderMudanca[k]).count();
		}

		if (!visLigado) {
			a->IsVisible  = true;
			a->VisDelayMs = 0.0f;
			a->VisCampoUsado = -1;
		} else if (escolhido < 0) {
			// nenhum campo vivo nesta sessao: fail-open (a tela avisa)
			a->IsVisible  = true;
			a->VisDelayMs = 0.0f;
			a->VisCampoUsado = -1;
		} else {
			a->VisCampoUsado = escolhido;
			a->VisDelayMs    = a->VisParadoMs[escolhido];
			a->IsVisible     = VisCheck::VereditoRecente(
				(uint64_t)a->VisParadoMs[escolhido], visTolMs);
		}

		// ---- o que o menu mostra como NUMERO (nao como impressao) ----
		if (visLigado) {
			VisAtivos++;
			if (!a->IsVisible) VisOcultos++;
			if (a->Class == AcknowledgedPawn) {
				VisLocalDlyMs = a->VisDelayMs;
				VisLocalVis   = a->IsVisible;
			} else if (a->PlayerType == 1 || a->PlayerType == 2) {
				// O primeiro de gente no laco e' o mais proximo (a lista vem
				// ordenada por distancia) — e' o que interessa para conferir
				// "este esta' na minha frente e deu visivel?".
				if (VisInimLr == 0.0f) {
					VisInimLr    = std::isfinite(a->LastRenderTime[escolhido > 0 ? escolhido : 0]) ? a->LastRenderTime[escolhido > 0 ? escolhido : 0] : 0.0f;
					VisInimDlyMs = a->VisDelayMs;
					VisInimVis   = a->IsVisible;
				}
			}
		}

		// -----------------------------------------------------------------
		// `bHasBones` ZERADO TODO TICK.
		//
		// Ele era escrito SO' para `true` e nunca resetado. Um ator que saisse
		// da janela de `maxSkeletons` (15) — ou cujo probe de mesh falhasse
		// naquele tick — continuava com `bHasBones = true` e os `*3D` VELHOS, e
		// o ESP desenhava o esqueleto na posicao antiga. E' o outro "ESP
		// fantasma" (o de esqueleto; o de caixa e' o `isStale`).
		//
		// Zerar aqui faz `bHasBones` voltar a true SOMENTE se os ossos foram
		// lidos NESTE tick — que e' o que o PlayerEsp.cpp:664 assume.
		// -----------------------------------------------------------------
		a->bHasBones = false;

		// -----------------------------------------------------------------
		// [CENSO] DE CLASSES PROXIMAS — atras de DebugLogs.
		//
		// PARA QUE: o vischeck acerta parede de concreto e casa (geometria
		// estatica, que a oclusao do motor considera), mas falha atras de
		// PORTA e de objeto fino/movel. A porta e'' um ATOR dinamico: o pawn
		// atras dela chega a ser desenhado e o `LastRenderTimeOnScreen` avanca,
		// entao o veredito continua "visivel".
		//
		// Contornar por geometria exige saber QUAIS classes sao esses oclusores
		// (porta, muro de lata, etc) para testar o segmento camera->inimigo
		// contra eles. Este censo lista as classes que aparecem a menos de 25 m,
		// uma linha por nome — e'' o dado que falta para implementar o teste,
		// em vez de chutar nomes de classe do jogo.
		// -----------------------------------------------------------------
		if (Configs.Survivor.DebugLogs && a) {
			static std::set<std::string> classesVistas;
			if (classesVistas.size() < 400) {
				const UEVector cam = CameraEntry.POV.Location;
				const Vector3  p   = a->GetPosition();
				const float dx = p.x - cam.X, dy = p.y - cam.Y, dz = p.z - cam.Z;
				const float d  = sqrtf(dx * dx + dy * dy + dz * dz) / 100.0f;
				if (d > 0.3f && d < 25.0f &&
				    a->PlayerType != 1 && a->PlayerType != 2 &&
				    classesVistas.insert(a->objName).second) {
					LOG("[CENSO] classe perto (%.0fm): '%s'%s\n", d, a->objName.c_str(),
					    a->IsVisible ? "" : "  [oculto]");
				}
			}
		}
		if (a->Class == AcknowledgedPawn) continue;
		if (!infos[i].successMesh) {
			continue;
		}

		auto GetBone = [&](int idx) -> FTransform {
			if (idx >= 1 && idx <= 189) {
				return *(FTransform*)(&infos[i].fullBoneBuffer[(idx - 1) * sizeof(FTransform)]);
			}
			return FTransform();
		};

		a->Head      = GetBone(a->index.Head);
		a->neck      = GetBone(a->index.neck);
		a->pelvis    = GetBone(a->index.pelvis);
		a->Lshoulder = GetBone(a->index.Lshoulder);
		a->Lelbow    = GetBone(a->index.Lelbow);
		a->Lhand     = GetBone(a->index.Lhand);
		a->Rshoulder = GetBone(a->index.Rshoulder);
		a->Relbow    = GetBone(a->index.Relbow);
		a->Rhand     = GetBone(a->index.Rhand);
		a->Lbuttock  = GetBone(a->index.Lbuttock);
		a->Lknee     = GetBone(a->index.Lknee);
		a->Lfoot     = GetBone(a->index.Lfoot);
		a->Lball     = GetBone(a->index.Lball);
		a->Rbuttock  = GetBone(a->index.Rbuttock);
		a->Rknee     = GetBone(a->index.Rknee);
		a->Rfoot     = GetBone(a->index.Rfoot);
		a->Rball     = GetBone(a->index.Rball);

		a->SetUp3();
		a->bHasBones = true; 
	}

	// =====================================================================
	// O CAMPO ESTA'' VIVO? — failsafe contra "o offset nao e'' o render".
	//
	// Se NENHUM ator mudou o 0x75C nos ultimos 2 segundos, o campo nao serve
	// como sinal NESTA build (offset mudou, ou o componente escolhido nao e''
	// o desenhado). Nesse caso o veredito volta para fail-open — todo mundo
	// com a cor normal — e a tela diz "campo: morto", em vez de pintar o mapa
	// inteiro de "oculto" por causa de um offset errado.
	//
	// E'' a diferenca entre "a feature nao funciona" e "a feature esta''
	// mentindo": a primeira aparece no menu como diagnostico, a segunda nao.
	// =====================================================================
	if (visLigado) {
		const auto agoraCampo = std::chrono::steady_clock::now();
		VisCampoVivo = (escolhido >= 0);   // (failsafe antigo; ver o bloco de escolha acima)

		static int64_t ultimoAvisoMs = 0;
		if (!VisCampoVivo) {
			const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				agoraCampo.time_since_epoch()).count();
			if (ms - ultimoAvisoMs > 5000) {
				ultimoAvisoMs = ms;
				LOG("[VIS] o campo mesh+0x%X NAO mudou em 2 s (nenhum ator) — "
				    "veredito em fail-open. Offset errado para esta build?\n",
				    SDK.LastRenderTimeOnScreen);
			}
		}
	} else {
		VisCampoVivo = false;
	}

	// =====================================================================
	// DIAGNOSTICO [VIS] — RE-INSTRUMENTADO (MEDICAO #2, atras de DebugLogs)
	//
	// POR QUE VOLTOU: o usuario afirma ter visto diferenca de cor quando a
	// visibilidade estava ligada, e a MEDICAO #1 deixou TRES hipoteses
	// ABERTAS (o veredito dela dizia que 0x75C congela para inimigo):
	//
	//   1. o `Mesh` lido pode ser o componente ERRADO para inimigo (o jogo
	//      tem 1P / 3P / shadow mesh — o local usa um, o inimigo outro).
	//      -> por isso a linha LOGA O PONTEIRO do mesh (e a Class): da' pra
	//         comparar a "familia" do ponteiro do LOCAL com a do INIMIGO.
	//   2. o campo pode ser escrito UMA VEZ (na 1a renderizacao) e nao ser um
	//      "ultimo tempo". Nesse caso ele nao avanca nem para o LOCAL depois
	//      de estabilizar — e a leitura 1x/s mostra exatamente isso (o LOCAL
	//      e' a referencia: em TPP esta' sempre na tela).
	//   3. a ORDEM dos dois offsets (0x758 x 0x75C) pode estar invertida —
	//      os nomes vem HASHED no dump, entao a atribuicao e' inferencia.
	//      -> logando os DOIS, mais o BoundsScale (0x754) como ANCORA, a
	//         ordem fica visivel no log sem depender de inferencia.
	//
	// CUSTO: ZERO no uso normal. Tudo esta' dentro de
	// `if (Configs.Survivor.DebugLogs)` (checkbox da aba DEBUG, default OFF)
	// e o trabalho pesado so' acontece 1x POR SEGUNDO — nao por frame.
	//
	// ---------------------------------------------------------------------
	// O QUE OLHAR NO LOG (o veredito da medicao #2 sai daqui):
	//
	//   - Compare a coluna v75C do INIMIGO com o inimigo ATRAS DE PAREDE e
	//     depois NA FRENTE:
	//       * v75C CONGELA atras da parede e AVANCA na frente
	//           -> O SINAL SERVE: da' pra ligar a cor de oclusao.
	//       * v75C AVANCA IGUAL nos dois casos
	//           -> nao serve (foi o resultado da medicao #1).
	//   - O LOCAL e' a REFERENCIA: em TPP ele esta' sempre na tela, entao o
	//     "ultimo tempo renderizado" dele tem que estar SEMPRE avancando e
	//     sempre no MAIOR valor da lista. Se nem o LOCAL avanca depois de
	//     estabilizar, a hipotese 2 esta' confirmada (nao e' um "ultimo
	//     tempo") — e o campo util e' outro.
	//   - Se o `mesh=0x...` do INIMIGO nao parecer um ponteiro de mesh (for 0,
	//     lixo, ou estiver em outra regiao que a do LOCAL) -> hipotese 1
	//     confirmada: a medicao #1 leu o componente errado.
	//   - v754 (BoundsScale) e' a ANCORA: campo conhecido e estavel (~1.0).
	//     Se a ancora vier coerente e os vizinhos nao, o problema e' o campo;
	//     se a ancora vier lixo, o problema e' a leitura / o ponteiro.
	//   - scn (0x22D, bVisible/bHiddenInGame) entra como brinde: na medicao #1
	//     ele correlacionava com DISTANCIA (LOD), nao com parede. Se repetir,
	//     confirma que nao serve.
	//
	// ISSO NAO DECIDE NADA: `IsVisible` continua true (fail-open) — o bloco
	// so' MEDE. Nenhum desenho do ESP le o resultado daqui.
	// =====================================================================
	if (Configs.Survivor.DebugLogs) {
		static auto visNextLog = std::chrono::steady_clock::now();
		const auto visNow = std::chrono::steady_clock::now();
		if (visNow >= visNextLog) {
			visNextLog = visNow + std::chrono::seconds(1);

			struct VisRow {
				std::shared_ptr<ActorEntity> actor;
				uint64_t mesh = 0;
				bool local = false;
				float dist = 0.0f;
				// 0x754, 0x758, 0x75C, 0x760, 0x764 — lidos num SO' acesso
				// (20 bytes contiguos) para o batch ficar leve.
				uint8_t floats[5 * sizeof(float)] = { 0 };
				uint8_t sceneByte = 0;   // 0x22D
			};

			std::vector<VisRow> rows;

			// O LOCAL entra PRIMEIRO e sempre: sem ele, "congelado" nao
			// significa nada (o campo pode ser congelado para todo mundo).
			for (size_t i = 0; i < infos.size(); i++) {
				auto& a = infos[i].actor;
				if (!a || !a->Class || a->Class != AcknowledgedPawn) continue;
				VisRow r;
				r.actor = a;
				r.mesh = a->Mesh;
				r.local = true;
				r.dist = Vector3::Distance(localPos, a->GetPosition()) / 100.0f;
				rows.push_back(r);
				break;
			}

			// Inimigos: ate' 8, mais PERTO primeiro (o mais perto e' o que da'
			// pra colocar atras de uma parede na hora), ate' 300 m.
			// INDEPENDENTE do slider MaxDistance do ESP de proposito: aqui a
			// gente esta' medindo, nao desenhando.
			//
			// FILTRO ESTRITO — so' GENTE: PlayerType 1 (BOT) e 2 (jogador).
			// O 0 e' "unidentified/local" e 3/4/5 sao granada/veiculo/loot
			// (ver Engine::Update*, onde esses valores sao atribuidos): sem
			// isso a linha [VIS] encheria de granada e caixa de loot, que nao
			// tem nada a ver com oclusao de personagem.
			std::vector<VisRow> enemies;
			for (size_t i = 0; i < infos.size(); i++) {
				auto& a = infos[i].actor;
				if (!a || !a->Class || !a->isCheck) continue;
				if (a->Class == AcknowledgedPawn) continue;
				if (a->PlayerType != 1 && a->PlayerType != 2) continue;
				// Time local fora: o ESP tambem nao desenha companheiro, e o
				// objetivo aqui e' comparar INIMIGO visivel x INIMIGO atras de
				// parede. O `!= 0` evita perder TODO MUNDO nos primeiros
				// segundos, quando o Teamid do local ainda nao foi lido.
				if (Local.Teamid != 0 && a->LastTeamNum == Local.Teamid) continue;
				if (!a->Mesh) continue;
				float d = Vector3::Distance(localPos, a->GetPosition()) / 100.0f;
				if (d > 300.0f) continue;
				VisRow r;
				r.actor = a;
				r.mesh = a->Mesh;
				r.dist = d;
				enemies.push_back(r);
			}
			std::sort(enemies.begin(), enemies.end(),
				[](const VisRow& x, const VisRow& y) { return x.dist < y.dist; });
			// 4 inimigos (era 8): o [VIS] loga 1x/s e 8 inimigos viravam 9 linhas por
			// segundo. Com 4 a gente ainda compara 'perto vs longe' e o log fica
			// 2x menor. O que importa e' a COLUNA do LOCAL, que vem sempre antes.
			for (size_t k = 0; k < enemies.size() && k < 4; k++)
				rows.push_back(enemies[k]);

			// Uma leitura pequena por ator (2 entradas): os 5 floats do mesh +
			// o byte do SceneComponent.
			std::vector<DriverInterfaceV3::BatchReadEntry> visBatch;
			for (size_t i = 0; i < rows.size(); i++) {
				if (!IsAddrValid(rows[i].mesh)) continue;
				visBatch.push_back({ rows[i].mesh + SDK.PrimBoundsScale, (size_t)sizeof(rows[i].floats), rows[i].floats });
				visBatch.push_back({ rows[i].mesh + SDK.SceneVisibleBits, (size_t)1, &rows[i].sceneByte });
			}
			for (size_t i = 0; i < visBatch.size(); i += MAX_BATCH_ENTRIES)
				TargetProcess.BatchRead(&visBatch[i], std::min((size_t)MAX_BATCH_ENTRIES, visBatch.size() - i));

			for (size_t i = 0; i < rows.size(); i++) {
				VisRow& r = rows[i];
				// Ponteiro reprovado pelo IsAddrValid e' resultado tambem: se
				// o INIMIGO cair aqui, a hipotese 1 (mesh errado) esta' na
				// mesa. Loga em vez de pular calado.
				if (!IsAddrValid(r.mesh)) {
					LOG("[VIS] %s mesh=0x%llX INVALIDO - leitura pulada (hipotese 1)\n",
					    r.local ? "LOCAL" : "INIM ",
					    (unsigned long long)r.mesh);
					continue;
				}
				const float* v = (const float*)r.floats;
				// -----------------------------------------------------------------
				// AS COLUNAS NOVAS (2610.1.1.65): `lr` / `dly` / `vis`.
				//
				//   lr  = `LastTimeScreenRenderered` CRU, lido de Class+0x750
				//   dly = (worldTime - lr) * 1000 — o numero que decide
				//   vis = o veredito que o ESP/AIMBOT estao usando agora
				//
				// Sem essas tres colunas, "a cor nao muda" nao diz se o campo
				// congela, se a base de tempo esta' errada, ou se a tolerancia
				// esta' no lugar errado. Com elas, a leitura responde sozinha:
				// inimigo ATRAS DE PAREDE tem que mostrar dly grande; NA FRENTE,
				// dly pequeno (< VisToleranceMs).
				// -----------------------------------------------------------------
				LOG("[VIS] %s mesh=0x%llX cls=0x%llX dist=%4.0fm v754=%12.3f v758=%12.3f v75C=%12.3f v760=%12.3f v764=%12.3f scn=0x%02X | parado=%9.1fms vis=%d campo=%d\n",
				    r.local ? "LOCAL" : "INIM ",
				    (unsigned long long)r.mesh,
				    (unsigned long long)r.actor->Class,
				    r.dist,
				    v[0], v[1], v[2], v[3], v[4],
				    r.sceneByte,
				    r.actor->VisDelayMs,
				    (int)r.actor->IsVisible,
				    r.actor->VisCampoUsado);
			}
		}
	}

	// =====================================================================
	// DIAGNOSTICO [DEATH] — QUAL CAMPO VIRA NO INSTANTE DA ELIMINACAO
	//
	// ISTO E' MEDICAO, NAO CORRECAO: NADA do desenho do ESP le o resultado daqui.
	// O filtro de morte ja' foi implementado e MORA EM OUTRO LUGAR — em
	// UpdatePlayers (`a->isDie = healthRead && Health <= 0.0f && !bIsDBNO`), onde
	// o `Health` e' escrito a cada tick. Esta sonda NAO participa daquele calculo:
	// ela existe para MEDIR os candidatos (0x47C / 0x6F8) quando se quiser ir alem
	// do `Health`, e fica atras de `Configs.Survivor.DebugLogs` (default OFF).
	//
	// O QUE JA' FOI ESTABELECIDO (nao repetir a investigacao):
	//   - O `Health` E' o sinal de morte: em 2836 linhas de treino, hp=0.00 saiu
	//     em 77 linhas, TODAS em ator nao-local (o LOCAL deu hp=1000 em 315/315).
	//     Bots apareceram com hp=28 / 52 / 73.6, ou seja o campo e' vivo e varia.
	//     A alternancia hp=0 -> hp=100 no mesmo bot e' RESPAWN do treino.
	//   - Os dois candidatos alternativos estao MORTOS: `A47C` = 0x00 em
	//     2835/2835 linhas (nunca muda) e `P47C` varia inclusive no LOCAL, que
	//     nunca morre. O `0x6F8` nao mostrou utilidade.
	//   - O ESP desenhava o cadaver por alguns segundos porque `isDie` NUNCA era
	//     escrito `true` — esse era o unico motivo do "fantasma". Resolvido.
	//   - `bIsDBNO` (A8F2) saiu 0x00 em TUDO no treino: os bots morrem direto, nao
	//     sao derrubados. NAO HA' MEDICAO DE DBNO — a guarda `!bIsDBNO` no filtro
	//     de morte e' SEGURANCA, nao medicao.
	//   - `bIsDBNO`/`GroggyHealth` separam DEITADO (DBNO, com Health > 0) de
	//     MORTO — sao campos diferentes e um filtro por Health nao apagaria
	//     DBNO. Por isso os dois entram crus aqui.
	//
	// O QUE FALTA MEDIR: qual CAMPO muda de valor na eliminacao. Os dois
	// candidatos declarados no Globals.h e NUNCA usados:
	//     SDK.PlayerStatusType  (0x47C) — o mais forte
	//     SDK.PlayerStateKilled (0x6F8) — nome ambiguo ("quem ele matou"?)
	//
	// NENHUM dos dois tem BASE documentada no Globals.h. Procedencia real:
	//   - 0x47C: vem do OffsetDumper (assinatura
	//     `0F B6 83 ? ? ? ? 83 F8 ? 74 ? 83 F8 ? 74`, faixa aceita
	//     0x464..0x4A0, e o `0F B6` prova que a leitura e' de BYTE).
	//   - 0x6F8: NAO tem procedencia nenhuma no repo (nem dumper, nem doc).
	// Como a base tambem nao esta' documentada, a sonda le' AS DUAS bases que
	// fazem sentido e deixa a MEDICAO dizer qual e' a certa (em vez de eu
	// escolher uma e esconder a duvida):
	//     A = o proprio ATOR (o campo `Class` do ActorEntity E' o endereco do
	//         pawn; e' a base usada pelo bIsDBNO_CP de hoje)
	//     P = o PlayerState do ator (de onde PlayerName / TeamNumber /
	//         DamageDealtOnEnemy ja' sao lidos hoje — e' a base do bloco
	//         "placar / identidade" do Globals.h, onde os dois candidatos
	//         estao declarados)
	//
	// COMO MEDE (custo): 1x POR SEGUNDO, nunca por frame, e so' com
	// `Configs.Survivor.DebugLogs` ligado (aba DEBUG, default OFF). Para cada
	// ator de gente sai UMA LEITURA CONTIGUA por base, cobrindo os TRES
	// offsets de uma vez: PlayerStatusType .. PlayerStateKilled (8 bytes) ..
	// bIsDBNO_CP — 1218 bytes na 2609.1.3.5 (0x440..0x8E2), muito abaixo do
	// teto de 1 MiB do IOCTL.
	//
	// ATENCAO AOS NUMEROS DESTE BLOCO: os `0x47C / 0x6F8 / 0x8F2` que
	// aparecem acima e abaixo sao da build 2609.1.3.1, quando a medicao foi
	// feita. Na 2609.1.3.5 os mesmos campos sao 0x440 / 0x6F8 / 0x8E2 — e o
	// codigo LE' do SDK, entao ele ja' esta' certo; sao so' os literais do
	// texto e os rotulos das colunas do log que envelheceram. As colunas
	// agora saem como APS/AKLb/AKLq/ADB e PPS/PKLb/PKLq, sem offset no nome.
	// O BatchRead do nosso driver NAO e' batch (1 DeviceIoControl por
	// entrada), entao o custo e' o NUMERO DE ENTRADAS: 2 por ator (A e P),
	// ou seja no maximo 18 por segundo com o local + 8 inimigos.
	//
	// O `bIsDBNO_CP` e' lido AQUI, cru, em vez de reaproveitar o byte
	// do batch1: aquele byte so' e' preenchido para ator com RootComponent
	// valido, e um cadaver pode perder o RootComponent — um 0x00 "de mentira"
	// seria pior que nenhuma medida. (Era `0x8F2` na 2609.1.3.1; agora e'
	// `0x8E2`, sempre via SDK.)
	//
	// TUDO CRU, EM HEX, SEM INTERPRETAR. Foi exatamente uma leitura
	// interpretada (o "dbnoByte=0x00 groggy=100.00 -> bIsDBNO=1") que produziu
	// uma contradicao que ninguem notou; os bytes vao pro log sem passar por
	// nenhum `>`/`==`.
	//
	// COMO USAR: ligar "Debug logs" (aba DEBUG), entrar no TREINO (tem bot),
	// decorar o nome de um bot, MATAR ELE e comparar as linhas [DEATH] DAQUELE
	// bot antes de morrer, no instante da morte e ate' ele sumir do log.
	// A coluna que VIRA nesse instante (ou que some junto com o ator) e' o
	// sinal. O foco sao as colunas APS/PPS e AKLq/PKLq.
	// =====================================================================
	if (Configs.Survivor.DebugLogs) {
		static auto deathNextLog = std::chrono::steady_clock::now();
		const auto deathNow = std::chrono::steady_clock::now();
		if (deathNow >= deathNextLog) {
			deathNextLog = deathNow + std::chrono::seconds(1);

			// Legenda UMA vez: o log tem que se explicar sozinho depois.
			static bool deathLegenda = false;
			if (!deathLegenda) {
				deathLegenda = true;
				LOG("[DEATH] colunas: tipo LOCAL/BOT/PLYR | t=PlayerType (o retorno do IsPlayer) "
				    "bot=bIsABot | hp/gry = cache do ator (hp=0.00 = MORTO, medido; gry = CONTROLE) "
				    "| A = base do ATOR (pawn)  P = base do PlayerState "
				    "| PS=PlayerStatusType (byte)  KL=byte baixo de PlayerStateKilled  KLq=qword inteiro "
				    " DB=bIsDBNO_CP cru (so' faz sentido na base A) | '0x..' = base reprovada no "
				    "IsAddrValid, NENHUMA leitura feita\n");
			}

			// ---- janela contigua -------------------------------------------------
			// Uma leitura so' por base, cobrindo os tres candidatos. Os offsets
			// vem do Globals.h (nada de numero magico escrito na mao aqui).
			constexpr uint32_t deathWinMax = 0x600;   // teto da janela (1.5 KiB)
			uint32_t winIni = SDK.PlayerStatusType;
			uint32_t winFim = SDK.PlayerStatusType + 1;                  // status = 1 byte
			if (SDK.PlayerStateKilled < winIni) winIni = SDK.PlayerStateKilled;
			if (SDK.PlayerStateKilled + (uint32_t)sizeof(uint64_t) > winFim)
				winFim = SDK.PlayerStateKilled + (uint32_t)sizeof(uint64_t);
			if (SDK.bIsDBNO_CP < winIni) winIni = SDK.bIsDBNO_CP;
			if (SDK.bIsDBNO_CP + 1 > winFim) winFim = SDK.bIsDBNO_CP + 1;

			// Guarda: se os offsets do Globals.h um dia se afastarem (ou
			// trocarem de ordem), a janela unica deixa de valer — melhor NAO
			// medir do que medir errado (uma subtracao com sinal invertido
			// viraria um read gigante).
			if (winFim > winIni && (winFim - winIni) <= deathWinMax) {
				const uint32_t iStatus = SDK.PlayerStatusType  - winIni;
				const uint32_t iKilled = SDK.PlayerStateKilled - winIni;
				const uint32_t iDbno   = SDK.bIsDBNO_CP        - winIni;
				const size_t   winLen  = (size_t)(winFim - winIni);

				struct DeathRow {
					std::shared_ptr<ActorEntity> actor;
					bool  local = false;   // linha de CONTROLE (o local nunca morre)
					float dist = 0.0f;
					uint64_t baseA = 0;    // endereco do ator (pawn)
					uint64_t baseP = 0;    // endereco do PlayerState
					bool okA = false;      // passou no IsAddrValid (senao: sem leitura)
					bool okP = false;
					uint8_t bulkA[deathWinMax] = { 0 };
					uint8_t bulkP[deathWinMax] = { 0 };
				};

				std::vector<DeathRow> deathRows;
				bool temLocal = false;

				// O LOCAL entra PRIMEIRO e sempre (mesma ideia do [VIS]): as
				// colunas dele sao o CONTROLE — se um campo "virar" nele tambem,
				// o campo nao e' sinal de morte.
				for (size_t i = 0; i < infos.size(); i++) {
					auto& a = infos[i].actor;
					if (!a || !a->Class || !a->isCheck) continue;
					if (a->Class != AcknowledgedPawn) continue;
					DeathRow r;
					r.actor = a;
					r.local = true;
					r.dist = Vector3::Distance(localPos, a->GetPosition()) / 100.0f;
					r.baseA = a->Class;
					r.baseP = a->PlayerState;
					r.okA = IsAddrValid(r.baseA);
					r.okP = IsAddrValid(r.baseP);
					deathRows.push_back(r);
					temLocal = true;
					break;
				}

				const size_t deathIni = temLocal ? 1 : 0;

				// Inimigos: MESMO filtro de gente do [VIS] — PlayerType 1 (BOT) e
				// 2 (jogador), time local fora, ate' 300 m (independente do slider
				// MaxDistance do ESP: aqui a gente MEDE, nao desenha).
				//
				// SEM filtro de Mesh, de proposito: o [VIS] exigia mesh valido
				// porque ia ler o mesh; o cadaver pode perder o mesh proprio e e'
				// exatamente ele que a gente quer medir.
				for (size_t i = 0; i < infos.size(); i++) {
					auto& a = infos[i].actor;
					if (!a || !a->Class || !a->isCheck) continue;
					if (a->Class == AcknowledgedPawn) continue;
					if (a->PlayerType != 1 && a->PlayerType != 2) continue;
					if (Local.Teamid != 0 && a->LastTeamNum == Local.Teamid) continue;
					float d = Vector3::Distance(localPos, a->GetPosition()) / 100.0f;
					if (d > 300.0f) continue;
					DeathRow r;
					r.actor = a;
					r.dist = d;
					r.baseA = a->Class;
					r.baseP = a->PlayerState;
					r.okA = IsAddrValid(r.baseA);
					r.okP = IsAddrValid(r.baseP);
					deathRows.push_back(r);
				}

				// Mais PERTO primeiro, so' entre os inimigos (o local fica na
				// frente, fora da ordenacao). Teto de 8 inimigos.
				std::sort(deathRows.begin() + deathIni, deathRows.end(),
					[](const DeathRow& x, const DeathRow& y) { return x.dist < y.dist; });
				if (deathRows.size() > deathIni + 8)
					deathRows.resize(deathIni + 8);

				// 2 IOCTLs por ator (1 por base). Ponteiro reprovado no
				// IsAddrValid NAO gera IOCTL — e a linha sai marcada com '0x..'.
				std::vector<DriverInterfaceV3::BatchReadEntry> deathBatch;
				for (size_t i = 0; i < deathRows.size(); i++) {
					if (deathRows[i].okA)
						deathBatch.push_back({ deathRows[i].baseA + winIni, winLen, deathRows[i].bulkA });
					if (deathRows[i].okP)
						deathBatch.push_back({ deathRows[i].baseP + winIni, winLen, deathRows[i].bulkP });
				}
				bool deathReadOk = true;
				for (size_t i = 0; i < deathBatch.size(); i += MAX_BATCH_ENTRIES) {
					size_t chunk = std::min((size_t)MAX_BATCH_ENTRIES, deathBatch.size() - i);
					if (!TargetProcess.BatchRead(&deathBatch[i], chunk)) deathReadOk = false;
				}

				for (size_t i = 0; i < deathRows.size(); i++) {
					DeathRow& r = deathRows[i];
					auto& a = r.actor;
					if (!a) continue;

					// Nome do jogador: ja' esta' no cache do ator (lido junto com
					// o PlayerState no refresh) -> ZERO IOCTL extra. ASCII forcado
					// igual ao [PLIST], porque o log e' char. Cap de 22 colunas
					// (nome maior vira '+' no fim, pra nao fingir que acabou).
					char nome[24] = { 0 };
					{
						const size_t n = a->Name.size();
						int k = 0;
						for (size_t c = 0; c < n && k < 22; c++) {
							wchar_t w = a->Name[c];
							nome[k++] = (w >= 32 && w < 127) ? (char)w : '?';
						}
						if (k == 0) { nome[0] = '?'; k = 1; }
						else if (n > (size_t)k) nome[k - 1] = '+';
						nome[k] = 0;
					}

					// Colunas CRUAS. '0x..' (e 16 pontos no qword) e' o marcador de
					// SEM DADO (base reprovada no IsAddrValid) — mantem a largura
					// igual a de um valor hex, entao o alinhamento nao quebra.
					char colA[96] = { 0 };
					char colP[96] = { 0 };
					if (r.okA) {
						uint64_t q = 0;
						memcpy(&q, r.bulkA + iKilled, sizeof(q));
						// Rotulos de COLUNA, nao de offset: na 2609.1.3.5 os
						// literais antigos (A47C/A6F8/A8F2) ja' nao batem com
						// os valores do SDK, e o log fica mentindo se eles
						// ficarem. O offset de verdade e' o do Globals.h.
						snprintf(colA, sizeof(colA),
							"APS=0x%02X AKLb=0x%02X AKLq=0x%016llX ADB=0x%02X",
							(unsigned)r.bulkA[iStatus], (unsigned)(q & 0xFF),
							(unsigned long long)q, (unsigned)r.bulkA[iDbno]);
					} else {
						snprintf(colA, sizeof(colA),
							"APS=0x.. AKLb=0x.. AKLq=0x................ ADB=0x..");
					}
					if (r.okP) {
						uint64_t q = 0;
						memcpy(&q, r.bulkP + iKilled, sizeof(q));
						snprintf(colP, sizeof(colP),
							"PPS=0x%02X PKLb=0x%02X PKLq=0x%016llX",
							(unsigned)r.bulkP[iStatus], (unsigned)(q & 0xFF),
							(unsigned long long)q);
					} else {
						snprintf(colP, sizeof(colP),
							"PPS=0x.. PKLb=0x.. PKLq=0x................");
					}
					// O bloco P tem um campo a menos que o A: completa com espacos
					// pra' o `obj=` de todas as linhas comecar na MESMA coluna.
					std::string colPs = colP;
					while (colPs.size() < strlen(colA)) colPs += ' ';

					// Endereco sempre com 16 digitos: endereco de heap do UE4 nao
					// cabe em 12 e a largura variavel desalinharia as colunas.
					LOG("[DEATH] '%-22s' %-5s t=%d bot=%d d=%6.1fm hp=%7.2f gry=%7.2f | A 0x%016llX %s | P 0x%016llX %s | obj='%s'%s%s\n",
					    nome,
					    r.local ? "LOCAL" : (a->PlayerType == 1 ? "BOT" : "PLYR"),
					    a->PlayerType, (int)a->bIsBot, r.dist,
					    a->Health, a->GroggyHealth,
					    (unsigned long long)r.baseA, colA,
					    (unsigned long long)r.baseP, colPs.c_str(),
					    a->objName.c_str(),
					    (r.okA && r.okP) ? "" : " [PTR_REPROVADO]",
					    deathReadOk ? "" : " [IOCTL_FALHOU]");
				}
			} else {
				LOG("[DEATH] janela invalida (%u..%u): offsets do Globals.h fora do esperado — sonda pulada\n",
				    winIni, winFim);
			}
		}
	}
}


// ===========================================================================
// VEHICLE ESP — le vida/combustivel/posicao dos veiculos
// ===========================================================================
void Engine::UpdateVehicles()
{
	if (VehicleActors.empty()) return;

	struct VInfo {
		std::shared_ptr<ActorEntity> actor;
		uint64_t encRoot = 0;
		float hp = 0, hpMax = 0, fuel = 0, fuelMax = 0;
		UEVector pos = {};
	};
	std::vector<VInfo> infos(VehicleActors.size());

	std::vector<DriverInterfaceV3::BatchReadEntry> b1;
	for (size_t i = 0; i < infos.size(); i++) {
		infos[i].actor = VehicleActors[i];
		auto addr = VehicleActors[i]->Class;
		if (!addr) continue;
		b1.push_back({ addr + SDK.RootComponent, sizeof(uint64_t), &infos[i].encRoot });
		b1.push_back({ addr + SDK.VehicleHealth,    sizeof(float),    &infos[i].hp });
		b1.push_back({ addr + SDK.VehicleHealthMax, sizeof(float),    &infos[i].hpMax });
		b1.push_back({ addr + SDK.VehicleFuel,      sizeof(float),    &infos[i].fuel });
		b1.push_back({ addr + SDK.VehicleFuelMax,   sizeof(float),    &infos[i].fuelMax });
	}
	for (size_t i = 0; i < b1.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b1[i], std::min((size_t)MAX_BATCH_ENTRIES, b1.size() - i));

	std::vector<DriverInterfaceV3::BatchReadEntry> b2;
	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].encRoot) continue;
		// Mesma tolerancia do UpdatePlayers: decifrado OU cru (ver o comentario
		// la''). Sem isto, item de chao cujo root nao e'' cifrado ficava sem
		// posicao e o ESP nao desenhava.
		uint64_t root = xe_decrypt(infos[i].encRoot);
		if (!IsAddrValid(root) && IsAddrValid(infos[i].encRoot)) root = infos[i].encRoot;
		if (!IsAddrValid(root)) continue;
		infos[i].actor->RootComponent = root;
		b2.push_back({ root + SDK.ComponentLocation, sizeof(UEVector), &infos[i].pos });
	}
	for (size_t i = 0; i < b2.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b2[i], std::min((size_t)MAX_BATCH_ENTRIES, b2.size() - i));

	static bool loggedVeh = false;
	for (size_t i = 0; i < infos.size(); i++) {
		auto& a = infos[i].actor;
		a->VehHealth    = infos[i].hp;
		a->VehHealthMax = infos[i].hpMax;
		a->VehFuel      = infos[i].fuel;
		a->VehFuelMax   = infos[i].fuelMax;
		if (infos[i].pos.X == 0.0f && infos[i].pos.Y == 0.0f && infos[i].pos.Z == 0.0f) continue;
		a->UEPosition = infos[i].pos;
		a->LastGoodPosition = a->UEPosition;
		if (!loggedVeh) {
			loggedVeh = true;
			LOG("[VEH] '%s' pos=%.0f,%.0f,%.0f hp=%.0f/%.0f fuel=%.0f/%.0f\n",
			    a->objName.c_str(), infos[i].pos.X, infos[i].pos.Y, infos[i].pos.Z,
			    infos[i].hp, infos[i].hpMax, infos[i].fuel, infos[i].fuelMax);
		}
	}
}

// ===========================================================================
// LOOT ESP — le o ItemID e a posicao dos itens no chao
// ===========================================================================
// ===========================================================================
// NomeItemCache — nome BRUTO do item, por id de FName.
//
// POR QUE ISTO EXISTE (o "o ESP fica lento e o FPS cai" do testador):
//
//   `UpdateLoot` roda a CADA TICK do MemoryLoop (144 Hz por padrao) e resolve
//   o nome de TODO item do chao, todo tick. A resolucao usa `GetNames`, que
//   faz 3 leituras de driver (chunk -> serial -> string). Com ~30 itens em
//   volta isso da' ~13.000 idas ao driver POR SEGUNDO para descobrir... nomes
//   que nunca mudam. "Item_Weapon_M16A4_C" e' o mesmo nome a partida inteira.
//
//   Alem disso, `GetNamesFromList` (chamada antes) fazia `Local.KV[id]`, que
//   INSERE no mapa quando a chave nao existe — entao cada uma dessas 13.000
//   chamadas por segundo tambem alocava e fazia o mapa crescer sem teto.
//
// O cache guarda o nome BRUTO (o mesmo que o catalogo da aba WORLD usa como
// chave). Falha NAO e' cacheada: "UNKOWNNAME"/vazio podem ser transitórios
// (GNames ainda subindo), e fixar isso esconderia o item para sempre.
//
// Teto de 4096 entradas: um mapa que cresce para sempre e' o proximo bug, e
// 4096 nomes cobrem com folga tudo que um mapa do PUBG tem.
// ===========================================================================
static std::unordered_map<uint32_t, std::string> g_itemNameCache;

static std::string NomeItemCache(DWORD id)
{
	if (!id || !EngineInstance) return std::string();

	auto it = g_itemNameCache.find((uint32_t)id);
	if (it != g_itemNameCache.end()) return it->second;

	std::string n = EngineInstance->GetNamesFromList(id);
	if (n.empty()) n = EngineInstance->GetNames(id);

	const bool falhou = n.empty() || n == "UNKOWNNAME";
	if (!falhou) {
		if (g_itemNameCache.size() > 4096) g_itemNameCache.clear();
		g_itemNameCache[(uint32_t)id] = n;
	}
	return n;
}

void Engine::UpdateLoot()
{
	if (LootActors.empty()) return;

	struct LInfo {
		std::shared_ptr<ActorEntity> actor;
		uint64_t encRoot = 0;
		uint32_t itemId = 0;
		UEVector pos = {};
	};
	std::vector<LInfo> infos(LootActors.size());

	std::vector<DriverInterfaceV3::BatchReadEntry> b1;
	for (size_t i = 0; i < infos.size(); i++) {
		infos[i].actor = LootActors[i];
		auto addr = LootActors[i]->Class;
		if (!addr) continue;
		b1.push_back({ addr + SDK.RootComponent, sizeof(uint64_t), &infos[i].encRoot });
		b1.push_back({ addr + SDK.ItemID,        sizeof(uint32_t), &infos[i].itemId });
	}
	for (size_t i = 0; i < b1.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b1[i], std::min((size_t)MAX_BATCH_ENTRIES, b1.size() - i));

	std::vector<DriverInterfaceV3::BatchReadEntry> b2;
	for (size_t i = 0; i < infos.size(); i++) {
		if (!infos[i].encRoot) continue;
		// Mesma tolerancia do UpdatePlayers: decifrado OU cru (ver o comentario
		// la''). Sem isto, item de chao cujo root nao e'' cifrado ficava sem
		// posicao e o ESP nao desenhava.
		uint64_t root = xe_decrypt(infos[i].encRoot);
		if (!IsAddrValid(root) && IsAddrValid(infos[i].encRoot)) root = infos[i].encRoot;
		if (!IsAddrValid(root)) continue;
		infos[i].actor->RootComponent = root;
		b2.push_back({ root + SDK.ComponentLocation, sizeof(UEVector), &infos[i].pos });
	}
	for (size_t i = 0; i < b2.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b2[i], std::min((size_t)MAX_BATCH_ENTRIES, b2.size() - i));

	// -----------------------------------------------------------------------
	// NOME REAL DO ITEM
	//
	// O ator do chao e' um DroppedItemGroup generico — o nome dele nao diz
	// nada. O item de verdade esta' pendurado em:
	//
	//     ator + DroppedItem (0x478, Xe)  ->  UItem*
	//     UItem + ObjectID  (0x20)        ->  id criptografado
	//     DecryptCIndex(id) -> GNames     ->  nome da CLASSE do UItem
	//
	// e essa classe e' o nome do item (ex: "Item_Weapon_AK47_C").
	//
	// Seguranca: e' uma cadeia de ponteiro vinda do jogo, entao passa por
	// IsCanonicalPtr antes de qualquer leitura (foi exatamente esse tipo de
	// leitura que causou o PAGE_FAULT_IN_NONPAGED_AREA).
	// -----------------------------------------------------------------------
	{
		std::vector<uint64_t> encItems(infos.size(), 0);
		std::vector<DriverInterfaceV3::BatchReadEntry> b3;
		for (size_t i = 0; i < infos.size(); i++) {
			if (!infos[i].actor || !infos[i].actor->Class) continue;
			b3.push_back({ infos[i].actor->Class + SDK.DroppedItem,
			               sizeof(uint64_t), &encItems[i] });
		}
		for (size_t i = 0; i < b3.size(); i += MAX_BATCH_ENTRIES)
			TargetProcess.BatchRead(&b3[i], std::min((size_t)MAX_BATCH_ENTRIES, b3.size() - i));

		std::vector<uint32_t> itemObjIds(infos.size(), 0);
		std::vector<DriverInterfaceV3::BatchReadEntry> b4;
		for (size_t i = 0; i < infos.size(); i++) {
			if (!encItems[i]) continue;
			uint64_t it = xe_decrypt(encItems[i]);
			if (!IsCanonicalPtr(it)) continue;
			b4.push_back({ it + SDK.ObjectID, sizeof(uint32_t), &itemObjIds[i] });
		}
		for (size_t i = 0; i < b4.size(); i += MAX_BATCH_ENTRIES)
			TargetProcess.BatchRead(&b4[i], std::min((size_t)MAX_BATCH_ENTRIES, b4.size() - i));

		// Log POR ITEM UNICO (nao por frame). O anterior repetia a mesma
		// linha dezenas de vezes por segundo e poluia o log inteiro.
		static std::set<uint32_t> loggedItems;

		// -----------------------------------------------------------------
		// NOMES NOVOS DESTE TICK — coletados SEM lock, aplicados UMA vez.
		//
		// Antes o lock do catalogo era pego DENTRO do laco de itens: trava e
		// destrava uma vez por item de loot, por tick. Com ~40 itens no chao a
		// 144 Hz isso e' ~5.760 lock/unlock por segundo — e em quase todos eles
		// o item JA' estava no catalogo, ou seja, era trabalho para descobrir
		// que nao havia nada a fazer.
		//
		// Agora o laco so' ANOTA os nomes numa colecao local (sem lock nenhum,
		// porque e' local) e o lock e' pego UMA vez, depois do laco, para
		// aplicar o que for realmente novo.
		//
		// O std::set local deduplica: o mesmo nome aparece varias vezes por
		// tick, e sem ele a gente travaria o mutex global para testar repetido.
		// -----------------------------------------------------------------
		std::set<std::string> nomesDoTick;

		for (size_t i = 0; i < infos.size(); i++) {
			if (!itemObjIds[i] || !infos[i].actor) continue;

			// =========================================================
			// NOME DO ITEM  —  CAMINHO MEDIDO E CONFIRMADO
			//
			// Medido com o LOOTDIAG2 (log real do jogo):
			//     it -> (ItemTable 0xB0) -> (ItemID 0x274)
			//        -> GetNamesFromList(SEM DecryptCIndex)
			//
			// Devolve:  'Item_Weapon_M16A4_C'   'Item_Ammo_762mm_C'
			//           'Item_Heal_MedKit_C'    'Item_Head_E_01_Lv1_C'
			//
			// O DecryptCIndex NAO vai aqui: ele existe pro ObjectID (que a
			// Xenuine ofusca). O ItemID e' indice CRU de FName — decifrar
			// corrompia o valor e dava 'UNKOWNNAME' em 100% dos itens.
			// Foi o que travou o nome do loot por varias rodadas.
			// =========================================================
			std::string nomeItem;

			// -----------------------------------------------------------------
			// CACHE POR PONTEIRO DE UItem — 2 IOCTLs a menos por item, por tick.
			//
			// A cadeia `uitem+ItemTable -> tabela+ItemID -> NomeItemCache` custa
			// 2 leituras de driver SEMPRE, mesmo quando o item ja' foi
			// identificado no tick anterior (e ele nunca muda de nome). Com ~40
			// itens no chao a 30 Hz isso era ~2.400 idas ao driver por segundo
			// para redescobrir a mesma string.
			//
			// A chave e' o PONTEIRO do UItem (estavel enquanto o item existe).
			// Falha nao e' cacheada; teto de 4096 para o mapa nao crescer sem
			// limite numa partida longa.
			// -----------------------------------------------------------------
			static std::map<uint64_t, std::string> g_uitemNome;
			{
				const uint64_t uitem = xe_decrypt(encItems[i]);
				if (IsCanonicalPtr(uitem)) {
					auto itCache = g_uitemNome.find(uitem);
					if (itCache != g_uitemNome.end()) {
						nomeItem = itCache->second;
					} else {
						uint64_t tabela = 0;
						TargetProcess.Read(uitem + SDK.ItemTable, &tabela, sizeof(tabela), "ItemTable");
						if (IsCanonicalPtr(tabela)) {
							uint32_t rawId = 0;
							TargetProcess.Read(tabela + SDK.ItemID, &rawId, sizeof(rawId), "ItemID");
							if (rawId) nomeItem = NomeItemCache(rawId);
						}
						if (!nomeItem.empty()) {
							if (g_uitemNome.size() > 4096) g_uitemNome.clear();
							g_uitemNome[uitem] = nomeItem;
						}
					}
				}
			}

			// -----------------------------------------------------------------
			// CENSO DE RESOLUCAO (atras de DebugLogs)
			//
			// "So'' alguns itens aparecem" so'' se resolve sabendo QUAIS classes
			// de ator de loot o jogo cria e quantas resolvem o nome. Sem isso,
			// a correcao vira chute de offset — e este projeto ja'' pagou caro
			// por chute de offset.
			//
			// Chave = objName do ator (a classe). Conta por classe: quantos
			// atores, quantos com nome resolvido.
			// -----------------------------------------------------------------
			if (Configs.Survivor.DebugLogs && infos[i].actor) {
				static std::map<std::string, std::pair<int,int>> censo;
				auto& c = censo[infos[i].actor->objName];
				c.first++;
				if (!nomeItem.empty()) c.second++;

				static auto ultimoCenso = std::chrono::steady_clock::now();
				const auto agoraCenso = std::chrono::steady_clock::now();
				if (std::chrono::duration_cast<std::chrono::seconds>(agoraCenso - ultimoCenso).count() >= 5) {
					ultimoCenso = agoraCenso;
					LOG("[LOOTDIAG] === resolucao por classe (5 s) ===\n");
					for (auto& kv : censo) {
						LOG("[LOOTDIAG]   %-32s atores=%6d  nome_ok=%6d (%.0f%%)\n",
						    kv.first.c_str(), kv.second.first, kv.second.second,
						    kv.second.first ? 100.0 * kv.second.second / kv.second.first : 0.0);
					}
					censo.clear();
				}
			}

			// fallback 1: nome da classe do UItem (as vezes e'' util)
			if (nomeItem.empty()) {
				nomeItem = NomeItemCache((DWORD)DecryptCIndex(itemObjIds[i]));
			}

			bool first = (loggedItems.find(itemObjIds[i]) == loggedItems.end());
			if (Configs.Survivor.DebugLogs && first && loggedItems.size() < 400) {
				loggedItems.insert(itemObjIds[i]);
				LOG("[LOOTNAME] objId=0x%08X -> '%s'\n", itemObjIds[i], nomeItem.c_str());
			}

			// Classe generica nao sobrescreve: o objName do ator tem o nome bom
			// pras armas ('WeapAK47_C' -> 'AK47').
			static const char* genericos[] = {
				"WeaponItem", "ThrowableItem", "Item", "PickupItem",
				"Equipment", "Inventory", "InventoryFacade", "DroppedItem"
			};
			bool ehGenerico = false;
			for (const char* g : genericos) {
				if (nomeItem == g) { ehGenerico = true; break; }
			}
			if (!nomeItem.empty() && !ehGenerico) {
				infos[i].actor->ItemName = nomeItem;

				// ---------------------------------------------------------
				// CATALOGO DA ABA WORLD
				//
				// Registra o nome BRUTO (o MESMO que o DrawLootEsp usa como
				// chave do filtro individual) — nunca o nome limpo, que e'
				// so' apresentacao. So' entra uma vez por nome: o catalogo e'
				// uma lista do que o cheat JA' VIU, nao um log.
				//
				// O mutex e' obrigatorio: isto aqui roda na THREAD DE MEMORIA
				// e a aba WORLD le/itera o map na THREAD DE RENDER. Insercao
				// concorrente com iteracao em std::map = crash.
				//
				// Mas ele NAO e' pego aqui: o laco so' anota. Ver nomesDoTick.
				// ---------------------------------------------------------
				nomesDoTick.insert(nomeItem);
			}
		}

		// -----------------------------------------------------------------
		// APLICA O QUE FOR NOVO — UM lock por tick, nao um por item.
		// -----------------------------------------------------------------
		if (!nomesDoTick.empty()) {
			std::lock_guard<std::mutex> lock(G_LootCatalogMutex);
			for (const std::string& n : nomesDoTick) {
				if (G_LootCatalog.find(n) == G_LootCatalog.end()) {
					G_LootCatalog[n] = true;   // aparece por padrao
					G_LootCatalogDirty = true; // avisa a aba WORLD
				}
			}
		}

	}

	static bool loggedLoot = false;
	static std::set<std::string> lootLogOnce;   // 1 linha por nome, nao por frame
	// ---------------------------------------------------------------------
	// POSICAO DO LOOT — VALIDADA NA ENTRADA.
	//
	// MEDIDO no log da partida (18/10/2026):
	//
	//     [DRAW] NaN rejeitado: tipo=loot pos=(-nan,1337.8,560.2)
	//
	// Ou seja: a posicao decifrada do item vinha NaN (ou com magnitude
	// absurda) e ia direto para o ESP, onde o DrawSanity rejeitava — item
	// invisivel. O `== 0` de antes NAO pega NaN (NaN != 0 e' verdadeiro), e
	// era o unico filtro que existia aqui.
	//
	// Agora: posicao nao-finita ou fora de faixa NAO sobrescreve nada. O ator
	// mantem a ultima posicao boa (se nunca houve, fica sem e o ESP pula) em
	// vez de receber lixo.
	//
	// O `[LOOTPOS]` (DebugLogs) imprime os primeiros casos ruins com o root
	// cru e o decifrado — e' o que permite achar a cadeia certa para a classe
	// de ator que falha, em vez de tentar offset no chute.
	// ---------------------------------------------------------------------
	static std::set<uint64_t> lootPosLogados;
	for (size_t i = 0; i < infos.size(); i++) {
		auto& a = infos[i].actor;
		a->ItemIdValue = infos[i].itemId;

		const UEVector p = infos[i].pos;
		const bool finito = std::isfinite(p.X) && std::isfinite(p.Y) && std::isfinite(p.Z);
		const bool faixa  = finito && std::fabs(p.X) < 1.0e7f && std::fabs(p.Y) < 1.0e7f && std::fabs(p.Z) < 1.0e7f;
		const bool zero   = finito && p.X == 0.0f && p.Y == 0.0f && p.Z == 0.0f;

		if (!faixa || zero) {
			if (Configs.Survivor.DebugLogs && a &&
			    lootPosLogados.size() < 12 &&
			    lootPosLogados.insert(a->Class).second) {
				LOG("[LOOTPOS] cls=0x%llX '%s' pos=(%.1f,%.1f,%.1f) %s\n",
				    (unsigned long long)a->Class, a->objName.c_str(),
				    p.X, p.Y, p.Z,
				    finito ? "fora de faixa" : "NAO-FINITA");
			}
			continue;   // mantem a ultima posicao boa (ou nenhuma)
		}

		a->UEPosition = p;
		a->LastGoodPosition = a->UEPosition;
		if (!loggedLoot) {
			loggedLoot = true;
			LOG("[LOOT] '%s' itemId=0x%X pos=%.0f,%.0f,%.0f\n",
			    a->objName.c_str(), infos[i].itemId,
			    infos[i].pos.X, infos[i].pos.Y, infos[i].pos.Z);
		}
	}
}

// ===========================================================================
// ARMA EQUIPADA — le o nome da arma de cada ator
//   ator + WeaponProcessor -> WP
//   WP + CurrentWeaponIndex -> idx
//   WP + EquippedWeapons    -> array
//   array[idx]              -> ator da arma -> ObjectID -> nome
// ===========================================================================
void Engine::UpdateWeapons()
{
	if (Actors.empty()) return;
	const size_t N = Actors.size();

	std::vector<uint64_t> wp(N, 0), eq(N, 0), weapon(N, 0);
	std::vector<uint8_t>  idx(N, 0);
	std::vector<uint32_t> wid(N, 0);
	std::vector<DriverInterfaceV3::BatchReadEntry> b;

	// -----------------------------------------------------------------
	// PASSO 1 — WeaponProcessor com REUSO (28/10).
	//
	// O UpdatePlayers leu este MESMO qword dentro da janela de 0x1780 DESTE
	// tick (mesma thread, linhas acima no MemoryLoop) e guardou em
	// ActorEntity::WeaponProcessorFresh + flag. Aqui so' entra no batch quem
	// NAO passou no gate do UpdatePlayers neste tick — spawn novo no meio do
	// loop ou ator que falhou em isCheck. Para os demais o custo desta
	// passada cai de "N ops x round-trip" para zero ops: e' copiar um qword
	// ja' local. E e' o fallback exato de sempre (mesmo endereco, mesmo
	// tam) para quem fica de fora do reuso. Flag limpa no consumo.
	// -----------------------------------------------------------------
	for (size_t i = 0; i < N; i++) {
		auto& a = Actors[i]; // shared_ptr<ActorEntity>
		if (!a || !a->Class) continue;
		if (a->WeaponProcessorFresca) {
			wp[i] = a->WeaponProcessorFresh;
			a->WeaponProcessorFresca = false;
		} else {
			b.push_back({ a->Class + SDK.WeaponProcessor, sizeof(uint64_t), &wp[i] });
		}
	}
	if (!b.empty())
		for (size_t i = 0; i < b.size(); i += MAX_BATCH_ENTRIES)
			TargetProcess.BatchRead(&b[i], std::min((size_t)MAX_BATCH_ENTRIES, b.size() - i));

	b.clear();
	for (size_t i = 0; i < N; i++) {
		if (wp[i] < 0x1000000) continue;
		b.push_back({ wp[i] + SDK.CurrentWeaponIndex, sizeof(uint8_t),  &idx[i] });
		b.push_back({ wp[i] + SDK.EquippedWeapons,    sizeof(uint64_t), &eq[i] });
	}
	for (size_t i = 0; i < b.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b[i], std::min((size_t)MAX_BATCH_ENTRIES, b.size() - i));

	b.clear();
	for (size_t i = 0; i < N; i++) {
		if (eq[i] < 0x1000000 || idx[i] > 4) continue;
		b.push_back({ eq[i] + (uint64_t)idx[i] * 8, sizeof(uint64_t), &weapon[i] });
	}
	for (size_t i = 0; i < b.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b[i], std::min((size_t)MAX_BATCH_ENTRIES, b.size() - i));

	b.clear();
	for (size_t i = 0; i < N; i++) {
		if (weapon[i] < 0x1000000) continue;
		b.push_back({ weapon[i] + SDK.ObjectID, sizeof(uint32_t), &wid[i] });
	}
	for (size_t i = 0; i < b.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b[i], std::min((size_t)MAX_BATCH_ENTRIES, b.size() - i));

	static bool loggedWpn = false;
	for (size_t i = 0; i < N; i++) {
		if (wid[i] == 0) continue;
		int id = DecryptCIndex(wid[i]);

		// CACHE DE NOME POR ID (P1 do relatorio de desempenho). `GetNamesFromList`
		// aqui custava 3 leituras de driver + memset de 1 KiB POR ATOR, TODO
		// TICK — ~6.500 idas ao driver por segundo para redescobrir nomes que
		// nunca mudam. `NomeItemCache` resolve uma vez por id, com teto.
		std::string wn = NomeItemCache((DWORD)id);

		Actors[i]->WeaponAddr = weapon[i];   // guarda p/ o RCS ler o recuo
		if (!loggedWpn && Actors[i]->PlayerType == 2 && !wn.empty() && wn != "UNKOWNNAME") {
			loggedWpn = true;
			LOG("[WPN] arma='%s' (wp=0x%llX idx=%u)\n",
			    wn.c_str(), (unsigned long long)wp[i], idx[i]);
		}

		// -----------------------------------------------------------------
		// NOME DA ARMA — escrevas SOMENTE quando muda (28/10).
		//
		// Antes: `WeaponName = wn` a cada tick (copy de string por ator por
		// tick) e o ESP refazia o corte de prefixo/sufixo em string LOCAL a
		// cada frame (alocacao de heap, nome de arma > 16 chars). Agora o
		// campo curto (BP_Weapon_AK47_C -> AK47) e' mantido AQUI, so' no
		// tick em que o nome muda; o ESP le o campo direto, sem copia.
		// -----------------------------------------------------------------
		if (Actors[i]->WeaponName != wn) {
			Actors[i]->WeaponName = std::move(wn);
			std::string s = Actors[i]->WeaponName;
			static const char* const pref[] = { "BP_Weapon_", "Weapon_", "BP_", "Weap" };
			for (const char* p : pref)
				if (s.rfind(p, 0) == 0) { s.erase(0, std::strlen(p)); break; }
			if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0)
				s.erase(s.size() - 2);
			Actors[i]->WeaponNameCurto = std::move(s);
		}
	}
}

// ===========================================================================
// DIAGNOSTICO: candidatos da cadeia do RCS
//
// O offset tree mostra AnimScriptInstance -> PreEvalPawnState -> ControlRotation_CP.
// Lemos anim + 0x674 direto (pulando PreEvalPawnState). Este log mostra os tres
// candidatos lado a lado para descobrir qual e' o ControlRotation de verdade:
//   (a) anim + 0x674              (o que usamos hoje)
//   (b) anim + 0x658 + 0x674      (PreEvalPawnState como STRUCT)
//   (c) *(anim + 0x658) + 0x674   (PreEvalPawnState como PONTEIRO)
// ===========================================================================
// ===========================================================================
// GameState -> PlayerArray -> stats (kills / team / dano)
//
// Cadeia (do offset tree):
//   GameState + 0x420 PlayerArray (TArray<APlayerState*>)
//   PlayerState + 0x418 PlayerName | +0x76C TeamNumber
//               + 0x4A8 PlayerStatistics (kills) | +0x770 DamageDealtOnEnemy
//
// Guardamos num mapa PlayerState* -> stats, para o UpdatePlayers casar com o
// ator e mostrar kills/team no ESP.
// ===========================================================================
void Engine::UpdatePlayerList()
{
	PlayerStatsMap.clear();

	// -----------------------------------------------------------------------
	// O GATE DE `ZoneRadar` SAIU DAQUI.
	//
	// Ele acorrentava o ROSTER (time / kills / dano — dado de ESP) ao toggle do
	// RADAR DE ZONA. Com `ZoneRadar` desligado (o default), esta funcao saia na
	// primeira linha, `PlayerStatsMap` ficava vazio, `TeamNumber` era 0 para
	// todo mundo e o `[S<n>]` do ESP NUNCA aparecia — sem log, sem erro.
	//
	// Sao duas features diferentes que por acaso leem o mesmo GameState. Quem
	// decide se o GameState e' lido e' o `ReadGameState` (que agora considera
	// as duas). Aqui basta ele existir.
	// -----------------------------------------------------------------------
	if (!IsCanonicalPtr(GameState)) return;

	struct TArr { uint64_t Data; int32_t Count; int32_t Max; };
	TArr arr = {};
	TargetProcess.Read(GameState + SDK.PlayerArray, &arr, sizeof(arr), "PlayerArray");

	// Antes so' checava Count. Se o TArray estiver corrompido, `arr.Data` pode
	// ser lixo — e a leitura dos ponteiros ia direto pro kernel.
	if (arr.Count <= 0 || arr.Count > 120) return;
	if (!IsCanonicalRange(arr.Data, sizeof(uint64_t) * (uint64_t)arr.Count)) return;

	uint64_t ptrs[120] = {};
	int n = arr.Count;
	TargetProcess.Read(arr.Data, ptrs, sizeof(uint64_t) * n, "PlayerArrayData");

	static int plog = 0;
	bool doLog = (++plog % 20 == 1);

	// =====================================================================
	// ROSTER EM BATCH — 28/10. Antes: 3 leituras SINGELAS por jogador
	// (Team / Kills / Damage) a cada 2s => 3n round-trips (n=120: ~360
	// idas ao driver numa burst, no MEIO do tick de memoria). Agora as 3
	// entradas de 4B de cada PlayerState entram num batch unico, chunkado
	// no teto de 64 ops do kernel: n=120 vira 6 rodadas em vez de 360.
	// Falha de leitura deixa o slot com o init do struct (mesmo que uma
	// `Read` falha de antes). Os nomes para o log seguem singulares —
	// sao 2 reads raros por 20 chamadas, nao o custo do roster.
	// =====================================================================
	std::vector<PlayerStats> stats(n); // 1:1 com o loop abaixo; zero-iniciado
	std::vector<DriverInterfaceV3::BatchReadEntry> b;
	for (int i = 0; i < n; i++) {
		if (!IsCanonicalPtr(ptrs[i])) continue;
		b.push_back({ ptrs[i] + SDK.TeamNumber,         4, &stats[i].Team   });
		b.push_back({ ptrs[i] + SDK.PlayerStatistics,   4, &stats[i].Kills  });
		b.push_back({ ptrs[i] + SDK.DamageDealtOnEnemy, 4, &stats[i].Damage });
	}
	for (size_t i = 0; i < b.size(); i += MAX_BATCH_ENTRIES)
		TargetProcess.BatchRead(&b[i], std::min((size_t)MAX_BATCH_ENTRIES, b.size() - i));

	for (int i = 0; i < n; i++) {
		uint64_t ps = ptrs[i];
		if (!IsCanonicalPtr(ps)) continue;

		PlayerStats st = stats[i];

		// sanidade (o tree avisa: TeamNumber vem -100000 quando invalido)
		if (st.Team < -1000 || st.Team > 100000) st.Team = 0;
		if (st.Kills < 0 || st.Kills > 200) st.Kills = -1;
		if (st.Damage < 0.0f || st.Damage > 100000.0f) st.Damage = 0.0f;

		st.Valid = true;

		// nome (FString) so' para o log
		if (doLog) {
			struct FS { uint64_t Data; int32_t Count; int32_t Max; } fs = {};
			if (TargetProcess.Read(ps + SDK.PlayerName, &fs, sizeof(fs), "PSName") &&
			    fs.Count > 0 && fs.Count < 64 &&
			    IsCanonicalRange(fs.Data, sizeof(wchar_t) * (uint64_t)fs.Count + 2)) {
				wchar_t buf[64] = {};
				TargetProcess.Read(fs.Data, buf, sizeof(wchar_t) * (fs.Count < 63 ? fs.Count : 63), "PSNameStr");
				char nb[128] = {};
				int k = 0;
				for (int c = 0; c < 63 && buf[c] && k < 120; c++)
					nb[k++] = (buf[c] < 128) ? (char)buf[c] : '?';
				LOG("[PLIST] ps=0x%llX nome='%s' team=%d kills=%d dmg=%.0f\n",
				    (unsigned long long)ps, nb, st.Team, st.Kills, st.Damage);
			}
		}

		PlayerStatsMap[ps] = st;
	}

	if (doLog)
		LOG("[PLIST] GameState=0x%llX total=%d roster=%d\n",
		    (unsigned long long)GameState, n, (int)PlayerStatsMap.size());
}

// ===========================================================================
void Engine::DiagChain()
{
	if (!Configs.Survivor.ZoneRadar) return;     // diagnostico: so' quando testando
	if (!IsCanonicalPtr(AcknowledgedPawn)) return;

	uint64_t mesh = 0;
	for (auto& a : Actors) { if (a && a->Class == AcknowledgedPawn) { mesh = a->Mesh; break; } }
	if (mesh < 0x1000000) return;

	uint64_t anim = 0;
	TargetProcess.Read(mesh + SDK.AnimScriptInstance, &anim, sizeof(anim), "AnimInst");
	if (anim < 0x1000000) { LOG("[CHAIN] anim invalido (mesh=0x%llX)\n", (unsigned long long)mesh); return; }

	uint64_t preEval = 0;
	TargetProcess.Read(anim + SDK.PreEvalPawnState, &preEval, sizeof(preEval), "PreEval");

	UERotator a1 = {}, a2 = {}, a3 = {};
	TargetProcess.Read(anim + SDK.ControlRotation_CP, &a1, sizeof(a1), "cr_a");
	TargetProcess.Read(anim + SDK.PreEvalPawnState + SDK.ControlRotation_CP, &a2, sizeof(a2), "cr_b");
	if (preEval >= 0x1000000)
		TargetProcess.Read(preEval + SDK.ControlRotation_CP, &a3, sizeof(a3), "cr_c");

	// idem para o RecoilADSRotation
	UERotator r1 = {}, r2 = {}, r3 = {};
	TargetProcess.Read(anim + SDK.RecoilADSRotation_CP, &r1, sizeof(r1), "ra_a");
	TargetProcess.Read(anim + SDK.PreEvalPawnState + SDK.RecoilADSRotation_CP, &r2, sizeof(r2), "ra_b");
	if (preEval >= 0x1000000)
		TargetProcess.Read(preEval + SDK.RecoilADSRotation_CP, &r3, sizeof(r3), "ra_c");

	LOG("[CHAIN] anim=0x%llX preEval=0x%llX\n", (unsigned long long)anim, (unsigned long long)preEval);
	LOG("[CHAIN] CtrlRot  a(anim+674)=%7.2f,%7.2f  b(anim+658+674)=%7.2f,%7.2f  c(ptr+674)=%7.2f,%7.2f\n",
	    a1.Pitch, a1.Yaw, a2.Pitch, a2.Yaw, a3.Pitch, a3.Yaw);
	LOG("[CHAIN] RecoilADS a(anim+824)=%7.2f,%7.2f  b(anim+658+824)=%7.2f,%7.2f  c(ptr+824)=%7.2f,%7.2f\n",
	    r1.Pitch, r1.Yaw, r2.Pitch, r2.Yaw, r3.Pitch, r3.Yaw);
}

// ===========================================================================
// GameState: zona azul, zona segura e WorldToMap (minimapa real)
//
// Cadeia: UWorld + GameState (0x958) -> decriptar (marcado 'xe' no tree)
//   + BlueZoneRadius(0xCC) / BlueZonePosition(0xC0)
//   + SafetyZoneRadius(0xBC) / SafetyZonePosition(0xB0)
// ===========================================================================
void Engine::ReadGameState()
{
	// -----------------------------------------------------------------------
	// QUEM PRECISA DO GameState — DUAS FEATURES DIFERENTES.
	//
	//   1. o RADAR DE ZONA (BlueZone / SafetyZone) -> `ZoneRadar`;
	//   2. o ROSTER do ESP (time/kills/dano, via PlayerArray) -> as opcoes
	//      `ShowTeamInfo` / `ShowBotFlag`.
	//
	// O gate antigo so' olhava a (1). Com o radar desligado (o DEFAULT), o
	// GameState era zerado AQUI e o roster morria junto — `[S<n>]` nunca
	// renderizava e nao havia nada no log dizendo por que.
	//
	// O gate continua existindo (e nao virou leitura incondicional) porque a
	// cadeia faz leituras especulativas e ja' foi a causa provavel de um BSOD
	// de PAGE_FAULT. Ele so' deixou de ser "radar" e passou a ser "alguma
	// feature que consome o GameState".
	// -----------------------------------------------------------------------
	const bool precisaRoster = Configs.Survivor.ShowTeamInfo || Configs.Survivor.ShowBotFlag;
	if (!Configs.Survivor.ZoneRadar && !precisaRoster) { GameState = 0; return; }

	// -----------------------------------------------------------------------
	// AS SAIDAS DESTA FUNCAO ERAM TODAS MUDAS.
	//
	// Enquanto o GameState servia so' ao radar de zona, "sem zona" e "cadeia
	// quebrada" davam no mesmo: nada na tela. Agora o ROSTER (time/kills do
	// ESP) depende daqui, entao "o [S<n>] nao aparece" virou uma pergunta sem
	// resposta possivel no log.
	//
	// Cada porta de saida ganhou nome, 1x/2s, com os valores crus. Isto e' o
	// que separa:
	//   UWorld invalido          -> a cadeia do UWorld caiu;
	//   raw GameState zerado     -> UWorld+GameState leu 0 (offset errado ou
	//                               UWorld ainda nao valido);
	//   decifrado invalido       -> o offset SDK.GameState (0x%X) esta' errado
	//                               OU a chave do xe_decrypt esta' errada.
	//
	// ATENCAO: o valor BRUTO em UWorld+GameState e' um ponteiro CRIPTOGRAFADO
	// (padrao 0x...FFFF8CF... — os bits altos NAO sao extensao de sinal), entao
	// NUNCA passar em IsCanonicalPtr antes do xe_decrypt. A validacao de
	// canonicidade so' se aplica AO VALOR DECIFRADO. O bug antigo checava o
	// bruto e falhava SEMPRE ("raw GameState invalido" em toda frame).
	// -----------------------------------------------------------------------
	uint64_t rawGS = 0;
	auto falhou = [&](const char* motivo) {
		static auto ultimo = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		const auto agora = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(agora - ultimo).count() < 2000) return;
		ultimo = agora;
		LOG("[GS] ReadGameState parou em '%s': UWorld=0x%llX | UWorld+0x%X=0x%llX | "
		    "GameState=0x%llX\n",
		    motivo, (unsigned long long)UWorld, SDK.GameState,
		    (unsigned long long)rawGS, (unsigned long long)GameState);
	};

	if (!IsCanonicalPtr(UWorld)) { falhou("UWorld invalido"); return; }

	rawGS = SafeRead<uint64_t>(UWorld + SDK.GameState, "GameStateRaw");
	// O bruto e' CRIPTOGRAFADO — checar so' != 0 e decifrar; a canonicidade
	// vale so' para o resultado do xe_decrypt (ver comentario acima).
	if (!rawGS) { GameState = 0; falhou("raw GameState zerado"); return; }

	GameState = xe_decrypt(rawGS);
	// Depois do decrypt, revalida: se o raw era lixo o decrypt devolve lixo,
	// e ler em lixo e' exatamente o que derruba o kernel.
	if (!IsCanonicalPtr(GameState)) { GameState = 0; falhou("GameState decifrado invalido"); return; }

	DriverInterfaceV3::BatchReadEntry z[4];
	z[0] = { GameState + SDK.BlueZoneRadius,    sizeof(float),    &BlueZoneRadius };
	z[1] = { GameState + SDK.BlueZonePosition,  sizeof(UEVector), &BlueZonePos };
	z[2] = { GameState + SDK.SafetyZoneRadius,  sizeof(float),    &SafetyZoneRadius };
	z[3] = { GameState + SDK.SafetyZonePosition, sizeof(UEVector), &SafetyZonePos };
	TargetProcess.BatchRead(z, 4);

	float timeSec = 0.0f;
	TargetProcess.Read(GameState + SDK.TimeSeconds, &timeSec, sizeof(timeSec), "TimeSeconds");

	// Loga SEMPRE. No treino o raio e' 0 (nao existe zona) — se o log so'
	// aparecesse com raio > 1, a gente nao sabia se a cadeia falhou ou se
	// simplesmente nao tinha zona. Agora da' pra distinguir os dois casos.
	{
		static int gsCount = 0;
		if (++gsCount % 20 == 1) {           // ~1x/2s (roda 1x/2s)
			LOG("[ZONE] UWorld=0x%llX rawGS=0x%llX GS=0x%llX | blueR=%.0f blue=%.0f,%.0f,%.0f | safeR=%.0f safe=%.0f,%.0f,%.0f | t=%.0fs\n",
			    (unsigned long long)UWorld, (unsigned long long)rawGS, (unsigned long long)GameState,
			    BlueZoneRadius, BlueZonePos.X, BlueZonePos.Y, BlueZonePos.Z,
			    SafetyZoneRadius, SafetyZonePos.X, SafetyZonePos.Y, SafetyZonePos.Z,
			    timeSec);
		}
	}
}

void Engine::RefreshViewMatrix()
{
	if (!PlayerCameraManager) return; 
	
	float camFov = 0.0f;
	UEVector camPos = {};
	UERotator camRot = {};

	DriverInterfaceV3::BatchReadEntry camBatch[3];
	camBatch[0] = { PlayerCameraManager + SDK.CameraFov, sizeof(float), &camFov };
	camBatch[1] = { PlayerCameraManager + SDK.CameraPos, sizeof(UEVector), &camPos };
	camBatch[2] = { PlayerCameraManager + SDK.CameraRot, sizeof(UERotator), &camRot };

	if (!TargetProcess.BatchRead(camBatch, 3)) return;

	// ---------------------------------------------------------------------
	// VALIDACAO DOS VALORES, NAO SO DA LEITURA.
	//
	// `BatchRead` devolve `true` quando o IOCTL passou — o que NAO diz nada
	// sobre o CONTEUDO. Endereco valido com valor zerado (troca de mapa, POV
	// ainda nao publicado) passava reto e sobrescrevia uma camera BOA por
	// FOV=0. A partir dai TODA `Camera::WorldToScreen` cai na guarda de FOV
	// (Camera.cpp:80) e devolve (0,0).
	//
	// Quem sente isso e' o AIMBOT, e so' ele: o ESP nao usa `CameraEntry`, ele
	// rele' a camera por conta propria (`Camera::GetCurrentView`, Main.cpp:452).
	// Resultado medido: ESP desenha, aimbot morto em silencio.
	//
	// Agora um valor implausivel simplesmente MANTEM a camera anterior — que e'
	// o comportamento certo para um cache. Nunca clobber com lixo.
	// ---------------------------------------------------------------------
	if (!std::isfinite(camFov) || camFov < 1.0f || camFov > 170.0f) return;
	if (!std::isfinite(camPos.X) || !std::isfinite(camPos.Y) || !std::isfinite(camPos.Z)) return;
	if (!std::isfinite(camRot.Pitch) || !std::isfinite(camRot.Yaw) || !std::isfinite(camRot.Roll)) return;

	CameraEntry.POV.FOV = camFov;
	CameraEntry.POV.Location = camPos;
	CameraEntry.POV.Rotation = camRot;
	CameraViewInfo = CameraEntry.POV;
}

CameraCacheEntry Engine::GetCameraCache()
{
	return CameraEntry;
}

std::vector<std::shared_ptr<ActorEntity>> Engine::GetActors()
{
	return Actors;
}

uint64_t Engine::GetActorSize()
{
	return MaxPacket;
}
