// JTAG 下 ID_DAP_TRANSFER_BLOCK(0x06) 专项复验（**直接**走 WinUSB，不经过 ORBMDK 驱动层）
//
// 目的
// ----
// COMPAT_ANALYSIS §18.9 第十步记录的结论是"JTAG 下 0x06 是禁区：设备不回应答并停止服务
// OUT 端点，必须插拔 USB"。**实测证据可靠**（会话 #84：5 s 读超时 + 之后连 Info 都写不进
// 去），但**定性存疑** —— orbtrace-1.4.3 的门级固件（Amaranth）明确实现了 JTAG 下的块传输：
//     * cmsis_dap.py:766-903 RESP_TransferBlock_Setup/Process 带 isJTAG 分支
//       （`readBDelay = isJTAG & rnw` = JTAG 下"任何读都延迟一拍"）；实现清单写着
//       `# DAP_TransferBlock : Done`，无 SWD-only 限定；
//     * dbgIF.v:480-502 的 CMD_TRANSACT 有 MODE_JTAG 路径（先 JTAG_CMD_IR 再 JTAG_CMD_TFR，
//       由 JTAG_trans_os 在 ST_DBG_WAIT_GOCLEAR 后自动补发）；
//     * jtagIF.v:142-273 实现了 35 位 DPACC/APACC 传输、多 TAP bypass、ACK 解析。
//   ⇒ "固件不认识 0x06"说不通：真不认识会走 RESP_Not_Implemented 回 0xFF（**立刻应答**），
//     而不是"彻底无声 + 端点停止服务"。后者更像**卡在传输执行里**（FSM 不回 IDLE ⇒ busy
//     恒 1 ⇒ 不再取 OUT 端点）。代码上可指认的卡死候选：
//     C1 链信息未下发/不符：jtagIF.v:242-273 的 ST_JTAG_WRITEIR 靠 ndevs/irlenx，
//        若 irlenx[i] 为 0 或与真实 IR 长度不符，`tdxcount+1 == irlenx[...]` 永不成立
//        （该路径**没有超时保护**）⇒ 永不 done ⇒ 上位机一直等不到应答。
//     C2 JTAG 时钟未跑：jtagIF 靠分频产生的 rising/falling 推进；SWJ_Clock 未设/过小
//        可能推不动它。
//     C3 事务握手：一次 0x06 的每笔都要 go 两拍（IR+TFR），从小 count 起验证。
//
// 判定（本程序存在的意义）
// ------------------------
// 每发一步 0x06 之后**立即 Ping（Info 0xF0）**，把结果分成三类：
//   A) 0x06 有应答 + ack=OK + count 匹配    -> **可用**（推翻"禁区"定性）
//   B) 0x06 无应答，但随后 Ping 仍活着      -> 命令未被处理/被吞（设备**没挂**）
//   C) 0x06 无应答，且随后 Ping 也写不进去  -> **挂死**（复现第十步，须插拔）
// A ⇒ JTAG 批量有救（对照 Todo.md §4 方案 B）；B ⇒ 要做的是补建链配置/时钟，而非永久禁用；
// C ⇒ 仍按禁区处理，但要继续查 C1/C2 是否就是触发条件。
//
// ⚠️⚠️ 风险：本程序**会真的发送 0x06**（与 jtagrawprobe.cpp 相反）。命中断定 C 时探针会变成
// "每条命令都立刻 write FAILED"，**只能重新插拔 USB**。因此默认任一步失败即停（--no-stop
// 关闭），从 count=1 最小步骤起步（--stage N 可只跑到第 N 步）。目标板接好上电、Keil 关闭、
// **上电后第一次连接就用 JTAG**（本目标 SWJ-DP 的 JTAG<-SWD 单向，先跑过 SWD 就回不来）。
//
// 构建： powershell -File test\build_test.ps1 -Source jtagblockprobe.cpp
// 运行： bin\jtagblockprobe.exe [选项]
//   --restore-swd       收尾时 Connect(1)+Disconnect 切回 SWD（**默认不做**：本目标切回 SWD 后
//                       本上电周期内再也进不了 JTAG；默认保持 JTAG 以便连续复验）
//   --force             链路无效（IDCODE 全 0/全 1）时也继续（危险：0x06 **写**会把探针跑挂）
//   --no-ir-rt          建链后跳过"IR 往返自检"
//   --ir-rt             即使 stage>=2 也**强制**做 IR 往返自检
//                       （★ 默认在 stage>=2 时**自动跳过**：它会改写 IR，而 0x05/0x06 阶段
//                        "IR 一被外部改写就必挂"——见 bug.md B9 第五/七轮 vs 第四/六轮；
//                        只想做这个诊断而不跑 0x06：--stage 1）
//   --no-xfer-cfg       不补发 0x04 TRANSFER_CONFIG（★ 默认**补发**：本探针此前从不发它，
//                       而本层在 Connect/JtagInitSequence 里是发的；它给固件侧 0x05/0x06
//                       的 WAIT/MATCH 重试设上限，未配置时默认值未知）
//   --wait-retry N      0x04 的 WAIT 重试上限（默认 100；0 = 不重试）
//   --match-retry N     0x04 的 MATCH 重试上限（默认 10）
//   --idle-cycles N     0x04 的 idle 周期（默认 0）
//   --swj-switch        建链前补发 ADIv5 的 SWD->JTAG 序列（默认不发）
//   --clock HZ          建链前设 SWJ_Clock；0 = 不设（默认 1000000）
//   --cfg 0x15|0x0C     DAP_JTAG_Configure 操作码（默认 0x15）
//   --ir 4,5            覆盖 IR 长度表（默认按 IDCODE 推断）
//   --skip-05           跳过第 1 步的 0x05 对照
//   --deep              追加第 4/5 步（count=2 读、AP 读）
//   --stage N           只跑到第 N 步（1..5）
//   --timeout MS        单条命令读超时（默认 1500）   --retry N  重发上限（默认 1）
//   --no-stop           失败后继续下发（默认失败即停）

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

static const GUID kIfGuid = {
    0xdee824ef, 0x729b, 0x4a0e,
    {0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17}
};

#define CMD_INFO            0x00
#define CMD_CONNECT         0x02
#define CMD_DISCONNECT      0x03
#define CMD_SWJ_CLOCK       0x11
#define CMD_SWJ_SEQUENCE    0x12
#define CMD_JTAG_SEQUENCE   0x14
#define CMD_JTAG_CONFIGURE  0x15
#define CMD_JTAG_IDCODE     0x16
#define CMD_TRANSFER_CONFIG 0x04
#define CMD_TRANSFER        0x05
#define CMD_TRANSFER_BLOCK  0x06

// request 编码：A[3:2]=地址, bit1=RnW, bit0=APnDP
#define REQ(addr, rnw, ap)  (uint8_t)((((addr) & 3) << 2) | ((rnw) ? 2 : 0) | ((ap) ? 1 : 0))

static bool g_stopOnFail = true;
static int  g_maxAttempts = 1;
static int  g_ioTimeoutMs = 1500;
static bool g_failed = false;
static bool g_softFail = false;   // 软失败：S1 的 0x05 对照用（JTAG 下已知不可用，失败只记录）

// ★ 0x04 TRANSFER_CONFIG 前置（2026-10-03 补）：**本探针此前从未发过这条**，
//   而本层在 CMSIS_DAP_Connect / JtagInitSequence 里是发的（DAP_ConfigureTransfer(0,100,10)）。
//   它决定固件侧 0x05/0x06 的 WAIT 重试上限；**未配置时的默认值是未知数** ——
//   若默认是"死等/极大值"，固件的 JTAG 事务就会表现为"执行即挂"。
//   ⇒ 这是"JTAG 下能否启用固件 0x05/0x06"的第一个待验前置，见 bug.md B9 第六轮后。
static bool g_xferCfg   = true;   // --no-xfer-cfg 可关掉，做干净 A/B
static int  g_idleCycles = 0;
static int  g_waitRetry  = 100;
static int  g_matchRetry = 10;

