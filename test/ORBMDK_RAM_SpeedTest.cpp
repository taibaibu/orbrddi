/**
 * @file ORBMDK_RAM_SpeedTest.cpp
 * @brief RAM read/write THROUGHPUT test through the RDDI DLL -- no Keil / no AGDI.
 *
 * WHY THIS EXISTS
 * ---------------
 * Keil (AGDI) sits between the user and this DLL: it adds its own per-call
 * overhead and it interleaves register accesses with the block transfers. So a
 * Keil session can NEVER answer "what is the DEVICE limit?".
 *
 * This tool drives DAP_RegWriteRepeat / DAP_RegReadRepeat directly -- the exact
 * path the flash download uses -- with nothing in the loop but this DLL, the
 * probe firmware and the target. The numbers it prints ARE the device limit.
 *
 * WHAT IT DOES
 * ------------
 *   1. open probe -> configure SWD -> DAP_Connect -> CMSIS_DAP_Connect;
 *   2. power up the DP (CTRL/STAT), then HALT the core. Halting matters: while
 *      the target runs, its own program keeps rewriting RAM near 0x20000000, so
 *      "read back what we wrote" is meaningless (measured: read-back was the
 *      running program's data, not ours);
 *   3. sweep  : a few buffer sizes -> throughput as a function of size;
 *   4. steady : N rounds at the requested size -> sustained rate = the limit;
 *   5. serial : the same first words written ONE WORD PER CALL (one USB round
 *      trip per word) -> per-round-trip baseline + block-transfer speedup.
 *
 * Throughput uses 1 kB = 1024 B, same convention as the DLL's TESTSPEED meter,
 * so these numbers are directly comparable with %TEMP%\ORBMDK_RDDI.log.
 *
 * USAGE  (target board connected and powered)
 * -------------------------------------------
 *     ORBMDK_RAM_SpeedTest.exe [ramAddr] [bytes] [rounds] [clockHz] [dllPath] [windowWords]
 *     defaults: ramAddr=0x20000000  bytes=4096  rounds=8
 *               clockHz=1000000     dll=ORBMDK_RDDI.dll
 *               windowWords=1024    (1024 words = one 4 KB TAR block. Splits by
 *                                    max_tar_block_size() and rewrites TAR on each
 *                                    block boundary = 方案 3, bug.md B7.
 *                                    0 = NO split: one Repeat() call for the whole
 *                                    buffer = reproduces the B7 defect; A/B switch)
 *
 * READ THE CLOCK ARGUMENT
 * -----------------------
 * Block-transfer throughput scales with the SWD clock, and at low clocks the
 * SWD shift time -- not USB -- is the limit: 500 B = 4000 bits, so 1 MHz alone
 * costs ~4 ms per block. Keil configures 10 MHz on this rig, which is why a
 * Keil flash session reaches ~565 kB/s while a 1 MHz run of this tool reaches
 * ~74 kB/s. Sweep it (1 MHz .. 10 MHz .. 20 MHz) to find the knee, then compare
 * against the USB-only ceiling. Raise it only as far as the wiring is stable:
 * a marginal clock corrupts data silently (the verify will catch it here, but
 * Keil would not).
 *
 * WARNING: this OVERWRITES [ramAddr, ramAddr+bytes). The core is left HALTED
 *          when the tool exits -- reset the target (or power cycle) before
 *          running a real program again.
 *
 * NOTE: ASCII-only on purpose (same reason as ORBMDK_BlockTransferTest.cpp):
 *       keeps the console output readable on a GBK code page.
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
typedef int (*PFN_CMSIS_DAP_Connect)(RDDIHandle, int*);
typedef int (*PFN_DAP_Connect)(RDDIHandle, RDDI_DAP_CONN_DETAILS*);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, int, int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, int, int, int);
typedef int (*PFN_DAP_RegReadRepeat)(RDDIHandle, int, int, int, int*);
typedef int (*PFN_DAP_RegWriteRepeat)(RDDIHandle, int, int, int, const int*);

/* --------------------------------------------------------------------------
 * ARM rddi_dap.h register IDs -- these are IDs, NOT byte offsets.
 *     DAP_REG_DP_0x8 = 2 -> DP SELECT
 *     DAP_REG_AP_0x0 = 4 -> AP CSW
 *     DAP_REG_AP_0x4 = 5 -> AP TAR
 *     DAP_REG_AP_0xC = 7 -> AP DRW
 *     DAP_REG_RnW    = 0x10000
 * (the same-named enum in ORBMDK_RDDI.h uses byte offsets and does NOT match
 *  the low-16-bit decoding of GetRegId() in ORBMDK_RDDI.cpp -- pass these IDs)
 * -------------------------------------------------------------------------- */
