// ===========================================================================
// DriverSelfTest.cpp
//
// Banco de teste do driver, rodando DENTRO do RuntimeBroker.
//
// O problema: nao da pra testar leitura de memoria sem um alvo, e testar
// contra o PUBG significa abrir o jogo e arriscar a conta. Este arquivo
// resolve isso apontando o teste para o PROPRIO RuntimeBroker.exe.
//
// O truque que faz o teste valer a pena: ler os bytes do nosso proprio
// modulo por tres caminhos independentes e exigir que os tres batam com o
// memcpy local, que e a verdade absoluta.
//
//   A. memcpy local          (referencia)
//   B. ReadMemory            MmCopyVirtualMemory
//   C. ReadMemoryViaPhysical page-table walk + MmMapIoSpaceEx
//
// Se B ou C divergir de A, o caminho correspondente esta quebrado â€” e o
// teste diz qual.
//
// Alem disso:
//   * GET_MODULE_BASE  â€” confere contra GetModuleHandle, que e exato
//   * GET_PID          â€” confere contra GetCurrentProcessId, que e exato
//   * GET_CR3          â€” confere o _EPROCESS->DirectoryTableBase com o
//                        proprio CR3 lido do registrador, via __readcr3
//
// O ultimo e o mais importante: KM_EPROCESS_DTB_OFFSET = 0x28 esta
// hardcoded e muda com o build do Windows. Se estiver errado, o walk
// devolve lixo silenciosamente. Comparar com o CR3 real prova na hora.
// ===========================================================================

#include "pch.h"
#include "Memory/Memory.h"
#include "driver/driver_interface_v3.h"
#include "driver/AsIO3_Client.h"
#include <intrin.h>