// ★ 判别实验二（--clock-late，2026-10-03）：基线 0x06 读之后**单独重发一笔 0x11 SWJ_Clock**，
//   紧接着再发一笔 0x06 读。唯一变量 = 这笔 0x11 的取值 ⇒ 判别"挂死是否由 JTAG 时钟域
//   （TCK 停摆 ⇒ jtagIF 永远等不到时钟沿 ⇒ WAIT_INFERIOR_START 永久等待）引起"。
//   对照 = --clock-late 1000000（与 S0 同值）；处理 = --clock-late 0 / 极大值。
//   依据/判据：bug.md B11.7 末条"判别实验二"。⚠ 用 --stage 2 跑，别让 S3 写插进来。
static bool     g_clockLate   = false;
static uint32_t g_clockLateHz = 0;

static void MarkFail(const char* where)
{
    if (g_softFail) { printf("      (软失败：%s —— 只记录，不中断)\n", where); return; }
    if (g_failed) return;
    g_failed = true;
    if (!g_stopOnFail) return;
    printf("\n!! 首次失败：%s\n", where);
    printf("!! 已停止后续全部命令（--no-stop 可继续）。\n");
    printf("!! 若连 Info/Connect 也写不进去 -> 探针已挂死，**重新插拔 USB** 再跑。\n");
}
static bool Aborted(void) { return g_stopOnFail && g_failed; }

typedef struct { HANDLE dev; WINUSB_INTERFACE_HANDLE winusb; UCHAR inPipe, outPipe; } V2Dev;

static bool V2Open(V2Dev* d)
{
    memset(d, 0, sizeof(*d));
    d->dev = INVALID_HANDLE_VALUE;
    HDEVINFO di = SetupDiGetClassDevsA(&kIfGuid, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA did = {0};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &kIfGuid, i, &did); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &did, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &did, det, need, NULL, NULL)) { free(det); continue; }
        HANDLE h = CreateFileA(det->DevicePath, GENERIC_WRITE | GENERIC_READ,
                               FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        free(det);
        if (h == INVALID_HANDLE_VALUE) continue;
        WINUSB_INTERFACE_HANDLE wu = NULL;
        if (!WinUsb_Initialize(h, &wu)) { CloseHandle(h); continue; }
        USB_INTERFACE_DESCRIPTOR idesc = {0};
        if (!WinUsb_QueryInterfaceSettings(wu, 0, &idesc) || idesc.bInterfaceClass != 0xFF) {
            WinUsb_Free(wu); CloseHandle(h); continue;
        }
        UCHAR ip = 0, op = 0;
        for (UCHAR n = 0; n < idesc.bNumEndpoints; ++n) {
            WINUSB_PIPE_INFORMATION pi = {0};
            if (!WinUsb_QueryPipe(wu, 0, n, &pi)) continue;
            if (pi.PipeType != UsbdPipeTypeBulk) continue;
            if (pi.PipeId & 0x80) ip = pi.PipeId; else op = pi.PipeId;
        }
        if (!ip || !op) { WinUsb_Free(wu); CloseHandle(h); continue; }
        d->dev = h; d->winusb = wu; d->inPipe = ip; d->outPipe = op;
        SetupDiDestroyDeviceInfoList(di);
        return true;
    }
    SetupDiDestroyDeviceInfoList(di);
    return false;
}

static void V2Close(V2Dev* d)
{
    if (d->winusb) {
        if (d->dev != INVALID_HANDLE_VALUE) CancelIoEx(d->dev, NULL);
        if (d->inPipe)  WinUsb_AbortPipe(d->winusb, d->inPipe);
        if (d->outPipe) WinUsb_AbortPipe(d->winusb, d->outPipe);
        WinUsb_Free(d->winusb);
        d->winusb = NULL;
    }
    if (d->dev != INVALID_HANDLE_VALUE) { CloseHandle(d->dev); d->dev = INVALID_HANDLE_VALUE; }
}

static bool WriteOnce(V2Dev* d, const uint8_t* buf, ULONG len, int timeoutMs)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;
    ULONG done = 0;
    BOOL ok = WinUsb_WritePipe(d->winusb, d->outPipe, (PUCHAR)buf, len, &done, &ov);
    bool ret = false;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0 &&
            GetOverlappedResult(d->dev, &ov, &done, FALSE)) ret = true;
    } else if (ok) ret = true;
    CloseHandle(ov.hEvent);
    return ret;
}

// 单次读；返回字节数，-1 失败，-2 超时。超时收尾必须彻底（ov 在栈上，IRP 未完成就
// CloseHandle 会让驱动攥着这块内存 -> 后续崩）：CancelIoEx -> 等 -> AbortPipe -> 再等。
static int ReadOnce(V2Dev* d, uint8_t* out, ULONG cap, int timeoutMs, ULONGLONG* msOut)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    const ULONGLONG t0 = GetTickCount64();
    ULONG rd = 0;
    BOOL ok = WinUsb_ReadPipe(d->winusb, d->inPipe, out, cap, &rd, &ov);
    int ret;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
            ret = GetOverlappedResult(d->dev, &ov, &rd, FALSE) ? (int)rd : -1;
        } else {
            CancelIoEx(d->dev, &ov);
            if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) {
                WinUsb_AbortPipe(d->winusb, d->inPipe);
                WinUsb_ResetPipe(d->winusb, d->inPipe);
                WaitForSingleObject(ov.hEvent, 3000);
            }
            DWORD dummy = 0;
            GetOverlappedResult(d->dev, &ov, &dummy, FALSE);
            ret = -2;
        }
    } else { ret = ok ? (int)rd : -1; }
    if (msOut) *msOut = GetTickCount64() - t0;
    if (ret != -2 || WaitForSingleObject(ov.hEvent, 0) == WAIT_OBJECT_0) CloseHandle(ov.hEvent);
    return ret;
}

static int DrainIn(V2Dev* d, int ms)
{
    uint8_t tmp[1024];
    int total = 0;
    for (int i = 0; i < 4; ++i) { const int n = ReadOnce(d, tmp, sizeof(tmp), ms, NULL); if (n <= 0) break; total += n; }
    return total;
}

static void Hex(const uint8_t* p, int n, int maxBytes)
{
    for (int i = 0; i < n && i < maxBytes; ++i) printf(" %02X", p[i]);
    if (n > maxBytes) printf(" ...");
}

// 发一条命令（出包 64 字节短包；V2 下绝不能用 512 整包，否则固件认为传输未结束）
static int Cmd(V2Dev* d, const uint8_t* cmd, int cmdLen, uint8_t* resp, int respCap,
               int readMs, const char* name)
{
    if (Aborted()) { printf("  %-28s SKIP（已失败停止）\n", name); return -1; }
    uint8_t tx[1024];
    memset(tx, 0, sizeof(tx));
    memcpy(tx, cmd, (size_t)cmdLen);
    if (readMs > g_ioTimeoutMs) readMs = g_ioTimeoutMs;
    for (int attempt = 0; attempt < g_maxAttempts; ++attempt) {
        if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
            WinUsb_AbortPipe(d->winusb, d->outPipe);
            WinUsb_ResetPipe(d->winusb, d->outPipe);
            printf("  %-28s write FAILED (%d/%d)\n", name, attempt + 1, g_maxAttempts);
            if (attempt + 1 >= g_maxAttempts) MarkFail(name);
            continue;
        }
        ULONGLONG ms = 0;
        const int n = ReadOnce(d, resp, (ULONG)respCap, readMs, &ms);
        if (n > 0) {
            printf("  %-28s len=%-3d (%5llu ms):", name, n, ms);
            Hex(resp, n, 12);
            printf("\n");
            return n;
        }
        printf("  %-28s no response (rc=%d, %llu ms)\n", name, n, ms);
        if (attempt + 1 >= g_maxAttempts) MarkFail(name);
    }
    return -1;
}

