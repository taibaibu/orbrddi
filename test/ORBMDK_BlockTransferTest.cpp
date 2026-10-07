/**
 * @file ORBMDK_BlockTransferTest.cpp
 * @brief Verification for ID_DAP_TRANSFER_BLOCK before enabling it in Keil.
 *
 * WHY THIS EXISTS
 * ---------------
 * DAP_RegWriteRepeat / DAP_RegReadRepeat are the path taken by the AGDI's
 * SWD_WriteBlock / SWD_VerifyBlock / SWD_ReadBlock, i.e. the flash download
 * hot path. They used to call DAP_Transfer once per word (one USB round trip
 * per word). Enabling block transfer made UV4 crash:
 *
 *     Application Error 0xc0000005, module "unknown", offset 0x00000000
 *     (typical signature of a call through a NULL pointer)
 *
 * Root cause: DAP_TransferBlock wrote past the caller's buffer.
 *     memcpy(readData, &resp[5], (respLen - 5) / 4 * 4);
 * HID always returns a whole report (64/65 bytes), so (respLen-5)/4 can be
 * one word MORE than the requested count. That function had zero call sites
 * before, so the defect had never been exercised.
 *
 * WHAT IT CHECKS
 * --------------
 *   1. Function: data written must read back identically.
 *   2. Overrun : the data array is wrapped in 8 canary words on each side;
 *                any corruption is reported immediately (targets the bug above).
 *   3. Boundary: word counts 1 / 13 / 14 / 15 / 28 / 64 -- covers the
 *                kMaxBlockWords = 14 chunk boundary.
 *   4. Speed   : runs the serial path and the block path in one process and
 *                prints the ratio.
 *
 * USAGE (target board connected and powered)
 * ------------------------------------------
 *     ORBMDK_BlockTransferTest.exe [ramAddress] [onlyWordCount]
 *     e.g.  ORBMDK_BlockTransferTest.exe 0x20000000
 *
 * The program sets ORBMDK_BLOCK_TRANSFER itself and reloads the DLL via
 * FreeLibrary/LoadLibrary so that the static cache inside BlockTransferEnabled()
 * is re-initialised. If the reload does not take effect (the two timings look
 * identical), run it twice manually instead:
 *     ORBMDK_BlockTransferTest.exe
 *     set ORBMDK_BLOCK_TRANSFER=1 && ORBMDK_BlockTransferTest.exe
 *
 * WARNING: this writes about 256 bytes to the given RAM address
 *          (default 0x20000000). Make sure the target does not depend on
 *          that memory region while the test runs.
 *
 * NOTE: this file is intentionally ASCII-only. MSVC warned C4819 (characters
 *       not representable in the current code page) when it contained Chinese
 *       text, which also produced a bogus C4474 printf warning.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "../include/ORBMDK_RDDI.h"

/* ==========================================================================
 * Function pointers (only the exports this test needs)
 * ========================================================================== */
typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_DAP_Connect)(RDDIHandle, RDDI_DAP_CONN_DETAILS*);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, int, int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, int, int, int);
typedef int (*PFN_DAP_RegReadRepeat)(RDDIHandle, int, int, int, int*);
typedef int (*PFN_DAP_RegWriteRepeat)(RDDIHandle, int, int, int, const int*);

/* --------------------------------------------------------------------------
 * ARM rddi_dap.h register IDs -- these are IDs, NOT byte offsets.
 *     DAP_REG_DP_0x8 = 2  -> DP SELECT
 *     DAP_REG_AP_0x0 = 4  -> AP CSW
 *     DAP_REG_AP_0x4 = 5  -> AP TAR
 *     DAP_REG_AP_0xC = 7  -> AP DRW
 *     DAP_REG_RnW    = 0x10000
 *
 * Careful: the same-named enum in ORBMDK_RDDI.h uses byte offsets
 * (e.g. DAP_REG_AP_DRW = 0x0C), which do NOT match the low-16-bit decoding
 * convention of GetRegId() in ORBMDK_RDDI.cpp. This test passes the official
 * ARM IDs.
 * -------------------------------------------------------------------------- */