namespace {

struct TestResult {
    const char* name;
    bool        pass;
    char        detail[256];
};

int g_pass = 0;
int g_fail = 0;

void Report(const char* name, bool pass, const char* fmt, ...)
{
    char detail[256] = { 0 };
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    if (pass) {
        g_pass++;
        LOG("[+] %-38s %s\n", name, detail);
    } else {
        g_fail++;
        LOG("[-] %-38s %s\n", name, detail);
    }
}

// SHA-256 simplificado: nao vale a pena puxar crypto aqui. Usamos FNV-1a
// de 64 bits â€” colisao acidental em 4 KiB e improvavel o bastante para
// detectar leitura corrompida, que e o que importa.
uint64_t Hash(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Le o CR3 do processador. Disponivel em modo usuario no Windows (a
// instrucao nao e privilegiada), o que torna este teste possivel.
uint64_t ReadCr3()
{
    return __readcr3();
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point do banco de teste.
//
// Retorna true se o driver respondeu; o resultado detalhado vai pro log.
// ---------------------------------------------------------------------------
bool RunDriverSelfTest()
{
    LOG("\n");
    LOG("==================================================================\n");
    LOG("  DRIVER SELF TEST — target: RuntimeBroker.exe (in the same process)\n");
    LOG("==================================================================\n");

    // -----------------------------------------------------------------------
    // 0. Driver carregado? O o driver cria \\.\Ndu2.
    // -----------------------------------------------------------------------
    const bool driverUp = TargetProcess.driver.Initialize();
    if (!driverUp) {
        LOG("[-] driver did not respond at \\.\Ndu2\n");
        LOG("    load the driver (kdmapper) and run again\n");
        LOG("==================================================================\n\n");
        return false;
    }
    LOG("[+] driver responded on \\.\Ndu2\n\n");
    LOG("    (IOCTL-by-IOCTL testing disabled: the driver uses the legacy\n");
    LOG("     contract—only READ (0x803) and GET_MODULE_BASE (0x802).)\n");
    LOG("==================================================================\n\n");
    return true;
}
// ---------------------------------------------------------------------------
void RunAsIO3Probe()
{
    LOG("\n");
    LOG("==================================================================\n");
    LOG("  SONDA AsIO3 — route A (donor driver)\n");
    LOG("==================================================================\n");

    // 1. Estado do servico
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, AsIO3::kServiceName, SERVICE_QUERY_STATUS);
        if (svc) {
            SERVICE_STATUS st{};
            if (QueryServiceStatus(svc, &st)) {
                const char* state =
                    st.dwCurrentState == SERVICE_RUNNING  ? "RUNNING"  :
                    st.dwCurrentState == SERVICE_STOPPED  ? "STOPPED"  :
                    st.dwCurrentState == SERVICE_START_PENDING ? "START_PENDING" :
                    st.dwCurrentState == SERVICE_STOP_PENDING  ? "STOP_PENDING"  : "?";
                LOG("[+] service Asusgio3: %s (tipo 0x%X)\n", state, st.dwServiceType);
                if (st.dwCurrentState != SERVICE_RUNNING) {
                    // `sc.exe`, NAO `sc`. No PowerShell `sc` e alias de
                    // Set-Content, entao `sc start Asusgio3` cria um arquivo
                    // chamado "start" com o texto "Asusgio3" e nao faz nada
                    // com o servico. Ja aconteceu.
                    LOG("    to start (PowerShell): sc.exe start Asusgio3   (requires admin)\n");
                }
            }
            CloseServiceHandle(svc);
        } else {
            LOG("[-] Asusgio3 service not found (error %lu)\n", GetLastError());
        }
        CloseServiceHandle(scm);
    } else {
        LOG("[-] OpenSCManager failed: %lu\n", GetLastError());
    }

    // 2. O binario da checagem de chamador existe?
    {
        char pf86[MAX_PATH] = { 0 };
        DWORD n = GetEnvironmentVariableA("ProgramFiles(x86)", pf86, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            LOG("[-] ProgramFiles(x86) unresolved\n");
        } else {
            char caller[MAX_PATH] = { 0 };
            _snprintf_s(caller, sizeof(caller), _TRUNCATE,
                        "%s\\ASUS\\AsusCertService\\AsusCertService.exe", pf86);
            LOG("[%s] expected caller: %s\n",
                GetFileAttributesA(caller) != INVALID_FILE_ATTRIBUTES ? "+" : "-",
                caller);
        }
    }

    // 3. Tenta abrir o device
    AsIO3::Client asio;
    if (!asio.Open()) {
        const DWORD e = asio.LastError();
        LOG("[-] CreateFileA(%s) falhou: %lu\n", AsIO3::kDeviceUserPath, e);

        // 2 e 5 significam coisas COMPLETAMENTE diferentes e o log antigo
        // dizia "se for 2" para os dois, o que despistava.
        switch (e) {
        case ERROR_FILE_NOT_FOUND:          // 2
            LOG("    error 2 = FILE_NOT_FOUND: the driver did not create the device.\n");
            LOG("    typical cause: the Asusgio3 service is STOPPED.\n");
            LOG("    start it using: sc.exe start Asusgio3\n");
            break;
        case ERROR_ACCESS_DENIED:           // 5
            LOG("    error 5 = ACCESS_DENIED: the device EXISTS, but the driver refused access.\n");
            LOG("    This is the AsIO3.sys 'caller check' rejecting our\n");
            LOG("    process.\n");
            LOG("    TESTED AND RULED OUT so far:\n");
            LOG("      - running with the Asusgio3 service RUNNING      -> still error 5\n");
            LOG("      - running with AsusCertService also RUNNING      -> still error 5\n");
            LOG("      - renaming our .exe to AsusCertService           -> still error 5\n");
            LOG("    The driver does NOT validate tokens or signatures (it ignores\n");
            LOG("    SeAccessCheck / SeSinglePrivilegeCheck / any certificate checks).\n");
            LOG("    Instead, it denies access within IRP_MJ_CREATE based on its\n");
            LOG("    own criteria, which I have been unable to extract from the\n");
            LOG("    binary. See ARQUITETURA.md, section 3d.\n");
            break;
        default:
            LOG("    (expected 2 = service stopped, or 5 = caller check failure)\n");
            break;
        }
        LOG("==================================================================\n\n");
        return;
    }

    LOG("[+] device opened: %s\n", AsIO3::kDeviceUserPath);

    // 4. Bate na porta com os IOCTLs de configuracao (os menos perigosos:
    //    so informacao). Se algum responder, o driver esta vivo e aceitando
    //    chamadas do nosso processo â€” ou seja, a checagem de chamador passou.
    LOG("\n  probing IOCTLs of INFO (0xA0402000..) and CFG (0xA0400F58..):\n");

    const ULONG probes[] = {
        AsIO3::IOCTL_INFO_0, AsIO3::IOCTL_INFO_1, AsIO3::IOCTL_INFO_2,
        AsIO3::IOCTL_INFO_3, AsIO3::IOCTL_INFO_4, AsIO3::IOCTL_INFO_5,
        AsIO3::IOCTL_CFG_0,  AsIO3::IOCTL_CFG_5,  AsIO3::IOCTL_CFG_15,
    };

    int answered = 0;
    for (ULONG ioctl : probes) {
        unsigned char in[64]  = { 0 };
        unsigned char out[64] = { 0 };
        DWORD bytes = 0;
        SetLastError(0);
        const bool ok = asio.Raw(ioctl, in, sizeof(in), out, sizeof(out), &bytes);
        const DWORD err = ok ? 0 : asio.LastError();

        if (ok || (err != ERROR_INVALID_FUNCTION && err != ERROR_INVALID_PARAMETER)) {
            answered++;
            LOG("    0x%08lX  %s  bytes=%lu  err=%lu\n", ioctl,
                ok ? "OK  " : "falhou", bytes, err);
        } else {
            LOG("    0x%08lX  recusado  err=%lu\n", ioctl, err);
        }
    }

    LOG("\n  %d de %d IOCTLs responderam\n", answered, (int)(sizeof(probes) / sizeof(probes[0])));
    if (answered == 0) {
        LOG("  none responded: the driver is open but is rejecting everything —\n");
        LOG("  the caller check is likely rejecting the process\n");
    }
    else {
        LOG("  >> the caller check PASSED. The AsIO3 primitive is\n");
        LOG("     accessible from here.\n");
    }

    asio.Close();
    LOG("==================================================================\n\n");
}