// ★ 判活：Info(caps)。这是最关键的探针 —— 0x06 之后能否拿到它，就是"未处理"与"挂死"的分界
static bool PingAlive(V2Dev* d, const char* tag)
{
    // ★ 注意：这里**故意不做** Aborted() 早退 —— Ping 是判定"未处理 vs 挂死"的唯一手段，
    //   哪怕前面已经失败也必须把它发出去（代价只是多等一次超时）。
    uint8_t r[64];
    const uint8_t c[2] = {CMD_INFO, 0xF0};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1200, tag);
    printf("      -> 探针 %s\n", (n >= 2) ? "**活着**" : "**无应答（连 Info 都发不进去 => 挂死）**");
    return n >= 2;
}

static int Connect2(V2Dev* d, int port, const char* name)
{
    uint8_t r[64];
    uint8_t c[2] = {CMD_CONNECT, (uint8_t)port};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, name);
    if (n < 2) return -1;
    printf("      -> port/mode = %d (%s)\n", r[1],
           (r[1] == 2) ? "JTAG" : (r[1] == 1 || r[1] == 0) ? "SWD" : "未知");
    return r[1];
}

static void JtagConfigure(V2Dev* d, uint8_t opcode, int devCount, const uint8_t* irs)
{
    uint8_t r[64];
    uint8_t c[2 + 8];
    c[0] = opcode; c[1] = (uint8_t)devCount;
    memcpy(&c[2], irs, (size_t)devCount);
    char name[64];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "JTAG_Configure@0x%02X(%d TAP)", opcode, devCount);
    const int n = Cmd(d, c, 2 + devCount, r, sizeof(r), 1500, name);
    printf("      -> status = %s%d  IR 表 =", (n >= 2) ? "" : "?", (n >= 2) ? r[1] : -1);
    for (int i = 0; i < devCount; ++i) printf(" %d", irs[i]);
    printf("\n");
}

static void JtagIdcode(V2Dev* d, uint32_t* ids, int* countOut)
{
    uint8_t r[64];
    const uint8_t c[1] = {CMD_JTAG_IDCODE};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, "JTAG_IDCODE(0x16)");
    int cnt = 0;
    if (n >= 2) {
        cnt = r[1];
        printf("      -> IDCODE count = %d", cnt);
        for (int i = 0; i < cnt && i < 8 && (5 + i * 4) < n; ++i) {
            const uint32_t id = (uint32_t)r[2 + i * 4] | ((uint32_t)r[3 + i * 4] << 8) |
                                ((uint32_t)r[4 + i * 4] << 16) | ((uint32_t)r[5 + i * 4] << 24);
            ids[i] = id;
            printf("  id[%d]=0x%08X", i, id);
        }
        printf("\n");
    }
    if (countOut) *countOut = cnt;
}

// 0x14 段式帧（**带 TDI 数据版**；JtagSeq1 = 它的 tdi=NULL 特例）。与 jtagrawprobe 同款，
// 其注释称"按 orbtrace 固件源码的正确帧"：
//   请求 [0x14][段数=1][info][TDI nB]   info = 位数(0..63，0 表示 64) | TMS?0x40 | TDO捕获?0x80
//   响应 [0x14][status][TDO nB]         数据字节数 = ceil(位数/8)，LSB first
static void JtagSeqData(V2Dev* d, int bits, int tmsLevel, int capture,
                        const uint8_t* tdi, uint8_t* tdoOut, const char* name)
{
    if (Aborted()) { printf("  %-28s SKIP（已失败停止）\n", name); return; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = CMD_JTAG_SEQUENCE;
    tx[1] = 1;
    tx[2] = (uint8_t)((bits & 0x3F) | (tmsLevel ? 0x40 : 0) | (capture ? 0x80 : 0));
    if (tdi) memcpy(&tx[3], tdi, (size_t)((bits + 7) / 8));  // TDI 数据区从 index 3 起（LSB first）
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) { printf("  %-28s write FAILED\n", name); MarkFail(name); return; }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n <= 0) MarkFail(name);
    printf("  %-28s n=%-3d :", name, n);
    Hex(r, n, 12);
    printf("\n");
    if (tdoOut) {
        const int nb = (bits + 7) / 8;
        const int avail = (n > 2) ? (n - 2) : 0;            // 跳过 [cmdId][status]
        memset(tdoOut, 0, (size_t)nb);
        if (avail > 0) memcpy(tdoOut, &r[2], (size_t)((avail < nb) ? avail : nb));
    }
}

static void JtagSeq1(V2Dev* d, int bits, int tmsLevel, int capture, uint8_t* tdoOut, const char* name)
{
    JtagSeqData(d, bits, tmsLevel, capture, NULL, tdoOut, name);
}

// ★ 手动扫链（**不依赖固件的 0x16**）：jtagrawprobe 的 JtagPathTest 就是靠这套在**同一目标**上
//   读出 cpu=0x4BA00477 / bs=0x06413041 的（见其源码 :796-836），而固件的 `0x16` 在同一目标上
//   回 count=0（jtagrawprobe 文件头 :3-7 已把它记为已知现象）。所以建链必须走这条。
//   TMS 状态机（LSB first）：6×1 -> Test-Logic-Reset，然后 RTI -> Select-DR -> Capture-DR -> Shift-DR。
static bool JtagReadIdcodes(V2Dev* d, int taps, uint32_t* idsOut)
{
    if (taps < 1) taps = 1;
    if (taps > 4) taps = 4;
    printf("  --- 手动扫链：Shift-DR 移 %d 位读 TDO（%d TAP 的 IDCODE）---\n", taps * 32, taps);
    JtagSeq1(d, 6, 1, 0, NULL, "TMS reset (6)");
    JtagSeq1(d, 1, 0, 0, NULL, "-> Run-Test/Idle");
    JtagSeq1(d, 1, 1, 0, NULL, "-> Select-DR");
    JtagSeq1(d, 1, 0, 0, NULL, "-> Capture-DR");
    JtagSeq1(d, 1, 0, 0, NULL, "-> Shift-DR");
    uint8_t tdo[16];
    memset(tdo, 0, sizeof(tdo));
    JtagSeq1(d, taps * 32, 0, 1, tdo, "shift N bits + TDO");
    JtagSeq1(d, 1, 1, 0, NULL, "-> Exit1-DR");

    bool any = false;
    for (int i = 0; i < taps; ++i) {
        uint32_t id = 0;
        memcpy(&id, &tdo[i * 4], 4);
        idsOut[i] = id;
        printf("      -> IDCODE[%d] = 0x%08X%s\n", i, id,
               (id == 0x00000000u) ? "  (全 0：该位无器件驱动)"
             : (id == 0xFFFFFFFFu) ? "  (全 1：悬空)" : "");
        if (id != 0 && id != 0xFFFFFFFFu) any = true;
    }
    return any;
}

