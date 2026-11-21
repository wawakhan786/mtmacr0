#include "driver_interface_v3.h"
#include "../Perf.h"   // instrumentacao: quantas leituras e quanto tempo no driver
#include <windows.h>
#include <iostream>
#include <winioctl.h>
#include <TlHelp32.h>
#include <random>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// OS IOCTLs NAO SAO DEFINIDOS AQUI.
//
// Eles vinham sendo redefinidos neste arquivo DEPOIS do `#include
// "driver_interface_v3.h"` -> `common.h`, e a redefinicao vencia (com
// `warning C4005`). Efeito: o header estava no padrao do driver velho
// (0x801 = READ) e o .cpp no do novo (0x801 = GET_PID); compilava, e o header
// errado nao fazia diferenca nenhuma porque este arquivo passava por cima.
//
// Agora a fonte unica e' `common.h`, e ele e' conferido contra o kernel pelo
// `tools\dump_driver\contract_check.py`. Se precisar de um IOCTL novo, ele
// entra LÁ e em `km_safe_source.c` — nao aqui.
// ---------------------------------------------------------------------------

// Injecao de mouse pelo KERNEL. Ver o comentario longo em km_safe_source.c
// (secao "4b"). O motivo de existir: `SendInput` marca o pacote com
// LLMHF_INJECTED, e qualquer hook WH_MOUSE_LL ve isso.
//
//   req.address = dx   (LONG, sinal estendido)
//   req.size    = dy   (LONG, sinal estendido)
// (o macro IOCTL_MOUSE_MOVE vem do common.h)

// ---------------------------------------------------------------------------
// IOCTLs de descarga (2.1)
//
//   GET_IMAGE_INFO (0x809) : out = { ULONG64 base; ULONG64 size; }
//   DEACTIVATE     (0x807) : sem entrada. Para o atendimento e sai do namespace.
//
// O SELFDESTRUCT (0x808) SAIU DE USO: o driver nao se libera mais. Ele continua
// definido no driver apenas para um user-mode de build antiga receber
// STATUS_NOT_IMPLEMENTED em vez de conversar com um caminho que nao existe.
// ---------------------------------------------------------------------------
// (IOCTL_KM_DEACTIVATE e IOCTL_KM_GET_IMAGE_INFO vem do common.h)

// ---------------------------------------------------------------------------
// UnitId e Flags da injecao de mouse.
//
// Vao no request como `pid` e `buffer` (campos que sobraram do contrato
// original) porque o layout de DRIVER_REQUEST e' fixo e o driver le' deles.
//
// UNITID: o `MouseClassServiceCallback` roteia o pacote por este numero para
// achar o device de classe. O valor certo foi MEDIDO na maquina real, varrendo
// as combinacoes com o tools\probe_driver.ps1:
//
//     UnitId=0 -> nada        UnitId=1 -> o cursor move        UnitId=2 -> nada
//
// Nao e' constante do jogo nem do driver: e' a unidade do device de mouse
// daquela maquina. Se um dia o cursor parar de novo, e' o primeiro suspeito —
// e a varredura esta' no probe, pronta (`-UnitId N`).
//
// FLAGS: 0 = MOUSE_MOVE_RELATIVE, ou seja LastX/LastY sao DELTAS (mickeys),
// a mesma unidade que o SendInput usa — por isso o tuning do aimbot/RCS nao
// precisou mudar quando o caminho virou kernel.
// ---------------------------------------------------------------------------
#define KM_MOUSE_UNITID  1
#define KM_MOUSE_FLAGS   0

// ---------------------------------------------------------------------------
// ESTADO DA INJECAO — para o diagnostico poder FALAR.
//
// O log deste arquivo e' `std::cout` (nao temos Pch.h/LOG aqui, ver acima), e
// o `std::cout` NAO chega no logs.txt. Resultado: a injecao podia estar
// falhando em silencio e ninguem sabia — nem no log, nem no console.
//
// Estes contadores sao lidos por quem TEM o LOG (Aimbot.cpp, na linha [RCS]) e
// aparecem no arquivo. E' o que distingue:
//
//   inj=none   -> InjectMouseMove nunca foi chamado (o RCS nao gerou movimento)
//   inj=0      -> chamado, e o IOCTL esta' FALHANDO (err = GetLastError)
//   inj=N      -> chamado e SUCEDIDO N vezes
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_mouseInjOk{ 0 };
std::atomic<uint64_t> g_mouseInjFail{ 0 };
std::atomic<uint32_t> g_mouseInjLastErr{ 0 };

// ---------------------------------------------------------------------------
// IOCTL_KM_GET_VERSION — QUEM respondeu.
//
// Tem que bater com o km_safe_source.c. Se os dois numeros divergirem, o
// driver em disco e o user-mode que o carrega sao incompativeis — e o sintoma
// e' exatamente o "driver fantasma": IOCTL devolvendo erro que parece bug de
// offset.
//
// O macro vem do common.h (era `IOCTL_GET_VERSION` local aqui, e o kernel
// chama de `IOCTL_KM_GET_VERSION` — mesmo valor 0x806, nome trocado).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// (IOCTL_KM_GET_VERSION, IOCTL_KM_DEACTIVATE, IOCTL_KM_SELFDESTRUCT,
//  KM_DESTRUCT_MAGIC e KM_DESTRUCT_UNMAP vem do common.h — aqui nao se
//  redefine nada. Ate' 2026 este bloco tinha `IOCTL_KM_DEACTIVATE` DUAS VEZES
//  no mesmo arquivo, e `IOCTL_GET_VERSION` com nome diferente do kernel
//  (`IOCTL_KM_GET_VERSION`): mesmo valor, nome divergente, zero aviso.)
// ---------------------------------------------------------------------------

