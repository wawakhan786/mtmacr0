// ===========================================================================
// km_ipc_bench.cpp — CLIENTE + BENCHMARK do protótipo (user mode, console)
//
// Faz o papel que nenhum outro binário pode fazer: cria a seção e os DOIS
// eventos SEM NOME, deriva a chave do rendezvous, grava os 5 valores, instala e
// inicia o serviço com o `Seed`, espera o `Ready` e mede.
//
// ORDEM (se a chave não existir antes do StartService, o driver morre no
// ZwOpenKey — foi exatamente o erro 2 medido na VM):
//
//   1. seção sem nome + 2 eventos sem nome
//   2. chave derivada + 5 valores REG_QWORD
//   3. Parameters\Seed + CreateService + StartService
//   4. poll de Hdr.Ready
//   5. lotes e telemetria
//   6. ControlService(STOP) -> DriverUnload -> KmpShutdown
//
// ---------------------------------------------------------------------------
// O QUE ESTE BINÁRIO MEDE — e o que ele NÃO mede
//
// MEDE as configurações C e D do relatório: shared memory com evento,
// operação individual (Count = 1) e em lote (Count = 64).
//
// NÃO MEDE A e B (baseline por IOCTL). O baseline exige um SEGUNDO driver, com
// device e dispatch de IRP — que este protótipo deliberadamente não tem. Sem
// ele, comparar seria comparar shared memory contra "nada", e o número não
// valeria nada. Some isso depois, com o mesmo payload e a mesma matriz.
//
// Precisa rodar ELEVADO (CreateService + escrita em HKLM + duplicação de handle
// no lado do kernel).
// ===========================================================================

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

#include "km_ipc_proto.h"

#pragma comment(lib, "advapi32.lib")

