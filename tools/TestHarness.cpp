// ===========================================================================
// TestHarness.cpp
//
// Testa o driver contra a classe de endereco que derrubou a maquina 3 vezes —
// SEM abrir o jogo (logo, sem BattlEye ativo e sem risco de ban).
//
// ---------------------------------------------------------------------------
// REGRA DE SEGURANCA DESTE PROGRAMA
//
// A PRIMEIRA coisa que este harness faz e' um PING: so' o driver novo devolve
// a assinatura. Se o driver carregado for o ANTIGO, o caso "endereco
// nao-canonico" NAO e' recusado — vai pro kernel e da' BSOD.
//
// Sem a assinatura, o harness NAO roda os testes perigosos. Ele aborta.
//
// ---------------------------------------------------------------------------
// COMO USAR (versao 1.2)
//
// A partir da 1.2 o nome do device NAO E' MAIS UM LITERAL. Ele sai do seed da
// sessao: "\\.\<16 hex>". Isso e' ofuscacao de verdade (o literal que estava no
// .sys saiu), e o preco e' que este harness precisa saber o seed.
//
//   1. jogo FECHADO
//   2. rode o cheat normalmente (ele MAPEIA e loga a sessao), OU carregue o
//      .sys na mao com o kdmapper
//   3. leia o seed em Build\Release\PubgExt\logs.txt:
//          [*] seed desta sessao: 0x0123456789ABCDEF
//   4. TestHarness.exe --seed 0123456789abcdef
//
// Sem --seed, o harness tenta ler o ULTIMO seed do logs.txt sozinho. Se
// tambem falhar, ele para e explica — em vez de abrir o device errado.
//
// ---------------------------------------------------------------------------
// O QUE ESTE HARNESS NAO TESTA
//
// O SELFDESTRUCT (que libera as paginas) NAO e' testado aqui de proposito: ele
// destroi o objeto de teste no meio da bateria. Ele tem teste proprio, no fim,
// com aviso explicito — e a maquina pode reiniciar por causa dele se algo
// estiver errado. Ver --selfdestruct.
// ===========================================================================

#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// --- espelho do contrato do driver -----------------------------------------
#pragma pack(push, 8)
struct DRIVER_REQUEST {
    int        pid;
    uintptr_t  address;
    void*      buffer;
    size_t     size;
    const wchar_t* name;
};
#pragma pack(pop)

#define IOCTL_GET_PID           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_MODULE_BASE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_READ_MEMORY       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MOUSE_MOVE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KM_GET_VERSION    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KM_PING           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KM_DEACTIVATE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KM_SELFDESTRUCT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define KM_PING_MAGIC           0x504B4D53
#define KM_DRIVER_VERSION       0x00010002
#define KM_DESTRUCT_MAGIC       0x4B4D5344UL
#define KM_DESTRUCT_UNMAP       2

static int g_pass = 0, g_fail = 0, g_skip = 0;