// ===========================================================================
// DERIVACAO DO NOME — O ALGORITMO TEM QUE SER IDENTICO AO DO KERNEL
//
// ==================== NAO MEXA EM UM LADO SO' ====================
//
// Este bloco e' a COPIA EXATA de KmMixSeed/KmHex16/KmMixSeed(seed,1) do
// km_safe_source.c. Se os dois divergirem em um unico bit, o CreateFile nao
// acha o device e o sintoma e' "o driver nao carregou" — que e' exatamente o
// lugar errado pra se procurar um bug de nome.
//
// AS DUAS CONSTANTES MAGICAS SAO AS MESMAS dos dois lados:
//   KM_NAME_MIX       = 0x9E3779B97F4A7C15ULL  (proporcao aurea / MurmurHash)
//   multiplicadores   = 0xFF51AFD7ED558CCDULL e 0xC4CEB9FE1A85EC53ULL
//                       (finalizador do murmur3, escolhido por avalanche boa)
//
// O `kind` separa os nomes: 0 = driver object, 1 = symlink/device. O user-mode
// so' precisa do kind 1 (e' o unico nome que ele ve'), mas a constante fica
// aqui documentada porque o kernel usa os dois.
// ===========================================================================
static const uint64_t KM_NAME_MIX = 0x9E3779B97F4A7C15ULL;

