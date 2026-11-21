#include <ntifs.h>

static PVOID g_P;

static PVOID A1(void) {
    return InterlockedExchangePointer((PVOID*)&g_P, NULL);
}

static PVOID A2(void) {
    return _InterlockedExchangePointer((void* volatile*)&g_P, NULL);
}

static PVOID A3(void) {
    return _InterlockedCompareExchangePointer((void* volatile*)&g_P, NULL, (void*)(ULONG_PTR)g_P);
}

int main(void) { (void)A1(); (void)A2(); (void)A3(); return 0; }