static void Say(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void Pass(const char* what) { Say("  [+] %-46s OK\n", what); g_pass++; }
static void Fail(const char* what, DWORD err) {
    Say("  [!] %-46s FALHOU (err=%lu / 0x%08lX)\n", what, err, err);
    g_fail++;
}
static void Skip(const char* what, DWORD err) {
    Say("  [~] %-46s NAO TESTADO (err=%lu)\n", what, err);
    g_skip++;
}

// ===========================================================================
// DERIVACAO DO NOME — COPIA EXATA DE driver_interface_v3.cpp
//
// ==================== NAO MEXA EM UM LADO SO ====================
//
// Mesma regra do DeviceName.ps1: tres implementacoes (kernel, user-mode,
// ferramentas) do mesmo algoritmo. Divergir aqui faz o harness nao achar o
// device e reportar "nao esta carregado" — que e' o lugar errado pra procurar.
// Ver a comparacao em tools\devname_check.ps1.
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

static void DeriveDeviceName(uint64_t seed, char* out, size_t n)
{
    static const char hexd[] = "0123456789abcdef";
    uint64_t v = KmMixSeedCpp(seed, 1);
    size_t p = 0;

    for (const char* s = "\\\\.\\"; *s && p + 1 < n; s++) out[p++] = *s;
    for (int i = 15; i >= 0 && p + 1 < n; i--)
        out[p++] = hexd[(v >> (i * 4)) & 0xF];
    out[p] = 0;
}

// ---------------------------------------------------------------------------
// Le' o ultimo seed do logs.txt do cheat. Mesmo padrao que Get-KmSeedFromLog
// (tools\DeviceName.ps1).
// ---------------------------------------------------------------------------
static bool SeedFromLog(const char* caminho, uint64_t* seed)
{
    FILE* f = fopen(caminho, "r");
    if (!f) return false;

    char linha[512];
    uint64_t achado = 0;
    bool tem = false;

    while (fgets(linha, sizeof(linha), f)) {
        const char* p = strstr(linha, "seed desta sessao: 0x");
        if (!p) continue;

        p += strlen("seed desta sessao: 0x");
        uint64_t v = strtoull(p, nullptr, 16);
        achado = v;
        tem = true;
    }
    fclose(f);

    if (tem) *seed = achado;
    return tem;
}

// ---------------------------------------------------------------------------
// Le um endereco do processo alvo. Devolve o STATUS cru do driver.
// ---------------------------------------------------------------------------
static int ReadMem(HANDLE h, int pid, uintptr_t addr, void* out, size_t size, DWORD* winerr) {
    DRIVER_REQUEST req = {};
    req.pid     = pid;
    req.address = addr;
    req.buffer  = out;
    req.size    = size;

    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_READ_MEMORY, &req, sizeof(req), out, (DWORD)size, &bytes, nullptr);
    if (winerr) *winerr = ok ? 0 : GetLastError();
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    SetConsoleTitleA("harness - TestHarness 1.2");

    bool   pediuSelfDestruct = false;
    uint64_t seed = 0;
    bool   temSeed = false;

    // ---- argumentos ----
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selfdestruct") == 0) {
            pediuSelfDestruct = true;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], nullptr, 16);
            temSeed = true;
        } else if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = strtoull(argv[i] + 7, nullptr, 16);
            temSeed = true;
        }
    }

    // ---- sem seed? tenta o logs.txt ----
    if (!temSeed) {
        if (SeedFromLog("logs.txt", &seed)) {
            temSeed = true;
            Say("[*] seed lido do logs.txt: 0x%016llX\n", (unsigned long long)seed);
        } else {
            Say("[X] nenhum seed. O nome do device NAO e' mais um literal —\n");
            Say("    ele sai do seed da sessao (ver \"seed desta sessao\" no logs.txt).\n\n");
            Say("    Rode:\n");
            Say("      TestHarness.exe --seed <hex-do-logs.txt>\n\n");
            Say("Pressione ENTER para sair..."); getchar();
            return 1;
        }
    }

    char deviceName[64];
    DeriveDeviceName(seed, deviceName, sizeof(deviceName));

    Say("=========================================================\n");
    Say(" harness do km_safe (protocolo 1.2)\n");
    Say("=========================================================\n\n");
    Say("  seed   : 0x%016llX\n", (unsigned long long)seed);
    Say("  device : %s\n\n", deviceName);

    // -----------------------------------------------------------------------
    // ABRIR O DEVICE
    // -----------------------------------------------------------------------
    HANDLE h = CreateFileA(deviceName, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        Say("[X] nao consegui abrir %s (err=%lu)\n", deviceName, e);
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_SUCH_DEVICE) {
            Say("    -> o driver nao esta carregado, OU o seed nao e' o da sessao\n");
            Say("       que mapeou. Confira a linha \"device desta sessao\" no log:\n");
            Say("       ela TEM que ser identica a linha acima.\n");
        } else if (e == ERROR_ACCESS_DENIED) {
            Say("    -> o device existe mas esta em desligamento (DEACTIVATE ja rodou).\n");
        }
        Say("\nPressione ENTER para sair..."); getchar();
        return 1;
    }
    Say("[ok] device aberto\n\n");

    // -----------------------------------------------------------------------
    // GUARDA DE SEGURANCA: e' o driver NOVO?
    // -----------------------------------------------------------------------
    Say("--- 0. FINGERPRINT (guarda de seguranca) ---\n");
    {
        ULONG magic = 0;
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_KM_PING, nullptr, 0, &magic, sizeof(magic), &bytes, nullptr);
        DWORD e = ok ? 0 : GetLastError();

        if (!ok || magic != KM_PING_MAGIC) {
            Say("  %-48s RECUSADO\n", "driver responde ao PING?");
            Say("      recebido : ok=%d magic=0x%08lX  winerr=%lu\n", (int)ok, magic, e);
            Say("      esperado : magic=0x%08lX\n", KM_PING_MAGIC);
            Say("\n");
            Say("  =====================================================\n");
            Say("  ABORTADO. O driver carregado NAO responde ao PING.\n");
            Say("  =====================================================\n");
            Say("  Rodar os testes perigosos contra um driver desconhecido\n");
            Say("  derruba a maquina — foi o que aconteceu 3 vezes.\n\n");
            Say("  Faca:\n");
            Say("    1. confirme que o seed acima e' o da sessao que mapeou\n");
            Say("       (linha \"device desta sessao\" no logs.txt do cheat)\n");
            Say("    2. se o driver for de uma build anterior: feche o cheat e\n");
            Say("       rode de novo (a 1.2 se descarrega sozinha pelo DEACTIVATE)\n\n");
            Say("Pressione ENTER para sair..."); getchar();
            CloseHandle(h);
            return 2;
        }
        Pass("driver novo confirmado (fingerprint)");
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 0b. GET_VERSION — QUAL build respondeu
    //
    // O PING prova que ha' um km_safe; ele nao prova QUAL. Esta e' a pergunta
    // que a 1.1 introduziu e que a 1.2 continua usando: o numero muda quando o
    // protocolo muda, e e' o unico sinal que o user-mode tem pra decidir se o
    // driver que respondeu e' o que ele espera.
    // -----------------------------------------------------------------------
    Say("--- 0b. GET_VERSION ---\n");
    {
        ULONG ver = 0;
        DWORD bytes = 0;
        if (DeviceIoControl(h, IOCTL_KM_GET_VERSION, nullptr, 0, &ver, sizeof(ver), &bytes, nullptr)) {
            if (ver == KM_DRIVER_VERSION) {
                Say("  [+] %-46s OK (0x%08lX)\n", "versao do protocolo", ver);
                g_pass++;
            } else {
                Say("  [!] %-46s DIVERGE\n", "versao do protocolo");
                Say("      recebido : 0x%08lX\n", ver);
                Say("      esperado : 0x%08lX\n", KM_DRIVER_VERSION);
                Say("      -> o .sys carregado e' de outra build. Os IOCTLs novos\n");
                Say("         (DEACTIVATE/SELFDESTRUCT) NAO existem nele.\n");
                g_fail++;
            }
        } else {
            Say("  [~] %-46s NAO EXISTE (err=%lu)\n", "GET_VERSION (0x806)", GetLastError());
            Say("      -> driver 1.0 ou anterior. Ele NAO sabe se desligar\n");
            Say("         (o DEACTIVATE e' de 1.2), entao o ciclo de recarga\n");
            Say("         sem reboot nao funciona com ele.\n");
            g_skip++;
        }
    }
    Say("\n");

    int myPid = (int)GetCurrentProcessId();
    Say("  PID deste harness: %d\n\n", myPid);

    // -----------------------------------------------------------------------
    // 1. GET_PID
    // -----------------------------------------------------------------------
    Say("--- 1. GET_PID ---\n");
    {
        DRIVER_REQUEST req = {};
        wchar_t nome[] = L"TestHarness.exe";
        req.name = nome;
        int pid = 0;
        DWORD bytes = 0;
        if (DeviceIoControl(h, IOCTL_GET_PID, &req, sizeof(req), &pid, sizeof(pid), &bytes, nullptr)
            && pid == myPid) {
            Pass("GET_PID devolve o PID certo");
        } else {
            Fail("GET_PID", GetLastError());
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 2. GET_MODULE_BASE
    // -----------------------------------------------------------------------
    Say("--- 2. GET_MODULE_BASE ---\n");
    {
        DRIVER_REQUEST req = {};
        wchar_t nome[] = L"TestHarness.exe";
        req.pid  = myPid;
        req.name = nome;
        uintptr_t base = 0;
        DWORD bytes = 0;
        if (DeviceIoControl(h, IOCTL_GET_MODULE_BASE, &req, sizeof(req), &base, sizeof(base), &bytes, nullptr)
            && base > 0x10000) {
            Say("  [+] %-46s OK (base=0x%llX)\n", "GET_MODULE_BASE", (unsigned long long)base);
            g_pass++;
        } else {
            Fail("GET_MODULE_BASE", GetLastError());
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 3. LEITURA VALIDA  (a prova de que o caminho bom funciona)
    // -----------------------------------------------------------------------
    Say("--- 3. leitura VALIDA (controle) ---\n");
    {
        volatile uint64_t magico = 0x1122334455667788ULL;
        uint64_t lido = 0;
        DWORD e = 0;
        if (ReadMem(h, myPid, (uintptr_t)&magico, &lido, sizeof(lido), &e) == 0 && lido == magico) {
            Pass("le endereco valido e devolve o valor certo");
        } else {
            Fail("leitura valida", e);
        }
    }
    Say("\n");

    // =======================================================================
    // A PARTIR DAQUI SAO OS CASOS QUE DERRUBARAM A MAQUINA.
    // Cada um imprime ANTES. Se a tela congelar, a ultima linha e' o culpado.
    // =======================================================================
    Say("=========================================================\n");
    Say(" CASOS PERIGOSOS (os que davam BSOD no driver antigo)\n");
    Say("=========================================================\n\n");

    // -----------------------------------------------------------------------
    // 4. ENDERECO NAO-CANONICO  <-- o bugcheck real: P1 = "ys 1"
    // -----------------------------------------------------------------------
    Say("--- 4. endereco NAO-CANONICO (0x0031002000730079 = \"ys 1\") ---\n");
    Say("      este foi o P1 do bugcheck de 02/10. Executando...\n");
    {
        uint64_t lido = 0;
        DWORD e = 0;
        int r = ReadMem(h, myPid, 0x0031002000730079ULL, &lido, sizeof(lido), &e);
        if (r != 0) {
            Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "endereco nao-canonico", e);
            Say("      ^ era isto que a gente queria: recusa, nao bugcheck\n");
            g_pass++;
        } else {
            Fail("endereco nao-canonico DEVERIA ser recusado", 0);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 5. CANONICO POREM NAO MAPEADO
    // -----------------------------------------------------------------------
    Say("--- 5. endereco canonico NAO MAPEADO ---\n");
    {
        // Reserva sem commitar: o endereco e' valido e canonico, mas qualquer
        // acesso levanta excecao — exatamente o caso "canonico porem morto".
        void* res = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        if (!res) {
            Skip("canonico nao mapeado", GetLastError());
        } else {
            Say("      reservado (sem commit) em 0x%p. Executando...\n", res);
            uint64_t lido = 0;
            DWORD e = 0;
            int r = ReadMem(h, myPid, (uintptr_t)res, &lido, sizeof(lido), &e);
            if (r != 0) {
                Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "canonico nao mapeado", e);
                g_pass++;
            } else {
                Fail("canonico nao mapeado DEVERIA ser recusado", 0);
            }
            VirtualFree(res, 0, MEM_RELEASE);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 6. ENDERECO ZERO
    // -----------------------------------------------------------------------
    Say("--- 6. endereco ZERO ---\n");
    {
        uint64_t lido = 0;
        DWORD e = 0;
        if (ReadMem(h, myPid, 0, &lido, sizeof(lido), &e) != 0) {
            Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "endereco 0", e);
            g_pass++;
        } else {
            Fail("endereco 0 DEVERIA ser recusado", 0);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 7. ENDERECO DE KERNEL (0xFFFF...)
    // -----------------------------------------------------------------------
    Say("--- 7. endereco de KERNEL (0xFFFF800000000000) ---\n");
    {
        uint64_t lido = 0;
        DWORD e = 0;
        if (ReadMem(h, myPid, 0xFFFF800000000000ULL, &lido, sizeof(lido), &e) != 0) {
            Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "endereco de kernel", e);
            g_pass++;
        } else {
            Fail("endereco de kernel DEVERIA ser recusado", 0);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 8. TAMANHO ABSURDO
    // -----------------------------------------------------------------------
    Say("--- 8. tamanho ABSURDO (16 MB) ---\n");
    {
        static uint8_t buf[16];
        volatile uint64_t magico = 0;
        DWORD e = 0;
        if (ReadMem(h, myPid, (uintptr_t)&magico, buf, 16u * 1024u * 1024u, &e) != 0) {
            Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "tamanho acima do teto", e);
            g_pass++;
        } else {
            Fail("tamanho absurdo DEVERIA ser recusado", 0);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 9. PID INEXISTENTE
    // -----------------------------------------------------------------------
    Say("--- 9. PID INEXISTENTE ---\n");
    {
        uint64_t lido = 0;
        volatile uint64_t magico = 0;
        DWORD e = 0;
        if (ReadMem(h, 0x7FFFFFF0, (uintptr_t)&magico, &lido, sizeof(lido), &e) != 0) {
            Say("  [+] %-46s RECUSADO (winerr=%lu)\n", "pid inexistente", e);
            g_pass++;
        } else {
            Fail("pid inexistente DEVERIA falhar", 0);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 10. VOLTA A LER VALIDO  (prova que o driver nao ficou quebrado)
    // -----------------------------------------------------------------------
    Say("--- 10. leitura VALIDA de novo (driver sobreviveu?) ---\n");
    {
        volatile uint64_t magico = 0xDEADBEEFCAFEBABEULL;
        uint64_t lido = 0;
        DWORD e = 0;
        if (ReadMem(h, myPid, (uintptr_t)&magico, &lido, sizeof(lido), &e) == 0 && lido == magico) {
            Pass("driver continua funcional depois dos casos ruins");
        } else {
            Fail("leitura valida final", e);
        }
    }
    Say("\n");

    // -----------------------------------------------------------------------
    // 11. DESCARGA — o que a 1.2 trouxe, e o que o harness NAO pode testar
    //     pela metade.
    //
    // O DEACTIVATE e' reversivel do ponto de vista do harness: ele para o
    // atendimento e o device sai do namespace, e depois disso NADA mais
    // funciona nesta sessao. Por isso ele roda por ULTIMO.
    //
    // E o SELFDESTRUCT e' IRREVERSIVEL: ele libera as paginas do driver. Se a
    // sequencia estiver errada, o resultado e' bugcheck — nao "erro". Por isso
    // ele exige --selfdestruct, e explica o que vai acontecer ANTES.
    // -----------------------------------------------------------------------
    if (pediuSelfDestruct) {
        Say("--- 11. SELFDESTRUCT (destrutivo, pedido com --selfdestruct) ---\n");
        Say("  [!] A partir daqui o driver NAO responde mais e as paginas dele\n");
        Say("  [!] sao liberadas. Se o stack estiver errado, a maquina reinicia.\n");
        Say("  [!] Confirme que o WinDbg esta' anexado OU que a VM tem snapshot.\n");

        Say("      mandando SELFDESTRUCT em 3s...\n");
        Sleep(3000);

        DRIVER_REQUEST req = {};
        req.pid    = KM_DESTRUCT_UNMAP;
        req.buffer = (void*)(uintptr_t)KM_DESTRUCT_MAGIC;

        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_KM_SELFDESTRUCT,
                                  &req, sizeof(req), nullptr, 0, &bytes, nullptr);
        DWORD e = ok ? 0 : GetLastError();

        if (!ok && e != ERROR_DEVICE_NOT_CONNECTED) {
            Fail("SELFDESTRUCT", e);
        } else {
            Pass("SELFDESTRUCT aceito (o driver esta' se liberando)");
        }

        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;

        // A thread de liberacao espera os handles zerarem e ainda dorme 100ms.
        Say("      aguardando 3s e conferindo se o device sumiu...\n");
        Sleep(3000);

        HANDLE h2 = CreateFileA(deviceName, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (h2 == INVALID_HANDLE_VALUE) {
            Pass("device NAO pode mais ser aberto (descarga completa)");
        } else {
            CloseHandle(h2);
            Fail("device AINDA abre depois do SELFDESTRUCT", 0);
            Say("      -> o unmap abortou (handle pendurado?) ou a resolucao de\n");
            Say("         MmFreeIndependentPages falhou. Veja PkmSafe.txt.\n");
        }
        Say("\n");
    } else {
        Say("--- 11. DESCARGA (nao testada) ---\n");
        Say("  [~] DEACTIVATE/SELFDESTRUCT NAO rodam sem --selfdestruct:\n");
        Say("      eles param o atendimento e destroem o objeto de teste.\n");
        Say("      Para testar: TestHarness.exe --seed <hex> --selfdestruct\n");
        Say("      (numa VM com snapshot — o unmap pode reiniciar a maquina)\n\n");
    }

    // -----------------------------------------------------------------------
    // RESULTADO
    // -----------------------------------------------------------------------
    Say("=========================================================\n");
    Say(" RESULTADO: %d passaram, %d falharam, %d pulados\n", g_pass, g_fail, g_skip);
    Say("=========================================================\n");
    if (g_fail == 0) {
        Say("\n  A maquina NAO caiu e todos os enderecos ruins foram\n");
        Say("  RECUSADOS em vez de causar bugcheck. O driver esta' seguro.\n");
        Say("  Proximo passo: testar no treino, com o jogo.\n");
    } else {
        Say("\n  %d caso(s) falharam. NAO use no jogo ainda — me mande\n", g_fail);
        Say("  esta saida que eu corrijo o driver.\n");
    }
    Say("\n");

    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    Say("Pressione ENTER para sair...");
    getchar();
    return g_fail == 0 ? 0 : 1;
}