static uint64_t KmMixSeedCpp(uint64_t seed, uint32_t kind)
{
    uint64_t k = seed ^ (KM_NAME_MIX * (uint64_t)(kind + 1));
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

// "\\.\<16 hex>" — o caminho que o CreateFileA usa.
static std::string DeriveDeviceName(uint64_t seed)
{
    static const char hexd[] = "0123456789abcdef";
    uint64_t v = KmMixSeedCpp(seed, 1);
    std::string s("\\\\.\\");

    s.reserve(4 + 16);
    for (int i = 15; i >= 0; i--)
        s.push_back(hexd[(v >> (i * 4)) & 0xF]);
    return s;
}

// ---------------------------------------------------------------------------
// DeviceNameAtual — o nome que ESTA sessao vai abrir.
//
// Existe para o LOG. O nome do device deixou de ser um literal conhecido (era
// "\\.\Ndu2"), e sem imprimi-lo nao ha' como conferir a mao se o device subiu —
// nem como as ferramentas de teste (probe_driver.ps1, TestHarness.exe) saberem
// em quem apontar.
//
// Isto NAO e' uma porta dos fundos da ofuscacao: o nome so' existe em memoria,
// derivado do seed da sessao, e o log fica no disco do proprio He. O ganho
// continua: o literal sumiu dos DOIS binarios.
// ---------------------------------------------------------------------------
std::string DeviceNameAtual(uint64_t seed)
{
    return DeriveDeviceName(seed);
}

struct DRIVER_REQUEST {
    int pid;
    uintptr_t address;
    void* buffer;
    size_t size;
    const wchar_t* name;
};

DriverInterfaceV3::DriverInterfaceV3() : hDriver(INVALID_HANDLE_VALUE), currentPid(0), baseAddress(0), driverVersion(0), seed(0) {}

bool DriverInterfaceV3::InitializeKevent(bool (*loader)(uint64_t seed))
{
    if (loader == nullptr) return false;
    if (kevent == nullptr) kevent = new DriverInterfaceKEvent();

    kevent->SetSeed(seed);
    if (!kevent->Initialize("kmproto_kevent", loader)) {
        std::cout << "[kevent] Initialize falhou" << std::endl;
        return false;
    }
    driverVersion = kevent->GetDriverVersion();
    std::cout << "[kevent] READY, backend = shared memory + KEVENT" << std::endl;
    return kevent->DriverVersionOk();
}

DriverInterfaceV3::~DriverInterfaceV3() {
    Cleanup();
}

void DriverInterfaceV3::Cleanup() {
    // -----------------------------------------------------------------------
    // O BACKEND KEVENT NAO USA `hDriver`.
    //
    // Ele vive de secao + dois eventos + uma thread no kernel, sem device e
    // sem handle de dispositivo. O `delete kevent` estava DENTRO do
    // `if (hDriver != INVALID_HANDLE_VALUE)`, entao numa sessao 100% KEVENT
    // (hDriver nunca e' aberto) o destrutor nunca rodava: view mapeada,
    // secao, eventos e a THREAD DO SERVIDOR ficavam vivos depois do "unload".
    //
    // E' exatamente o vazamento que o RELATORIO_ARQUITETURA.md marca como
    // criterio de aprovacao ("200 ciclos de abrir/fechar com !handle estavel").
    // -----------------------------------------------------------------------
    if (kevent != nullptr) {
        // Handshake de parada ANTES de destruir: o `ParadaConfirmada()`
        // e' escrito dentro do Cleanup do backend (que espera Ativo==0).
        // O valor fica guardado porque, depois do `delete`, nao ha' mais
        // objeto para perguntar — e e' ele que autoriza liberar paginas.
        kevent->Cleanup();
        paradaKeventOk = kevent->ParadaConfirmada();
        delete kevent;
        kevent = nullptr;
    }
    if (hDriver != INVALID_HANDLE_VALUE) {
        CloseHandle(hDriver);
        hDriver = INVALID_HANDLE_VALUE;
    }
}

bool DriverInterfaceV3::Initialize() {
    // -----------------------------------------------------------------------
    // IDEMPOTENTE — nao reabre o que ja' esta' aberto.
    //
    // Esta funcao e' chamada a cada SEGUNDO pelo laco de espera do TslGame
    // (Main.cpp: `while (GIsRunning) { TargetProcess.Init(...); sleep(1s); }`),
    // e cada chamada fazia um CreateFile SEM fechar o anterior.
    //
    // O resultado era um handle vazando por segundo enquanto o jogo nao
    // abria — 60 por minuto, 3.600 numa hora. O log denunciou, porque a linha
    // "[+] Driver initialized successfully." aparecia repetida, uma por
    // segundo, ao lado de "Processo nao encontrado, aguardando...".
    //
    // Devolver true sem reabrir e' o comportamento correto: o handle ja' esta'
    // valido e aponta pro mesmo device.
    // -----------------------------------------------------------------------
    if (hDriver != INVALID_HANDLE_VALUE) return true;

    // -----------------------------------------------------------------------
    // BACKEND KEVENT NAO TEM DEVICE PARA ABRIR.
    //
    // Esta funcao faz `CreateFile` no symbolic link derivado do seed. Num
    // backend de secao + eventos esse device NAO EXISTE, entao ela devolvia
    // false — e o `Memory::Init` abortava antes de qualquer leitura, mesmo
    // com o canal KEVENT pronto. O sintoma seria "Initialization failed! Is
    // the driver loaded?" com o driver carregado.
    //
    // Quem manda aqui e' a view da secao: se ela existe, o canal existe.
    // -----------------------------------------------------------------------
    if (kevent != nullptr) return kevent->HandleValido();

    // O NOME VEM DO SEED (versao 1.2), NAO MAIS DE UM LITERAL.
    //
    // Era "\\\\.\\PUBG_KM", depois "\\\\.\\Ndu2". Os dois eram strings que
    // ficavam DENTRO do mtmacr0.exe, entao uma varredura do binario achava o
    // nome do driver e o alvo de uma vez.
    //
    // Agora: DeriveDeviceName(seed) monta "\\.\<16 hex>" com o MESMO algoritmo
    // que o km_safe_source.c usa. Nenhum literal sobra nos dois binarios.
    //
    // Se o seed ainda for 0 (SetSeed nao foi chamado), cai no valor default do
    // MapDriver — que tambem faz o driver recusar a carga. E' de proposito:
    // melhor falhar alto do que abrir um device com nome que ninguem derivou.
    {
        std::string nome = DeriveDeviceName(seed);
        hDriver = CreateFileA(nome.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    }
    if (hDriver == INVALID_HANDLE_VALUE) {
        return false;
    }

    // -----------------------------------------------------------------------
    // QUEM RESPONDEU — a pergunta que o CreateFile NAO faz.
    //
    // O CreateFile acima ter sucesso prova so' que existe um device com esse
    // nome. E o kdmapper NAO descarrega: o device de uma sessao anterior
    // continua vivo. Entao "abriu" nao significa "e' o driver que eu espero".
    //
    // O PING tambem nao resolve — KM_PING_MAGIC e' igual em toda build. Foi
    // assim que a gente passou rodadas concluindo "driver antigo" a partir de
    // um [err] que na verdade significava "o driver novo nunca subiu".
    //
    // AQUI SO' PERGUNTA E GUARDA. Nao imprime nada.
    //
    // A primeira versao disto imprimia em `std::cout`, e o log mostrou que
    // isso nao serve: o `std::cout` deste arquivo NAO chega no logs.txt (ele
    // nao inclui o Pch.h, entao nao tem o LOG), e ainda rodava a cada segundo
    // pelo mesmo motivo do handle. Quem decide e REPORTA e' o Main.cpp, que
    // tem o LOG e ve as duas versoes lado a lado.
    // -----------------------------------------------------------------------
    driverVersion = GetDriverVersion();

    return true;
}

// ---------------------------------------------------------------------------
// GetDriverVersion — devolve 0 quando o driver nao conhece o IOCTL.
//
// Zero NAO e' "erro de comunicacao": e' a resposta correta para "voce e' a
// versao 1.1?" vinda de um driver 1.0. Por isso o valor de sucesso nunca pode
// ser 0 — e o KM_DRIVER_VERSION comeca em 0x00010000.
// ---------------------------------------------------------------------------
ULONG DriverInterfaceV3::GetDriverVersion() {
    if (hDriver == INVALID_HANDLE_VALUE) return 0;

    ULONG ver = 0;
    DWORD bytes = 0;
    if (DeviceIoControl(hDriver, IOCTL_KM_GET_VERSION, nullptr, 0, &ver, sizeof(ver), &bytes, nullptr)) {
        return ver;
    }
    return 0;
}

bool DriverInterfaceV3::DriverVersionOk() const {
    // -----------------------------------------------------------------------
    // CADA BACKEND TEM A SUA VERSAO — e o comentario aqui ja' dizia isso.
    //
    // MEDIDO no smoke de 20/10/2026: o driver respondeu 0x00000003 (KMP_VERSION)
    // e esta funcao devolveu FALSE porque comparava com
    // `KM_DRIVER_VERSION_ESPERADA` (0x00010002), que e' a versao do driver
    // IOCTL. O log saiu contraditorio:
    //
    //     [-] transporte respondeu versao 0x00000003 (esperado 0x00000003)
    //
    // ...numero igual, veredito "errado". O loader abortava com o driver
    // carregado e o canal aberto.
    // -----------------------------------------------------------------------
    if (kevent != nullptr) return kevent->DriverVersionOk();
    return driverVersion == KM_DRIVER_VERSION_ESPERADA;
}

// ---------------------------------------------------------------------------
// Seed — ver o comentario no header e o bloco de derivacao no topo do arquivo.
// ---------------------------------------------------------------------------
void DriverInterfaceV3::SetSeed(uint64_t s) { seed = s; }
uint64_t DriverInterfaceV3::GetSeed() const { return seed; }

// Ver o comentario no header: e' o que impede o caminho de saida de tentar
// descarregar um driver que nunca foi mapeado (gate bloqueado, falha de mapa,
// modo map-only).
bool DriverInterfaceV3::HandleValido() const {
    // -----------------------------------------------------------------------
    // "O CANAL ESTA ABERTO?", NAO "O DEVICE ESTA ABERTO?".
    //
    // Era `hDriver != INVALID_HANDLE_VALUE`, e num backend SEM device isso e
    // sempre falso. O `DescarregarDriver` usa esta funcao como PRIMEIRA guarda:
    // ela devolvia false, o unload saia na primeira linha e nada era liberado —
    // nem a thread do servidor, nem a secao, nem as paginas mapeadas.
    //
    // MEDIDO no smoke de 20/10/2026: o log mostrou "parando o servidor e
    // liberando as paginas" e "FIM" sem NENHUMA linha do descarregamento (nem
    // "servidor KEVENT parado", nem "PAGINAS LIBERADAS"). A guarda matou o
    // unload em silencio, e o teste passou achando que tinha liberado tudo.
    // -----------------------------------------------------------------------
    if (kevent != nullptr) return kevent->HandleValido();
    return hDriver != INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
// Deactivate — passo 1 e 2 da descarga.
//
// Manda o IOCTL e fecha o NOSSO handle. O fechamento nao e' opcional: enquanto
// este handle existir, a contagem de handles no kernel nao zera e o unmap
// (SELFDESTRUCT com KM_DESTRUCT_UNMAP) aborta por seguranca — deixando o
// codigo residente.
//
// O IOCTL devolve STATUS_DELETE_PENDING em algumas situacoes normais (o driver
// ja' tinha desligado). Isso NAO e' erro: o objetivo foi alcancado.
// ---------------------------------------------------------------------------
bool DriverInterfaceV3::Deactivate()
{
    bool enviado = false;

    if (hDriver != INVALID_HANDLE_VALUE) {
        DRIVER_REQUEST req = {0};
        DWORD bytes = 0;

        enviado = DeviceIoControl(hDriver, IOCTL_KM_DEACTIVATE,
                                  &req, sizeof(req), nullptr, 0, &bytes, nullptr) != FALSE;
        if (!enviado) {
            DWORD err = GetLastError();
            // 1167 = ERROR_DEVICE_NOT_CONNECTED; 0x8007001F = device removido.
            // Nos dois casos o device ja' foi: tratamos como sucesso.
            if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_NOT_FOUND ||
                err == ERROR_NO_SUCH_DEVICE) {
                enviado = true;
            }
        }

        Cleanup();
    } else {
        enviado = true;   // nada aberto, nada a desativar
    }

    driverVersion = 0;
    return enviado;
}

// ===========================================================================
// UnmapComInfo — a descarga na 2.1.
//
// ==================== O QUE ESTA FUNCAO NAO FAZ ====================
//
// ELA NAO LIBERA AS PAGINAS. Quem faz isso e' o mapper, com o
// `intel_driver::MmFreeIndependentPages`. Esta classe nao tem acesso ao mapper,
// e foi justamente por tentar fazer isso DENTRO DO KERNEL que a maquina levou
// KERNEL_SECURITY_CHECK_FAILURE (ver o bloco no km_safe_source.c).
//
// O QUE ELA FAZ, e a ordem importa:
//
//   1. GET_IMAGE_INFO  -> le' { base, size } ENQUANTO o device existe.
//                         Depois do passo 2 nao ha' mais como perguntar.
//   2. DEACTIVATE      -> para o atendimento, tira device e symlink.
//                         Seguro: so' mexe em objetos do object manager.
//   3. Cleanup         -> fecha o NOSSO handle.
//
// E devolve base/size para o chamador entregar ao mapper.
//
// POR QUE A ESPERA DE 150 ms
//
// O `Cleanup` fecha o handle no user-mode, mas o kernel pode ainda estar
// processando o IRP_MJ_CLOSE. O mapper vai liberar as paginas via "call gadget"
// logo depois; sem este respiro, existiria uma janela em que a liberacao
// acontece com uma referencia ao device ainda em transito.
//
// DEPOIS DESTA FUNCAO O DEVICE NAO EXISTE MAIS. Para voltar a usar, mapear de
// novo com um seed novo (o seed desta sessao ja' foi usado).
// ===========================================================================
bool DriverInterfaceV3::UnmapComInfo(uint64_t* base, uint64_t* size)
{
    if (base) *base = 0;
    if (size) *size = 0;

    if (hDriver == INVALID_HANDLE_VALUE) {
        return true;   // nada aberto: nada a desativar
    }

    // ---- 1) ONDE O DRIVER ESTA' (antes de qualquer coisa sair do ar) ----
    //
    // -----------------------------------------------------------------------
    // DOIS CASOS, E ELES PEDEM RESPOSTAS OPOSTAS.
    //
    // A primeira versao logava so' "GET_IMAGE_INFO falhou", cobrindo:
    //
    //   (a) o DeviceIoControl FALHOU
    //       -> o driver nao conhece o IOCTL (build anterior a 2.1), ou o
    //          dispatch devolve DELETE_PENDING (driver ja' desativado numa
    //          sessao anterior e deixado residente)
    //
    //   (b) o DeviceIoControl deu CERTO e os valores vieram ZERO
    //       -> o DriverEntry nao leu o tamanho da propria imagem e zerou a base
    //
    // Sao caminhos opostos: (a) e' protocolo/estado, (b) e' bug no DriverEntry.
    // Um log so' para os dois e' erro de INSTRUMENTACAO.
    //
    // E foi exatamente o que aconteceu: o log do teste mostrou
    // "GET_IMAGE_INFO devolveu 0" e eu fiquei adivinhando qual dos dois era.
    // Adivinhar e' o que este projeto inteiro tenta nao fazer.
    // -----------------------------------------------------------------------
    {
        ULONG64 info[2] = { 0, 0 };
        DWORD   bytes   = 0;

        SetLastError(0);
        const BOOL ok = DeviceIoControl(hDriver, IOCTL_KM_GET_IMAGE_INFO,
                                        nullptr, 0, info, sizeof(info), &bytes, nullptr);
        const DWORD err = ok ? 0 : GetLastError();

        if (base) *base = info[0];
        if (size) *size = info[1];

        if (!ok) {
            std::cout << "[drv] GET_IMAGE_INFO FALHOU: err=" << err
                      << " bytes=" << bytes << std::endl;
            if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_NO_SUCH_DEVICE) {
                std::cout << "[drv]   -> device desconectado: o driver ja' foi"
                             " desativado (DELETE_PENDING)" << std::endl;
            } else if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
                std::cout << "[drv]   -> o driver NAO conhece este IOCTL:"
                             " build anterior a 2.1" << std::endl;
            }
        } else if (info[0] == 0 || info[1] == 0) {
            std::cout << "[drv] GET_IMAGE_INFO RESPONDEU ZERO: base=0x"
                      << std::hex << info[0] << " size=0x" << info[1]
                      << std::dec << std::endl;
            std::cout << "[drv]   -> o DriverEntry nao leu o SizeOfImage da"
                         " propria imagem e zerou a base por seguranca" << std::endl;
        } else {
            std::cout << "[drv] GET_IMAGE_INFO OK: base=0x" << std::hex << info[0]
                      << " size=0x" << info[1] << std::dec << std::endl;
        }
    }

    // ---- 2) DEACTIVATE (o caminho seguro) ----
    const bool ok = Deactivate();   // ja' faz o Cleanup() no fim

    // ---- 3) CONFIRMAR a demolicao, em vez de dormir e torcer ----
    //
    // A versao anterior era `sleep_for(150ms)` com o comentario "o kernel
    // precisa VER o close". Isso e' esperanca, nao sincronizacao: 150 ms nao
    // prova nada sobre o estado do kernel, e num disco ocupado pode ser pouco.
    //
    // O que da' para PROVAR daqui, sem IOCTL novo: o nome do device e' derivado
    // do seed desta sessao, entao basta tentar abrir de novo. Depois que o
    // driver apaga o symlink (KmShutdownDevice, no ultimo CLOSE), o CreateFile
    // para de resolver o nome.
    //
    //   ERROR_FILE_NOT_FOUND / PATH_NOT_FOUND
    //       -> a demolicao COMPLETOU: symlink e device fora do namespace.
    //
    //   ERROR_ACCESS_DENIED / ERROR_DELETE_PENDING (ou qualquer falha)
    //       -> o nome ainda existe, mas o device esta' quiescido e recusa
    //          tudo. Operacionalmente e' o mesmo: ninguem usa mais.
    //
    //   ABRIU
    //       -> o driver NAO foi demolido. E' o caso que o sleep escondia: o
    //          mapper ia liberar as paginas de um device ainda vivo.
    //
    // Este laco e' a diferenca entre "eu esperei" e "eu verifiquei".
    {
        const uint64_t seedSessao = seed;
        const std::string nome = DeriveDeviceName(seedSessao);

        const auto limite = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(500);
        DWORD ultimoErro = ERROR_SUCCESS;
        bool  aindaAbre  = false;
        int   tentativas = 0;

        while (std::chrono::steady_clock::now() < limite) {
            ++tentativas;

            HANDLE h = CreateFileA(nome.c_str(),
                                   GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);

            if (h == INVALID_HANDLE_VALUE) {
                ultimoErro  = GetLastError();
                aindaAbre   = false;
                if (ultimoErro == ERROR_FILE_NOT_FOUND ||
                    ultimoErro == ERROR_PATH_NOT_FOUND) {
                    break;      // demolicao completa: parar aqui e' o caso bom
                }
                // Qualquer outra recusa tambem serve: o device nao aceita mais
                // ninguem. Nao insiste — quem garante o resto e' o g_ShuttingDown.
                break;
            }

            // Abriu: o device ainda responde. Fecha e tenta de novo.
            CloseHandle(h);
            aindaAbre = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (aindaAbre) {
            std::cout << "[drv] ATENCAO: o device AINDA ABRE depois do deactivate"
                         " (" << tentativas << " tentativas). O mapper NAO deve"
                         " liberar as paginas agora." << std::endl;
        } else if (ultimoErro == ERROR_FILE_NOT_FOUND ||
                   ultimoErro == ERROR_PATH_NOT_FOUND) {
            std::cout << "[drv] teardown CONFIRMADO: o nome nao resolve mais"
                         " (err=" << ultimoErro << ", " << tentativas
                      << " tentativas)" << std::endl;
        } else {
            std::cout << "[drv] device quiescido (err=" << ultimoErro
                      << ") - nao aceita mais ninguem, nome ainda no namespace"
                      << std::endl;
        }
    }

    hDriver = INVALID_HANDLE_VALUE;
    driverVersion = 0;
    return ok;
}

// ---------------------------------------------------------------------------
// Unmap — MANTIDO por compatibilidade, mas NAO e' mais o caminho de saida.
//
// Ele faz o DEACTIVATE e devolve, sem liberar as paginas — porque quem libera e'
// o mapper, e esta classe nao fala com ele. Quem chama `Unmap()` continua com o
// device fora do namespace; as paginas ficam.
//
// O CAMINHO CERTO E' `UnmapComInfo`, que devolve base/tamanho para o
// `DescarregarDriver()` do Main.cpp entregar ao mapper.
// ---------------------------------------------------------------------------
bool DriverInterfaceV3::Unmap()
{
    return UnmapComInfo(nullptr, nullptr);
}

int DriverInterfaceV3::GetProcessId(const wchar_t* name)
{
    if (UsandoKevent()) return kevent->GetProcessId(name);
    if (hDriver == INVALID_HANDLE_VALUE) return 0;
    DRIVER_REQUEST req = {0};
    req.name = name;
    DWORD bytes = 0;
    int pid = 0;
    if (DeviceIoControl(hDriver, IOCTL_GET_PID, &req, sizeof(req), &pid, sizeof(pid), &bytes, nullptr)) {
        return pid;
    }
    
    // Fallback to usermode PID grab if kernel fails
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                if (wcscmp(pe32.szExeFile, name) == 0) {
                    pid = pe32.th32ProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
    return pid;
}

void DriverInterfaceV3::SetCurrentPid(int pid) {
    currentPid = pid;
}

uintptr_t DriverInterfaceV3::GetModuleBase(int pid, const wchar_t* module_name)
{
    if (UsandoKevent()) return kevent->GetModuleBase(pid, module_name);
    if (hDriver == INVALID_HANDLE_VALUE) return 0;
    DRIVER_REQUEST req = {0};
    req.pid = pid;
    req.name = module_name;
    DWORD bytes = 0;
    uintptr_t base = 0;
    if (DeviceIoControl(hDriver, IOCTL_GET_MODULE_BASE, &req, sizeof(req), &base, sizeof(base), &bytes, nullptr)) {
        return base;
    }
    return 0;
}

void DriverInterfaceV3::SetBaseAddress(uintptr_t base) {
    baseAddress = base;
}

uintptr_t DriverInterfaceV3::FindRealCr3(int pid, uintptr_t base_address, uintptr_t data_anchor) {
    return 0; // Handled internally by driver if needed
}

uintptr_t DriverInterfaceV3::GetProcessCr3(int pid) {
    return 0; 
}

bool DriverInterfaceV3::ReadMemory(int pid, uintptr_t address, void* buffer, size_t size, const char* debugName)
{
    if (UsandoKevent()) return kevent->ReadMemory(pid, address, buffer, size, debugName);
    if (hDriver == INVALID_HANDLE_VALUE || !address || !buffer || !size) return false;
    DRIVER_REQUEST req = {0};
    req.pid = pid;
    req.address = address;
    req.buffer = buffer;
    req.size = size;
    DWORD bytes = 0;
    // -----------------------------------------------------------------------
    // INSTRUMENTADO — o SUSPEITO NUMERO 1 da auditoria. NAO muda comportamento.
    //
    // `BatchReadMemory` nao e' um lote: e' um laco chamando esta funcao uma vez
    // por entrada. Cada chamada aqui e' um `DeviceIoControl` COMPLETO —
    // transicao user->kernel mais a copia que o METHOD_BUFFERED faz dos dois
    // lados. O `MAX_BATCH_ENTRIES=128` e o fatiamento em Engine.cpp existem para
    // reduzir idas ao kernel e nao reduzem nenhuma.
    //
    // O numero que decide se isso importa e' `ioctlNs / singleReads` — a
    // latencia MEDIA por leitura. Sem ele, IOCTL/s nao vira ms/tick.
    // -----------------------------------------------------------------------
    if (Perf::Ativo()) {
        Perf::Contar(Perf::C.singleReads);
        Perf::Escopo eIo(Perf::C.ioctlNs);
        return DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req),
                               req.buffer, req.size, &bytes, nullptr) != FALSE;
    }
    return DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req), req.buffer, req.size, &bytes, nullptr);
}