// ★ IR 往返自检（走**安全的** 0x14 原始位流路径，**不经 DAP 事务**）。回答两个问题：
//   (a) "IR 写这条路"本身能不能跑通（若这一步就挂 ⇒ 卡的比 0x06 更基础）；
//   (b) IR 长度表（默认 4,5）是否符合**真实链**。
//   做法：写 IR = **全 1**（= JTAG 标准 BYPASS 指令，任何 TAP 都支持；全 1 与 TAP 顺序无关，
//   故不依赖链上顺序）→ 再 Shift-DR 读 2 位：
//     · BYPASS 生效 ⇒ 每个 TAP 的 DR 都变成 1 位旁路寄存器（值为 0）⇒ 读回 **全 0**；
//     · 若 IR 总长度与真实链不符 ⇒ 移位错位 ⇒ DR 不是 BYPASS ⇒ 读回非 0（多为某个 IDCODE 低位）。
//   对照：随后再扫一次 IDCODE，确认链仍活着。
static bool JtagIrRoundTrip(V2Dev* d, const uint8_t* irs, int taps, uint32_t* idcode0Out)
{
    int irSum = 0;
    for (int i = 0; i < taps; ++i) irSum += (irs[i] ? irs[i] : 4);
    if (irSum < 1) irSum = 1;
    if (irSum > 63) irSum = 63;                             // 0x14 单段最多 64 位
    printf("  --- IR 往返自检：Shift-IR 移 %d 位全 1(BYPASS) -> Shift-DR 读 2 位（期望 0）---\n", irSum);

    JtagSeqData(d, 6, 1, 0, NULL, NULL, "TMS reset (6)");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Run-Test/Idle");
    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Select-DR");
    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Select-IR");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Capture-IR");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Shift-IR");

    uint8_t tdi[8];
    memset(tdi, 0xFF, sizeof(tdi));                         // TDI 全 1 => BYPASS
    JtagSeqData(d, irSum, 0, 0, tdi, NULL, "shift IR (all 1)");

    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Exit1-IR");
    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Update-IR");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Run-Test/Idle");
    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Select-DR");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Capture-DR");
    JtagSeqData(d, 1, 0, 0, NULL, NULL, "-> Shift-DR");

    uint8_t tdo[4];
    memset(tdo, 0, sizeof(tdo));
    JtagSeqData(d, 2, 0, 1, NULL, tdo, "shift 2 bits + TDO");
    JtagSeqData(d, 1, 1, 0, NULL, NULL, "-> Exit1-DR");

    const uint8_t dr = (uint8_t)(tdo[0] & 0x03);
    printf("      -> BYPASS DR 读回 = 0x%02X（期望 0x00）\n", dr);

    // 对照：IR 往返之后再扫一次 IDCODE（读 IDCODE 无需 IR），确认链没被搞死
    if (idcode0Out) {
        uint32_t ids[8] = {0};
        const bool ok = JtagReadIdcodes(d, taps, ids);
        *idcode0Out = ids[0];
        printf("      -> IR 往返后 IDCODE[0] = 0x%08X（链%s）\n", ids[0], ok ? "仍活" : "异常");
    }

    if (dr == 0x00) {
        printf("      => IR **写路径可跑通**，且移 %d 位后 BYPASS 生效 ⇒ **IR 总长度与真实链一致**。\n", irSum);
        return true;
    }
    printf("      => BYPASS **未生效** ⇒ 要么 IR 写路径不通，要么**IR 总长度(%d)与真实链不符**\n", irSum);
    printf("         （可试 --ir 4,5 / --ir 5,5 / --ir 4,4 交叉验证；注意每次改动都要重跑）。\n");
    return false;
}

// 0x05 单笔传输（0x06 的对照基线）：请求 [05][0][1][request][写 4B?]；响应 [05][status][count][数据 4B]
static int DapTransfer05(V2Dev* d, uint8_t request, const uint32_t* wdata,
                         int* statusOut, uint32_t* rdataOut, const char* name)
{
    if (Aborted()) { printf("  %-28s SKIP（已失败停止）\n", name); return -1; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = CMD_TRANSFER; tx[1] = 0; tx[2] = 1; tx[3] = request;
    if (wdata) memcpy(&tx[4], wdata, 4);
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) { printf("  %-28s write FAILED\n", name); MarkFail(name); return -1; }
    uint8_t r[64];
    ULONGLONG ms = 0;
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, &ms);
    int status = -1; uint32_t v = 0;
    if (n >= 2) status = r[1];
    if (n >= 7) memcpy(&v, &r[3], 4);
    if (statusOut) *statusOut = status;
    if (rdataOut) *rdataOut = v;
    if (n <= 0) MarkFail(name);
    printf("  %-28s n=%-2d (%5llu ms) status=%-3d -> 0x%08X\n", name, n, ms, status, v);
    return (n <= 0) ? -1 : n;
}

// 0x06 块传输：请求 [06][dapId=0][count lo][count hi][request][写数据 count*4]
//   响应 [cmd][count lo][count hi][ack][数据...]
//   ⚠️ 走本层 DLL 时响应前面多一个"报告ID"字节，**直连 WinUSB 时没有**。下面自动探测两种
//      布局（并在输出里标注），免得栽在偏移上。
typedef struct {
    int      n, respCount, ack, dataOff, nData, layout;
    uint32_t data[8];
} TbResult;

static TbResult TransferBlock(V2Dev* d, uint16_t count, uint8_t request,
                              const uint32_t* wdata, const char* name)
{
    TbResult res;
    memset(&res, 0, sizeof(res));
    res.n = -2; res.respCount = -1; res.ack = -1; res.dataOff = -1; res.layout = -1;

    if (Aborted()) { printf("  %-28s SKIP（已失败停止）\n", name); return res; }

    uint8_t tx[1024];
    memset(tx, 0, sizeof(tx));
    tx[0] = CMD_TRANSFER_BLOCK;
    tx[1] = 0;
    tx[2] = (uint8_t)(count & 0xFF);
    tx[3] = (uint8_t)((count >> 8) & 0xFF);
    tx[4] = request;
    if (wdata && (request & 0x02) == 0) memcpy(&tx[5], wdata, (size_t)count * 4);

    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        printf("  %-28s write FAILED\n", name);
        MarkFail(name);
        res.n = -1;
        return res;
    }
    uint8_t r[512];
    memset(r, 0, sizeof(r));
    ULONGLONG ms = 0;
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, &ms);
    res.n = n;
    if (n <= 0) {
        printf("  %-28s **无应答** (rc=%d, %llu ms)%s\n", name, n, ms,
               (n == -2) ? " <- 读超时（设备不吐字节）" : "");
        MarkFail(name);
        return res;
    }

    int base = -1;
    if (n >= 4 && r[0] == CMD_TRANSFER_BLOCK)      { base = 0; res.layout = 0; }
    else if (n >= 5 && r[1] == CMD_TRANSFER_BLOCK) { base = 1; res.layout = 1; }
    if (base >= 0) {
        res.respCount = (int)r[base + 1] | ((int)r[base + 2] << 8);
        res.ack       = r[base + 3] & 0x07;
        res.dataOff   = base + 4;
        res.nData     = (n - res.dataOff) / 4;
        for (int i = 0; i < res.nData && i < 8; ++i) {
            res.data[i] = (uint32_t)r[res.dataOff + i * 4] |
                          ((uint32_t)r[res.dataOff + i * 4 + 1] << 8) |
                          ((uint32_t)r[res.dataOff + i * 4 + 2] << 16) |
                          ((uint32_t)r[res.dataOff + i * 4 + 3] << 24);
        }
    }
    printf("  %-28s n=%-3d (%5llu ms) layout=%s count=%d ack=%d:",
           name, n, ms, (res.layout < 0) ? "??" : (res.layout == 0) ? "无报告ID" : "带报告ID",
           res.respCount, res.ack);
    Hex(r, n, 16);
    printf("\n");
    if (base < 0) {
        printf("      !! 响应首字节不是 0x06（%02X/%02X）—— 固件可能回了别的命令"
               "（RESP_Not_Implemented=0xFF 或 Error 都算**立刻应答**，说明命令被认了）\n",
               r[0], (n > 1) ? r[1] : 0);
    } else if (res.nData > 0) {
        printf("      -> 数据:");
        for (int i = 0; i < res.nData && i < 8; ++i) printf(" [%d]=0x%08X", i, res.data[i]);
        printf("\n");
    }
    return res;
}

