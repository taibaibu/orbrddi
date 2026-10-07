// hidprobe.cpp — 走 ifNo=1（CMSIS-DAP v1 / HID）的极简功能回归
//
// 用途：验证 HID 通路（V1）的功能与日志都正常，与 swdprobe.cpp 的 V2 回归成对使用：
//         bin\swdprobe.exe             -> ifNo=0 (v2 / Bulk)
//         bin\swdprobe.exe ORBMDK_RDDI.dll v1   -> ifNo=1 (v1 / HID)
//         bin\hidprobe.exe             -> ifNo=1 (v1 / HID)，更小更快
// 纯 ASCII，避免 MSVC C4819。
#include <windows.h>
#include <stdio.h>

typedef int RDDIHandle;
typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, const int, const int, const int);

#define RID_DP_CTRLSTAT 1

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";

    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) { printf("LoadLibrary failed (%s)\n", dllPath); return 1; }

    PFN_RDDI_Open pOpen = (PFN_RDDI_Open)GetProcAddress(dll, "RDDI_Open");
    PFN_RDDI_Close pClose = (PFN_RDDI_Close)GetProcAddress(dll, "RDDI_Close");
    PFN_CMSIS_DAP_ConfigureInterface pCfg =
        (PFN_CMSIS_DAP_ConfigureInterface)GetProcAddress(dll, "CMSIS_DAP_ConfigureInterface");
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNum =
        (PFN_CMSIS_DAP_DetectNumberOfDAPs)GetProcAddress(dll, "CMSIS_DAP_DetectNumberOfDAPs");
    PFN_DAP_ReadReg pRd = (PFN_DAP_ReadReg)GetProcAddress(dll, "DAP_ReadReg");
    PFN_DAP_WriteReg pWr = (PFN_DAP_WriteReg)GetProcAddress(dll, "DAP_WriteReg");

    if (!pOpen || !pClose || !pCfg || !pNum || !pRd || !pWr) {
        printf("missing exports\n");
        return 1;
    }

    RDDIHandle h = 0;
    printf("RDDI_Open           -> %d (handle=%d)\n", pOpen(&h, NULL), h);

    char cfg[] = "Port=SW;SWJ=Y;Clock=1000000;";
    printf("ConfigureInterface  -> %d  (ifNo=1 = CMSIS-DAP v1 / HID)\n", pCfg(h, 1, cfg));

    int nd = 0;
    printf("DetectNumberOfDAPs  -> %d (noOfDAPs=%d)\n", pNum(h, &nd), nd);

    // DP 上电并等 READOK + CDBGPWRUPACK —— 一次完整的 HID 命令往返
    pWr(h, 0, RID_DP_CTRLSTAT, (int)0x50000000);
    int ctrl = 0;
    for (int i = 0; i < 50; ++i) {
        pRd(h, 0, RID_DP_CTRLSTAT, &ctrl);
        if ((ctrl & 0xA0000000) == 0xA0000000) break;
        Sleep(10);
    }
    printf("DP CTRLSTAT         -> 0x%08X  %s\n",
           (unsigned)ctrl, ((ctrl & 0xA0000000) == 0xA0000000) ? "[powered up]" : "[no ack]");

    pClose(h);
    return ((ctrl & 0xA0000000) == 0xA0000000) ? 0 : 2;
}
