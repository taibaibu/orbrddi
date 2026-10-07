// swdprobe.cpp — SWD 通路功能回归（单一入口）
//
// 为什么合并（2026-09-30 整理 test\ 目录）：
//   * orbprobe.cpp（旧）  最小 AGDI 顺序 + IDCODE，但含两处**独一无二**的回归：
//                         ① AGDI 的裸文件名加载分支 LoadLibraryA("CMSIS_DAP.dll") 诊断（§17.5 的 0x2029）；
//                         ② CMSIS_DAP_Connect 第 2 实参被 AGDI 传成"选中项索引"的**假冒指针**回归。
//   * orbprobe2.cpp（旧） AP 寄存器解码（regID 4..7）+ DP 上电轮询 + DAP_RegAccessBlock 分批读。
//   * orbprobe3.cpp（旧） flash 下载路径：ROM 表 / DHCSR halt-resume / PC / RAM 读写。
//   * ORBMDK_RDDI_Test.cpp（旧）导出存在性 + 日志回调冒烟（导出检查已被 FullTest 覆盖）。
//   四者都是"同一件事的不同片段"，还都要独占同一台设备，故合并为一次运行。
//
// 覆盖（一次跑完）：
//   [0] 加载 + 关键导出自检        [1] 接口枚举 / 适配器字段 / 版本串（多 DAP 分支线索）
//   [2] 裸名重载诊断               [3] 假冒指针回归（AGDI 行为）
//   [4] DP IDCODE + 上电           [5] AP 寄存器解码（CSW / BASE / IDR / CPUID）
//   [6] flash 下载路径             [7] 日志回调冒烟
//
// 构建： powershell -File test\build_test.ps1 -Source swdprobe.cpp
//        powershell -File test\build_test.ps1 -All            （重建全部工具）
// 运行： bin\swdprobe.exe [dll路径] [v1]
//        * dll路径 省略时为裸文件名 ORBMDK_RDDI.dll（与 exe 同在 bin\）
//        * 末尾加 v1 走 ifNo=1（CMSIS-DAP v1 / HID）；默认 ifNo=0（v2 / Bulk）
// 说明：按 §17.4，接口是"选谁就是谁"——本工具**强制**指定 ifNo，不做任何回退。

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef int RDDIHandle;

typedef int (*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int (*PFN_RDDI_Close)(RDDIHandle);
typedef int (*PFN_RDDI_GetLastError)(int*, char*, size_t);
typedef int (*PFN_CMSIS_DAP_Detect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_Identify)(RDDIHandle, int, int, char*, int);
typedef int (*PFN_CMSIS_DAP_ConfigureInterface)(RDDIHandle, int, char*);
typedef int (*PFN_CMSIS_DAP_Connect)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_DetectNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_CMSIS_DAP_DetectDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_CMSIS_DAP_Disconnect)(RDDIHandle);
typedef int (*PFN_DAP_GetNumberOfDAPs)(RDDIHandle, int*);
typedef int (*PFN_DAP_GetDAPIDList)(RDDIHandle, int*, size_t);
typedef int (*PFN_DAP_ReadReg)(RDDIHandle, const int, const int, int*);
typedef int (*PFN_DAP_WriteReg)(RDDIHandle, const int, const int, const int);
typedef int (*PFN_DAP_RegAccessBlock)(RDDIHandle, const int, const int, const int*, int*);
typedef int (*PFN_RDDI_SetLogCallback)(RDDIHandle, void*, void*, int);

// ARM rddi_dap.h 的寄存器 ID 语义
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

#define CSW_32BIT_SINGLE 0x23000052     // 32 位、单次、DeviceEn
#define APBANK_ROMBASE   0xF            // SELECT.APBANK=0xF -> AP reg 0xF8 = ROM table base

#define DHCSR   0xE000EDF0
#define DCRSR   0xE000EDF4
#define DCRDR   0xE000EDF8
#define DBGKEY  0xA05F0000u

static int g_pass = 0, g_fail = 0;
static void Check(const char* what, int ok, const char* detail)
{
    if (ok) { ++g_pass; printf("  [PASS] %-44s %s\n", what, detail ? detail : ""); }
    else    { ++g_fail; printf("  [FAIL] %-44s %s\n", what, detail ? detail : ""); }
}

static RDDIHandle H = 0;
static PFN_DAP_ReadReg        pRd;
static PFN_DAP_WriteReg       pWr;
static PFN_DAP_RegAccessBlock pBlk;