static int IrLenForId(uint32_t id)
{
    if ((id & 0x0FFFFFFFu) == 0x0BA0047u) return 4;   // ARM JTAG-DP / SWJ-DP
    if ((id & 0x000FFFFFu) == 0x13041u)   return 5;   // 目标板链上第二颗（实测）
    return 4;
}

static bool ParseIrList(const char* s, uint8_t* irs, int* count)
{
    int n = 0;
    const char* p = s;
    while (*p && n < 8) {
        char* end = NULL;
        const long v = strtol(p, &end, 10);
        if (end == p || v < 1 || v > 32) return false;
        irs[n++] = (uint8_t)v;
        p = end;
        if (*p == ',') ++p;
    }
    *count = n;
    return n > 0;
}

static void SwjClock(V2Dev* d, uint32_t hz)
{
    uint8_t r[64];
    const uint8_t c[5] = {CMD_SWJ_CLOCK, (uint8_t)(hz & 0xFF), (uint8_t)((hz >> 8) & 0xFF),
                          (uint8_t)((hz >> 16) & 0xFF), (uint8_t)((hz >> 24) & 0xFF)};
    Cmd(d, c, sizeof(c), r, sizeof(r), 1500, "SWJ_Clock");
}

// ★ 0x04 = ID_DAP_TRANSFER_CONFIG：决定固件侧 0x05/0x06 的 WAIT/MATCH 重试上限。
//   载荷：[04][idle_cycles][wait_retry lo][wait_retry hi][match_retry lo][match_retry hi]
//   本探针此前从未发过它 —— 而"JTAG 下 0x05/0x06 执行即挂（且无超时）"与"wait_retry 默认值
//   是死等"在现象上完全一致。故先把它补上，再做 A/B（--no-xfer-cfg）。
static void TransferConfigure(V2Dev* d, const char* name)
{
    uint8_t r[64];
    const uint8_t c[6] = {
        CMD_TRANSFER_CONFIG, (uint8_t)g_idleCycles,
        (uint8_t)(g_waitRetry & 0xFF), (uint8_t)((g_waitRetry >> 8) & 0xFF),
        (uint8_t)(g_matchRetry & 0xFF), (uint8_t)((g_matchRetry >> 8) & 0xFF),
    };
    printf("      [xfer-cfg] idle=%d wait_retry=%d match_retry=%d\n",
           g_idleCycles, g_waitRetry, g_matchRetry);
    Cmd(d, c, sizeof(c), r, sizeof(r), 1500, name);
}

