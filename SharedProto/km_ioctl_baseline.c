// ===========================================================================
// km_ioctl_baseline.c — BASELINE por IOCTL, para comparação com o shared memory
//
// Existe UMA razão: medir o custo do transporte convencional com o MESMO
// payload e a MESMA operação do protótipo. Sem isto, "shared memory é rápido"
// não significa nada — falta o número do outro lado.
//
// NÃO faz leitura de memória de processo. Só soma bytes de um buffer. É tão
// inofensivo quanto o outro, e é o que permite a comparação ser honesta.
//
// ---------------------------------------------------------------------------
// DIFERENÇA ESTRUTURAL (é ela que a medição captura)
//
//   IOCTL METHOD_BUFFERED:
//     1. user -> kernel: o I/O manager copia o input inteiro para SystemBuffer
//     2. kernel processa
//     3. kernel -> user: copia o output inteiro de volta
//     = 2 cópias de ~6 KB por chamada
//
//   shared memory:
//     0 cópias. O driver escreve direto na página compartilhada.
//
// É por isso que o lote é onde o ganho aparece: as duas cópias custam por
// CHAMADA, e no lote o custo é dividido por 64 operações.
// ===========================================================================

#include <ntifs.h>
#include "km_ipc_proto.h"

#define KMPB_TAG        'bpmK'
#define KMPB_DEVICE     L"\\Device\\KMPBaseline"
#define KMPB_SYMLINK    L"\\DosDevices\\KMPBaseline"
#define KMPB_IOCTL_RUN  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// O lote vai e volta no MESMO buffer, como qualquer IOCTL bem comportado.
#define KMPB_DATA_BYTES  4096

#pragma pack(push, 8)
typedef struct _KMPB_BATCH {
    kmp_u32 Magic;
    kmp_u32 Version;
    kmp_u32 Count;
    kmp_u32 _pad;
    kmp_u64 PayloadUsed;
    KMP_OP  Ops[KMP_MAX_OPS];
    kmp_u8  Data[KMPB_DATA_BYTES];
} KMPB_BATCH, *PKMPB_BATCH;
#pragma pack(pop)

static PDEVICE_OBJECT g_Dev = NULL;

// Mesma semântica do KmpProcessBatch do outro driver, inclusive a validação.
static void KmpbRun(PKMPB_BATCH b)
{
    kmp_u8* data = b->Data;
    ULONG64 cursor = (b->PayloadUsed + 7ULL) & ~7ULL;
    ULONG   i, falhas = 0;

    if (b->Magic != KMP_MAGIC || b->Version != KMP_VERSION) return;
    if (b->Count > KMP_MAX_OPS) return;
    if (b->PayloadUsed > KMPB_DATA_BYTES) return;

    for (i = 0; i < b->Count; ++i) {
        KMP_OP op = b->Ops[i];
        op.Out0 = 0; op.DataOff = 0; op.Status = KMP_OK;

        if (op.Kind == KMP_OP_SUM) {
            const ULONGLONG fim = (ULONGLONG)op.PayloadOff + (ULONGLONG)op.Size;
            if (fim > (ULONGLONG)b->PayloadUsed) { op.Status = KMP_ERR_BAD_PARAMETER; ++falhas; }
            else {
                ULONG64 s = 0, k;
                for (k = 0; k < op.Size; ++k) s += data[(SIZE_T)op.PayloadOff + (SIZE_T)k];
                op.Out0 = s;
            }
        } else if (op.Kind == KMP_OP_NOP) {
            /* nada */
        } else if (op.Kind == KMP_OP_FILL) {
            if ((ULONGLONG)cursor + op.Size > (ULONGLONG)KMPB_DATA_BYTES) {
                op.Status = KMP_ERR_BAD_PARAMETER; ++falhas;
            } else {
                RtlFillMemory(data + cursor, (SIZE_T)op.Size, (UCHAR)(op.Out0 & 0xFF));
                op.DataOff = cursor;
                cursor = (cursor + op.Size + 7ULL) & ~7ULL;
            }
        } else {
            op.Status = KMP_ERR_NOT_IMPLEMENTED; ++falhas;
        }

        b->Ops[i] = op;
    }
    b->_pad = falhas;
}

static NTSTATUS KmpbDispatch(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PVOID  buf = Irp->AssociatedIrp.SystemBuffer;
    ULONG  in  = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG  out = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG  code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    UNREFERENCED_PARAMETER(DevObj);

    switch (stack->MajorFunction) {

    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        st = STATUS_SUCCESS;
        break;

    case IRP_MJ_DEVICE_CONTROL:
        if (code != KMPB_IOCTL_RUN) break;
        if (buf == NULL) { st = STATUS_INVALID_PARAMETER; break; }
        if (in < sizeof(KMPB_BATCH) || out < sizeof(KMPB_BATCH)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KmpbRun((PKMPB_BATCH)buf);
        st = STATUS_SUCCESS;
        info = sizeof(KMPB_BATCH);
        break;
    }

    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

static ULONG g_Step = 0;

// Grava o passo em <servico>\Parameters\FailStep. Mesmo instrumento do driver
// de shared memory, pelo mesmo motivo: sem numero, toda falha vira "erro 2".
static void KmpbTrace(PUNICODE_STRING svc)
{
    WCHAR             path[300];
    UNICODE_STRING    kn, vn;
    OBJECT_ATTRIBUTES oa;
    HANDLE            k = NULL;
    int               n, i;

    if (!svc || !svc->Buffer) return;
    n = (int)(svc->Length / sizeof(WCHAR));
    if (n <= 0 || n > 260) return;
    for (i = 0; i < n; ++i) path[i] = svc->Buffer[i];
    { static const WCHAR s[] = L"\\Parameters"; for (i = 0; s[i]; ++i) path[n + i] = s[i]; path[n + i] = 0; }

    RtlInitUnicodeString(&kn, path);
    RtlInitUnicodeString(&vn, L"FailStep");
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    if (NT_SUCCESS(ZwOpenKey(&k, KEY_SET_VALUE, &oa))) {
        ZwSetValueKey(k, &vn, 0, REG_DWORD, &g_Step, sizeof(g_Step));
        ZwClose(k);
    }
}
static VOID KmpbUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING sym;
    UNREFERENCED_PARAMETER(DriverObject);

    RtlInitUnicodeString(&sym, KMPB_SYMLINK);
    IoDeleteSymbolicLink(&sym);
    if (g_Dev) { IoDeleteDevice(g_Dev); g_Dev = NULL; }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING devName, symName;
    NTSTATUS st;
    int i;

    RtlInitUnicodeString(&devName, KMPB_DEVICE);
    RtlInitUnicodeString(&symName, KMPB_SYMLINK);

    g_Step = 1;
    st = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                        0, FALSE, &g_Dev);
    if (!NT_SUCCESS(st)) { KmpbTrace(RegistryPath); return st; }

    g_Step = 2;
    st = IoCreateSymbolicLink(&symName, &devName);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(g_Dev); g_Dev = NULL; KmpbTrace(RegistryPath); return st; }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = KmpbDispatch;
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = KmpbDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = KmpbDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KmpbDispatch;
    DriverObject->DriverUnload = KmpbUnload;

    g_Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    g_Step = 0;                       // 0 = subiu
    KmpbTrace(RegistryPath);
    return STATUS_SUCCESS;
}