bool DriverInterfaceV3::BatchReadMemory(int pid, BatchReadEntry* entries, size_t count)
{
    if (UsandoKevent()) {
        // O lote do KEVENT aceita 64 por vez; o adaptador fatia. O cast e'
        // seguro porque os dois BatchReadEntry tem layout identico (3.
        // ponteiros/tamanhos, mesmos nomes).
        return kevent->BatchReadMemory(pid, (DriverInterfaceKEvent::BatchReadEntry*)entries, count);
    }
    if (hDriver == INVALID_HANDLE_VALUE || !entries || count == 0) return false;
    // `batchCalls` = quantos LOTES; `batchEntries` = quantas ENTRADAS — e o
    // numero de entradas e' o numero de `DeviceIoControl` de verdade. A razao
    // entre os dois mostra o quanto o nome "lote" esta' mentindo.
    Perf::Contar(Perf::C.batchCalls);
    Perf::Contar(Perf::C.batchEntries, (uint64_t)count);

    bool allSuccess = true;
    for (size_t i = 0; i < count; i++) {
        if (!ReadMemory(pid, entries[i].Address, entries[i].Buffer, entries[i].Size, nullptr)) {
            allSuccess = false;
        }
    }
    return allSuccess;
}

void DriverInterfaceV3::TestRandomMouseMoveLoop(std::atomic_bool& running) {
    // Movimento leve e aleatorio enquanto `running` estiver ligado.
    // Usado no teste de "hardware spoofer" (ALT). Nao faz parte do aimbot.
    static std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(-2, 2);
    while (running.load(std::memory_order_relaxed)) {
        InjectMouseMove(dist(rng), dist(rng));
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }
}