// ===========================================================================
// DERIVAÇÃO — tem que ser IDÊNTICA à do driver (km_ipc_driver.c)
// ===========================================================================
static uint64_t KmpMix(uint64_t seed, uint32_t kind)
{
    uint64_t k = seed ^ (KMP_MIX * (uint64_t)(kind + 1));
    k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

static void Hex16(wchar_t* out, uint64_t v, int digits)
{
    static const wchar_t h[] = L"0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) { out[i] = h[v & 0xF]; v >>= 4; }
}

// Caminho relativo ao HKLM (o kernel usa \Registry\Machine\...).
static void KeyPathRel(wchar_t* out, uint64_t seed)
{
    wcscpy_s(out, 128, L"SOFTWARE\\Classes\\CLSID\\");
    size_t n = wcslen(out);
    Hex16(out + n, KmpMix(seed, KMP_MIX_KEY), KMP_REG_GUID_CHARS);
    // O NUL DE VOLTA. O wcscpy_s escreveu NUL na posicao do prefixo, mas o
    // Hex16 sobrescreve justamente ali: sem esta linha o nome do key carrega
    // LIXO DE PILHA depois do hex e o RegCreateKeyExW pode criar um nome
    // diferente do que o kernel vai procurar. Bug real, encontrado aqui.
    out[n + KMP_REG_GUID_CHARS] = 0;
}

static void ValueName(wchar_t* out, uint64_t seed, uint32_t idx)
{
    Hex16(out, KmpMix(seed, KMP_MIX_VAL + idx), KMP_VALUE_NAME_LEN);
    out[KMP_VALUE_NAME_LEN] = 0;
}

static bool WriteQword(HKEY key, uint64_t seed, uint32_t idx, uint64_t v)
{
    wchar_t name[KMP_VALUE_NAME_LEN + 1];
    ValueName(name, seed, idx);
    return RegSetValueExW(key, name, 0, REG_QWORD,
                          (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
}

// ===========================================================================
// MÉTRICAS
// ===========================================================================
static double Pct(std::vector<double>& v, double p)
{
    if (v.empty()) return 0.0;
    size_t i = (size_t)((p / 100.0) * (double)(v.size() - 1) + 0.5);
    if (i >= v.size()) i = v.size() - 1;
    return v[i];
}

struct Resultado {
    const char* nome;
    size_t      ops;
    double      media, p50, p95, p99, pior, opsPorSeg;
};

// ===========================================================================
int main(int argc, char** argv)
{
    uint64_t seed    = 0;
    const char* svcName = "kmproto";
    const char* drvPath = "km_ipc_proto.sys";
    int  iteracoes   = 20000;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)   seed = _strtoui64(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--service") && i + 1 < argc) svcName = argv[++i];
        else if (!strcmp(argv[i], "--driver") && i + 1 < argc)  drvPath = argv[++i];
        else if (!strcmp(argv[i], "--iter") && i + 1 < argc)    iteracoes = atoi(argv[++i]);
    }
    if (!seed) seed = (uint64_t)GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 32);

    // Nome do servico em wide: as APIs de servico usadas aqui sao as W.
    wchar_t svcW[256];
    MultiByteToWideChar(CP_ACP, 0, svcName, -1, svcW, 256);

    printf("\n=== km_ipc_bench — shared memory + KEVENT (sem nome) ===\n");
    printf("  seed     : 0x%016llX\n", (unsigned long long)seed);
    printf("  servico  : %s\n", svcName);
    printf("  driver   : %s\n", drvPath);

    // PRIORIDADE: o bench e' um ping-pong de duas threads. Com prioridade
    // normal, o P99 mede o escalonador do Windows, nao o transporte — foi o que
    // apareceu com --iter 20000 (p50=1,3us, media=36,8us).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const SIZE_T tamSecao = (SIZE_T)KMP_SHARED_BYTES;

    // ---- 1) seção e eventos SEM NOME ----
    HANDLE hSec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     (DWORD)(tamSecao >> 32), (DWORD)tamSecao, nullptr);
    if (!hSec) { printf("  [err] CreateFileMapping: %lu\n", GetLastError()); return 1; }

    PKMP_SHARED sh = (PKMP_SHARED)MapViewOfFile(hSec, FILE_MAP_ALL_ACCESS, 0, 0, tamSecao);
    if (!sh) { printf("  [err] MapViewOfFile: %lu\n", GetLastError()); return 1; }
    memset(sh, 0, tamSecao);

    // AUTO-RESET (FALSE): cada SetEvent acorda UMA espera e não acumula.
    HANDLE hReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE hRsp = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hReq || !hRsp) { printf("  [err] CreateEvent: %lu\n", GetLastError()); return 1; }

    // ---- 2) rendezvous: chave derivada + os 5 valores ----
    wchar_t keyRel[128];
    KeyPathRel(keyRel, seed);

    HKEY hKey = nullptr;
    // REG_OPTION_VOLATILE FOI O BUG: chave volatil e criada em memoria e o
    // `ZwOpenKey` do kernel NAO a resolve de forma confiavel — o cliente via a
    // chave (reg query mostrava) e o driver recebia OBJECT_NAME_NOT_FOUND, o que
    // aparecia como "erro 2" no StartService. Chave normal (opcao 0): e criada
    // no hive, o kernel acha, e o driver a apaga no fim do boot de qualquer jeito.
    LSTATUS ls = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyRel, 0, nullptr,
                                 0, KEY_ALL_ACCESS, nullptr, &hKey, nullptr);
    if (ls != ERROR_SUCCESS) { printf("  [err] RegCreateKeyEx %ls: %ld\n", keyRel, ls); return 1; }

    const uint64_t sig = KmpMix(seed, KMP_MIX_SIG);
    const uint64_t pid = (uint64_t)GetCurrentProcessId();
    bool okv = true;
    okv &= WriteQword(hKey, seed, KMP_V_SIGNATURE, sig);
    okv &= WriteQword(hKey, seed, KMP_V_PID,       pid);
    okv &= WriteQword(hKey, seed, KMP_V_SECTION,   (uint64_t)(uintptr_t)hSec);
    okv &= WriteQword(hKey, seed, KMP_V_REQEVT,    (uint64_t)(uintptr_t)hReq);
    okv &= WriteQword(hKey, seed, KMP_V_RSPEVT,    (uint64_t)(uintptr_t)hRsp);
    RegCloseKey(hKey);

    // PROVA DE PERSISTENCIA: o '[ok] rendezvous' era so' o retorno do
    // RegCreateKeyExW. Reabrir e contar os valores separa 'o cliente nao criou'
    // de 'o kernel nao ve'.
    {
        HKEY hVer = nullptr;
        LSTATUS v = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyRel, 0, KEY_READ, &hVer);
        if (v != ERROR_SUCCESS) {
            printf("  [ERR] a chave NAO reabre apos criada! err=%ld\n", v);
            return 1;
        }
        DWORD nValores = 0, nSub = 0;
        RegQueryInfoKeyW(hVer, nullptr, nullptr, nullptr, &nSub, nullptr, nullptr,
                         &nValores, nullptr, nullptr, nullptr, nullptr);
        printf("  [ok] chave reaberta: %lu valor(es)\n", nValores);
        RegCloseKey(hVer);
        if (nValores != KMP_VALUE_COUNT) {
            printf("  [ERR] esperava %d valores\n", KMP_VALUE_COUNT);
            return 1;
        }
    }
    if (!okv) { printf("  [err] falha gravando os valores do rendezvous\n"); return 1; }

    // CONFERENCIA DO QUE FICOU NO DISCO. O driver recebeu INVALID_HANDLE, e a
    // causa classica e' handle 0 (ou valor truncado) nos 5 REG_QWORD. Ler de
    // volta e comparar com os valores reais do processo separa "gravamos errado"
    // de "o driver duplicou errado".
    {
        HKEY hR = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyRel, 0, KEY_READ, &hR) == ERROR_SUCCESS) {
            const uint64_t esperado[KMP_VALUE_COUNT] = {
                sig, pid, (uint64_t)(uintptr_t)hSec,
                (uint64_t)(uintptr_t)hReq, (uint64_t)(uintptr_t)hRsp
            };
            printf("  [i] valores no disco (pid=%llu  sec=%llu  req=%llu  rsp=%llu)\n",
                   (unsigned long long)pid, (unsigned long long)(uintptr_t)hSec,
                   (unsigned long long)(uintptr_t)hReq, (unsigned long long)(uintptr_t)hRsp);
            for (uint32_t i = 0; i < KMP_VALUE_COUNT; ++i) {
                wchar_t nm[KMP_VALUE_NAME_LEN + 1];
                ValueName(nm, seed, i);
                uint64_t v = 0; DWORD sz = sizeof(v), tipo = 0;
                LONG r = RegQueryValueExW(hR, nm, nullptr, &tipo, (LPBYTE)&v, &sz);
                printf("      V%u = 0x%016llX  %s\n", i, (unsigned long long)v,
                       (r == ERROR_SUCCESS && tipo == REG_QWORD && v == esperado[i]) ? "ok" : "DIVERGE");
            }
            RegCloseKey(hR);
        }
    }
    printf("  [ok] rendezvous: %ls\n", keyRel);

    // ---- 3) serviço: Seed em Parameters, depois Start ----
    SC_HANDLE hScm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hScm) { printf("  [err] OpenSCManager (rode como Admin): %lu\n", GetLastError()); return 1; }

    char drvFullA[MAX_PATH];
    if (!GetFullPathNameA(drvPath, MAX_PATH, drvFullA, nullptr)) {
        printf("  [err] caminho do driver invalido: %s\n", drvPath);
        return 1;
    }
    wchar_t drvW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, drvFullA, -1, drvW, MAX_PATH);

    SC_HANDLE hSvc = OpenServiceW(hScm, svcW, SERVICE_ALL_ACCESS);
    if (!hSvc) {
        hSvc = CreateServiceW(hScm, svcW, svcW, SERVICE_ALL_ACCESS,
                              SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                              SERVICE_ERROR_NORMAL, drvW,
                              nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!hSvc) { printf("  [err] CreateService: %lu\n", GetLastError()); return 1; }
        printf("  [ok] servico criado: %s\n", svcName);
    } else {
        printf("  [i]  servico ja existia: %s\n", svcName);
    }

    // Parameters\Seed — é daqui que o driver lê o seed quando sobe como serviço.
    {
        wchar_t par[256];
        swprintf_s(par, L"SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters", svcW);
        HKEY hPar = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, par, 0, nullptr, 0,
                            KEY_ALL_ACCESS, nullptr, &hPar, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hPar, L"Seed", 0, REG_QWORD, (const BYTE*)&seed, sizeof(seed));
            RegCloseKey(hPar);
        }
    }

    printf("  [..] StartService...\n");
    if (!StartServiceW(hSvc, 0, nullptr)) {
        DWORD e = GetLastError();
        printf("  [err] StartService: %lu\n", e);
        if (e == 2)     printf("        erro 2 = o DriverEntry parou no ZwOpenKey (rendezvous nao achado)\n");
        if (e == 87)    printf("        erro 87 = Seed invalido\n");
        if (e == 577)   printf("        577 = assinatura invalida\n");
        if (e == 1275)  printf("        1275 = blocklist de driver\n");
        return 1;
    }

    // ---- 4) esperar o driver publicar Ready ----
    bool ready = false;
    for (int i = 0; i < 300 && !ready; ++i) {          // ate 3 s
        MemoryBarrier();
        if (sh->Hdr.Ready == 1 && sh->Hdr.Magic == KMP_MAGIC) ready = true;
        else Sleep(10);
    }
    if (!ready) {
        printf("  [err] Ready nunca virou 1 (Magic=0x%08X Ready=%u)\n",
               sh->Hdr.Magic, sh->Hdr.Ready);
        return 1;
    }
    printf("  [ok] READY. driver publicou: sig=0x%016llX dataOff=%llu dataBytes=%llu\n",
           (unsigned long long)sh->Hdr.Signature,
           (unsigned long long)sh->Hdr.DataOffset,
           (unsigned long long)sh->Hdr.DataBytes);

    // ---- 5) lotes ----
    uint8_t* data = (uint8_t*)sh + (SIZE_T)sh->Hdr.DataOffset;
    const SIZE_T dataBytes = (SIZE_T)sh->Hdr.DataBytes;

    // payload de entrada: 1 KiB com padrao conhecido
    const uint64_t kPayload = 1024;
    for (uint64_t i = 0; i < kPayload; ++i) data[i] = (uint8_t)(i * 7 + 3);

    Resultado res[2];
    const size_t contagens[2] = { 1, 64 };
    const char*  nomes[2]     = { "C: shm individual (1 op)", "D: shm lote (64 ops)" };

    for (int cfg = 0; cfg < 2; ++cfg) {
        const size_t n = contagens[cfg];
        std::vector<double> lat;
        lat.reserve((size_t)iteracoes);

        uint32_t seq = 0;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);

        // aquecimento
        for (int w = 0; w < 100; ++w) {
            sh->Req.Magic = KMP_MAGIC; sh->Req.Version = KMP_VERSION;
            sh->Req.Count = (kmp_u32)n; sh->Req.PayloadUsed = kPayload;
            for (size_t k = 0; k < n; ++k) {
                sh->Req.Ops[k].Kind = KMP_OP_SUM;
                sh->Req.Ops[k].Size = kPayload;
                sh->Req.Ops[k].PayloadOff = 0;
            }
            ++seq;
            MemoryBarrier();
            sh->Req.Seq = seq;
            MemoryBarrier();
            SetEvent(hReq);
            WaitForSingleObject(hRsp, 2000);
        }

        int falhas = 0;
        for (int it = 0; it < iteracoes; ++it) {
            sh->Req.Magic = KMP_MAGIC; sh->Req.Version = KMP_VERSION;
            sh->Req.Count = (kmp_u32)n; sh->Req.PayloadUsed = kPayload;
            for (size_t k = 0; k < n; ++k) {
                sh->Req.Ops[k].Kind = KMP_OP_SUM;
                sh->Req.Ops[k].Size = kPayload;
                sh->Req.Ops[k].PayloadOff = 0;
            }

            QueryPerformanceCounter(&t0);
            ++seq;
            MemoryBarrier();
            sh->Req.Seq = seq;
            MemoryBarrier();
            SetEvent(hReq);

            if (WaitForSingleObject(hRsp, 2000) != WAIT_OBJECT_0) { ++falhas; continue; }
            QueryPerformanceCounter(&t1);

            MemoryBarrier();
            if (sh->Resp.AckSeq != seq || sh->Resp.GlobalStatus != KMP_OK) { ++falhas; continue; }

            lat.push_back((double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart);
        }

        std::sort(lat.begin(), lat.end());
        Resultado& r = res[cfg];
        r.nome = nomes[cfg];
        r.ops  = n;
        r.media = 0; for (double d : lat) r.media += d;
        r.media = lat.empty() ? 0 : r.media / (double)lat.size();
        r.p50 = Pct(lat, 50); r.p95 = Pct(lat, 95); r.p99 = Pct(lat, 99);
        r.pior = lat.empty() ? 0 : lat.back();
        r.opsPorSeg = (r.media > 0) ? (1e6 / r.media) * (double)n : 0;

        printf("\n  %s\n", r.nome);
        printf("    amostras=%zu  falhas=%d\n", lat.size(), falhas);
        printf("    media=%.1f us   p50=%.1f   p95=%.1f   p99=%.1f   pior=%.1f\n",
               r.media, r.p50, r.p95, r.p99, r.pior);
        printf("    %.0f ops/s\n", r.opsPorSeg);
    }

    printf("\n  --- resumo ---\n");
    for (int i = 0; i < 2; ++i)
        printf("    %-26s p99=%8.1f us   %10.0f ops/s\n",
               res[i].nome, res[i].p99, res[i].opsPorSeg);

    // ---- 5b) BASELINE POR IOCTL (configs A e B) ----
    //
    // Mesmo payload, mesma operacao (SUM de 1 KiB), mesmo laco. A unica
    // diferenca e o transporte: aqui o I/O manager copia o lote inteiro para o
    // kernel e copia a resposta de volta (METHOD_BUFFERED).
    {
        struct KMPB_BATCH {
            kmp_u32 Magic, Version, Count, _pad;
            kmp_u64 PayloadUsed;
            KMP_OP  Ops[KMP_MAX_OPS];
            kmp_u8  Data[4096];
        };
        const DWORD KMPB_IOCTL_RUN =
            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);

        HANDLE hDev = CreateFileA("\\\\.\\KMPBaseline", GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDev == INVALID_HANDLE_VALUE) {
            printf("\n  [i] baseline por IOCTL indisponivel (erro %lu)\n", GetLastError());
            printf("      carregue o km_baseline.sys e rode de novo\n");
        } else {
            static KMPB_BATCH b;
            const size_t conts[2] = { 1, 64 };
            const char*  nms[2]   = { "A: IOCTL individual (1 op)", "B: IOCTL lote (64 ops)" };
            Resultado io[2];

            for (size_t i2 = 0; i2 < kPayload; ++i2) b.Data[i2] = (uint8_t)(i2 * 7 + 3);

            LARGE_INTEGER fq, q0, q1;
            QueryPerformanceFrequency(&fq);

            for (int cfg = 0; cfg < 2; ++cfg) {
                const size_t n = conts[cfg];
                std::vector<double> lat;
                lat.reserve((size_t)iteracoes);
                int falhas = 0;

                for (int it = 0; it < iteracoes; ++it) {
                    b.Magic = KMP_MAGIC; b.Version = KMP_VERSION;
                    b.Count = (kmp_u32)n; b.PayloadUsed = kPayload;
                    for (size_t k = 0; k < n; ++k) {
                        b.Ops[k].Kind = KMP_OP_SUM;
                        b.Ops[k].Size = kPayload;
                        b.Ops[k].PayloadOff = 0;
                    }
                    DWORD bytes = 0;
                    QueryPerformanceCounter(&q0);
                    BOOL ok = DeviceIoControl(hDev, KMPB_IOCTL_RUN, &b, sizeof(b),
                                              &b, sizeof(b), &bytes, nullptr);
                    QueryPerformanceCounter(&q1);
                    if (!ok) { ++falhas; continue; }
                    lat.push_back((double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)fq.QuadPart);
                }

                std::sort(lat.begin(), lat.end());
                Resultado& r = io[cfg];
                r.nome = nms[cfg]; r.ops = n;
                r.media = 0; for (double d : lat) r.media += d;
                r.media = lat.empty() ? 0 : r.media / (double)lat.size();
                r.p50 = Pct(lat, 50); r.p95 = Pct(lat, 95); r.p99 = Pct(lat, 99);
                r.pior = lat.empty() ? 0 : lat.back();
                r.opsPorSeg = (r.media > 0) ? (1e6 / r.media) * (double)n : 0;

                printf("\n  %s\n", r.nome);
                printf("    amostras=%zu  falhas=%d\n", lat.size(), falhas);
                printf("    media=%.1f us   p50=%.1f   p95=%.1f   p99=%.1f   pior=%.1f\n",
                       r.media, r.p50, r.p95, r.p99, r.pior);
                printf("    %.0f ops/s\n", r.opsPorSeg);
            }
            CloseHandle(hDev);

            printf("\n  ===== COMPARATIVO (mesmo payload, mesma operacao) =====\n");
            printf("    %-28s p50=%8.1f us   p99=%8.1f   %10.0f ops/s\n", "A: IOCTL individual", io[0].p50, io[0].p99, io[0].opsPorSeg);
            printf("    %-28s p50=%8.1f us   p99=%8.1f   %10.0f ops/s\n", "C: shm individual",   res[0].p50,  res[0].p99,  res[0].opsPorSeg);
            printf("    %-28s p50=%8.1f us   p99=%8.1f   %10.0f ops/s\n", "B: IOCTL lote",        io[1].p50, io[1].p99, io[1].opsPorSeg);
            printf("    %-28s p50=%8.1f us   p99=%8.1f   %10.0f ops/s\n", "D: shm lote",          res[1].p50, res[1].p99, res[1].opsPorSeg);
            if (io[0].p50 > 0) printf("\n    individual: shm e' %.2fx o IOCTL (p50)\n", io[0].p50 / (res[0].p50 > 0 ? res[0].p50 : 1));
            if (io[1].p50 > 0) printf("    lote      : shm e' %.2fx o IOCTL (p50)\n",   io[1].p50 / (res[1].p50 > 0 ? res[1].p50 : 1));
        }
    }
    // ---- 6) descarregar: STOP -> DriverUnload -> KmpShutdown ----
    printf("\n  [..] ControlService(STOP) — exercita o DriverUnload\n");
    SERVICE_STATUS ss;
    if (ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
        printf("  [ok] driver parado e memoria liberada\n");
    else
        printf("  [err] STOP falhou: %lu\n", GetLastError());

    // ---- limpeza ----
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyRel);   // o driver ja apaga; idempotente
    UnmapViewOfFile(sh);
    CloseHandle(hSec);
    CloseHandle(hReq);
    CloseHandle(hRsp);
    CloseHandle(hSvc);
    CloseHandle(hScm);

    printf("\n=== fim ===\n");
    return 0;
}
