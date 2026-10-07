// jtagprobe.cpp - JTAG end-to-end probe (COMPAT_ANALYSIS 18.9)
//
// Purpose: verify the JTAG path from the command line, without Keil:
//   ConfigureInterface(Port=JTAG) -> DetectNumberOfDAPs -> IR lengths -> IDCODE -> DP power up
//
// Preconditions: the target board MUST have TDI/TDO physically wired (SWD-only boards
//   can never work; the log then stops at "scan returned no IDCODE (TDI/TDO unconnected?)").
//
// Build: powershell -File test\build_test.ps1 -Source jtagprobe.cpp
// Run  : bin\jtagprobe.exe            (board connected and powered)
//
// DIAGNOSTICS ADDED (hang hunting):
//   - unbuffered stdout + per-step timestamps (ms since start)
//   - a watchdog thread prints every 2 s WHICH stage is still running, so a hang
//     is immediately localized to one DLL call instead of looking "frozen"
//   - the DLL debug log level file is written (DEBUG) so internal HID/USB steps land in
//     %TEMP%\ORBMDK_RDDI.log; the path is printed at startup
//   - pass --keep to leave the DEBUG log level in place; by default it is restored at exit
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

typedef int RDDIHandle;
typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_Connect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_JTAG_GetIRLengths)(RDDIHandle, int*, uint8_t*);
typedef int (*PFN_CMSIS_DAP_JTAG_GetIDCODEs)(RDDIHandle, int*, uint32_t*);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, const int, const int, const int);

#define RID_DP_CTRLSTAT 1

// ---------------------------------------------------------------------------
// Stage tracking + watchdog
// ---------------------------------------------------------------------------
static const char* const kStages[] = {
    "startup",                       /* 0 */
    "LoadLibrary(DLL)",              /* 1 */
    "GetProcAddress(x9)",            /* 2 */
    "RDDI_Open",                     /* 3 */
    "ConfigureInterface(JTAG)",      /* 4 */
    "DetectNumberOfDAPs",            /* 5 */
    "JTAG_GetIRLengths",             /* 6 */
    "JTAG_GetIDCODEs",               /* 7 */
    "CMSIS_DAP_Connect",             /* 8 */
    "DAP_WriteReg(CTRL/STAT)",       /* 9 */
    "DAP_ReadReg(CTRL/STAT) poll",   /* 10 */
    "RDDI_Close",                    /* 11 */
    "done",                          /* 12 */
};
static const int kStageCount = (int)(sizeof(kStages) / sizeof(kStages[0]));

static volatile LONG      g_stage   = 0;
static volatile LONG      g_running = 1;
static ULONGLONG          g_t0      = 0;

static void Stamp(const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    fprintf(stdout, "[%8llu ms] %s\n", (unsigned long long)(GetTickCount64() - g_t0), body);
    fflush(stdout);
}

static void Stage(int idx, const char* fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    if (idx >= 0 && idx < kStageCount) {
        InterlockedExchange(&g_stage, idx);
    }
    fprintf(stdout, "[%8llu ms] =====> %s\n",
            (unsigned long long)(GetTickCount64() - g_t0), body);
    fflush(stdout);
}

static DWORD WINAPI Watchdog(LPVOID)
{
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        Sleep(2000);
        if (InterlockedCompareExchange(&g_running, 1, 1) != 1) break;
        const LONG s = InterlockedCompareExchange(&g_stage, 0, 0);
        const char* name = (s >= 0 && s < kStageCount) ? kStages[s] : "?";
        fprintf(stdout, "[%8llu ms] [WATCHDOG] still inside: %s   <-- if this repeats, the hang is HERE\n",
                (unsigned long long)(GetTickCount64() - g_t0), name);
        fflush(stdout);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DLL debug log helpers (%TEMP%\ORBMDK_LOG_LEVEL: 0=DEBUG .. 3=ERROR)
// ---------------------------------------------------------------------------
static bool TempPathJoin(char* out, size_t cap, const char* name)
{
    char tmp[MAX_PATH] = {0};
    const DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) return false;
    const int w = snprintf(out, cap, "%s%s", tmp, name);
    return w > 0 && (size_t)w < cap;
}

static void SetDllLogLevel(int level)   // -1 = delete file (restore default)
{
    char p[MAX_PATH] = {0};
    if (!TempPathJoin(p, sizeof(p), "ORBMDK_LOG_LEVEL")) return;
    if (level < 0) { DeleteFileA(p); return; }
    FILE* f = nullptr;
    if (fopen_s(&f, p, "w") == 0 && f) { fprintf(f, "%d\n", level); fclose(f); }
}