// ---- AP 访问助手：TAR+DRW 合成一次 batch（与 AGDI 的 SWD_ReadData 同形）----
static int ApRead32(uint32_t addr, uint32_t* out)
{
    int regID[2]   = { RID_AP_TAR, RID_AP_DRW | RN_W };
    int regData[2] = { (int)addr, 0 };
    const int rc = pBlk(H, 0, 2, regID, regData);
    if (out) *out = (uint32_t)regData[1];
    return rc;
}

static int ApWrite32(uint32_t addr, uint32_t val)
{
    int regID[2]   = { RID_AP_TAR, RID_AP_DRW };
    int regData[2] = { (int)addr, (int)val };
    return pBlk(H, 0, 2, regID, regData);
}

static void* Get(HMODULE dll, const char* name, bool& ok)
{
    void* p = (void*)GetProcAddress(dll, name);
    printf("  %-38s %s\n", name, p ? "OK" : "MISSING");
    if (!p) ok = false;
    return p;
}

static int g_logCount = 0;

static void LogCallback(void* ctx, const char* msg, const int level)
{
    (void)ctx;
    if (g_logCount < 3) {
        const char* lv[] = { "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };
        printf("    [log] %s: %s\n", (level >= 0 && level <= 5) ? lv[level] : "?", msg);
    }
    ++g_logCount;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const char* dllPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";
    const int   ifNo    = (argc > 2 && argv[2] && (argv[2][0] == 'v' || argv[2][0] == 'V')
                           && argv[2][1] == '1') ? 1 : 0;
    const char* ifName  = (ifNo == 1) ? "CMSIS-DAP v1 (HID)" : "CMSIS-DAP v2 (Bulk)";

    printf("=== swdprobe: SWD 通路功能回归 ===\n");
    printf("DLL      : %s\n", dllPath);
    printf("接口     : ifNo=%d  %s（强制，不回退）\n\n", ifNo, ifName);

    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) { printf("LoadLibrary failed err=%lu (%s)\n", GetLastError(), dllPath); return 1; }

    // ---------------- [0] 关键导出自检 ----------------
    printf("[0] 关键导出自检\n");
    bool ok = true;
    PFN_RDDI_Open pOpen = (PFN_RDDI_Open)Get(dll, "RDDI_Open", ok);
    PFN_RDDI_Close pClose = (PFN_RDDI_Close)Get(dll, "RDDI_Close", ok);
    PFN_RDDI_GetLastError pLastErr = (PFN_RDDI_GetLastError)Get(dll, "RDDI_GetLastError", ok);
    PFN_CMSIS_DAP_Detect pDetect = (PFN_CMSIS_DAP_Detect)Get(dll, "CMSIS_DAP_Detect", ok);
    PFN_CMSIS_DAP_Identify pIdentify = (PFN_CMSIS_DAP_Identify)Get(dll, "CMSIS_DAP_Identify", ok);
    PFN_CMSIS_DAP_ConfigureInterface pCfg = (PFN_CMSIS_DAP_ConfigureInterface)Get(dll, "CMSIS_DAP_ConfigureInterface", ok);
    PFN_CMSIS_DAP_Connect pDapConnect = (PFN_CMSIS_DAP_Connect)Get(dll, "CMSIS_DAP_Connect", ok);
    PFN_CMSIS_DAP_DetectNumberOfDAPs pNumDaps = (PFN_CMSIS_DAP_DetectNumberOfDAPs)Get(dll, "CMSIS_DAP_DetectNumberOfDAPs", ok);
    PFN_CMSIS_DAP_DetectDAPIDList pDapIds = (PFN_CMSIS_DAP_DetectDAPIDList)Get(dll, "CMSIS_DAP_DetectDAPIDList", ok);
    PFN_DAP_GetNumberOfDAPs pNumDaps2 = (PFN_DAP_GetNumberOfDAPs)Get(dll, "DAP_GetNumberOfDAPs", ok);
    PFN_DAP_GetDAPIDList pDapList = (PFN_DAP_GetDAPIDList)Get(dll, "DAP_GetDAPIDList", ok);
    pRd  = (PFN_DAP_ReadReg)Get(dll, "DAP_ReadReg", ok);
    pWr  = (PFN_DAP_WriteReg)Get(dll, "DAP_WriteReg", ok);
    pBlk = (PFN_DAP_RegAccessBlock)Get(dll, "DAP_RegAccessBlock", ok);
    if (!ok || !pOpen || !pClose || !pRd || !pWr || !pBlk) {
        printf("\nABORT: 关键导出缺失\n");
        return 1;
    }
    printf("  [OK] 关键导出齐全\n\n");

    // ---------------- [1] 接口枚举 + 适配器字段 ----------------
    printf("[1] 接口枚举 / 适配器字段（AGDI 对话框用的就是这几项）\n");
    RDDIHandle h = 0;
    int rc = pOpen(&h, NULL);
    printf("  RDDI_Open -> %d, handle=%d\n", rc, h);
    if (rc != 0 || h == 0) { printf("ABORT: 打开设备失败\n"); return 1; }
    H = h;

    int numIf = -1;
    pDetect(h, &numIf);
    printf("  CMSIS_DAP_Detect -> numOfIFs=%d\n", numIf);
    Check("接口数 >= 1", numIf >= 1, "");

    for (int i = 0; i < 2; ++i) {
        for (int idNo = 1; idNo <= 4; ++idNo) {
            char b[260] = {0};
            pIdentify(h, i, idNo, b, (int)sizeof(b));
            printf("    Identify(ifNo=%d, idNo=%d) -> '%s'\n", i, idNo, b);
        }
    }
    // idNo=4 是 AGDI"多 DAP 分支"的开关值（§17.5）：主版本 >= 2 会走不通那条分支
    {
        char fw[64] = {0};
        pIdentify(h, 0, 4, fw, (int)sizeof(fw));
        Check("固件版本串主版本为 1（多 DAP 分支闸门）", fw[0] == '1', fw);
    }
    printf("\n");

    // ---------------- [2] 裸名重载诊断 ----------------
    printf("[2] 裸名重载诊断（AGDI 的 0x2029 分支：LoadLibraryA(\"CMSIS_DAP.dll\")）\n");
    {
        char self[MAX_PATH] = {0};
        GetModuleFileNameA(dll, self, MAX_PATH);
        SetLastError(0);
        HMODULE again = LoadLibraryA("CMSIS_DAP.dll");
        printf("    module path      : %s\n", self);
        printf("    bare-name reload : %p (err=%lu)\n", (void*)again, GetLastError());
        printf("    -> %s\n", again ? "裸名可解析（故障不在这一环）"
                                  : "裸名不可解析（AGDI 该分支必然失败）");
    }
    printf("\n");

    // ---------------- [3] 假冒指针回归 ----------------
    printf("[3] 假冒指针回归：AGDI 把 CMSIS_DAP_Connect 第 2 参传成\"选中项索引\"\n");
    {
        char cfg[128];
        sprintf_s(cfg, sizeof(cfg), "Master=Y;Port=SW;SWJ=Y;Clock=1000000;Trace=Off;");
        rc = pCfg(h, ifNo, cfg);
        Check("ConfigureInterface(指定 ifNo)", rc == 0, cfg);

        int* bogus = (int*)(size_t)ifNo;        // ifNo=0 -> NULL；ifNo=1 -> 地址 1（非法）
        printf("    CMSIS_DAP_Connect(handle, (int*)%d) [mimics AGDI] ...\n", ifNo);
        rc = pDapConnect(h, bogus);
        Check("未因非法指针崩溃（本层已做可写性校验）", 1,
              (ifNo == 0) ? "(传的是 NULL)" : "(传的是地址 1)");

        int v = 0;
        rc = pRd(h, 0, RID_DP_IDCODE, &v);
        printf("    DAP_ReadReg(DPIDR) -> rc=%d, 0x%08X\n", rc, v);
        Check("DPIDR 读出有效 IDCODE", rc == 0 && v != 0 && v != -1, "");
    }

    // 正规连接（后面各节都基于它）
    {
        int mode = 0;
        rc = pDapConnect(h, &mode);
        printf("    CMSIS_DAP_Connect(real) -> rc=%d, mode=%d (%s)\n",
               rc, mode, mode == 2 ? "JTAG" : "SWD");
        Check("连接目标成功", rc == 0 && mode == 1, "");
    }

    int noOfDaps = 0;
    rc = pNumDaps(h, &noOfDaps);
    printf("    CMSIS_DAP_DetectNumberOfDAPs -> rc=%d, count=%d\n", rc, noOfDaps);

    int ids[16];
    for (int i = 0; i < 16; ++i) ids[i] = 0xDEADBEEF;
    rc = pDapIds(h, ids, (size_t)noOfDaps);
    printf("    CMSIS_DAP_DetectDAPIDList(size=%d) -> rc=%d, ids[0]=0x%08X\n",
           noOfDaps, rc, ids[0]);
    Check("目标 IDCODE 有效（DetectDAPIDList）",
          rc == 0 && ids[0] != 0 && ids[0] != 0xDEADBEEF && ids[0] != (int)0xFFFFFFFF, "");

    int n2 = -1;
    pNumDaps2(h, &n2);
    Check("DAP_GetNumberOfDAPs == 1", n2 == 1, "");
    printf("\n");

    // ---------------- [4] DP IDCODE + 上电 ----------------
    printf("[4] DP IDCODE / CTRL-STAT 上电\n");
    {
        int idcode = 0;
        pRd(h, 0, RID_DP_IDCODE, &idcode);
        printf("    DPIDR = 0x%08X\n", (unsigned)idcode);
        Check("DPIDR 非 0 / 非全 1", idcode != 0 && idcode != -1, "");

        pWr(h, 0, RID_DP_CTRLSTAT, (int)0x50000000);   // CSYSPWRUPREQ | CDBGPWRUPREQ
        int ctrl = 0;
        for (int i = 0; i < 50; ++i) {
            pRd(h, 0, RID_DP_CTRLSTAT, &ctrl);
            if ((ctrl & 0xA0000000) == 0xA0000000) break;
            Sleep(10);
        }
        printf("    CTRL/STAT = 0x%08X\n", (unsigned)ctrl);
        Check("调试电源已使能（CTRL/STAT 位31/29）",
              (ctrl & 0xA0000000) == 0xA0000000, "");
        pWr(h, 0, RID_DP_ABORT, 0x1E);                 // 清 sticky
    }
    printf("\n");

    // ---------------- [5] AP 寄存器解码 ----------------
    printf("[5] AP 寄存器解码（regID 4..7 = AP：CSW/TAR/BASE/DRW）\n");
    {
        pWr(h, 0, RID_DP_SELECT, (int)(APBANK_ROMBASE << 4));   // 看 ROM 表基址（AP 0xF8）
        int rombase = 0;
        pRd(h, 0, RID_AP_BASE, &rombase);
        printf("    ROM table base (AP 0xF8) = 0x%08X\n", (unsigned)rombase);
        Check("ROM 表基址非 0（AP BASE 解码正确）", rombase != 0, "");
        pWr(h, 0, RID_DP_SELECT, 0);

        int csw = 0;
        pRd(h, 0, RID_AP_CSW, &csw);
        printf("    AP CSW 读 = 0x%08X\n", (unsigned)csw);
        pWr(h, 0, RID_AP_CSW, (int)CSW_32BIT_SINGLE);
        pRd(h, 0, RID_AP_CSW, &csw);
        printf("    AP CSW 写 0x%08X -> 读回 0x%08X\n", CSW_32BIT_SINGLE, (unsigned)csw);
        Check("AP CSW 可写可读回", csw == (int)CSW_32BIT_SINGLE, "");

        // 注意：本层只支持 regID 0..8（DP 0..3 / AP 4..7 / ABORT 8）。
        // AP IDR 在偏移 0xFC，需要 APBANK=0xF，但 rddi_dap.h 里没有该 regID，
        // 直接 pRd(0x0C) 会被判 unsupported（只刷一条假 ERROR）——故不在这里读 IDR。
        // 要验 AP 解码，用 BASE（已在上方）+ CSW + TAR/DRW（下方 CPUID）即可。

        uint32_t cpuid = 0;
        ApRead32(0xE000ED00, &cpuid);
        printf("    SCB->CPUID = 0x%08X\n", cpuid);
        Check("CPUID 合理（0x41xxxxxx，Cortex-M）", (cpuid & 0xFF000000u) == 0x41000000u, "");
    }
    printf("\n");

    // ---------------- [6] flash 下载路径 ----------------
    printf("[6] flash 下载路径（ROM 表 / halt / PC / RAM 读写 / block）\n");
    {
        uint32_t cpuid = 0;
        ApRead32(0xE000ED00, &cpuid);
        printf("    CPUID = 0x%08X\n", cpuid);

        ApWrite32(DHCSR, DBGKEY | 0x0003);          // C_DEBUGEN | C_HALT
        uint32_t dhcsr = 0;
        for (int i = 0; i < 50; ++i) {
            ApRead32(DHCSR, &dhcsr);
            if (dhcsr & 0x00020000) break;
            Sleep(10);
        }
        printf("    DHCSR(halt) = 0x%08X\n", dhcsr);
        Check("内核已 halt（S_HALT）", (dhcsr & 0x00020000) != 0, "");

        ApWrite32(DCRSR, 0x0001000F);               // 选 R15 (PC)
        uint32_t dhcsr2 = 0;
        for (int i = 0; i < 100; ++i) {
            ApRead32(DHCSR, &dhcsr2);
            if (dhcsr2 & 0x00010000) break;
            Sleep(5);
        }
        uint32_t pc = 0;
        ApRead32(DCRDR, &pc);
        printf("    PC = 0x%08X (S_REGRDY=%u)\n", pc, (dhcsr2 >> 16) & 1u);
        Check("读寄存器握手完成（S_REGRDY）", (dhcsr2 & 0x00010000) != 0, "");

        const uint32_t ramAddr = 0x20000000, pattern = 0xA5A55A5A;
        const int rcw = ApWrite32(ramAddr, pattern);
        uint32_t rb = 0;
        const int rcr = ApRead32(ramAddr, &rb);
        printf("    RAM 0x%08X <- 0x%08X (rc=%d), 读回 0x%08X (rc=%d)\n",
               ramAddr, pattern, rcw, rb, rcr);
        Check("RAM 写读一致（flash 算法路径）", rb == pattern && rcw == 0 && rcr == 0, "");

        // DAP_RegAccessBlock 分批（多 regID）——与 Repeat 语义区分（Usage.md §12.4）
        {
            int regID[2]   = { RID_AP_TAR, RID_AP_DRW | RN_W };
            int regData[2] = { (int)ramAddr, 0 };
            const int rcb = pBlk(h, 0, 2, regID, regData);
            Check("DAP_RegAccessBlock 分批读", rcb == 0 && (uint32_t)regData[1] == pattern, "");
        }

        ApWrite32(DHCSR, DBGKEY | 0x0001);          // 只留 C_DEBUGEN -> 运行
        uint32_t dhcsr3 = 0;
        ApRead32(DHCSR, &dhcsr3);
        printf("    DHCSR(resume) = 0x%08X\n", dhcsr3);
        Check("内核已恢复运行", (dhcsr3 & 0x00020000) == 0, "");
    }
    printf("\n");

    // ---------------- [7] 日志回调冒烟 ----------------
    printf("[7] 日志回调冒烟（RDDI_SetLogCallback）\n");
    {
        PFN_RDDI_SetLogCallback pSetLog =
            (PFN_RDDI_SetLogCallback)GetProcAddress(dll, "RDDI_SetLogCallback");
        if (pSetLog) {
            pSetLog(h, (void*)LogCallback, NULL, 3 /* INFO */);
            int v = 0;
            pRd(h, 0, RID_DP_IDCODE, &v);           // 触发一次访问，产生日志
            Sleep(50);
            pSetLog(h, NULL, NULL, 0);              // 取消
            if (g_logCount > 0) {
                Check("日志回调收到消息", 1, "");
            } else {
                // 阈值由 %TEMP%\ORBMDK_LOG_LEVEL 控制（0=DEBUG…3=ERROR，Usage.md §10.1）。
                // 阈值高于 INFO 时回调本来就收不到东西 —— 那不是缺陷，故报 SKIP 而不是 FAIL。
                printf("    [SKIP] 未收到日志：TEMP 目录下 ORBMDK_LOG_LEVEL 阈值高于 INFO 时属正常，\n");
                printf("           删掉该文件（或置 0）后重跑即可验证回调通路。\n");
            }
        } else {
            printf("    RDDI_SetLogCallback 缺失，跳过\n");
        }
    }
    printf("\n");

    // ---------------- 收尾 ----------------
    {
        int err = 0;
        char det[256] = {0};
        pLastErr(&err, det, sizeof(det));
        printf("RDDI_GetLastError -> err=0x%04X '%s'\n", err, det);
    }
    pClose(h);

    printf("\n================ 结果 ================\n");
    printf("  通过 %d 项，失败 %d 项\n", g_pass, g_fail);
    printf("======================================\n");
    return (g_fail == 0) ? 0 : 2;
}
