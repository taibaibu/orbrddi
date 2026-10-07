// Probe 3: exercise exactly the path flash download needs --
//   ROM table read, DHCSR halt/resume (run control), RAM write/read via the AHB-AP.
// Uses DAP_RegAccessBlock (batch) like the AGDI does for memory access.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef int RDDIHandle;
typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_CMSIS_DAP_Detect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, const int, const int, const int);
typedef int (*PFN_DAP_RegAccessBlock)(RDDIHandle, const int, const int, const int*, int*);

#define RID_DP_CTRLSTAT 1
#define RID_DP_SELECT   2
#define RID_DP_RDBUFF   3
#define RID_AP_CSW      4
#define RID_AP_TAR      5
#define RID_AP_BASE     6
#define RID_AP_DRW      7
#define RID_DP_ABORT    8
#define RN_W            0x10000

// AHB-AP CSW: 32-bit, addr increment single, DeviceEn(bit6)
#define CSW_32BIT_SINGLE 0x23000052
// Cortex-M debug registers
#define DHCSR   0xE000EDF0
#define DCRSR   0xE000EDF4
#define DCRDR   0xE000EDF8
#define DBGKEY  0xA05F0000u   // DHCSR: key goes into bits[31:16]

static PFN_DAP_ReadReg  pRd;
static PFN_DAP_WriteReg pWr;
static PFN_DAP_RegAccessBlock pBlk;
static RDDIHandle H;

// AP read helper: set TAR and read DRW in ONE batch, take that batch's result
// (same shape as Keil's RDDI_DAP_ReadData / AGDI's SWD_ReadData).
static int ApRead32(uint32_t addr, uint32_t* out)
{
    int regID[2]   = { RID_AP_TAR, RID_AP_DRW | RN_W };
    int regData[2] = { (int)addr, 0 };
    int rc = pBlk(H, 0, 2, regID, regData);
    if (out) *out = (uint32_t)regData[1];
    return rc;
}

static int ApWrite32(uint32_t addr, uint32_t val)
{
    int regID[2]   = { RID_AP_TAR, RID_AP_DRW };
    int regData[2] = { (int)addr, (int)val };
    return pBlk(H, 0, 2, regID, regData);
}

