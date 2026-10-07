// Full AP access probe: power up DP, set SELECT, read the AHB-AP registers and
// SCB->CPUID through the AHB-AP. This validates the ARM RDDI register-ID decoding
// in ORBMDK_RDDI.dll (regID 0..3 = DP, 4..7 = AP).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef int RDDIHandle;

typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_RDDI_GetLastError)(int*, char*, size_t);
typedef int (*PFN_CMSIS_DAP_Detect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_DetectDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, const int, const int, const int);
typedef int (*PFN_DAP_RegAccessBlock)(RDDIHandle, const int, const int, const int*, int*);

// ARM rddi_dap.h register IDs
#define RID_DP_IDCODE   0
#define RID_DP_CTRLSTAT 1
#define RID_DP_SELECT   2
#define RID_DP_RDBUFF   3
#define RID_AP_CSW      4
#define RID_AP_TAR      5
#define RID_AP_BASE     6
#define RID_AP_DRW      7
#define RID_DP_ABORT    8
#define RN_W            0x10000

static void* Get(HMODULE d, const char* n, bool& ok)
{
    void* p = (void*)GetProcAddress(d, n);
    if (!p) { printf("  %-34s MISSING\n", n); ok = false; }
    return p;
}

int main(void)
{
    HMODULE dll = LoadLibraryA("C:\\Users\\234896\\Desktop\\orbmdk\\ORBMDK\\bin\\ORBMDK_RDDI.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }

    bool ok = true;
    PFN_RDDI_Open  pOpen   = (PFN_RDDI_Open)Get(dll, "RDDI_Open", ok);
    PFN_RDDI_Close pClose  = (PFN_RDDI_Close)Get(dll, "RDDI_Close", ok);
    PFN_RDDI_GetLastError pLastErr = (PFN_RDDI_GetLastError)Get(dll, "RDDI_GetLastError", ok);
    PFN_CMSIS_DAP_Detect pDetect = (PFN_CMSIS_DAP_Detect)Get(dll, "CMSIS_DAP_Detect", ok);
    PFN_CMSIS_DAP_ConfigureInterface pCfgIf = (PFN_CMSIS_DAP_ConfigureInterface)Get(dll, "CMSIS_DAP_ConfigureInterface", ok);
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNumDaps = (PFN_CMSIS_DAP_DetectNumberOfDAPs)Get(dll, "CMSIS_DAP_DetectNumberOfDAPs", ok);
    PFN_CMSIS_DAP_DetectDAPIDList pDapIds = (PFN_CMSIS_DAP_DetectDAPIDList)Get(dll, "CMSIS_DAP_DetectDAPIDList", ok);
    PFN_DAP_ReadReg  pRd = (PFN_DAP_ReadReg)Get(dll, "DAP_ReadReg", ok);
    PFN_DAP_WriteReg pWr = (PFN_DAP_WriteReg)Get(dll, "DAP_WriteReg", ok);
    PFN_DAP_RegAccessBlock pBlk = (PFN_DAP_RegAccessBlock)Get(dll, "DAP_RegAccessBlock", ok);
    if (!ok) return 1;

    RDDIHandle h = 0;
    if (pOpen(&h, NULL) != 0) { printf("RDDI_Open failed\n"); return 1; }
    printf("RDDI_Open -> handle=%d\n", h);

    int nif = 0; pDetect(h, &nif);
    char cfg[] = "Port=SW;SWJ=Y;Clock=1000000;";
    pCfgIf(h, 0, cfg);

    int nd = 0;
    int rc = pNumDaps(h, &nd);
    int idcode = 0;
    pDapIds(h, &idcode, 1);
    printf("DetectNumberOfDAPs rc=%d nd=%d, IDCODE=0x%08X\n", rc, nd, idcode);

    // ---- power up the DP -------------------------------------------------
    pWr(h, 0, RID_DP_CTRLSTAT, (int)0x50000000);   // CSYSPWRUPREQ|CDBGPWRUPREQ
    int ctrl = 0;
    for (int i = 0; i < 50; i++) {
        pRd(h, 0, RID_DP_CTRLSTAT, &ctrl);
        if ((ctrl & 0xA0000000) == 0xA0000000) break;
        Sleep(10);
    }
    printf("DP CTRL/STAT (regID=1) = 0x%08X  %s\n", ctrl,
           ((ctrl & 0xA0000000) == 0xA0000000) ? "[powered up]" : "[NOT powered]");

    // clear sticky errors
    pWr(h, 0, RID_DP_ABORT, (int)0x1E);

    // ---- SELECT: APSEL=0, APBANK=0 --------------------------------------
    pWr(h, 0, RID_DP_SELECT, 0);
    printf("DP SELECT  (regID=2) written 0\n");

    // ---- AP registers ----------------------------------------------------
    int csw = 0, base = 0, tar = 0, drw = 0;
    pRd(h, 0, RID_AP_CSW,  &csw);
    printf("AP CSW     (regID=4) = 0x%08X\n", csw);
    pRd(h, 0, RID_AP_BASE, &base);
    printf("AP BASE    (regID=6) = 0x%08X\n", base);

    // TAR = SCB->CPUID, then read DRW (AP reads are posted: read twice)
    pWr(h, 0, RID_AP_TAR, (int)0xE000ED00);
    pRd(h, 0, RID_AP_DRW, &drw);
    pRd(h, 0, RID_AP_DRW, &drw);
    printf("SCB->CPUID (via AP DRW regID=7) = 0x%08X\n", drw);

    // ---- DAP_RegAccessBlock: TAR=0xE000ED00 then read DRW ----------------
    {
        int regID[2]  = { RID_AP_TAR, RID_AP_DRW | RN_W };
        int regData[2] = { (int)0xE000ED00, 0 };
        rc = pBlk(h, 0, 2, regID, regData);
        printf("RegAccessBlock(TAR+DRW read) rc=%d data=0x%08X\n", rc, regData[1]);
        // AP posted read: issue a second access to fetch the value
        int regID2[1]  = { RID_AP_DRW | RN_W };
        int regData2[1] = { 0 };
        rc = pBlk(h, 0, 1, regID2, regData2);
        printf("RegAccessBlock(DRW read again) rc=%d data=0x%08X\n", rc, regData2[0]);
    }

    int err = 0; char det[256] = {0};
    pLastErr(&err, det, sizeof(det));
    printf("LastError: err=0x%04X '%s'\n", err, det);

    pClose(h);
    return 0;
}
