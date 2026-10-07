// Minimal probe: replicate exactly what the AGDI does to obtain the IDCODE.
// Built as x86 so it can load the 32-bit ORBMDK_RDDI.dll.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef int RDDIHandle;

typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_RDDI_GetLastError)(int*, char*, size_t);
typedef int (*PFN_CMSIS_DAP_Detect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_Identify)(RDDIHandle, int, int, char*, const int);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_DetectDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_Connect)(RDDIHandle, void*);
typedef int (*PFN_DAP_GetDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_DAP_GetNumberOfDAPs)(RDDIHandle, int*);

static void* Get(HMODULE dll, const char* n, bool& ok)
{
    void* p = (void*)GetProcAddress(dll, n);
    printf("  %-38s %s\n", n, p ? "OK" : "MISSING");
    if (!p) ok = false;
    return p;
}

int main(void)
{
    const char* path = "C:\\Users\\234896\\Desktop\\orbmdk\\ORBMDK\\bin\\ORBMDK_RDDI.dll";
    printf("LoadLibrary: %s\n", path);
    HMODULE dll = LoadLibraryA(path);
    if (!dll) { printf("  FAILED err=%lu\n", GetLastError()); return 1; }
    printf("  OK\n\nResolving exports:\n");

    bool ok = true;
    PFN_RDDI_Open pOpen = (PFN_RDDI_Open)Get(dll, "RDDI_Open", ok);
    PFN_RDDI_Close pClose = (PFN_RDDI_Close)Get(dll, "RDDI_Close", ok);
    PFN_RDDI_GetLastError pLastErr = (PFN_RDDI_GetLastError)Get(dll, "RDDI_GetLastError", ok);
    PFN_CMSIS_DAP_Detect pDetect = (PFN_CMSIS_DAP_Detect)Get(dll, "CMSIS_DAP_Detect", ok);
    PFN_CMSIS_DAP_Identify pIdentify = (PFN_CMSIS_DAP_Identify)Get(dll, "CMSIS_DAP_Identify", ok);
    PFN_CMSIS_DAP_ConfigureInterface pCfgIf = (PFN_CMSIS_DAP_ConfigureInterface)Get(dll, "CMSIS_DAP_ConfigureInterface", ok);
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNumDaps = (PFN_CMSIS_DAP_DetectNumberOfDAPs)Get(dll, "CMSIS_DAP_DetectNumberOfDAPs", ok);
    PFN_CMSIS_DAP_DetectDAPIDList pDapIds = (PFN_CMSIS_DAP_DetectDAPIDList)Get(dll, "CMSIS_DAP_DetectDAPIDList", ok);
    PFN_DAP_ReadReg pReadReg = (PFN_DAP_ReadReg)Get(dll, "DAP_ReadReg", ok);
    PFN_DAP_Connect pConnect = (PFN_DAP_Connect)Get(dll, "DAP_Connect", ok);
    PFN_DAP_GetDAPIDList pDapList = (PFN_DAP_GetDAPIDList)Get(dll, "DAP_GetDAPIDList", ok);
    PFN_DAP_GetNumberOfDAPs pNumDaps2 = (PFN_DAP_GetNumberOfDAPs)Get(dll, "DAP_GetNumberOfDAPs", ok);
    if (!ok) { printf("\nABORT: missing exports\n"); return 1; }

    RDDIHandle h = 0;
    int rc = pOpen(&h, NULL);
    printf("\nRDDI_Open -> %d, handle=%d\n", rc, h);
    if (rc != 0) return 1;

    int numIf = -1;
    rc = pDetect(h, &numIf);
    printf("CMSIS_DAP_Detect -> %d, numOfIFs=%d\n", rc, numIf);

    for (int idNo = 1; idNo <= 4; ++idNo) {
        char buf[256] = {0};
        rc = pIdentify(h, 0, idNo, buf, (int)sizeof(buf));
        printf("CMSIS_DAP_Identify(idNo=%d) -> %d, '%s'\n", idNo, rc, buf);
    }

    char cfg[] = "Port=SW;SWJ=Y;Clock=1000000;";
    rc = pCfgIf(h, 0, cfg);
    printf("CMSIS_DAP_ConfigureInterface -> %d\n", rc);

    rc = pConnect(h, NULL);
    printf("DAP_Connect -> %d\n", rc);

    int nDaps = -1;
    rc = pNumDaps(h, &nDaps);
    printf("\n*** CMSIS_DAP_DetectNumberOfDAPs -> %d, noOfDAPs=%d\n", rc, nDaps);

    int ids[16];
    for (int i = 0; i < 16; ++i) ids[i] = 0xDEADBEEF;
    rc = pDapIds(h, ids, (size_t)nDaps);
    printf("*** CMSIS_DAP_DetectDAPIDList(size=%d) -> %d, ids[0]=0x%08X\n", nDaps, rc, ids[0]);

    int v = 0;
    rc = pReadReg(h, 0, 0, &v);
    printf("*** DAP_ReadReg(regID=0 DPIDR) -> %d, value=0x%08X\n", rc, v);

    rc = pReadReg(h, 0, 4, &v);
    printf("*** DAP_ReadReg(regID=4 AP CSW) -> %d, value=0x%08X\n", rc, v);

    int n2 = -1;
    rc = pNumDaps2(h, &n2);
    printf("DAP_GetNumberOfDAPs -> %d, n=%d\n", rc, n2);
    int list[16] = {0};
    rc = pDapList(h, list, sizeof(list));
    printf("DAP_GetDAPIDList -> %d, list[0]=0x%08X\n", rc, list[0]);

    int err = 0; char det[256] = {0};
    rc = pLastErr(&err, det, sizeof(det));
    printf("\nRDDI_GetLastError -> %d, err=0x%04X, '%s'\n", rc, err, det);

    pClose(h);
    return 0;
}
