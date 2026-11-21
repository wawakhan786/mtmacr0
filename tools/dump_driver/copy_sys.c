// copy_sys.c — copia um .sys de System32\drivers para o diretorio do projeto
// usando so Win32 (sem depender de permissao de leitura do sandbox).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static BOOL CopyOne(const char* src, const char* dst)
{
    HANDLE hIn = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hIn == INVALID_HANDLE_VALUE) {
        printf("[-] CreateFile %s falhou: %lu\n", src, GetLastError());
        return FALSE;
    }

    HANDLE hOut = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        printf("[-] CreateFile %s falhou: %lu\n", dst, GetLastError());
        CloseHandle(hIn);
        return FALSE;
    }

    BYTE buf[64 * 1024];
    DWORD rd = 0, wr = 0;
    ULONGLONG total = 0;
    while (ReadFile(hIn, buf, sizeof(buf), &rd, NULL) && rd)
    {
        if (!WriteFile(hOut, buf, rd, &wr, NULL) || wr != rd) {
            printf("[-] WriteFile falhou: %lu\n", GetLastError());
            CloseHandle(hOut); CloseHandle(hIn);
            return FALSE;
        }
        total += rd;
    }

    CloseHandle(hOut);
    CloseHandle(hIn);
    printf("[+] %s -> %s (%llu bytes)\n", src, dst, total);
    return TRUE;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("uso: copy_sys <src.sys> <dst.sys>\n"); return 1; }
    return CopyOne(argv[1], argv[2]) ? 0 : 2;
}