int main(int argc, char** argv)
{
    const char* dllPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";
    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) { printf("LoadLibrary failed (%s)\n", dllPath); return 1; }

    PFN_RDDI_Open  pOpen  = (PFN_RDDI_Open)GetProcAddress(dll, "RDDI_Open");
    PFN_RDDI_Close pClose = (PFN_RDDI_Close)GetProcAddress(dll, "RDDI_Close");
    PFN_CMSIS_DAP_Detect pDetect = (PFN_CMSIS_DAP_Detect)GetProcAddress(dll, "CMSIS_DAP_Detect");
    PFN_CMSIS_DAP_ConfigureInterface pCfg = (PFN_CMSIS_DAP_ConfigureInterface)GetProcAddress(dll, "CMSIS_DAP_ConfigureInterface");
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNum = (PFN_CMSIS_DAP_DetectNumberOfDAPs)GetProcAddress(dll, "CMSIS_DAP_DetectNumberOfDAPs");
    pRd  = (PFN_DAP_ReadReg)GetProcAddress(dll, "DAP_ReadReg");
    pWr  = (PFN_DAP_WriteReg)GetProcAddress(dll, "DAP_WriteReg");
    pBlk = (PFN_DAP_RegAccessBlock)GetProcAddress(dll, "DAP_RegAccessBlock");
    if (!pOpen || !pRd || !pWr || !pBlk) { printf("missing exports\n"); return 1; }

    H = 0;
    pOpen(&H, NULL);
    int nif = 0; pDetect(H, &nif);
    char cfg[] = "Port=SW;SWJ=Y;Clock=1000000;";
    pCfg(H, 0, cfg);
    int nd = 0; pNum(H, &nd);
    printf("connected, noOfDAPs=%d\n", nd);

    // power up DP
    pWr(H, 0, RID_DP_CTRLSTAT, (int)0x50000000);
    int ctrl = 0;
    for (int i = 0; i < 50; i++) { pRd(H, 0, RID_DP_CTRLSTAT, &ctrl); if ((ctrl & 0xA0000000) == 0xA0000000) break; Sleep(10); }
    pWr(H, 0, RID_DP_ABORT, 0x1E);

    // ---- 1) ROM table base (AP reg 0xF8, bank 0xF) -----------------------
    pWr(H, 0, RID_DP_SELECT, (int)(0xF << 4));
    int rombase = 0;
    pRd(H, 0, RID_AP_BASE, &rombase);
    printf("[1] ROM table base (AP 0xF8) = 0x%08X\n", rombase);
    pWr(H, 0, RID_DP_SELECT, 0);

    // ---- 2) AP CSW + write it ------------------------------------------
    int csw = 0;
    pRd(H, 0, RID_AP_CSW, &csw);
    printf("[2] AP CSW read  = 0x%08X\n", csw);
    pWr(H, 0, RID_AP_CSW, (int)CSW_32BIT_SINGLE);
    pRd(H, 0, RID_AP_CSW, &csw);
    printf("    AP CSW write 0x%08X -> read back 0x%08X\n", CSW_32BIT_SINGLE, csw);

    // ---- 3) CPUID -------------------------------------------------------
    uint32_t cpuid = 0;
    ApRead32(0xE000ED00, &cpuid);
    printf("[3] SCB->CPUID = 0x%08X\n", cpuid);

    // ---- 4) halt the core (DHCSR) --------------------------------------
    ApWrite32(DHCSR, DBGKEY | 0x0003);            // C_DEBUGEN | C_HALT
    uint32_t dhcsr = 0;
    for (int i = 0; i < 50; i++) { ApRead32(DHCSR, &dhcsr); if (dhcsr & 0x00020000) break; Sleep(10); }
    printf("[4] DHCSR after halt = 0x%08X  %s\n", dhcsr, (dhcsr & 0x00020000) ? "[S_HALT set]" : "[NOT halted]");

    // ---- 5) read PC via DCRSR/DCRDR ------------------------------------
    ApWrite32(DCRSR, 0x0001000F);                 // select R15 (PC)
    uint32_t dhcsr2 = 0;
    for (int i = 0; i < 100; i++) { ApRead32(DHCSR, &dhcsr2); if (dhcsr2 & 0x00010000) break; Sleep(5); }
    uint32_t pc = 0;
    ApRead32(DCRDR, &pc);
    printf("[5] PC = 0x%08X  (S_REGRDY=0x%X)\n", pc, (dhcsr2 >> 16) & 1);

    // ---- 6) RAM write / read back (what the flash algo does) -----------
    const uint32_t ramAddr = 0x20000000;
    const uint32_t pattern = 0xA5A55A5A;
    int rcw = ApWrite32(ramAddr, pattern);
    uint32_t rb = 0;
    int rcr = ApRead32(ramAddr, &rb);
    printf("[6] RAM write 0x%08X <- 0x%08X (rc=%d), read back 0x%08X (rc=%d)  %s\n",
           ramAddr, pattern, rcw, rb, rcr, (rb == pattern) ? "[OK]" : "[MISMATCH]");

    // ---- 7) resume ------------------------------------------------------
    ApWrite32(DHCSR, DBGKEY | 0x0001);            // C_DEBUGEN only
    uint32_t dhcsr3 = 0;
    ApRead32(DHCSR, &dhcsr3);
    printf("[7] DHCSR after resume = 0x%08X  %s\n", dhcsr3, (dhcsr3 & 0x00020000) ? "[still halted]" : "[running]");

    pClose(H);
    return 0;
}