static void SwjSwitchSeq(V2Dev* d)
{
    uint8_t r[64];
    const uint8_t lineReset[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t c[16];
    c[0] = CMD_SWJ_SEQUENCE; c[1] = 64; memcpy(&c[2], lineReset, 8);
    Cmd(d, c, 10, r, sizeof(r), 1500, "SWJ_Seq(64 x 1)");
    const uint8_t swd2jtag[2] = {0x3C, 0xE7};   // 0xE73C LSB first
    uint8_t c2[4] = {CMD_SWJ_SEQUENCE, 16, 0x3C, 0xE7};
    (void)swd2jtag;
    Cmd(d, c2, 4, r, sizeof(r), 1500, "SWJ_Seq(0xE73C)");
}

// ---------------------------------------------------------------------------
// 逐阶段记录，最后出判定表
// ---------------------------------------------------------------------------
typedef struct { const char* step; int resp; int ack; int count; int alive; } ProbeStep;
static ProbeStep g_steps[8];
static int g_stepCount = 0;

static void RecordStep(const char* step, const TbResult* r, bool alive)
{
    if (g_stepCount >= 8) return;
    ProbeStep* s = &g_steps[g_stepCount++];
    s->step = step;
    s->resp = r->n;
    s->ack = r->ack;
    s->count = r->respCount;
    s->alive = alive ? 1 : 0;
}

static void PrintVerdict(void)
{
    printf("\n================ 判定汇总 ================\n");
    printf("%-34s %-8s %-5s %-6s %s\n", "步骤", "应答", "ack", "count", "随后 Ping");
    bool anyResp = false, anyDead = false, anyNoRespAlive = false;
    for (int i = 0; i < g_stepCount; ++i) {
        const ProbeStep* s = &g_steps[i];
        printf("%-34s %-8s %-5d %-6d %s\n", s->step,
               (s->resp > 0) ? "有" : (s->resp == -2) ? "超时" : "写失败",
               s->ack, s->count, s->alive ? "活着" : "无应答(挂死?)");
        if (s->resp > 0) anyResp = true;
        else if (s->alive) anyNoRespAlive = true;
        else anyDead = true;
    }
    if (g_clockLate) {
        printf("\n结论（判别实验二 --clock-late %u Hz）：看上面 **S2** 与 **S2.5** 两行的对照 ——\n", g_clockLateHz);
        printf("  S2 有应答、S2.5 无应答/挂死 ⇒ **时钟域嫌疑成立**（那笔 0x11 改分频 ⇒ TCK 停摆）；\n");
        printf("  两行都活着 ⇒ **排除时钟停摆**，回到 B11.6 的 (a) posted-read / (b) WAIT 重试风暴。\n");
        printf("  注意：本模式下**不要**看下面的 A/B/C 结论（S2 有应答必然输出 A，会误导）。\n");
        return;
    }
    printf("\n结论：");
    if (anyResp) {
        printf("【A】JTAG 下 0x06 **有应答** ⇒ 该命令在 JTAG 下**可用**（\"禁区\"定性被推翻）。\n");
        printf("      下一步：核对 ack/count 与数据是否符合预期；若 count 与请求数不一致，\n");
        printf("      先查 JTAG 的\"延迟读\"（cmsis_dap.py:778-786，JTAG 任何读都延迟一拍）。\n");
    } else if (anyDead) {
        printf("【C】0x06 之后**连 Info 都发不进去** ⇒ **复现挂死**（§18.9 第十步）⇒ 须插拔 USB。\n");
        printf("      下一步：把上面哪一步先死、当时的 count/TAP 数/时钟记下，并单独验证\n");
        printf("      C1（JTAG_Configure 的 IR 表是否与真实链一致）与 C2（SWJ_Clock 是否已设）。\n");
    } else if (anyNoRespAlive) {
        printf("【B】0x06 **无应答，但设备还活着** ⇒ 命令未被处理/被吞（**不是挂死**）。\n");
        printf("      下一步：查建链配置（JTAG_Configure 的 IR 表、SWJ_Clock），以及是否\n");
        printf("      需要先跑通 0x05；这类失败是**可重试**的，不必永久禁用。\n");
    } else {
        printf("未取得结论（0x06 一步都没跑起来）。\n");
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
    // ★ noSwd 默认 true（= 收尾**不**切回 SWD）：本目标 SWJ-DP 的 JTAG<-SWD 是**单向**的，
    //   收尾一旦切回 SWD，本次上电周期内**再也进不了 JTAG** —— 已实测（2026-10-03）：
    //   上一轮收尾做了 Connect(1)，下一轮扫链就全是 0xFFFFFFFF（TDO 悬空）。
    bool noSwd = true;
    bool swjSwitch = false, skip05 = false, deep = false, pingOnly = false, force = false;
    bool noIrRt = false;   // 跳过"IR 往返自检"
    bool forceIrRt = false; // 强制做 IR 往返（默认在 stage>=2 时自动跳过，见下方防呆）
    bool writeFirst = false; // ★ 实验：跳过 S2 前置读，直接发 S3 写（验"写挂死是否由前置读带坏"）
    bool resetBetween = false; // ★ 实验：两次 0x06 之间夹 TAP reset + 重发 0x15（试探"解楔"）
    bool reconnectBetween = false; // ★ 实验：两次 0x06 之间做 DAP 全套重初始化（Disconnect+Connect+Configure）
    uint32_t clockHz = 1000000;
    uint8_t cfgOp = CMD_JTAG_CONFIGURE;
    int stageMax = 5;
    int taps = 2;                                   // 链上 TAP 数（手动扫链用；本目标 = 2：cpu + bs）
    uint8_t irOv[8] = {4, 5, 0, 0, 0, 0, 0, 0};     // 默认 IR 表（本目标实测 4,5）
    int irOvCount = 2;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--no-swd"))          noSwd = true;     // 默认值（保持 JTAG，不切回）
        else if (!strcmp(a, "--restore-swd")) noSwd = false;    // 显式要求切回 SWD 收尾
        else if (!strcmp(a, "--force"))       force = true;     // 链路无效时也继续（危险）
        else if (!strcmp(a, "--swj-switch")) swjSwitch = true;
        else if (!strcmp(a, "--skip-05"))  skip05 = true;
        else if (!strcmp(a, "--write-first")) writeFirst = true;
        else if (!strcmp(a, "--reset-between")) resetBetween = true;
        else if (!strcmp(a, "--reconnect-between")) reconnectBetween = true;
        else if (!strcmp(a, "--no-ir-rt")) noIrRt = true;
        else if (!strcmp(a, "--ir-rt"))    forceIrRt = true;
        else if (!strcmp(a, "--no-xfer-cfg")) g_xferCfg = false;
        else if (!strcmp(a, "--clock-late") && i + 1 < argc) {
            g_clockLateHz = (uint32_t)strtoul(argv[++i], NULL, 0);
            g_clockLate = true;
        }
        else if (!strcmp(a, "--wait-retry") && i + 1 < argc)  g_waitRetry = atoi(argv[++i]);
        else if (!strcmp(a, "--match-retry") && i + 1 < argc) g_matchRetry = atoi(argv[++i]);
        else if (!strcmp(a, "--idle-cycles") && i + 1 < argc) g_idleCycles = atoi(argv[++i]);
        else if (!strcmp(a, "--deep"))     deep = true;
        else if (!strcmp(a, "--no-stop"))  g_stopOnFail = false;
        else if (!strcmp(a, "--ping-only")) pingOnly = true;
        else if (!strcmp(a, "--clock") && i + 1 < argc)  clockHz = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--timeout") && i + 1 < argc) g_ioTimeoutMs = atoi(argv[++i]);
        else if (!strcmp(a, "--retry") && i + 1 < argc)   g_maxAttempts = atoi(argv[++i]);
        else if (!strcmp(a, "--stage") && i + 1 < argc)   stageMax = atoi(argv[++i]);
        else if (!strcmp(a, "--taps") && i + 1 < argc)    taps = atoi(argv[++i]);
        else if (!strcmp(a, "--cfg") && i + 1 < argc)     cfgOp = (uint8_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ir") && i + 1 < argc) {
            if (!ParseIrList(argv[++i], irOv, &irOvCount)) { printf("--ir 解析失败\n"); return 2; }
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            printf("用法: jtagblockprobe.exe [--restore-swd] [--swj-switch] [--clock HZ] [--cfg 0x15|0x0C]\n"
                   "       [--taps N] [--ir 4,5] [--skip-05] [--deep] [--stage N] [--timeout MS]\n"
                   "       [--retry N] [--no-stop] [--force] [--no-ir-rt] [--ir-rt] [--ping-only]\n"
                   "       [--write-first]  ★ 跳过 S2 前置读、直接发 S3 写（查写挂死是否被前置读带坏）\n"
                   "       [--reset-between] ★ 两次 0x06 之间夹 TAP reset + 重发 0x15（试探解楔）\n"
                   "       [--reconnect-between] ★ 两次 0x06 之间做全套重初始化(0x03+0x02+0x15)，试不插拔解楔\n"
                   "       [--no-xfer-cfg] [--wait-retry N] [--match-retry N] [--idle-cycles N]\n"
                   "       [--clock-late HZ] ★ 判别实验二：基线 0x06 读之后**单独重发一笔 0x11 SWJ_Clock(HZ)**，\n"
                   "                          紧接着再发同一笔 0x06 读；唯一变量 = HZ（判据见 bug.md B11.7）。\n"
                   "                          跑法：--skip-05 --stage 2 --clock-late 1000000（对照，与 S0 同值）\n"
                   "                                              --skip-05 --stage 2 --clock-late 0（处理，极端分频）\n"
                   "  ★ 建链后默认补发 0x04 TRANSFER_CONFIG（本探针此前从不发它）：它决定固件侧\n"
                   "    0x05/0x06 的 WAIT 重试上限，未配置时默认值未知 —— 是'JTAG 执行即挂'的\n"
                   "    头号待验前置。--wait-retry 0/1/1000 可扫；--no-xfer-cfg 做干净 A/B。\n"
                   "  建链后默认做一次 IR 往返自检（写 IR=全1/BYPASS 再读 DR，安全、走原始位流）：\n"
                   "  用它区分 IR 路径坏 还是 DAP 事务路径坏，并验证 IR 长度表是否与真实链一致。\n"
                   "  默认收尾**不**切回 SWD（保持 JTAG，便于连续复验）；--restore-swd 才切回（本目标\n"
                   "  切回后本上电周期内就进不了 JTAG 了）。\n");
            return 0;
        } else {
            printf("未知参数: %s（--help）\n", a);
            return 2;
        }
    }
    if (g_maxAttempts < 1) g_maxAttempts = 1;
    if (taps < 1) taps = 1;
    if (taps > 4) taps = 4;

    printf("=========== JTAG 下 0x06 专项复验 ===========\n");
    printf("!! 本程序会真的发送 cmd=0x06。命中断定 C 时探针会挂死，**只能重新插拔 USB**。\n");
    printf("!! 目标板接好并上电、Keil 关闭、上电后第一次连接就用 JTAG。\n");
    printf("参数: no-swd=%d swj-switch=%d clock=%u cfg=0x%02X taps=%d stage<=%d timeout=%dms retry=%d stop=%d\n\n",
           noSwd, swjSwitch, clockHz, cfgOp, taps, stageMax, g_ioTimeoutMs, g_maxAttempts, g_stopOnFail);
    printf("参数: xfer-cfg(0x04)=%d idle=%d wait_retry=%d match_retry=%d no-ir-rt=%d write-first=%d reset-between=%d reconnect-between=%d\n\n",
           (int)g_xferCfg, g_idleCycles, g_waitRetry, g_matchRetry, (int)noIrRt, (int)writeFirst, (int)resetBetween, (int)reconnectBetween);
    if (g_clockLate) printf("参数: clock-late（判别实验二） = %u Hz\n", g_clockLateHz);

    V2Dev d;
    if (!V2Open(&d)) { printf("open FAILED（探针没插 / 驱动没装 / 已被其它进程占用）\n"); return 1; }
    printf("opened: in=0x%02X out=0x%02X\n", d.inPipe, d.outPipe);
    DrainIn(&d, 200);

    // --ping-only：只发一条 Info(0xF0)，用于"探针是死是活"的最短判定（0/3 退出码）
    if (pingOnly) {
        printf("\n=== ping-only：只发 Info(0xF0) ===\n");
        const bool a = PingAlive(&d, "PingOnly");
        V2Close(&d);
        printf("exit=%d（3 = 不响应 -> 若怀疑挂死就重新插拔 USB）\n", a ? 0 : 3);
        return a ? 0 : 3;
    }

    // ---------------- S0 建链 ----------------
    // 顺序照 jtagrawprobe 的 JtagPathTest（那条路已在**同一目标**上读通 2 颗 IDCODE），
    // 只多一步"先 Configure"：固件 jtagIF 的 ST_JTAG_WRITEIR 需要 ndevs/irlenx 已就位（候选 C1）。
    printf("\n=== S0 建链（Connect(2) -> JTAG_Configure -> TAP reset -> 手动扫链 -> 校正 IR -> Ping）===\n");
    // SWJ_Clock / SWJ 切换序列都是**准备性**步骤（软失败）：它们不响应不代表设备死了，
    // 不该把后面的 Connect 一起拦掉。
    if (clockHz) { g_softFail = true; SwjClock(&d, clockHz); g_softFail = false; }
    if (swjSwitch) { g_softFail = true; SwjSwitchSeq(&d); g_softFail = false; }
    if (Connect2(&d, 2, "Connect(2 JTAG)") != 2) {
        printf("!! 固件没进 JTAG。若是刚用过 SWD：本目标 SWJ-DP 的 JTAG<-SWD 单向，"
               "**给目标断电重启**再试。\n");
        PingAlive(&d, "Connect 失败后 Ping（死活判定）");
        g_failed = false; g_stopOnFail = false;
        if (!noSwd) Connect2(&d, 1, "Connect(1 SWD restore)");
        V2Close(&d);
        return 1;
    }

    // (0.9) ★ 补发 0x04 TRANSFER_CONFIG（探针此前**从不发它**，而本层是发的）。
    //       放在 0x15 之前：它给固件侧 0x05/0x06 的 WAIT/MATCH 重试设上限，
    //       若"执行即挂"的真因是"wait_retry 默认为死等"，补上它 0x06 写就该活。
    if (g_xferCfg) TransferConfigure(&d, "TRANSFER_CONFIG(0x04)");

    // (1) 先按"假定链"配置固件：ndevs + 各 TAP 的 IR 长度
    uint8_t irs[8] = {0};
    for (int i = 0; i < taps && i < 8; ++i) irs[i] = (i < irOvCount) ? irOv[i] : irOv[irOvCount - 1];
    JtagConfigure(&d, cfgOp, taps, irs);

    // (2) 手动扫链读真实 IDCODE（**不依赖固件的 0x16**，它在同一目标上回 count=0）
    uint32_t ids[8] = {0};
    const bool chainOk = JtagReadIdcodes(&d, taps, ids);

    // (3) 对照：固件 0x16 仅作信息（不作为前置条件）
    {
        uint32_t fwIds[8] = {0};
        int fwCount = 0;
        JtagIdcode(&d, fwIds, &fwCount);
        printf("      -> 对照：固件 0x16 回报 count=%d（本目标已知为 0，不影响后续）\n", fwCount);
    }

    // (3.5) ★ 闸门：链路无效就**立刻中止** —— 实测（2026-10-03）在"TDO 全 1（悬空）"的坏链路上
    //       跑 0x06 **写**会直接把探针跑挂，所以这种状态下不允许继续加码。
    if (!chainOk && !force) {
        printf("\n!! **中止**：JTAG 链无效（IDCODE 全 0 / 全 1）。此状态下继续跑 0x06 无意义，\n"
               "   且实测会把探针**跑挂**（0x06 写）。\n");
        printf("   最常见原因：**上一次收尾把 SWJ-DP 切回了 SWD** —— 本目标 SWJ-DP 的 JTAG<-SWD 是\n"
               "   单向的，切回 SWD 后本上电周期内**再也进不了 JTAG**。\n");
        printf("   处理：① 给**目标板断电重启**（不是探针）；② 之后运行**不要**加 --restore-swd。\n");
        printf("   先确认通路：bin\\jtagrawprobe.exe --path\n");
        printf("   强行继续（不建议）：--force。\n");
        V2Close(&d);
        return 1;
    }
    if (!chainOk && force) {
        printf("\n⚠️  --force：链路无效仍继续 —— 0x06 **写**很可能把探针跑挂（须重新插拔 USB）。\n");
    }

    // (4) 用真实 IDCODE 校正 IR 表，不一致就**重配置一次**（让固件与真实链一致）
    if (chainOk) {
        uint8_t fixed[8] = {0};
        for (int i = 0; i < taps && i < 8; ++i) fixed[i] = (uint8_t)IrLenForId(ids[i]);
        bool same = true;
        for (int i = 0; i < taps; ++i) if (fixed[i] != irs[i]) same = false;
        if (!same) {
            memcpy(irs, fixed, sizeof(irs));
            JtagConfigure(&d, cfgOp, taps, irs);
        } else {
            printf("      -> IR 表与真实链一致，无需重配置\n");
        }
    } else {
        printf("!! 手动扫链也没读到有效 IDCODE（TDO 全 0/全 1？）。继续跑只会得到无意义的 ACK，\n"
               "   建议先用 jtagrawprobe.cpp --path 诊断通路（引脚/位拷）。\n");
    }
    // (4.5) ★ IR 往返自检（**安全步骤**：走 0x14 原始位流，不经 DAP 事务）。默认做。
    //       目的：把"IR/位流引擎本身坏没坏"与"DAP 事务路径坏没坏"**分开** —— 若这一步安全通过，
    //       说明卡的确实在 DAP 事务那条路里（0x05/0x06 写）；若这一步就挂，则问题更基础。
    if (!noIrRt && !forceIrRt && stageMax >= 2) {
        printf("  !! **自动跳过** IR 往返自检：它会改写 IR，而 0x05/0x06 阶段一旦‘IR 被外部改写’\n"
               "     就**必挂**（bug.md B9：第五/七轮 vs 第四/六轮，2:2 可重复）—— 跑了等于白烧\n"
               "     一次探针插拔。要强制做这个诊断：显式 --ir-rt；只想诊断不想跑 0x06：--stage 1。\n");
        noIrRt = true;
    }
    if (!noIrRt) {
        uint32_t rt0 = 0;
        JtagIrRoundTrip(&d, irs, taps, &rt0);
    }
    const bool alive0 = PingAlive(&d, "S0 建链后 Ping");

    // ---------------- S1 0x05 对照 ----------------
    bool alive1 = alive0;
    if (!skip05 && stageMax >= 1) {
        printf("\n=== S1 0x05 对照（DP IDCODE 读 + RDBUFF 读；用于判断通路是否本来就通）===\n");
        printf("    （0x05 在 JTAG 下**已知不可用**，所以本段是**软失败**：超时不中断、不影响 S2）\n");
        g_softFail = true;
        uint32_t v0 = 0, v1 = 0;
        DapTransfer05(&d, REQ(0, 1, 0) /*DPIDR*/, NULL, NULL, &v0, "0x05 read DPIDR");
        DapTransfer05(&d, REQ(3, 1, 0) /*RDBUFF*/, NULL, NULL, &v1, "0x05 read RDBUFF");
        if (v1 == 0x4BA00477u || v1 == 0x2BA01477u || v0 == 0x4BA00477u || v0 == 0x2BA01477u)
            printf("      -> 看起来读到了 DP IDCODE（0x%08X / 0x%08X）\n", v0, v1);
        else
            printf("      -> 基线：JTAG 下 0x05 读不到 DP IDCODE（与 §18.9 第八步一致）\n");
        g_softFail = false;
        alive1 = PingAlive(&d, "S1 0x05 之后 Ping");
    }

    // ---------------- S2 0x06 count=1 读（最小风险；--stage 1 = 只到"建链 + 0x05"，不发 0x06） ----------------
    TbResult r2;
    memset(&r2, 0, sizeof(r2));
    r2.n = -3; r2.ack = -1; r2.respCount = -1;
    bool alive2 = alive1;
    if (stageMax >= 2 && !writeFirst) {
        printf("\n=== S2 0x06 最小步骤：count=1 读 DP IDCODE ===\n");
        r2 = TransferBlock(&d, 1, REQ(0, 1, 0), NULL, "0x06 read DP IDCODE x1");
        alive2 = PingAlive(&d, "S2 之后 Ping");
        RecordStep("S2 0x06 count=1 读 DP", &r2, alive2);
    } else if (writeFirst) {
        printf("\n=== S2 跳过（--write-first：**不做前置读**，直接进 S3 写）===\n");
    } else {
        printf("\n=== S2 跳过（--stage 1：只做到建链 + 0x05 对照，**不发 0x06**）===\n");
    }

    // ---------------- S2.5 ★ 判别实验二（--clock-late） ----------------
    //   基线（S2 那笔 0x06 读）已经跑完，这里**单独**补发一笔 0x11 SWJ_Clock，再重复 S2。
    //   唯一变量 = 这笔 0x11 的取值 ⇒ 判别"挂死是否由 JTAG 时钟域（TCK 停摆）引起"。
    //   对照（HZ=1000000，与 S0 同值）能排除"第二笔 0x06 读本身就挂"这个混淆项。
    if (g_clockLate && stageMax >= 2) {
        printf("\n=== S2.5 判别实验二：重发 SWJ_Clock(0x11) = %u Hz -> 再发同一笔 0x06 读 ===\n", g_clockLateHz);
        g_softFail = true; SwjClock(&d, g_clockLateHz); g_softFail = false;
        const bool aliveC = PingAlive(&d, "发 0x11 之后 Ping");
        if (aliveC) {
            const TbResult rc = TransferBlock(&d, 1, REQ(0, 1, 0), NULL, "0x06 read DP IDCODE x1 (0x11 后)");
            // ★ 关键：上一笔 0x06 一旦失败，Cmd() 会因 g_failed 而**早退**（"SKIP"），
            //   于是这条"死活判定 Ping"根本没发出去 —— 那 "挂死" 就是假结论。
            //   判定死活是本模式的唯一目的，所以这里强制把停止开关清掉，逼 Ping 真的发出去。
            g_failed = false; g_stopOnFail = false;
            const bool aliveC2 = PingAlive(&d, "0x11+0x06 之后 Ping");
            RecordStep("S2.5 0x11 后 0x06 count=1 读", &rc, aliveC2);
            printf("      => %s\n", (rc.n > 0 && aliveC2)
                   ? "【排除时钟停摆】换 0x11 取值后 0x06 读仍秒回 ⇒ 时钟域与本次挂死无关（回查 B11.6 (a)/(b)）"
                   : "【命中】0x11 之后 0x06 无应答/挂死 ⇒ 坐实‘换分频 ⇒ TCK 停摆 ⇒ 永久等待’");
        } else {
            printf("      => 【意外】连 0x11 之后的 Ping 都没回应 ⇒ 这笔 0x11 本身就把设备弄哑了\n");
        }
    }

    // ---------------- S3 0x06 count=1 写 ----------------
    bool alive3 = alive2;
    if (stageMax >= 3 && (writeFirst || r2.n > 0)) {
        printf("\n=== S3 0x06 count=1 写（DP SELECT <- 0）%s ===\n",
               writeFirst ? "★ write-first：这是本次会话**第一笔 0x06**" : "");
        const uint32_t sel = 0;
        TbResult r3 = TransferBlock(&d, 1, REQ(2, 0, 0) /*DP SELECT 写*/, &sel,
                                    "0x06 write DP SELECT x1");
        alive3 = PingAlive(&d, "S3 之后 Ping");
        RecordStep("S3 0x06 count=1 写 DP SELECT", &r3, alive3);
        if (alive3 && !skip05) {
            uint32_t rb = 0;
            DapTransfer05(&d, REQ(2, 1, 0) /*SELECT 读*/, NULL, NULL, &rb, "0x05 read DP SELECT");
        }
    } else if (stageMax >= 3) {
        printf("\n=== S3 跳过（S2 没有应答；严格按风险控制：不继续加码）===\n");
    }

    // ---------------- S3.5 可选：TAP reset + 重发 0x15，夹在两次 0x06 之间（--reset-between） ----------------
    if (resetBetween && alive3) {
        printf("\n=== S3.5 TAP reset + 重发 0x15（赶在第二笔 0x06 之前，试探能否解开被第一笔楔住的 FSM）===\n");
        JtagSeq1(&d, 6, 1, 0, NULL, "TMS reset (6)");
        JtagSeq1(&d, 1, 0, 0, NULL, "-> Run-Test/Idle");
        JtagConfigure(&d, cfgOp, taps, irs);
        alive3 = PingAlive(&d, "S3.5 之后 Ping");
        printf("      -> %s\n", alive3 ? "仍活着：0x14/0x15 未挂，可继续看 S4"
                                         : "0x14/0x15 也挂了 ⇒ 楔住的是整条 JTAG 路径（不止 DAP 事务）");
    }

    // ---------------- S3.6 可选：全套重初始化，夹在两次 0x06 之间（--reconnect-between） ----------------
    if (reconnectBetween && alive3) {
        printf("\n=== S3.6 全套重初始化（TAP reset -> Disconnect(0x03) -> Connect(0x02 JTAG) -> 0x15）===\n");
        JtagSeq1(&d, 6, 1, 0, NULL, "TMS reset (6)");
        {
            uint8_t r[64];
            const uint8_t c[1] = {CMD_DISCONNECT};
            Cmd(&d, c, sizeof(c), r, sizeof(r), 800, "Disconnect(0x03)");
        }
        Connect2(&d, 2, "Re-Connect(2 JTAG)");
        JtagConfigure(&d, cfgOp, taps, irs);
        alive3 = PingAlive(&d, "S3.6 之后 Ping");
        printf("      -> %s\n", alive3 ? "仍活着：重初始化完成，看着 S4 能不能救回来"
                                         : "重初始化途中就挂了");
    }

    // ---------------- S4/S5 --deep ----------------
    if (deep && stageMax >= 4 && alive3) {
        printf("\n=== S4 0x06 count=2 读（验证 count>1 与 JTAG 延迟读）===\n");
        TbResult r4 = TransferBlock(&d, 2, REQ(0, 1, 0), NULL, "0x06 read DP x2");
        const bool alive4 = PingAlive(&d, "S4 之后 Ping");
        RecordStep("S4 0x06 count=2 读 DP", &r4, alive4);

        if (stageMax >= 5 && alive4) {
            printf("\n=== S5 0x06 读 AP CSW（可能因 AP 未上电而 FAULT；只关心有没有应答）===\n");
            TbResult r5 = TransferBlock(&d, 1, REQ(0, 1, 1), NULL, "0x06 read AP CSW x1");
            const bool alive5 = PingAlive(&d, "S5 之后 Ping");
            RecordStep("S5 0x06 count=1 读 AP CSW", &r5, alive5);
        }
    }

    PrintVerdict();

    // ---------------- 收尾 ----------------
    // 收尾必须无条件尝试（哪怕前面已判定失败）—— 它的作用是"给设备留个可用状态"和
    // "再取一次存活证据"，所以这里显式清掉停止开关。
    printf("\n=== 收尾 ===\n");
    g_failed = false;
    g_stopOnFail = false;
    if (!noSwd) {
        printf("  --restore-swd：切回 SWD 并 Disconnect。\n"
               "  ⚠️ 本目标 SWJ-DP 的 JTAG<-SWD 单向：切回后**本上电周期内再也进不了 JTAG**，\n"
               "     下次要测 JTAG 必须给**目标板断电重启**。\n");
        Connect2(&d, 1, "Connect(1 SWD restore)");
        {
            uint8_t r[64];
            const uint8_t c[1] = {CMD_DISCONNECT};
            Cmd(&d, c, sizeof(c), r, sizeof(r), 800, "Disconnect");
        }
    } else {
        {
            uint8_t r[64];
            const uint8_t c[1] = {CMD_DISCONNECT};
            Cmd(&d, c, sizeof(c), r, sizeof(r), 800, "Disconnect（保持 JTAG）");
        }
        printf("  默认**不**切回 SWD（保持 JTAG 模式），方便紧接着再跑一次复验；\n"
               "  要回 SWD（Keil/常规调试）：给**目标板断电重启**，或显式加 --restore-swd。\n");
    }
    V2Close(&d);
    printf("done.\n");
    return 0;
}
