// ===========================================================================
// AsIO3_Client.h
//
// Cliente do AsIO3.sys (ASUS), o driver doador da rota A.
//
// ESTADO NESTA MAQUINA (verificado):
//
//   servico   Asusgio3   Type=1 (kernel)  Start=3 (manual)  Stopped
//   binario   C:\Windows\System32\drivers\AsIO3.sys     59344 bytes
//   assinado  ASUSTeK COMPUTER INC.  Status: Valid
//   device    \Device\Asusgio3  ->  \DosDevices\Asusgio3
//
//   checagem de chamador: o driver compara o processo que chama com
//   <ProgramFilesDir(x86)>\ASUS\AsusCertService\AsusCertService.exe
//   --> C:\Program Files (x86)\ASUS\AsusCertService\AsusCertService.exe
//       EXISTE, assinado por ASUSTeK, versao 1.02.22 (mesma do driver)
//
// Ou seja: o driver esta instalado como servico legitimo, assinado, e o
// binario que satisfaz a checagem esta no lugar. Nada de kdmapper para ele.
//
// PARA QUE SERVE: acesso a MSR, porta I/O e config space PCI, e — o que
// interessa — acesso a memoria fisica (ZwOpenSection em \Device\PhysicalMemory
// e MmMapIoSpace). NAO da memoria virtual: nao importa MmCopyVirtualMemory,
// ZwOpenProcess nem KeStackAttachProcess.
//
// O contrato foi extraido estaticamente; ver AsIO3_IOCTL.md. Os offsets do
// buffer ainda NAO foram confirmados em runtime.
// ===========================================================================

#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>