#define RID_DP_CTRLSTAT 1
#define RID_DP_SELECT   2
#define RID_AP_CSW      4
#define RID_AP_TAR      5
#define RID_AP_DRW      7
#define REG_RNW         0x10000

/* CSW: 32-bit width + address auto-increment (block transfer relies on it). */
#define CSW_32BIT_INC   0x23000052

/* DHCSR: DBGKEY | C_DEBUGEN | C_HALT */
#define DHCSR_ADDR      0xE000EDF0u
#define DHCSR_HALT      0xA05F0003

#define GUARD_WORDS     8
#define CANARY          0xA5A5A5A5u

/* Sweep ladder (words); filtered to <= requested size. */
static const int kLadder[] = { 16, 64, 256, 1024 };
#define LADDER_N ((int)(sizeof(kLadder)/sizeof(kLadder[0])))

/* Serial baseline length (words, one USB round trip per word). */
#define SERIAL_WORDS    64

static int g_Failed = 0;

static const char* g_DllPath = "ORBMDK_RDDI.dll";

/* ==========================================================================
 * helpers
 * ========================================================================== */
static double NowUs(void)
{
    LARGE_INTEGER f, c;
    static LARGE_INTEGER sFreq = {0};
    if (sFreq.QuadPart == 0) QueryPerformanceFrequency(&sFreq);
    f = sFreq;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

static double KBps(size_t bytes, double us)
{
    if (us <= 0.0) return 0.0;
    return (double)bytes * 1e6 / us / 1024.0;     /* 1 kB = 1024 B */
}

struct Buf {
    uint32_t* raw;      /* GUARD + n + GUARD words */
    int       n;
    uint32_t*       data()       { return raw + GUARD_WORDS; }
    const uint32_t* data() const { return raw + GUARD_WORDS; }
};

static bool BufAlloc(Buf* b, int nWords)
{
    b->n = nWords;
    b->raw = (uint32_t*)malloc((size_t)(GUARD_WORDS + nWords + GUARD_WORDS) * 4);
    if (!b->raw) { printf("FATAL: out of memory\n"); return false; }
    for (int i = 0; i < GUARD_WORDS + nWords + GUARD_WORDS; i++) b->raw[i] = CANARY;
    return true;
}
static void BufFree(Buf* b) { free(b->raw); b->raw = NULL; }

/* Returns the number of corrupted canary words (0 = intact). */
static int BufGuardBroken(const Buf* b, const char* tag)
{
    int bad = 0;
    for (int i = 0; i < GUARD_WORDS; i++) {
        if (b->raw[i] != CANARY) bad++;
        if (b->raw[GUARD_WORDS + b->n + i] != CANARY) bad++;
    }
    if (bad) printf("      !! %s: %d canary word(s) corrupted -> out-of-bounds access\n",
                    tag, bad);
    return bad;
}

struct Api {
    HMODULE                            mod;
    PFN_RDDI_Open                      Open;
    PFN_RDDI_Close                     Close;
    PFN_CMSIS_DAP_ConfigureInterface   ConfigureInterface;
    PFN_CMSIS_DAP_Connect              CmsisConnect;
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
    BIND(CmsisConnect, CMSIS_DAP_Connect)
    BIND(Connect, DAP_Connect)
    BIND(ReadReg, DAP_ReadReg)
    BIND(WriteReg, DAP_WriteReg)
    BIND(ReadRepeat, DAP_RegReadRepeat)
    BIND(WriteRepeat, DAP_RegWriteRepeat)
#undef BIND
    return true;
}

/* Select bank, set CSW to 32-bit + auto-increment, point TAR at addr.
 * CSW uses AddrInc=single, so TAR must be re-armed per TAR BLOCK (see below).
 *
 * ★ The auto-increment is NOT unbounded -- it is limited to one TAR block.
 *   Public basis: OpenOCD src/target/arm_adi_v5.c
 *     L85   "ARM ADI Specification requires at least 10 bits used for TAR autoincrement"
 *     L91   max_tar_block_size(block, addr) = block - ((block - 1) & addr)
 *     L191  mem_ap_update_tar_cache(): crossing the block sets tar_valid = false,
 *           so the NEXT access rewrites TAR
 *     L1256 ap->tar_autoincr_block = (1 << 10)     (OpenOCD's default 1 KB block)
 *   The spec only demands AT LEAST 10 bits; the block size is a property of the
 *   AP. Measured on THIS chain (STM32F407RET6 + CMSIS-DAP v2, 2026-10-03) it is
 *   12 bit = 4096 B: writing 2048 words from 0x20000000 leaves pattern[1024] AT
 *   0x20000000, and 8192 words leave pattern[7168] (= last index 0 mod 1024).
 *   The read folds identically, so the first collision surfaces at word 0 -- NOT
 *   at word 1024, which is what a read-side fold would look like.
 *   Recorded as bug.md B7 (+ appendix A6). If a target with a smaller block
 *   (down to the 1 KB lower bound) ever turns up, lower kTarBlockBytes; B7 has
 *   the repro command.
 *
 *   The DLL cannot do this itself: DAP_RegWriteRepeat is handed the AP register
 *   id, never the address -- so 方案 3 lives here, in the test path. */
static constexpr uint32_t kTarBlockBytes = 4096;   /* measured 12-bit TAR block */

/* max_tar_block_size(): bytes from addr up to the next TAR block boundary. */
static uint32_t TarBlockRemain(uint32_t addr)
{
    return kTarBlockBytes - (addr & (kTarBlockBytes - 1u));
}

/* 方案 3: arm TAR ONLY -- no SELECT/CSW rewrite, no readback, no waiting.
 * "default OK": the write is assumed to succeed and the transfer carries on. */
static bool ArmTar(const Api& a, RDDIHandle h, uint32_t addr)
{
    return a.WriteReg(h, 0, RID_AP_TAR, (int)addr) == RDDI_SUCCESS;
}

static bool PrepareAP(const Api& a, RDDIHandle h, uint32_t addr, bool showCsw)
{
    if (a.WriteReg(h, 0, RID_DP_SELECT, 0) != RDDI_SUCCESS) return false;
    if (a.WriteReg(h, 0, RID_AP_CSW, CSW_32BIT_INC) != RDDI_SUCCESS) return false;
    if (showCsw) {
        int csw = 0;
        if (a.ReadReg(h, 0, RID_AP_CSW | REG_RNW, &csw) == RDDI_SUCCESS) {
            const uint32_t v = (uint32_t)csw;
            static const char* kSz[8]  = { "8-bit", "16-bit", "32-bit", "64-bit",
                                           "128-bit", "256-bit", "resv", "resv" };
            static const char* kInc[4] = { "off", "single", "packed", "resv" };
            printf("  CSW readback = 0x%08X (expected 0x%08X)%s\n",
                   (unsigned)v, (unsigned)CSW_32BIT_INC,
                   (v == CSW_32BIT_INC) ? "" : "  <- mismatch, target may be absent");
            /* Decode the two fields that govern a block transfer: access Size and
             * AddrInc. If either were wrong, the 1024-word ladder step could not
             * land 1024 consecutive 32-bit words at 4-byte stride -- it does, with
             * every word verified, so both are accepted by the AP as intended.
             * Type/Bits[31:24] are echoed here because a real AHB-AP would report
             * Type=1 and reserved=0; this dump is how that gets noticed. */
            printf("    Size=%s  AddrInc=%s  DeviceEn=%u  Type=%s  Prot=0x%02X  [31:24]=0x%02X\n",
                   kSz[v & 7u], kInc[(v >> 4) & 3u], (v >> 6) & 1u,
                   ((v >> 8) & 1u) ? "AHB" : "APB", (v >> 16) & 0xFFu, (v >> 24) & 0xFFu);
        } else {
            printf("  CSW read failed\n");
            return false;
        }
    }
    if (a.WriteReg(h, 0, RID_AP_TAR, (int)addr) != RDDI_SUCCESS) return false;
    return true;
}

/* ==========================================================================
 * one timed write + read of n words, with verify
 *
 * 方案 3 (bug.md B7): split by max_tar_block_size(), and when a chunk lands on a
 * TAR block boundary, write TAR (ArmTar) and carry straight on with the next
 * block -- no waiting, no readback, no error path (TAR writes are assumed OK).
 * Inside a block the AP keeps auto-incrementing, so nothing else changes.
 * The TAR write itself is NOT counted into wUs/rUs -- those stay "pure transfer"
 * so the numbers remain comparable with the historical COMPAT_ANALYSIS 21.1.
 *
 * g_WindowWords is the A/B switch (6th CLI arg, default = one full block):
 *     windowWords > 0  -> handle the block boundary (方案 3)
 *     windowWords == 0 -> ONE Repeat() call for the whole buffer = exactly what
 *                         the DLL does internally, i.e. the defect under test
 * ========================================================================== */
struct Rate { double wUs, rUs; bool ok; };

static int g_WindowWords = (int)(kTarBlockBytes / 4u);   /* 1024 words = 1 block */

static Rate Measure(const Api& a, RDDIHandle h, uint32_t addr,
                    const Buf& wr, Buf& rd, bool verbose)
{
    Rate r; r.wUs = 0.0; r.rUs = 0.0; r.ok = false;

    if (!PrepareAP(a, h, addr, false)) { printf("      PrepareAP(write) failed\n"); return r; }
    double wSum = 0.0;
    for (int off = 0; off < wr.n; ) {
        const uint32_t wa = addr + (uint32_t)off * 4u;
        int cnt = wr.n - off;
        if (g_WindowWords > 0) {
            const int byBlock = (int)(TarBlockRemain(wa) / 4u);   /* never cross a block */
            if (cnt > byBlock) cnt = byBlock;
            if (cnt > g_WindowWords) cnt = g_WindowWords;
            /* Exactly on a block boundary, and not the caller-armed first block:
             * 方案 3 -- rewrite TAR, then carry on immediately. */
            if (off != 0 && (wa & (kTarBlockBytes - 1u)) == 0u && !ArmTar(a, h, wa)) {
                printf("      ArmTar(write +%d) failed\n", off);
                return r;
            }
        }
        const double t1 = NowUs();
        const int wst = a.WriteRepeat(h, 0, cnt, RID_AP_DRW, (const int*)wr.data() + off);
        wSum += NowUs() - t1;
        if (wst != RDDI_SUCCESS) {
            printf("      write failed: result=%d (0x%X) at word %d\n", wst, wst, off);
            return r;
        }
        off += cnt;
    }
    r.wUs = wSum;

    for (int i = 0; i < rd.n; i++) rd.data()[i] = 0;
    if (!PrepareAP(a, h, addr, false)) { printf("      PrepareAP(read) failed\n"); return r; }
    double rSum = 0.0;
    for (int off = 0; off < rd.n; ) {
        const uint32_t ra = addr + (uint32_t)off * 4u;
        int cnt = rd.n - off;
        if (g_WindowWords > 0) {
            const int byBlock = (int)(TarBlockRemain(ra) / 4u);   /* never cross a block */
            if (cnt > byBlock) cnt = byBlock;
            if (cnt > g_WindowWords) cnt = g_WindowWords;
            /* Exactly on a block boundary, and not the caller-armed first block:
             * 方案 3 -- rewrite TAR, then carry on immediately. */
            if (off != 0 && (ra & (kTarBlockBytes - 1u)) == 0u && !ArmTar(a, h, ra)) {
                printf("      ArmTar(read +%d) failed\n", off);
                return r;
            }
        }
        const double t1 = NowUs();
        const int rst = a.ReadRepeat(h, 0, cnt, RID_AP_DRW | REG_RNW, (int*)rd.data() + off);
        rSum += NowUs() - t1;
        if (rst != RDDI_SUCCESS) {
            printf("      read failed: result=%d (0x%X) at word %d\n", rst, rst, off);
            return r;
        }
        off += cnt;
    }
    r.rUs = rSum;

    int mismatch = -1;
    for (int i = 0; i < wr.n; i++) {
        if (rd.data()[i] != wr.data()[i]) { mismatch = i; break; }
    }
    const int badGuard = BufGuardBroken(&wr, "write buffer") + BufGuardBroken(&rd, "read buffer");

    if (mismatch >= 0) {
        printf("      FAIL: data mismatch at word %d  wrote=0x%08X read=0x%08X\n",
               mismatch, (unsigned)wr.data()[mismatch], (unsigned)rd.data()[mismatch]);
        g_Failed++;
        return r;
    }
    if (badGuard) {
        printf("      FAIL: data correct but buffer overrun detected\n");
        g_Failed++;
        return r;
    }

    r.ok = true;
    if (verbose) {
        printf("  %6d %7u   %9.1f %9.1f   %10.1f %10.1f\n",
               wr.n, (unsigned)wr.n * 4, r.wUs, r.rUs,
               KBps((size_t)wr.n * 4, r.wUs), KBps((size_t)rd.n * 4, r.rUs));
    }
    return r;
}

/* Serial baseline: one word per call = one USB round trip per word.
 * CSW AddrInc=single makes TAR auto-increment, so TAR is armed once -- valid
 * only while nWords stays inside one TAR block (kTarBlockBytes / 4 words).
 * Beyond that the caller must use the 方案 3 chunking above (bug.md B7). */
static void SerialBaseline(const Api& a, RDDIHandle h, uint32_t addr,
                           const Buf& wr, int nWords, double* wKBps, double* rKBps)
{
    printf("\n== serial baseline: %d words, ONE WORD PER CALL ==\n", nWords);
    printf("  (each call = one USB round trip; this is what the DLL does when the\n");
    printf("   firmware has no block transfer, and the 'before' number for the ratio)\n");

    if (!PrepareAP(a, h, addr, false)) { g_Failed++; return; }
    const double w0 = NowUs();
    for (int i = 0; i < nWords; i++) {
        if (a.WriteRepeat(h, 0, 1, RID_AP_DRW, (const int*)&wr.data()[i]) != RDDI_SUCCESS) {
            printf("      serial write failed at word %d\n", i);
            g_Failed++;
            return;
        }
    }
    const double wUs = NowUs() - w0;

    int tmp = 0;
    if (!PrepareAP(a, h, addr, false)) { g_Failed++; return; }
    const double r0 = NowUs();
    for (int i = 0; i < nWords; i++) {
        if (a.ReadRepeat(h, 0, 1, RID_AP_DRW | REG_RNW, &tmp) != RDDI_SUCCESS) {
            printf("      serial read failed at word %d\n", i);
            g_Failed++;
            return;
        }
    }
    const double rUs = NowUs() - r0;

    *wKBps = KBps((size_t)nWords * 4, wUs);
    *rKBps = KBps((size_t)nWords * 4, rUs);
    printf("  write %8.1f us (%8.1f us/word) -> %8.1f kB/s\n",
           wUs, wUs / nWords, *wKBps);
    printf("  read  %8.1f us (%8.1f us/word) -> %8.1f kB/s\n",
           rUs, rUs / nWords, *rKBps);
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(int argc, char* argv[])
{
    printf("================================================================\n");
    printf("  ORBMDK RAM Read/Write SPEED Test\n");
    printf("  (DAP_RegWriteRepeat / DAP_RegReadRepeat -- NO Keil, NO AGDI)\n");
    printf("================================================================\n");

    uint32_t addr    = 0x20000000u;
    long     bytes   = 4096;
    int      rounds  = 8;
    long     clockHz = 1000000;
    if (argc > 1 && argv[1] && argv[1][0]) addr    = (uint32_t)strtoul(argv[1], NULL, 0);
    if (argc > 2 && argv[2] && argv[2][0]) bytes   = strtol(argv[2], NULL, 0);
    if (argc > 3 && argv[3] && argv[3][0]) rounds  = atoi(argv[3]);
    if (argc > 4 && argv[4] && argv[4][0]) clockHz = strtol(argv[4], NULL, 0);
    if (argc > 5 && argv[5] && argv[5][0]) g_DllPath = argv[5];
    if (argc > 6 && argv[6] && argv[6][0]) g_WindowWords = atoi(argv[6]);
    if (g_WindowWords < 0) g_WindowWords = 0;
    if (bytes < 4)    bytes = 4;
    if (rounds < 1)   rounds = 1;
    if (clockHz < 1)  clockHz = 1000000;
    const int totalWords = (int)(bytes / 4);

    printf("DLL        : %s\n", g_DllPath);
    printf("RAM addr   : 0x%08X\n", (unsigned)addr);
    printf("buffer     : %ld bytes (%d words)\n", bytes, totalWords);
    printf("steady     : %d round(s)\n", rounds);
    if (g_WindowWords > 0)
        printf("TAR block  : %u B -- TAR rewritten on each block boundary "
               "(方案 3, bug.md B7); chunk cap = %d words\n",
               (unsigned)kTarBlockBytes, g_WindowWords);
    else
        printf("TAR block  : OFF -- ONE Repeat() call for the whole buffer "
               "(reproduces the B7 defect)\n");
    printf("SWD clock  : %ld Hz   <- block throughput scales with this:\n", clockHz);
    printf("             500 B = 4000 bit, so 1 MHz alone caps one block at ~4 ms\n");
    printf("WARNING    : this OVERWRITES [0x%08X, 0x%08X)\n",
           (unsigned)addr, (unsigned)(addr + (uint32_t)bytes));
    printf("             the core is HALTED first and left halted on exit\n\n");

    Api a;
    if (!ApiLoad(&a)) return 1;

    RDDIHandle h = 0;
    if (a.Open(&h, NULL) != RDDI_SUCCESS || h == 0) {
        printf("FATAL: RDDI_Open failed (is the debugger plugged in?)\n");
        FreeLibrary(a.mod);
        return 1;
    }
    printf("  RDDI_Open -> handle=%d\n", h);

    char cfg[128];
    snprintf(cfg, sizeof(cfg), "Master=Y;Port=SW;SWJ=Y;Clock=%ld;", clockHz);
    const int cst = a.ConfigureInterface(h, 0, cfg);
    printf("  ConfigureInterface -> %d   (%s)\n", cst, cfg);

    /* Order is fixed by ARM rddi_dap.h: ConfigureInterface -> DAP_Connect ->
     * CMSIS_DAP_Connect (see ORBMDK_BlockTransferTest.cpp for why). */
    RDDI_DAP_CONN_DETAILS cd;
    memset(&cd, 0, sizeof(cd));
    const int conn = a.Connect(h, &cd);
    printf("  DAP_Connect -> %d  implementor='%s'\n", conn, cd.implementorName);

    int mode = 0;
    const int cconn = a.CmsisConnect(h, &mode);
    printf("  CMSIS_DAP_Connect -> %d (mode=%d)\n", cconn, mode);

    /* Cortex-M power-up: CDBGPWRUPREQ | CSYSPWRUPREQ, then wait for the ACKs. */
    int ctrl = 0;
    a.WriteReg(h, 0, RID_DP_CTRLSTAT, (int)0x50000000);
    for (int i = 0; i < 50; ++i) {
        if (a.ReadReg(h, 0, RID_DP_CTRLSTAT | REG_RNW, &ctrl) == RDDI_SUCCESS &&
            (ctrl & 0xA0000000) == 0xA0000000) break;
        Sleep(10);
    }
    const bool powered = ((ctrl & 0xA0000000) == 0xA0000000);
    printf("  DP CTRLSTAT -> 0x%08X %s\n", (unsigned)ctrl,
           powered ? "[powered up]" : "[NO POWER]");

    if (!powered || cst != RDDI_SUCCESS || cconn != RDDI_SUCCESS || conn != RDDI_SUCCESS) {
        printf("ABORT: link not usable (configure/connect/power)\n");
        a.Close(h);
        FreeLibrary(a.mod);
        return 1;
    }

    /* Halt the core: otherwise the running program rewrites RAM and the
     * read-back check compares our data against its data. */
    PrepareAP(a, h, addr, false);
    a.WriteReg(h, 0, RID_AP_TAR, (int)DHCSR_ADDR);
    a.WriteReg(h, 0, RID_AP_DRW, (int)DHCSR_HALT);
    Sleep(20);
    /* CSW is AddrInc=single: the write above auto-incremented TAR to DHCSR+4,
     * so a bare DRW read would fetch DCRSR (0) and claim "not halted". Re-arm. */
    a.WriteReg(h, 0, RID_AP_TAR, (int)DHCSR_ADDR);
    int dhcsr = 0;
    a.ReadReg(h, 0, RID_AP_DRW | REG_RNW, &dhcsr);
    printf("  DHCSR -> 0x%08X %s\n", (unsigned)dhcsr,
           (dhcsr & 0x00020000) ? "[halted]" : "[NOT halted -- read-back may be unreliable]");

    if (!PrepareAP(a, h, addr, true)) {
        printf("ABORT: AP not usable (CSW/TAR)\n");
        a.Close(h);
        FreeLibrary(a.mod);
        return 1;
    }

    /* ---- buffers ---- */
    Buf wr, rd;
    if (!BufAlloc(&wr, totalWords) || !BufAlloc(&rd, totalWords)) return 1;
    for (int i = 0; i < totalWords; i++) wr.data()[i] = 0xA0000000u + (uint32_t)i;

    /* ================= 1) size sweep ================= */
    printf("\n== throughput vs buffer size (1 round each) ==\n");
    printf("   words   bytes    write_us   read_us   write_kB/s  read_kB/s\n");
    for (int k = 0; k < LADDER_N; k++) {
        const int n = kLadder[k];
        if (n >= totalWords) break;             /* the requested size is timed below */
        /* A real buffer per step -- NOT a slice of the big one: slicing would
         * place the tail canaries in the middle of live data and report a bogus
         * out-of-bounds write. */
        Buf sw, sr;
        if (!BufAlloc(&sw, n) || !BufAlloc(&sr, n)) { g_Failed++; break; }
        for (int i = 0; i < n; i++) sw.data()[i] = 0xA0000000u + (uint32_t)i;
        Measure(a, h, addr, sw, sr, true);
        BufFree(&sw);
        BufFree(&sr);
    }
    Measure(a, h, addr, wr, rd, true);          /* the requested size */

    /* ================= 2) steady state ================= */
    printf("\n== steady state: %d words (%.1f kB) x %d round(s) ==\n",
           totalWords, (double)totalWords * 4 / 1024.0, rounds);
    printf("   round   write_us   read_us   write_kB/s  read_kB/s\n");
    double bestW = 0.0, bestR = 0.0, sumW = 0.0, sumR = 0.0;
    int okRounds = 0;
    for (int i = 0; i < rounds; i++) {
        const Rate r = Measure(a, h, addr, wr, rd, false);
        if (!r.ok) break;
        const double wk = KBps((size_t)totalWords * 4, r.wUs);
        const double rk = KBps((size_t)totalWords * 4, r.rUs);
        printf("   %5d %10.1f %10.1f   %10.1f %10.1f\n", i + 1, r.wUs, r.rUs, wk, rk);
        if (wk > bestW) bestW = wk;
        if (rk > bestR) bestR = rk;
        sumW += r.wUs; sumR += r.rUs;
        okRounds++;
    }

    /* ================= 3) serial baseline ================= */
    double serW = 0.0, serR = 0.0;
    const int serN = (totalWords < SERIAL_WORDS) ? totalWords : SERIAL_WORDS;
    SerialBaseline(a, h, addr, wr, serN, &serW, &serR);

    /* ================= summary ================= */
    printf("\n================================================================\n");
    printf("  DEVICE LIMIT (no Keil in the loop)\n");
    printf("================================================================\n");
    printf("  buffer            : %d words (%ld bytes)\n", totalWords, bytes);
    if (okRounds > 0) {
        printf("  write best/avg    : %8.1f / %8.1f kB/s   (%d round(s))\n",
               bestW, KBps((size_t)totalWords * 4, sumW / okRounds), okRounds);
        printf("  read  best/avg    : %8.1f / %8.1f kB/s\n",
               bestR, KBps((size_t)totalWords * 4, sumR / okRounds));
    } else {
        printf("  steady state did NOT complete\n");
    }
    if (serW > 0.0) printf("  1-word serial write: %8.1f kB/s  -> block speedup %.1fx\n", serW, bestW / serW);
    if (serR > 0.0) printf("  1-word serial read : %8.1f kB/s  -> block speedup %.1fx\n", serR, bestR / serR);
    printf("  NOTE: USB 2.0 High Speed = 480 Mbit/s = 61440 kB/s; compare against\n");
    printf("        the numbers above to see what fraction of the bus is used.\n");
    if (g_Failed) printf("  FAILURES: %d\n", g_Failed);
    printf("================================================================\n");

    BufFree(&wr);
    BufFree(&rd);
    a.Close(h);
    FreeLibrary(a.mod);
    return g_Failed == 0 ? 0 : 1;
}