#define RID_DP_SELECT   2
#define RID_AP_CSW      4
#define RID_AP_TAR      5
#define RID_AP_DRW      7
#define REG_RNW         0x10000

/* CSW: 32-bit width + address auto-increment (block transfer relies on it).
 * Same value commonly used by OpenOCD / Keil. */
#define CSW_32BIT_INC   0x23000052

#define GUARD_WORDS     8
#define MAX_WORDS       64
#define CANARY          0xA5A5A5A5u

static int g_Passed = 0;
static int g_Failed = 0;

/* Path relative to the test exe -- build_test.ps1 puts both the exe and the
 * DLL in bin\. LoadLibraryA searches the exe's own directory first, so no
 * machine-specific absolute path is needed here. argv[1] is the RAM address
 * for this test, hence no argv override. */
static const char* g_DllPath = "ORBMDK_RDDI.dll";

/* ==========================================================================
 * API table
 * ========================================================================== */
struct Api {
    HMODULE                            mod;
    PFN_RDDI_Open                      Open;
    PFN_RDDI_Close                     Close;
    PFN_CMSIS_DAP_ConfigureInterface   ConfigureInterface;
    PFN_DAP_Connect                    Connect;
    PFN_DAP_ReadReg                    ReadReg;
    PFN_DAP_WriteReg                   WriteReg;
    PFN_DAP_RegReadRepeat              ReadRepeat;
    PFN_DAP_RegWriteRepeat             WriteRepeat;
};