namespace AsIO3 {

// ---------------------------------------------------------------------------
// Identidade
// ---------------------------------------------------------------------------
inline constexpr const char* kDeviceUserPath = "\\\\.\\Asusgio3";
inline constexpr const wchar_t* kServiceName = L"Asusgio3";
inline constexpr const wchar_t* kDriverPath  = L"\\SystemRoot\\System32\\drivers\\AsIO3.sys";
inline constexpr const char* kCallerExeName  = "AsusCertService.exe";

// ---------------------------------------------------------------------------
// IOCTLs — FILE_DEVICE = 0xA040
//
// Extraidos com tools\dump_driver\ioctl_xref.py, que reconhece o padrao de
// instrucao do switch (`81 F9 imm32` + `0F 84 rel32`) em vez de varrer bytes.
// ---------------------------------------------------------------------------
constexpr ULONG kDev = 0xA040;

#define ASIO3_CTL(func, method, access) \
    CTL_CODE(0xA040, (func), (method), (access))

// leitura de baixo nivel, tres larguras
//   contexto do dispatcher: 0F 30 (wrmsr) / 0F 32 (rdmsr) / ED / EE / 66 EF
constexpr ULONG IOCTL_READ_LOW_0 = ASIO3_CTL(0x900, METHOD_BUFFERED, FILE_READ_ACCESS); // 0xA0406400
constexpr ULONG IOCTL_READ_LOW_1 = ASIO3_CTL(0x901, METHOD_BUFFERED, FILE_READ_ACCESS); // 0xA0406404
constexpr ULONG IOCTL_READ_LOW_2 = ASIO3_CTL(0x902, METHOD_BUFFERED, FILE_READ_ACCESS); // 0xA0406408

// escrita de baixo nivel, tres larguras
//   contexto: __inbyte/__indword/__outbyte/__outdword
constexpr ULONG IOCTL_WRITE_LOW_0 = ASIO3_CTL(0x910, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A440
constexpr ULONG IOCTL_WRITE_LOW_1 = ASIO3_CTL(0x911, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A444
constexpr ULONG IOCTL_WRITE_LOW_2 = ASIO3_CTL(0x912, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A448

// segunda base de escrita
constexpr ULONG IOCTL_WRITE_ALT_0 = ASIO3_CTL(0x950, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A540
constexpr ULONG IOCTL_WRITE_ALT_1 = ASIO3_CTL(0x951, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A544
constexpr ULONG IOCTL_WRITE_ALT_2 = ASIO3_CTL(0x952, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A548

// especiais, uma por familia de largura
constexpr ULONG IOCTL_WRITE_SPECIAL_0 = ASIO3_CTL(0x913, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A44C
constexpr ULONG IOCTL_WRITE_SPECIAL_1 = ASIO3_CTL(0x953, METHOD_BUFFERED, FILE_WRITE_ACCESS); // 0xA040A54C

// config / tune
constexpr ULONG IOCTL_CFG_0 = ASIO3_CTL(0x3D6, METHOD_BUFFERED, FILE_ANY_ACCESS); // 0xA0400F58
constexpr ULONG IOCTL_CFG_1 = ASIO3_CTL(0x3D7, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_2 = ASIO3_CTL(0x3D8, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_3 = ASIO3_CTL(0x3D9, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_4 = ASIO3_CTL(0x3DA, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_5 = ASIO3_CTL(0x3DB, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_6 = ASIO3_CTL(0x3DC, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_7 = ASIO3_CTL(0x3DD, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_8 = ASIO3_CTL(0x3DE, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_9 = ASIO3_CTL(0x3DF, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_10 = ASIO3_CTL(0x3E0, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_11 = ASIO3_CTL(0x3E1, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_12 = ASIO3_CTL(0x3E2, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_13 = ASIO3_CTL(0x3E3, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_14 = ASIO3_CTL(0x3E4, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_CFG_15 = ASIO3_CTL(0x3E5, METHOD_BUFFERED, FILE_ANY_ACCESS);

// info
constexpr ULONG IOCTL_INFO_0 = ASIO3_CTL(0x800, METHOD_BUFFERED, FILE_ANY_ACCESS); // 0xA0402000
constexpr ULONG IOCTL_INFO_1 = ASIO3_CTL(0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_INFO_2 = ASIO3_CTL(0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_INFO_3 = ASIO3_CTL(0x804, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_INFO_4 = ASIO3_CTL(0x805, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG IOCTL_INFO_5 = ASIO3_CTL(0x806, METHOD_BUFFERED, FILE_ANY_ACCESS);

// ---------------------------------------------------------------------------
// Cliente minimo
// ---------------------------------------------------------------------------

class Client {
public:
    Client() : m_dev(INVALID_HANDLE_VALUE) {}
    ~Client() { Close(); }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool IsOpen() const { return m_dev != INVALID_HANDLE_VALUE; }
    HANDLE Device() const { return m_dev; }

    // Abre \\.\Asusgio3. Nao carrega o driver: se o servico estiver parado,
    // falha com ERROR_FILE_NOT_FOUND e a mensagem diz o que fazer.
    bool Open()
    {
        if (IsOpen()) return true;
        m_dev = CreateFileA(kDeviceUserPath,
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        m_lastError = (m_dev == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
        return IsOpen();
    }

    void Close()
    {
        if (IsOpen()) {
            CloseHandle(m_dev);
            m_dev = INVALID_HANDLE_VALUE;
        }
    }

    DWORD LastError() const { return m_lastError; }

    // Envia um IOCTL cru. O cliente monta o buffer; o layout por familia
    // esta em AsIO3_IOCTL.md e ainda nao foi confirmado em runtime.
    bool Raw(ULONG ioctl, void* inBuf, DWORD inLen, void* outBuf, DWORD outLen, DWORD* bytesOut = nullptr)
    {
        if (!IsOpen()) return false;
        DWORD bytes = 0;
        const BOOL ok = DeviceIoControl(m_dev, ioctl, inBuf, inLen, outBuf, outLen, &bytes, nullptr);
        m_lastError = ok ? 0 : GetLastError();
        if (bytesOut) *bytesOut = bytes;
        return ok != FALSE;
    }

private:
    HANDLE m_dev;
    DWORD  m_lastError = 0;
};

} // namespace AsIO3