// ---------------------------------------------------------------------------
// Injecao de mouse (movimento RELATIVO).
//
// O QUE MUDOU, E POR QUE
//
// Antes isto era so' `SendInput`, e o comentario aqui dizia que o driver nao
// expunha IOCTL de mouse. Isso era verdade e virou um problema medivel:
//
//   `SendInput` faz o PROPRIO Windows marcar o pacote com `LLMHF_INJECTED` no
//   `MSLLHOOKSTRUCT`. Qualquer hook `WH_MOUSE_LL` le essa flag — e anti-cheat
//   instala hook de mouse de baixo nivel rotineiramente. Ou seja: TODA
//   correcao do RCS e todo movimento do aimbot chegavam ao outro lado com uma
//   etiqueta do sistema dizendo "isto nao veio de um mouse de verdade".
//
// Agora o caminho preferido e' o IOCTL 0x805, que entrega o pacote direto ao
// service callback do driver de CLASSE do mouse (dentro do kernel, o mesmo
// caminho que o dispositivo de hardware usa). Esse pacote nao nasce com a
// flag.
//
// O `SendInput` CONTINUA AQUI como fallback, e isso e' deliberado: o callback
// do Mouclass e' descoberto em runtime (nao ha' offset fixo, ver o driver).
// Se a descoberta falhar naquela versao do Windows, o cheat tem que continuar
// funcionando — com a flag, mas funcionando. Falhar em voz alta e degradar e'
// melhor que nao mexer o mouse.
//
// POR QUE ISTO NAO LOGa TODA HORA: o fallback roda a 144 Hz junto com o RCS.
// Uma linha de log por movimento encheria o arquivo e custaria mais que a
// propria injecao (ver BaseLog: abre, escreve, da flush e fecha o arquivo).
// Loga UMA vez, para o diagnostico saber qual caminho esta' ativo.
// ---------------------------------------------------------------------------
// ===========================================================================
// SUBDIVISAO DO MOVIMENTO — a parte que o JOGO ve'.
//
// ---------------------------------------------------------------------------
// O PROBLEMA, e ele nao e' de ofuscacao
//
// Ate' aqui, todo movimento do RCS e do aimbot saia como UM pacote so':
//
//     IOCTL_MOUSE_MOVE(dx = 40, dy = 12)   ->  1 pacote
//
// Um mouse REAL nao gera isso. O HID reporta enquanto o mouse esta' sendo
// movido e, no instante do pooling, reporta o DELTA daquele intervalo. Com o
// mouse parado, `i8042prt` reporta 0,0 — e nao ha' "delta acumulado" esperando
// para sair quando o mouse voltar a se mexer.
//
// Ou seja: a soma daqueles 40 pixels nao "estava" la'. Um hand move que pula
// 40 pixels num unico pacote e', fisicamente, ~40 ms de movimento em um
// instante — e o pooling do mouse roda a 125/500/1000 Hz. Sao coisas
// diferentes, e a diferenca e' observavel por quem olha a RAJADA de pacotes,
// nao o pacote.
//
// ---------------------------------------------------------------------------
// O QUE ESTA FUNCAO FAZ
//
// Quebra o delta em pacotes menores e os emite em SEQUENCIA, com:
//
//   - tamanho por pacote de 3 a 9 pixels (por eixo) — a ordem de grandeza do
//     que um mouse real reporta por polling em movimento normal;
//   - FRONTEIRA ALEATORIA entre os pedacos, entao dois movimentos de mesmo
//     tamanho nao produzem a mesma sequencia de pacotes;
//   - atraso de ~1 ms entre pacotes, para a rajada ter a mesma escala de tempo
//     que uma rajada de hardware (e nao um `dump` instantaneo).
//
// Tenha claro o que isso compra e o que NAO compra:
//
//   COMPRA: a RAJADA de pacotes passa a ter tamanho e timing plausiveis. Se
//   alguem estiver olhando o stream de input em vez do pacote isolado, deixa
//   de existir o salto de 40 px num instante.
//
//   NAO COMPRA: o pacote em si ja' era nativo (verdes o caminho de kernel,
//   `LLMHF_INJECTED` ausente). E o PONTO FINAL do movimento e' o mesmo — se o
//   aimbot mirar com precisao impossivel, subdividir nao muda isso.
//
// ---------------------------------------------------------------------------
// POR QUE NAO SUBDIVIDIR SEMPRE
//
// Correcao de RCS e' de 1 a 3 pixels. Subdividir isso daria UM pacote de
// qualquer forma, com um `Sleep` de custo puro no meio do frame. O limiar
// existe para o caminho comum nao pagar nada.
//
// ---------------------------------------------------------------------------
// O CUSTO DE TEMPO — e o teto e' de proposito
//
// A 144 Hz o frame dura ~7 ms. O cap de pacotes por chamada (`kMaxPacotes`)
// existe para o desenho nunca furar o frame: 6 pacotes x ~1 ms = ~5 ms no pior
// caso. Se o delta fosse grande demais para subdividir dentro do cap, o
// RESTANTE e' emitido num pacote final — melhor um pacote grande no fim do que
// perder movimento e a mira ficar sistematicamente atras.
// ===========================================================================
namespace {

constexpr int kDivergirAcimaDe = 8;    // por eixo; abaixo disso, 1 pacote so'
constexpr int kMaxPacotes      = 6;    // teto de tempo por chamada (~5 ms)
constexpr int kPacoteMin       = 3;    // pixels por pacote (minimo)
constexpr int kPacoteMax       = 9;    // pixels por pacote (maximo)

// Gerador simples — nao precisa ser bom, precisa nao repetir.
uint32_t g_jitEstado = 0x9E3779B9u;
inline uint32_t Jit(void) {
    g_jitEstado ^= g_jitEstado << 13;
    g_jitEstado ^= g_jitEstado >> 17;
    g_jitEstado ^= g_jitEstado << 5;
    return g_jitEstado;
}
inline int JitEntre(int a, int b) {
    if (b <= a) return a;
    return a + (int)(Jit() % (uint32_t)(b - a + 1));
}

// ---------------------------------------------------------------------------
// POR QUE NAO `std::min`/`std::max` AQUI
//
// O `windows.h` define `min` e `max` como MACRO (a menos que NOMINMAX esteja
// definido, e neste projeto nao esta'). Resultado de tentar usar os dois:
//
//     error C2589: '(': token invalido no lado direito de '::'
//
// A macro come o nome antes do compilador ver o `std::`. Escrever
// `(std::min)(a, b)` com parenteses contorna — e e' feio de ler e facil de
// esquecer em uma linha.
//
// Estes tres helpers evitam a discussao inteira, e sao auto-explicativos em
// ponto de uso. Sao `inline` e triviais: o compilador gera o mesmo codigo.
// ---------------------------------------------------------------------------
inline int KmAbs(int v)          { return v < 0 ? -v : v; }
inline int KmMin(int a, int b)   { return a < b ? a : b; }
inline int KmMax(int a, int b)   { return a > b ? a : b; }

} // namespace