static bool ApiLoad(Api* a)
{
    memset(a, 0, sizeof(*a));
    a->mod = LoadLibraryA(g_DllPath);
    if (!a->mod) {
        printf("FATAL: LoadLibrary failed, err=%lu\n", (unsigned long)GetLastError());
        return false;
    }
#define BIND(field, name)                                                  \
    a->field = (PFN_##name)GetProcAddress(a->mod, #name);                  \
    if (!a->field) { printf("FATAL: export %s missing\n", #name); return false; }
    BIND(Open, RDDI_Open)
    BIND(Close, RDDI_Close)
    BIND(ConfigureInterface, CMSIS_DAP_ConfigureInterface)
    BIND(Connect, DAP_Connect)
    BIND(ReadReg, DAP_ReadReg)
    BIND(WriteReg, DAP_WriteReg)
    BIND(ReadRepeat, DAP_RegReadRepeat)
    BIND(WriteRepeat, DAP_RegWriteRepeat)
#undef BIND
    return true;
}

/* ==========================================================================
 * Buffer wrapped in canaries
 * ========================================================================== */
struct Guarded {
    uint32_t raw[GUARD_WORDS + MAX_WORDS + GUARD_WORDS];
    uint32_t* data() { return raw + GUARD_WORDS; }

    void fill(uint32_t v) {
        for (int i = 0; i < (int)(sizeof(raw) / sizeof(raw[0])); i++) raw[i] = v;
    }
    void clearData() {
        for (int i = 0; i < MAX_WORDS; i++) data()[i] = 0;
    }
    /* Returns the number of corrupted canary words (0 = intact). */
    int guardBroken(const char* tag) const {
        int bad = 0;
        for (int i = 0; i < GUARD_WORDS; i++) {
            if (raw[i] != CANARY) bad++;
            if (raw[GUARD_WORDS + MAX_WORDS + i] != CANARY) bad++;
        }
        if (bad) {
            printf("      !! %s: %d canary word(s) corrupted -> out-of-bounds write\n",
                   tag, bad);
        }
        return bad;
    }
};

/* ==========================================================================
 * Prepare the AP: select bank, set CSW, set TAR
 * ========================================================================== */
static bool PrepareAP(const Api& a, RDDIHandle h, uint32_t addr, bool verbose)
{
    if (a.WriteReg(h, 0, RID_DP_SELECT, 0) != RDDI_SUCCESS) {
        printf("      DP_SELECT write failed\n");
        return false;
    }
    if (a.WriteReg(h, 0, RID_AP_CSW, CSW_32BIT_INC) != RDDI_SUCCESS) {
        printf("      CSW write failed\n");
        return false;
    }
    int csw = 0;
    if (a.ReadReg(h, 0, RID_AP_CSW | REG_RNW, &csw) != RDDI_SUCCESS) {
        printf("      CSW read failed\n");
        return false;
    }
    if (verbose) {
        printf("      CSW readback = 0x%08X (expected 0x%08X)%s\n",
               (unsigned)csw, (unsigned)CSW_32BIT_INC,
               ((uint32_t)csw == CSW_32BIT_INC) ? "" : "  <- mismatch, target may be absent");
    }
    if (a.WriteReg(h, 0, RID_AP_TAR, (int)addr) != RDDI_SUCCESS) {
        printf("      TAR write failed\n");
        return false;
    }
    return true;
}

/* ==========================================================================
 * One case: write -> read back -> compare + canary check.
 * Returns elapsed microseconds, or -1 on failure.
 * ========================================================================== */
static double RunCase(const Api& a, RDDIHandle h, uint32_t addr, int n, bool verbose)
{
    Guarded wr, rd;
    wr.fill(CANARY);
    rd.fill(CANARY);

    for (int i = 0; i < n; i++) {
        wr.data()[i] = 0x10000000u + (uint32_t)i;
    }
    rd.clearData();

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    /* ---- write ---- */
    if (!PrepareAP(a, h, addr, verbose)) return -1.0;
    QueryPerformanceCounter(&t0);
    const int wst = a.WriteRepeat(h, 0, n, RID_AP_DRW, (const int*)wr.data());
    QueryPerformanceCounter(&t1);
    if (wst != RDDI_SUCCESS) {
        printf("      write failed: result=%d (0x%X)\n", wst, wst);
        return -1.0;
    }
    const double wUs = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)freq.QuadPart;

    /* ---- read back ---- */
    if (!PrepareAP(a, h, addr, false)) return -1.0;
    QueryPerformanceCounter(&t0);
    const int rst = a.ReadRepeat(h, 0, n, RID_AP_DRW | REG_RNW, (int*)rd.data());
    QueryPerformanceCounter(&t1);
    if (rst != RDDI_SUCCESS) {
        printf("      read failed: result=%d (0x%X)\n", rst, rst);
        return -1.0;
    }
    const double rUs = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)freq.QuadPart;

    /* ---- verify ---- */
    int mismatch = -1;
    for (int i = 0; i < n; i++) {
        if (rd.data()[i] != wr.data()[i]) {
            mismatch = i;
            break;
        }
    }
    const int badGuard = wr.guardBroken("write buffer") + rd.guardBroken("read buffer");

    if (mismatch >= 0) {
        printf("      FAIL: data mismatch at %d  wrote=0x%08X read=0x%08X\n",
               mismatch, (unsigned)wr.data()[mismatch], (unsigned)rd.data()[mismatch]);
        g_Failed++;
    } else if (badGuard) {
        printf("      FAIL: data correct but buffer overrun detected\n");
        g_Failed++;
    } else {
        printf("      PASS  n=%-3d write %8.1f us   read %8.1f us\n", n, wUs, rUs);
        g_Passed++;
    }

    return wUs + rUs;
}

/* ==========================================================================
 * One full round: open -> configure -> connect -> run all cases.
 * Returns total microseconds, or -1 on failure.
 * ========================================================================== */
static double RunRound(const char* tag, bool blockMode, uint32_t addr,
                       const int* sizes, int nSizes)
{
    printf("\n================================================================\n");
    printf("  mode: %s   (ORBMDK_BLOCK_TRANSFER=%s)\n",
           tag, blockMode ? "1" : "unset");
    printf("================================================================\n");

    Api a;
    if (!ApiLoad(&a)) {
        g_Failed++;
        return -1.0;
    }

    RDDIHandle h = 0;
    if (a.Open(&h, NULL) != RDDI_SUCCESS || h == 0) {
        printf("  RDDI_Open failed (is the debugger plugged in?)\n");
        FreeLibrary(a.mod);
        g_Failed++;
        return -1.0;
    }
    printf("  RDDI_Open -> handle=%d\n", h);

    char cfg[] = "Master=Y;Port=SW;SWJ=Y;Clock=1000000;";
    const int cst = a.ConfigureInterface(h, 0, cfg);
    printf("  ConfigureInterface -> %d\n", cst);

    RDDI_DAP_CONN_DETAILS cd;
    memset(&cd, 0, sizeof(cd));
    const int conn = a.Connect(h, &cd);
    printf("  DAP_Connect -> %d  implementor='%s'\n", conn, cd.implementorName);

    double total = 0.0;
    bool ok = (cst == RDDI_SUCCESS && conn == RDDI_SUCCESS);

    if (ok) {
        for (int i = 0; i < nSizes; i++) {
            const double us = RunCase(a, h, addr, sizes[i], true);
            if (us < 0) {
                ok = false;
                break;
            }
            total += us;
        }
    }

    a.Close(h);
    FreeLibrary(a.mod);

    if (!ok) {
        printf("  >> this mode did NOT pass\n");
        return -1.0;
    }
    printf("  >> this mode passed, total %.1f us\n", total);
    return total;
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(int argc, char* argv[])
{
    printf("================================================================\n");
    printf("  ORBMDK Block Transfer Test  (DAP_RegWriteRepeat/ReadRepeat)\n");
    printf("================================================================\n");

    printf("DLL: %s\n", g_DllPath);

    uint32_t addr = 0x20000000u;
    if (argc > 1 && argv[1] && argv[1][0]) {
        addr = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    printf("RAM address = 0x%08X, writing about %d bytes\n",
           (unsigned)addr, MAX_WORDS * 4);
    printf("WARNING: this overwrites that memory region\n");

    /* Boundary set covering kMaxBlockWords = 14 */
    int sizes[6] = { 1, 13, 14, 15, 28, 64 };
    int nSizes = 6;
    if (argc > 2 && argv[2] && argv[2][0]) {
        sizes[0] = atoi(argv[2]);
        nSizes = 1;
        printf("only testing n=%d\n", sizes[0]);
    }

    /* ---- round 1: serial (baseline) ----
     * ORBMDK_BLOCK_TRANSFER must be "0" (explicitly disabled). An empty value
     * counts as "not set", which now means "enabled, decide by auto probe". */
    _putenv_s("ORBMDK_BLOCK_TRANSFER", "0");
    const double tSerial = RunRound("serial transfer (baseline)", false,
                                    addr, sizes, nSizes);

    /* ---- round 2: block transfer ----
     * FreeLibrary/LoadLibrary reloads the DLL so the static cache inside
     * BlockTransferEnabled() is re-initialised, allowing both modes to be
     * measured in one process. */
    _putenv_s("ORBMDK_BLOCK_TRANSFER", "1");
    const double tBlock = RunRound("block transfer", true, addr, sizes, nSizes);

    /* ---- summary ---- */
    printf("\n================================================================\n");
    printf("  summary\n");
    printf("================================================================\n");
    printf("  cases passed %d, failed %d\n", g_Passed, g_Failed);
    if (tSerial > 0 && tBlock > 0) {
        printf("  serial total %.1f us\n", tSerial);
        printf("  block  total %.1f us\n", tBlock);
        printf("  speedup      %.2fx\n", tSerial / tBlock);
        if (tSerial / tBlock < 1.5) {
            printf("  NOTE: low speedup. Either the firmware does not implement\n");
            printf("        ID_DAP_TRANSFER_BLOCK (it fell back automatically),\n");
            printf("        or the in-process DLL reload did not take effect.\n");
            printf("        Try running the two modes manually instead.\n");
        }
    } else {
        printf("  NOTE: one of the modes did not complete, cannot compare speed\n");
    }
    printf("================================================================\n");

    return g_Failed == 0 ? 0 : 1;
}
