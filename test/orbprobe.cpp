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
// AGDI 实际调用的是这个（不是 DAP_Connect），必须一起复现
typedef int (*PFN_CMSIS_DAP_Connect)(RDDIHandle, int*);
typedef int (*PFN_DAP_GetDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_DAP_GetNumberOfDAPs)(RDDIHandle, int*);

static void* Get(HMODULE dll, const char* n, bool& ok)
{
    void* p = (void*)GetProcAddress(dll, n);
    printf("  %-38s %s\n", n, p ? "OK" : "MISSING");
    if (!p) ok = false;
    return p;
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";
    printf("LoadLibrary: %s\n", path);
    HMODULE dll = LoadLibraryA(path);
    if (!dll) { printf("  FAILED err=%lu\n", GetLastError()); return 1; }
    printf("  OK\n\nResolving exports:\n");

    // ------------------------------------------------------------------
    // AGDI 的"多 DAP"分支里有一句**裸文件名** LoadLibraryA("CMSIS_DAP.dll")
    // （反汇编 0x1002C200 附近，常量字符串 "CMSIS_DAP.dll"）。
    // 这里验证：当同名模块已按全路径加载时，裸名能否取到它。
    //   - 返回非 NULL  -> 裸名加载本身没问题，故障在别处
    //   - 返回 NULL    -> AGDI 那个分支必然失败（0x2029 -> RDDI-DAP Error）
    // ------------------------------------------------------------------
    {
        char self[MAX_PATH] = {0};
        GetModuleFileNameA(dll, self, MAX_PATH);
        SetLastError(0);
        HMODULE again = LoadLibraryA("CMSIS_DAP.dll");
        DWORD err = GetLastError();
        printf("\n[probe] module path      : %s\n", self);
        printf("[probe] bare-name reload : %p (err=%lu)\n", (void*)again, err);
        printf("[probe] CWD              : ");
        { char cwd[MAX_PATH] = {0}; GetCurrentDirectoryA(MAX_PATH, cwd); printf("%s\n", cwd); }
        if (again) { printf("[probe] -> bare name resolves to an existing module\n\n"); }
        else       { printf("[probe] -> bare name NOT resolvable (this is AGDI's 0x2029)\n\n"); }
    }

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
    PFN_CMSIS_DAP_Connect pDapConnect = (PFN_CMSIS_DAP_Connect)Get(dll, "CMSIS_DAP_Connect", ok);
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

    // 按 AGDI 对话框的做法：对每个 ifNo 都问一遍产品名/序列号，
    // 再按用户选择调用 ConfigureInterface —— 这里把两个 ifNo 都走一遍，
    // 验证"两种模式都显示、可切换"。
    for (int ifNo = 0; ifNo < numIf; ++ifNo) {
        for (int idNo = 2; idNo <= 3; ++idNo) {
            char buf[256] = {0};
            rc = pIdentify(h, ifNo, idNo, buf, (int)sizeof(buf));
            printf("CMSIS_DAP_Identify(ifNo=%d, idNo=%d) -> %d, '%s'\n", ifNo, idNo, rc, buf);
        }
    }

    // AGDI 的适配器条目字段：name=Identify(ifNo,2)、serial=Identify(ifNo,3)、
    // version=Identify(ifNo,4)。version 会被按 "%lu.%lu.%lu" 解析取主版本号，
    // 主版本 >= 2 会让 AGDI 走"多 DAP"分支。先把这几个串打出来。
    for (int ifn = 0; ifn < 2; ++ifn) {
        for (int idn = 1; idn <= 4; ++idn) {
            char b[260] = {0};
            rc = pIdentify(h, ifn, idn, b, (int)sizeof(b));
            printf("  Identify(ifNo=%d, idNo=%d) -> %d, '%s'\n", ifn, idn, rc, b);
        }
    }

    // 完全照 AGDI 的顺序复现：
    //   ConfigureInterface(ifNo) -> DAP_Configure -> CMSIS_DAP_Connect -> 后续访问
    // 用户报的崩溃就发生在 CMSIS_DAP_Connect 里。
    char cfg[] = "Master=Y;Port=SW;SWJ=Y;Clock=1000000;Trace=Off;";

    for (int pass = 0; pass < 2; ++pass) {
        const int ifNo = (pass == 0) ? 1 : 0;   // 先 v1(HID)，再 v2(Bulk)
        rc = pCfgIf(h, ifNo, cfg);
        printf("CMSIS_DAP_ConfigureInterface(ifNo=%d) -> %d\n", ifNo, rc);

        // 回归测试：AGDI 传给 CMSIS_DAP_Connect 的第 2 个实参不是 int*，
        // 而是它保存的"选中项索引"（ifNo=0 等价于 NULL；非 0 就是非法指针）。
        // 本层必须自己挡下来，否则会向地址 1 写入 -> 0xc0000005。
        int *connOut = (int *)(size_t)ifNo;
        printf("  CMSIS_DAP_Connect(handle, (int*)%d) [mimics AGDI] ...\n", ifNo);
        fflush(stdout);
        rc = pDapConnect(h, connOut);
        printf("  CMSIS_DAP_Connect(ifNo=%d) -> %d  (survived the bogus pointer)\n", ifNo, rc);

        int v = 0;
        rc = pReadReg(h, 0, 0, &v);
        printf("  DAP_ReadReg(DPIDR) -> %d, 0x%08X\n", rc, v);
    }

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