// Emite UM pacote: kernel se der, SendInput se nao. Devolve true se foi pelo
// kernel (o chamador usa para os contadores de diagnostico).
bool DriverInterfaceV3::EnviarUmPacote(int x, int y, bool forcarSendInput) {
    if (x == 0 && y == 0) return false;

    // -----------------------------------------------------------------------
    // BACKEND KEVENT PRIMEIRO.
    //
    // Este caminho nao existia: o pacote ia direto para o IOCTL, e num backend
    // sem device o `hDriver` e' invalido — caia no `SendInput` SEMPRE, mesmo
    // com o `KMP_OP_MOUSE` implementado no driver. O mouse "funcionava", mas
    // com `LLMHF_INJECTED`, que e' justamente o que a injecao por kernel
    // existe para evitar.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // BISSEÇÃO PARA A VM: `set MTMACR0_NO_KERNEL_MOUSE=1` desliga a
    // injeção por kernel e força o SendInput. Serve para separar "o
    // transporte está quebrado" de "a resolução do Mouclass está quebrada"
    // sem recompilar nada — e o driver de KEVENT é o único lugar onde um
    // ponteiro ruim vira tela azul em vez de erro.
    // -----------------------------------------------------------------------
    static const bool semMouseKernel = [] {
        char* v = nullptr; size_t n = 0;
        if (_dupenv_s(&v, &n, "MTMACR0_NO_KERNEL_MOUSE") != 0 || v == nullptr) return false;
        const bool ligado = (v[0] == '1');
        free(v);
        return ligado;
    }();
    if (semMouseKernel) forcarSendInput = true;

    if (!forcarSendInput && UsandoKevent()) {
        if (kevent->EnviarUmPacote(x, y)) {
            g_mouseInjOk.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_mouseInjFail.fetch_add(1, std::memory_order_relaxed);
    }

    if (!forcarSendInput && !UsandoKevent() && hDriver != INVALID_HANDLE_VALUE) {
        DRIVER_REQUEST req = {0};
        req.pid     = KM_MOUSE_UNITID;
        req.address = (uintptr_t)(intptr_t)x;                  // dx
        req.buffer  = (void*)(uintptr_t)KM_MOUSE_FLAGS;        // Flags
        req.size    = (size_t)(intptr_t)y;                     // dy

        DWORD bytes = 0;
        if (DeviceIoControl(hDriver, IOCTL_MOUSE_MOVE, &req, sizeof(req),
                            nullptr, 0, &bytes, nullptr)) {
            g_mouseInjOk.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        g_mouseInjFail.fetch_add(1, std::memory_order_relaxed);
        g_mouseInjLastErr.store((uint32_t)GetLastError(), std::memory_order_relaxed);
    }

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = (LONG)x;
    input.mi.dy = (LONG)y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(INPUT));
    return false;
}

void DriverInterfaceV3::InjectMouseMove(int x, int y, bool forcarSendInput) {
    if (x == 0 && y == 0) return;

    // -----------------------------------------------------------------------
    // CAMINHO COMUM: movimento pequeno vai num pacote so'.
    //
    // RCS e correcoes finas caem aqui, e nao pagam nem o laco nem o Sleep.
    // -----------------------------------------------------------------------
    const int maxEixo = KmMax(KmAbs(x), KmAbs(y));

    if (maxEixo <= kDivergirAcimaDe || forcarSendInput) {
        static bool logouKernel = false;
        const bool foiKernel = EnviarUmPacote(x, y, forcarSendInput);

        // Log UMA vez, para o diagnostico saber qual caminho esta' ativo.
        // (std::cout e nao LOG(): este arquivo nao inclui o Pch.h.)
        if (foiKernel && !logouKernel) {
            logouKernel = true;
            std::cout << "[mouse] injecao por KERNEL ativa (" << (UsandoKevent() ? "KEVENT" : "IOCTL 0x805") << ")" << std::endl;
        } else if (!foiKernel && !forcarSendInput) {
            static bool logouFalha = false;
            if (!logouFalha) {
                logouFalha = true;
                std::cout << "[mouse] injecao por kernel falhou (err=" << g_mouseInjLastErr.load()
                          << ") - usando SendInput" << std::endl;
            }
        }
        return;
    }

    // -----------------------------------------------------------------------
    // MOVIMENTO GRANDE: subdivide em rajada.
    //
    // `restante` e' SEMPRE o que falta para o total. Cada pacote consome uma
    // fracao dele, nunca mais. Se o laco terminar no cap, o restante sai num
    // pacote final — o ponto final e' exato, sempre.
    //
    // A fronteira aleatoria e' o que impede a assinatura: dois movimentos de
    // mesmo tamanho dao sequencias de pacotes diferentes.
    // -----------------------------------------------------------------------
    int restanteX = x;
    int restanteY = y;

    for (int n = 0; n < kMaxPacotes; n++) {
        const int absX = KmAbs(restanteX);
        const int absY = KmAbs(restanteY);

        if (absX <= kDivergirAcimaDe && absY <= kDivergirAcimaDe) break;

        const int passoX = KmMin(absX, JitEntre(kPacoteMin, kPacoteMax));
        const int passoY = KmMin(absY, JitEntre(kPacoteMin, kPacoteMax));

        // Preserva o SINAL de cada eixo (o delta pode ser negativo).
        const int sx = (restanteX > 0) ? passoX : ((restanteX < 0) ? -passoX : 0);
        const int sy = (restanteY > 0) ? passoY : ((restanteY < 0) ? -passoY : 0);

        EnviarUmPacote(sx, sy, forcarSendInput);
        restanteX -= sx;
        restanteY -= sy;

        // ~1 ms: a rajada passa a ter a escala de tempo de uma rajada de
        // hardware, e nao um dump instantaneo. `Sleep(1)` e' o menor passo
        // confiavel; a resolucao de 1 ms ja' esta' ligada (timeBeginPeriod).
        if (n + 1 < kMaxPacotes && (restanteX || restanteY))
            Sleep(1);
    }

    // O resto — o que nao coube no cap, ou o que sobrou do ultimo passo.
    if (restanteX || restanteY)
        EnviarUmPacote(restanteX, restanteY, forcarSendInput);
}