static void ShowDllLogTail(int maxLines)
{
    char p[MAX_PATH] = {0};
    if (!TempPathJoin(p, sizeof(p), "ORBMDK_RDDI.log")) return;
    FILE* f = nullptr;
    if (fopen_s(&f, p, "r") != 0 || !f) {
        Stamp("(no DLL log at %s)", p);
        return;
    }
    Stamp("---- tail of DLL log: %s ----", p);
    // keep the last maxLines lines using a small ring of offsets
    long offsets[64];
    int  count = 0;
    if (maxLines > 64) maxLines = 64;
    long pos = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        offsets[count % maxLines] = pos;
        ++count;
        pos = ftell(f);
    }
    if (count > 0) {
        const int start = (count > maxLines) ? (count % maxLines) : 0;
        long seekTo = (count > maxLines) ? offsets[start] : 0;
        fseek(f, seekTo, SEEK_SET);
        while (fgets(line, sizeof(line), f)) {
            size_t L = strlen(line);
            while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            Stamp("  | %s", line);
        }
    }
    fclose(f);
    Stamp("---- end DLL log ----");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // never buffer: a hang/crash still shows the last step
    g_t0 = GetTickCount64();

    const char* dllPath = "ORBMDK_RDDI.dll";
    bool keepLog = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(argv[i], "--keep") == 0) keepLog = true;
        } else if (argv[i] && argv[i][0]) {
            dllPath = argv[i];
        }
    }

    Stamp("== jtagprobe: DLL=%s  pid=%lu ==", dllPath, (unsigned long)GetCurrentProcessId());

    HANDLE wd = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);

    // Enable the DLL's DEBUG logging so HID/USB round-trips are visible if it hangs.
    SetDllLogLevel(0);
    char logPath[MAX_PATH] = {0};
    TempPathJoin(logPath, sizeof(logPath), "ORBMDK_RDDI.log");
    Stamp("DLL debug log enabled -> %s", logPath);

    int rc = 2;

    Stage(1, "LoadLibrary(%s)", dllPath);
    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) {
        Stage(1, "LoadLibrary FAILED (%s) err=%lu", dllPath, (unsigned long)GetLastError());
        rc = 1;
        goto cleanup;
    }
    Stage(1, "LoadLibrary OK (h=0x%p)", (void*)dll);

    PFN_RDDI_Open pOpen = (PFN_RDDI_Open)GetProcAddress(dll, "RDDI_Open");
    PFN_RDDI_Close pClose = (PFN_RDDI_Close)GetProcAddress(dll, "RDDI_Close");
    PFN_CMSIS_DAP_ConfigureInterface pCfg =
        (PFN_CMSIS_DAP_ConfigureInterface)GetProcAddress(dll, "CMSIS_DAP_ConfigureInterface");
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNum =
        (PFN_CMSIS_DAP_DetectNumberOfDAPs)GetProcAddress(dll, "CMSIS_DAP_DetectNumberOfDAPs");
    PFN_CMSIS_DAP_Connect pConn =
        (PFN_CMSIS_DAP_Connect)GetProcAddress(dll, "CMSIS_DAP_Connect");
    PFN_CMSIS_DAP_JTAG_GetIRLengths pIrl =
        (PFN_CMSIS_DAP_JTAG_GetIRLengths)GetProcAddress(dll, "CMSIS_DAP_JTAG_GetIRLengths");
    PFN_CMSIS_DAP_JTAG_GetIDCODEs pIds =
        (PFN_CMSIS_DAP_JTAG_GetIDCODEs)GetProcAddress(dll, "CMSIS_DAP_JTAG_GetIDCODEs");
    PFN_DAP_ReadReg pRd = (PFN_DAP_ReadReg)GetProcAddress(dll, "DAP_ReadReg");
    PFN_DAP_WriteReg pWr = (PFN_DAP_WriteReg)GetProcAddress(dll, "DAP_WriteReg");
    Stage(2, "GetProcAddress done: Open=%p Close=%p Cfg=%p Num=%p Conn=%p Irl=%p Ids=%p Rd=%p Wr=%p",
          (void*)pOpen, (void*)pClose, (void*)pCfg, (void*)pNum, (void*)pConn,
          (void*)pIrl, (void*)pIds, (void*)pRd, (void*)pWr);

    if (!pOpen || !pClose || !pCfg || !pNum || !pConn || !pRd || !pWr || !pIrl || !pIds) {
        Stamp("!! missing exports (need Open/Close/Cfg/Num/Conn/Irl/Ids/Rd/Wr) -> abort");
        rc = 1;
        goto cleanup;
    }

    RDDIHandle h = 0;
    Stage(3, "RDDI_Open ...");
    const int rcOpen = pOpen(&h, NULL);
    Stage(3, "RDDI_Open -> %d (h=%d)", rcOpen, h);

    char cfg[] = "Master=Y;Port=JTAG;SWJ=Y;Clock=1000000;";
    Stage(4, "CMSIS_DAP_ConfigureInterface(\"%s\") ...", cfg);
    const int rcCfg = pCfg(h, 0, cfg);
    Stage(4, "ConfigureInterface -> %d", rcCfg);

    int noOfDaps = 0;
    Stage(5, "DetectNumberOfDAPs ...");
    const int rcNum = pNum(h, &noOfDaps);
    Stage(5, "DetectNumberOfDAPs -> %d (count=%d)", rcNum, noOfDaps);

    int irCount = 0;
    uint8_t irLens[8] = {0};
    Stage(6, "JTAG_GetIRLengths ...");
    const int rcIrl = pIrl(h, &irCount, irLens);
    Stage(6, "JTAG_GetIRLengths -> %d (count=%d) ir = [%u %u %u %u %u %u %u %u]",
          rcIrl, irCount,
          (unsigned)irLens[0], (unsigned)irLens[1], (unsigned)irLens[2], (unsigned)irLens[3],
          (unsigned)irLens[4], (unsigned)irLens[5], (unsigned)irLens[6], (unsigned)irLens[7]);

    int idCount = 0;
    uint32_t ids[8] = {0};
    Stage(7, "JTAG_GetIDCODEs ...");
    const int rcIds = pIds(h, &idCount, ids);
    Stage(7, "JTAG_GetIDCODEs -> %d (count=%d) id = [0x%08X 0x%08X 0x%08X 0x%08X]",
          rcIds, idCount,
          (unsigned)ids[0], (unsigned)ids[1], (unsigned)ids[2], (unsigned)ids[3]);

    int mode = 0;
    Stage(8, "CMSIS_DAP_Connect ...");
    const int rcConn = pConn(h, &mode);
    Stage(8, "CMSIS_DAP_Connect -> %d (mode=%d, %s)",
          rcConn, mode, (mode == 2) ? "JTAG" : (mode == 1 || mode == 0) ? "SWD/other" : "unknown");

    int ctrl = 0;
    Stage(9, "DAP_WriteReg(CTRL/STAT <- 0x50000000) ...");
    const int rcWr = pWr(h, 0, RID_DP_CTRLSTAT, (int)0x50000000);
    Stage(9, "DAP_WriteReg -> %d", rcWr);

    Stage(10, "DAP_ReadReg(CTRL/STAT) poll (max 50 x 10 ms) ...");
    for (int i = 0; i < 50; ++i) {
        const int rcRd = pRd(h, 0, RID_DP_CTRLSTAT, &ctrl);
        if (i < 3 || (ctrl & 0xA0000000) == 0xA0000000) {
            Stamp("   poll[%02d] rc=%d ctrl=0x%08X", i, rcRd, (unsigned)ctrl);
        }
        if ((ctrl & 0xA0000000) == 0xA0000000) break;
        Sleep(10);
    }

    const bool powered = ((ctrl & 0xA0000000) == 0xA0000000);
    Stamp("DP CTRLSTAT (JTAG) -> 0x%08X  %s",
          (unsigned)ctrl, powered ? "[powered up]" : "[NO ACK / not powered]");
    rc = powered ? 0 : 2;

    // AP IDR 读取：RDDI 的最终用途（经 AHB-AP 访问内存）。
    // AP IDR 位于 AP 地址 0xFC：先写 DP SELECT=0x000000F0（APSEL=0, APBANKSEL=0xF）。
    Stage(11, "DP SELECT <- 0x000000F0, then read AP IDR (0xFC) ...");
    pWr(h, 0, 2 /*DP SELECT*/, (int)0x000000F0);
    {
        int v = 0;
        const int rcAp = pRd(h, 0, 7 /*AP bank0xF reg0xC = IDR*/, &v);
        Stamp("   AP_IDR rc=%d = 0x%08X (expect 0x24770011)", rcAp, (unsigned)v);
    }

    Stage(11, "RDDI_Close ...");
    pClose(h);
    Stage(11, "RDDI_Close done");

cleanup:
    Stage(12, "showing DLL log tail ...");
    ShowDllLogTail(25);

    InterlockedExchange(&g_running, 0);
    if (wd) {
        WaitForSingleObject(wd, 1500);
        CloseHandle(wd);
    }

    if (!keepLog) {
        SetDllLogLevel(-1);       // restore default (ERROR) for the user's Keil session
    } else {
        Stamp("(--keep: leaving ORBMDK_LOG_LEVEL=0 in %TEMP%)");
    }

    Stamp("== exit code %d ==", rc);
    return rc;
}
