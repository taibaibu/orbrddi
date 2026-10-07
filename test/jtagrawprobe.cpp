// JTAG 通路直连探测（**直接**走 WinUSB，不经过 ORBMDK 驱动层）
//
// 目的（§18.9）：本层已实现 JTAG 建链（Port=JTAG → Connect(2) → JTAG_Configure →
// TMS 复位 → JTAG_IDCODE 扫链），但扫链结果为 0。要判定卡在哪一层：
//   A) 固件根本不支持 JTAG（Connect(2) 被强行回成 1=SWD，或 JTAG_* 命令返回非 0）；
//   B) 固件支持，是本层 TMS/IR 序列的细节不对；
//   C) 目标板没引出 TDI/TDO/TRST（那就无论如何都不通，见 §18.9 第 5 条）。
//
// 直接对固件发原始 CMSIS-DAP 帧，逐个隔离上面三种情况，避免每次改驱动都要重编。
//
// 操作码取自 include/ORBMDK_DAP.h（与本层发送的完全一致）：
//   0x00 Info  0x02 Connect  0x03 Disconnect  0x12 SWJ_Sequence
//   0x14 JTAG_Sequence  0x15 JTAG_Configure  0x16 JTAG_IDCODE
//
// 2026-10-03：GetIDCODEs 只回报 DP 一个（Keil 因此只画 1 行）的旧问题已修复，详见
// src/ORBMDK_RDDI.cpp 的 DetectTargetDapIdList（JTAG 模式按扫链结果逐 TAP 回报）。
// 本工具的 `--no-swd --path` 仍可用于直接验证链上 2 颗（cpu + bs）。
//
// 构建： powershell -File test\build_test.ps1 -Source jtagrawprobe.cpp
// 运行： bin\jtagrawprobe.exe [--no-swd] [--quick] [--path]
//                            [--bitbang] [--mode-switch] [--no-stop] [--retry N] [--timeout MS]
//       （目标板接好并上电；Keil 必须关闭）
//
// ⚠️ 出包一律 64 字节（短包）—— 绝不用端点 wMaxPacketSize(512) 的整包，
//    否则固件认为传输未结束、命令永不派发（§17.2）。
// ⚠️ 结束前恢复 SWD 并 Disconnect，绝不把设备留在 JTAG/异常态（上次的教训）。
//
// ⚠️⚠️ 失败处理（2026-10-03 加固）—— orbtrace 在 JTAG 下**容易挂死**：已知的一条是
//    JTAG 模式下收到 `ID_DAP_TRANSFER_BLOCK`(cmd=0x06) 后**不回应答并停止服务 OUT 端点**，
//    只能重新插拔 USB（COMPAT_ANALYSIS §18.9 第十步；本程序**不发送** 0x06）。
//    ⇒ 0x06 的**专项复验**已独立成 `test\jtagblockprobe.cpp`（它会真发 0x06，风险自担）。
//      注意该记录"**现象可靠、定性待更正**"：orbtrace 1.4.3 的固件其实**实现了** JTAG 下的
//      块传输（见 bug.md B9），挂死根因未定性 —— 判定用 jtagblockprobe 的"每步 Ping"分界。
//    挂死后的表现：**每一条命令都立刻 write FAILED**。旧版本"失败也继续往下打"，会把已经
//    挂死的探针反复怼（上百条命令、每条还要等超时），看起来就是"必定卡死"。现在：
//      ① 默认 **任一步失败即停**（`--no-stop` 关闭该行为）；
//      ② 重发/超时收紧：默认 **1 次重发、800 ms**（`--retry N` / `--timeout MS`）；
//      ③ **引脚位拷 [13]/[14] 与"来回切模式"的段默认不跑**，需显式 `--bitbang` /
//         `--mode-switch`（这两段命令最密集，最可能把探针怼死）。
//    一旦看到 `!! 首次失败`：**重新插拔探针**再跑，不要原地重试。

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
#define CMD_SWJ_PINS        0x10
#define CMD_SWJ_CLOCK       0x11
#define CMD_SWJ_SEQUENCE    0x12
#define CMD_JTAG_SEQUENCE   0x14
#define CMD_JTAG_CONFIGURE  0x15
#define CMD_JTAG_IDCODE     0x16

// 是否允许在任何 JTAG 动作之前去碰 SWD（Connect(0)/Connect(1)）。
//   ★ 关键：本目标（STM32 一类 SWJ-DP）的 JTAG->SWD 切换是**单向**的 ——
//     上电后执行过一次 SWD，就再也回不到 JTAG（实测：补发 ADIv5 的 SWD->JTAG
//     序列 0xE73C 也无效），只能重新给目标上电。
//   所以"验证 JTAG 是否通"必须在**上电后第一次连接就用 JTAG**，
//   探测程序自己更不能先来一发 Connect(0)。
//   --no-swd  即"JTAG-only"模式：跳过所有 Connect(0)/Connect(1)，也不做 SWD 收尾。
static bool g_allowSwd = true;

// ---------------------------------------------------------------------------
// 失败处理（见文件头 ⚠️⚠️ 一节）。核心：**任一步失败即停**，不再对已挂死的设备继续下发。
//   MarkFail()  记录首次失败；g_stopOnFail 为真时后续所有 Cmd/写读直接 SKIP。
//   Aborted()   各段/各循环用它提前收尾（避免刷屏几百行 SKIP）。
// ---------------------------------------------------------------------------
static bool g_stopOnFail   = true;    // 首次失败即停止后续全部命令（--no-stop 关闭）
static int  g_maxAttempts  = 1;       // Cmd() 重发次数上限（首条可能被吞；需要时 --retry 2）
static int  g_ioTimeoutMs  = 800;     // 单条命令的写/读超时（ms；--timeout 调整）
static bool g_doBitBang    = false;   // [13]/[14] 引脚位拷（默认关，需 --bitbang）
static bool g_doModeSwitch = false;   // 来回切模式的段（默认关，需 --mode-switch）
static bool g_failed       = false;   // 粘性：一旦失败就不再发命令

static void MarkFail(const char* where)
{
    if (g_failed) return;
    g_failed = true;
    if (!g_stopOnFail) return;
    printf("\n!! 首次失败：%s\n", where);
    printf("!! 已停止后续全部命令（--no-stop 可继续下发）。\n");
    printf("!! 若连 Info/Connect 也写不进去，说明探针已挂死 -> **重新插拔 USB** 再跑。\n");
    printf("!! （orbtrace 在 JTAG 下的已知挂死模式见 COMPAT_ANALYSIS §18.9 第十步）\n");
}

static bool Aborted(void) { return g_stopOnFail && g_failed; }

typedef struct {
    HANDLE  dev;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR   inPipe;
    UCHAR   outPipe;
} V2Dev;

// --no-swd 保护：端口不是 JTAG(2) 时，JTAG-only 模式下直接跳过。
// （必须放在 V2Dev 定义之后 —— 否则签名里的 V2Dev 未知，整个函数会被编译器丢弃，
//   调用处报 C3861"找不到标识符"。踩过一次。）
static int ConnectSwdIfAllowed(V2Dev* d, int port, const char* name);

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
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(di, &did, det, need, NULL, NULL)) {
            free(det);
            continue;
        }

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

// 单次读；返回字节数，-1 失败，-2 超时
//
// ⚠️ 超时后的收尾必须彻底（这是本探测程序之前"跑着跑着就崩"的原因，也是
//    ORBMDK 驱动修过的同一个坑）：ov 是**栈上**变量、hEvent 是句柄，
//    若 IRP 还没完成就 CloseHandle + 返回，驱动手里还攥着这块内存 →
//    崩在后续操作里。所以：CancelIoEx → 等待 → 仍不回来就 AbortPipe 强制收尾
//    → 再等 → 只有确认完成才 CloseHandle；否则**故意弃置**（宁可泄漏少量内存）。
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
            if (WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0) {
                DWORD dummy = 0;
                GetOverlappedResult(d->dev, &ov, &dummy, FALSE);
                ret = -2;
            } else {
                // WinUsb_AbortPipe 是同步的：返回即该管线上的 IRP 已完成
                WinUsb_AbortPipe(d->winusb, d->inPipe);
                WinUsb_ResetPipe(d->winusb, d->inPipe);
                if (WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0) {
                    DWORD dummy = 0;
                    GetOverlappedResult(d->dev, &ov, &dummy, FALSE);
                }
                ret = -2;
            }
        }
    } else { ret = ok ? (int)rd : -1; }
    if (msOut) *msOut = GetTickCount64() - t0;
    if (WaitForSingleObject(ov.hEvent, 0) == WAIT_OBJECT_0 || ret != -2) {
        CloseHandle(ov.hEvent);
    }
    return ret;
}

static int DrainIn(V2Dev* d, int ms)
{
    uint8_t tmp[1024];
    int total = 0;
    for (int i = 0; i < 4; ++i) {
        const int n = ReadOnce(d, tmp, sizeof(tmp), ms, NULL);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

static void Hex(const uint8_t* p, int n)
{
    for (int i = 0; i < n && i < 16; ++i) printf(" %02X", p[i]);
    if (n > 16) printf(" ...");
}

static int Connect2(V2Dev* d, int port, const char* name);   // 定义在下方

static int ConnectSwdIfAllowed(V2Dev* d, int port, const char* name)
{
    if (!g_allowSwd && port != 2) {
        printf("  %-22s SKIP（--no-swd：绝不在 JTAG 之前碰 SWD）\n", name);
        return -1;
    }
    return Connect2(d, port, name);
}

// 发一条命令（出包 64 字节短包；"打开后第一条可能被吞"，故最多重发 2 次）
// （ConnectSwdIfAllowed 定义见下方 Connect2 之后）
static int Cmd(V2Dev* d, const uint8_t* cmd, int cmdLen, uint8_t* resp, int respCap,
               int readMs, const char* name)
{
    if (Aborted()) { printf("  %-22s SKIP（已失败停止，不再下发）\n", name); return -1; }
    uint8_t tx[1024];
    memset(tx, 0, sizeof(tx));
    memcpy(tx, cmd, cmdLen);

    // 统一收紧读超时：挂死的设备每条都要等满超时，旧值 1.5 s × 上百条 = "卡死"观感
    if (readMs > g_ioTimeoutMs) readMs = g_ioTimeoutMs;

    for (int attempt = 0; attempt < g_maxAttempts; ++attempt) {
        if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
            WinUsb_AbortPipe(d->winusb, d->outPipe);
            WinUsb_ResetPipe(d->winusb, d->outPipe);   // 写失败必复位，避免一直超时
            printf("  %-22s write FAILED (attempt %d/%d)\n", name, attempt + 1, g_maxAttempts);
            if (attempt + 1 >= g_maxAttempts) MarkFail(name);
            continue;
        }
        ULONGLONG ms = 0;
        const int n = ReadOnce(d, resp, (ULONG)respCap, readMs, &ms);
        if (n > 0) {
            printf("  %-22s len=%-3d (%4llu ms):", name, n, ms);
            Hex(resp, n);
            printf("\n");
            return n;
        }
        printf("  %-22s no response (rc=%d, %llu ms)%s\n", name, n, ms,
               (attempt + 1 < g_maxAttempts) ? " -> 重发（首条可能被吞）" : "");
        if (attempt + 1 >= g_maxAttempts) MarkFail(name);
    }
    return -1;
}

static void ShowCaps(V2Dev* d)
{
    uint8_t r[64];
    uint8_t c[2] = {CMD_INFO, 0xF0};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1000, "Info(0xF0 caps)");
    if (n >= 3) {
        const uint8_t caps = r[2];
        printf("      -> caps = 0x%02X  [SWD=%d JTAG=%d SWO_UART=%d SWO_MANCH=%d "
               "ATOMIC=%d STREAM=%d]\n",
               caps, caps & 1, (caps >> 1) & 1, (caps >> 2) & 1, (caps >> 3) & 1,
               (caps >> 4) & 1, (caps >> 5) & 1);
    }
}

// 返回固件实际建立的模式：0/1=SWD，2=JTAG，<0 失败
static int Connect2(V2Dev* d, int port, const char* name)
{
    uint8_t r[64];
    uint8_t c[2] = {CMD_CONNECT, (uint8_t)port};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, name);
    if (n < 2) return -1;
    printf("      -> 固件回报的 port/mode = %d  (%s)\n", r[1],
           (r[1] == 2) ? "JTAG ✓" : (r[1] == 1 || r[1] == 0) ? "SWD" : "未知");
    return r[1];
}

static void JtagConfigure(V2Dev* d, uint8_t devCount, uint8_t ir)
{
    uint8_t r[64];
    uint8_t c[3] = {CMD_JTAG_CONFIGURE, devCount, ir};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, "JTAG_Configure");
    if (n >= 2) printf("      -> status = %d\n", r[1]);
}

// 多器件版：IR 数组长度必须 = devCount（少发字节会被固件判为非法请求 -> status 0xFF）
static void JtagConfigureN(V2Dev* d, int devCount, const uint8_t* irs, const char* tag)
{
    uint8_t r[64];
    uint8_t c[2 + 8];
    c[0] = CMD_JTAG_CONFIGURE;
    c[1] = (uint8_t)devCount;
    memcpy(&c[2], irs, (size_t)devCount);
    const int n = Cmd(d, c, (size_t)(2 + devCount), r, sizeof(r), 1500, tag);
    if (n >= 2) printf("      -> status = %d\n", r[1]);
}

static void JtagIdcode(V2Dev* d, const char* tag)
{
    uint8_t r[64];
    uint8_t c[1] = {CMD_JTAG_IDCODE};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, tag);
    if (n >= 2) {
        const int cnt = r[1];
        printf("      -> IDCODE count = %d", cnt);
        for (int i = 0; i < cnt && (3 + i * 4 + 3) < n; ++i) {
            const uint32_t id = (uint32_t)r[2 + i * 4] | ((uint32_t)r[3 + i * 4] << 8) |
                                ((uint32_t)r[4 + i * 4] << 16) | ((uint32_t)r[5 + i * 4] << 24);
            printf("  id[%d] = 0x%08X", i, id);
        }
        printf("\n");
    }
}

// TMS 序列（TAP 复位）：info bit0=TDO捕获, bit1=TMS 序列；8 位数据 0x1F = 11111 00
static void JtagTmsReset(V2Dev* d)
{
    uint8_t r[64];
    uint8_t c[4] = {CMD_JTAG_SEQUENCE, 0x03, 8, 0x1F};
    Cmd(d, c, sizeof(c), r, sizeof(r), 1500, "JTAG_Seq(TMS reset)");
}

// 纯 SWJ 序列发 JTAG 复位（TMS 高 5 拍 + 低 2 拍），info bit0=1 表示 TMS 序列
static void SwjJtagReset(V2Dev* d)
{
    uint8_t r[64];
    uint8_t c[4] = {CMD_SWJ_SEQUENCE, 8, 0x1F, 0x00};   // count=8, data=0x1F
    Cmd(d, c, sizeof(c), r, sizeof(r), 1500, "SWJ_Seq(TMS reset)");
}

// ★ 判定 TDO 线上到底有没有东西：
//   把 TAP 送进 Shift-DR，再移 32 位（TDI=0）并**读回原始 TDO**。
//   - 全 0x00 → TDO 恒低：线路没接 / 器件没在驱动（引脚未引出、被当 GPIO、JTAG 被选项字节关掉）
//   - 全 0xFF → TDO 悬空被上拉
//   - 有变化 → 链上有器件响应（那就该是 IDCODE 0x4BA00477 之类的位模式）
//
//   TMS 序列（9 位，LSB first）：1,1,1,1,1(Test-Logic-Reset) 0(Run-Test/Idle)
//                                1(Select-DR) 0(Capture-DR) 0(Shift-DR) → 字节 0x5F,0x00
static void CharacterizeTdo(V2Dev* d, uint8_t infoTms, uint8_t infoShift)
{
    uint8_t r[64];

    uint8_t c1[5] = {CMD_JTAG_SEQUENCE, infoTms, 9, 0x5F, 0x00};
    Cmd(d, c1, sizeof(c1), r, sizeof(r), 1500, "JTAG_Seq(TMS->ShiftDR)");

    uint8_t c2[7] = {CMD_JTAG_SEQUENCE, infoShift, 32, 0, 0, 0, 0};
    const int n = Cmd(d, c2, sizeof(c2), r, sizeof(r), 1500, "JTAG_Seq(shift32,read)");
    if (n > 2) {
        bool allZero = true, allOne = true;
        for (int i = 2; i < n; ++i) {
            if (r[i] != 0x00) allZero = false;
            if (r[i] != 0xFF) allOne = false;
        }
        printf("      -> TDO 判定: %s\n",
               allZero ? "全 0 = TDO 无器件驱动（线路没接 / 引脚被复用 / JTAG 被关）"
             : allOne ? "全 1 = TDO 悬空被上拉（同样说明链上无器件）"
                      : "有变化 = 链上有器件响应（再看上面的字节是否像 IDCODE）");
    }
}

// 读接口引脚状态（DAP_SWJ_Pins 只读）：
//   bit0 TCK/SWCLK, bit1 TMS/SWDIO, bit2 TDI, bit3 TDO, bit4 nTRST, bit5 nRESET
// 用途：看 TDO 线的静态电平（判断线是否被驱动/是否悬空）。
static void JtagPinRead(V2Dev* d, const char* tag)
{
    uint8_t r[64];
    // SWJ_Pins: [cmd][pinOut][pinSelect][wait 4B]（pinSelect=0 表示只读不驱动）
    uint8_t c[7] = {CMD_SWJ_PINS, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, tag);
    if (n >= 2) {
        const uint8_t p = r[n - 1];
        printf("      -> pins=0x%02X  TCK/SWCLK=%d TMS/SWDIO=%d TDI=%d TDO=%d nTRST=%d nRESET=%d\n",
               p, p & 1, (p >> 1) & 1, (p >> 2) & 1, (p >> 3) & 1, (p >> 4) & 1, (p >> 5) & 1);
    }
}

// 穷举 JTAG_Sequence 的 info 位组合，找"会回 TDO 数据"的那一种
static void InfoMatrix(V2Dev* d)
{
    const uint8_t tmsVals[2] = {0x02, 0x03};
    const uint8_t shiftVals[4] = {0x00, 0x01, 0x02, 0x03};
    uint8_t r[128];

    for (int t = 0; t < 2; ++t) {
        if (Aborted()) { printf("  [10c] 提前收尾（已失败停止）\n"); return; }
        uint8_t prep[5] = {CMD_JTAG_SEQUENCE, tmsVals[t], 9, 0x5F, 0x00};
        Cmd(d, prep, sizeof(prep), r, sizeof(r), 1500, "  TMS->ShiftDR");

        for (int s = 0; s < 4; ++s) {
            uint8_t c[7] = {CMD_JTAG_SEQUENCE, shiftVals[s], 32, 0, 0, 0, 0};
            char nm[48];
            sprintf_s(nm, sizeof(nm), "  shift32 info=0x%02X", shiftVals[s]);
            const int n = Cmd(d, c, sizeof(c), r, sizeof(r), 1500, nm);
            if (n > 2) {
                printf("        ^ 有额外数据（很可能是 TDO）:");
                Hex(&r[2], n - 2);
                printf("\n");
            }
        }
        // 回到 Shift-DR 再进下一轮前，先复位 TAP
        uint8_t rst[4] = {CMD_JTAG_SEQUENCE, 0x02, 8, 0x1F};
        Cmd(d, rst, sizeof(rst), r, sizeof(r), 1500, "  TAP reset");
    }
}

// ★ 关键：把一条命令的**多段响应**全部读出来。
//   这份固件的响应会"分片/滞后"（§17 期间在 DAP_Info 上实测过），
//   每条命令只读一段就可能把 TDO 数据误判成"固件不回"。
static void MultiSeg(V2Dev* d, const uint8_t* cmd, int cmdLen, const char* name, int segs)
{
    if (Aborted()) { printf("  %-24s SKIP（已失败停止）\n", name); return; }
    uint8_t tx[1024];
    memset(tx, 0, sizeof(tx));
    memcpy(tx, cmd, cmdLen);

    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        WinUsb_AbortPipe(d->winusb, d->outPipe);
        WinUsb_ResetPipe(d->winusb, d->outPipe);
        printf("  %-24s write FAILED\n", name);
        MarkFail(name);
        return;
    }
    printf("  %-24s", name);
    int total = 0;
    for (int i = 0; i < segs; ++i) {
        uint8_t r[256];
        ULONGLONG ms = 0;
        const int n = ReadOnce(d, r, sizeof(r), (i == 0) ? g_ioTimeoutMs : 400, &ms);
        if (n <= 0) { printf("  [seg%d rc=%d]", i, n); break; }
        printf("  [seg%d len=%d%s]", i, n, (ms > 200) ? " 迟到" : "");
        Hex(r, n);
        total += n;
    }
    printf("   (共 %d 字节)\n", total);
}

// ★ SWD → JTAG 切换序列（ADIv5 / OpenOCD 的 swd_seq_swd_to_jtag）
//   ① 线复位：TMS/SWDIO 保持 1 连续 ≥50 个时钟
//   ② 在 TMS/SWDIO 线上发 16 位 0xE73C（LSB first）→ SWJ-DP 从 SWD 切到 JTAG
//
//   为什么必须发：SWJ-DP 是有"模式记忆"的。我们刚刚用 SWD 跟它通信过，
//   它就停在 SWD 模式；gateware 的 CMD_SET_JTAG 只是把**探针侧**切到 JTAG 硬件，
//   不会替宿主发这个模式切换序列（jtagIF.v 只实现 IR/TFR/READID/RESET 四件事）。
//   不发就直接扫链，TDO 上自然什么都没有 → IDCODE count = 0。
//
//   这两步走 SWJ 序列（SWDIO 与 TMS 是同一根物理线），与 §3.4 的 JTAG→SWD 对称。
static void SwjSwitchToJtag(V2Dev* d)
{
    uint8_t r[64];

    uint8_t c1[11];
    c1[0] = CMD_SWJ_SEQUENCE;
    c1[1] = 64;                          // 64 个 1（>50 拍线复位）
    memset(&c1[2], 0xFF, 8);
    Cmd(d, c1, 10, r, sizeof(r), 1500, "SWJ_Seq(line reset 64x1)");

    uint8_t c2[5] = {CMD_SWJ_SEQUENCE, 16, 0x3C, 0xE7, 0x00};   // 0xE73C, LSB first
    Cmd(d, c2, 4, r, sizeof(r), 1500, "SWJ_Seq(0xE73C -> JTAG)");
}

// ============================================================================================
// 引脚级判定（不经过 gateware 的 JTAG 引擎）
//
// 依据 orbtrace-1.4.3/verilog/dbgIF.v 的 CMD_PINS_WRITE：
//   "write pins specified in pinsin[7:0], masked by pinsin[15:8], wait and then
//    return pins in pinsout[7:0]"
//   位含义： bit0=SWCLK/TCK  bit1=SWDIO/TMS  bit2=TDI  bit3=TDO(只读)
//            bit4=SWO写      bit7=nRESET(可驱)
// 映射到 CMSIS-DAP: [0x07][PinOutput][PinSelect][Wait 4B] -> [PinState]
// ============================================================================================
#define PIN_TCK    0x01
#define PIN_TMS    0x02
#define PIN_TDI    0x04
#define PIN_TDO    0x08
#define PIN_NRESET 0x80

// 只读引脚（不驱动任何东西），返回 pinState；失败 <0
static int PinsRead(V2Dev* d, int readMs, int verbose, const char* tag)
{
    uint8_t r[64];
    uint8_t c[7] = {CMD_SWJ_PINS, 0x00, 0x00, 0, 0, 0, 0};
    const int n = Cmd(d, c, sizeof(c), r, sizeof(r), readMs, tag);
    if (n < 1) return -1;
    const int st = r[n - 1];
    if (verbose) {
        printf("      -> pinState=0x%02X  TCK=%d TMS=%d TDI=%d TDO=%d nRESET=%d\n",
               st, (st & PIN_TCK) ? 1 : 0, (st & PIN_TMS) ? 1 : 0, (st & PIN_TDI) ? 1 : 0,
               (st & PIN_TDO) ? 1 : 0, (st & PIN_NRESET) ? 1 : 0);
    }
    return st;
}

// 静默版：驱动 out 中 sel 选中的引脚并读回（用于位拷，不打日志）
static int PinsDriveQuiet(V2Dev* d, uint8_t out, uint8_t sel)
{
    if (Aborted()) return -1;
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = CMD_SWJ_PINS; tx[1] = out; tx[2] = sel;
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        WinUsb_AbortPipe(d->winusb, d->outPipe);
        WinUsb_ResetPipe(d->winusb, d->outPipe);
        MarkFail("SWJ_Pins(引脚驱动)");
        return -1;
    }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n < 1) MarkFail("SWJ_Pins(引脚读回)");
    return (n >= 1) ? r[n - 1] : -1;
}

// 位拷一个 JTAG 位：TMS/TDI 就绪后给 TCK 一个上升沿，返回采样到的 TDO
static int JtagBit(V2Dev* d, int tms, int tdi)
{
    const uint8_t sel = PIN_TCK | PIN_TMS | PIN_TDI;
    uint8_t out = (uint8_t)((tms ? PIN_TMS : 0) | (tdi ? PIN_TDI : 0));   // TCK=0
    if (PinsDriveQuiet(d, out, sel) < 0) return -1;
    out |= PIN_TCK;                                                        // TCK: 0 -> 1
    const int st = PinsDriveQuiet(d, out, sel);
    return (st < 0) ? -1 : ((st & PIN_TDO) ? 1 : 0);
}

// 完全用引脚位拷读 32 位 DR（TAP 复位后 IR=IDCODE，故 DR 就是 IDCODE）
// 期望：Cortex-M4 的 JTAG-DP = 0x4BA00477；若为 0x2BA01477 说明 DP 仍在 SWD 侧。
static void BitBangIdcode(V2Dev* d)
{
    printf("  [位拷] TAP 复位 + 进 Shift-DR + 移 32 位读 DR\n");

    for (int i = 0; i < 6; ++i) JtagBit(d, 1, 0);   // Test-Logic-Reset
    JtagBit(d, 0, 0);                               // -> Run-Test/Idle
    JtagBit(d, 1, 0);                               // -> Select-DR
    JtagBit(d, 0, 0);                               // -> Capture-DR
    JtagBit(d, 0, 0);                               // -> Shift-DR

    uint32_t v = 0;
    int ones = 0;
    for (int i = 0; i < 32; ++i) {
        const int tdo = JtagBit(d, 0, 0);
        if (tdo < 0) { printf("      位拷中断（引脚写/读失败）\n"); return; }
        if (tdo) { v |= (1u << i); ++ones; }
    }
    JtagBit(d, 1, 0);                               // Exit1-DR
    JtagBit(d, 0, 0);                               // -> Run-Test/Idle

    printf("      -> 位拷读出的 DR = 0x%08X  (TDO 中 1 的个数=%d/32)\n", v, ones);
    if (v == 0x4BA00477u) {
        printf("      => JTAG-DP 存在且链路通！gateware 的 JTAG_IDCODE 命令有 bug\n");
    } else if (v == 0x2BA01477u) {
        printf("      => 读到的是 SW-DP 的 ID：SWJ-DP 仍停在 SWD 模式\n");
    } else if (v == 0) {
        printf("      => TDO 恒 0：链上没有任何器件驱动 TDO（接线/引脚复用/器件未使能）\n");
    } else if (v == 0xFFFFFFFFu) {
        printf("      => TDO 恒 1：TDO 悬空被上拉，同样说明无器件驱动\n");
    } else {
        printf("      => TDO 有变化但不是已知 IDCODE，请看原始值判断位序/相位\n");
    }
}

// 设 SWJ 时钟：DAP_SWJ_Clock = [0x11][Hz little-endian 4B]
static void SwjClock(V2Dev* d, uint32_t hz, int verbose)
{
    uint8_t r[64];
    uint8_t c[6] = {CMD_SWJ_CLOCK,
                    (uint8_t)(hz & 0xFF), (uint8_t)((hz >> 8) & 0xFF),
                    (uint8_t)((hz >> 16) & 0xFF), (uint8_t)((hz >> 24) & 0xFF), 0};
    Cmd(d, c, 5, r, sizeof(r), 1500, verbose ? "SWJ_Clock" : "SWJ_Clock(q)");
}

// ---- DAP_Transfer（JTAG 模式下同样走这条；这才是真正的通路验证）----
// 请求 [0x05][dapId=0][count=1][request][写数据 4B?]；响应 [0x05][status][count][数据 4B]
// request 编码：A[3:2]<<2 | APnDP<<0 | RnW<<1
//   DPIDR 读 0x02 | DP CTRL/STAT 读 0x06 写 0x04 | DP SELECT 写 0x08
//   AP CSW 读 0x03 | AP TAR 写 0x05 | AP DRW 读 0x0F
static uint32_t DapRead(V2Dev* d, uint8_t request, int* stOut, const char* name)
{
    if (Aborted()) { printf("  %-22s SKIP（已失败停止）\n", name); return 0; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = 0x05; tx[1] = 0; tx[2] = 1; tx[3] = request;
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        printf("  %-22s write FAILED\n", name);
        MarkFail(name);
        return 0;
    }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n <= 0) MarkFail(name);
    uint32_t v = 0;
    if (n >= 7) memcpy(&v, &r[3], 4);
    const int st = (n >= 2) ? r[1] : -1;
    if (stOut) *stOut = st;
    printf("  %-22s n=%-2d status=%d -> 0x%08X\n", name, n, st, v);
    return v;
}

static void DapWrite(V2Dev* d, uint8_t request, uint32_t val, const char* name)
{
    if (Aborted()) { printf("  %-22s SKIP（已失败停止）\n", name); return; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = 0x05; tx[1] = 0; tx[2] = 1; tx[3] = request;
    memcpy(&tx[4], &val, 4);
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        printf("  %-22s write FAILED\n", name);
        MarkFail(name);
        return;
    }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n <= 0) MarkFail(name);
    printf("  %-22s n=%-2d status=%d (val=0x%08X)\n", name, n,
           (n >= 2) ? r[1] : -1, val);
}

// 裸发一条 JTAG 序列类命令：[op][info][count][TDI 数据...]，打印原始响应（看 TDO 回不回来）
static int JtagSeqRaw(V2Dev* d, uint8_t op, uint8_t info, uint8_t count,
                      const uint8_t* tdi, int tdiLen, const char* name)
{
    if (Aborted()) { printf("  %-30s SKIP（已失败停止）\n", name); return 0; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    tx[0] = op; tx[1] = info; tx[2] = count;
    if (tdi && tdiLen) memcpy(&tx[3], tdi, (size_t)tdiLen);
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        printf("  %-30s write FAILED\n", name);
        MarkFail(name);
        return 0;
    }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n <= 0) MarkFail(name);
    printf("  %-30s n=%-3d :", name, n);
    for (int i = 0; i < n && i < 10; ++i) printf(" %02X", r[i]);
    printf("\n");
    return n;
}

// --quick：只做最小判定（约 15 条命令，秒级返回）。
// 默认那份 18 步完整排查里有"手工位拷贝(~80 条)"与"时钟扫描(5 档)"——
// 设备不回时每条都要等超时，整体能拖到几分钟，看起来就是**卡死**。
// 所以只要判"通/不通"，用 --quick。
static int QuickOpcodeTest(void)
{
    V2Dev d;
    if (!V2Open(&d)) { printf("open FAILED\n"); return 1; }
    printf("opened: in=0x%02X out=0x%02X\n", d.inPipe, d.outPipe);
    DrainIn(&d, 200);

    Connect2(&d, 2, "Connect(2 JTAG)");

    const uint8_t nav[2]  = { 0x5F, 0x00 };   // TMS 9 位: 复位 + RTI + SelDR + CapDR + ShDR
    const uint8_t zero[4] = { 0, 0, 0, 0 };

    // 标准 0x0C（DAP_JTAG_Configure）与本套件 0x15 各配一次
    {
        uint8_t tx[64];
        memset(tx, 0, sizeof(tx));
        tx[0] = 0x0C; tx[1] = 2; tx[2] = 4; tx[3] = 5;
        if (!Aborted() && WriteOnce(&d, tx, 64, g_ioTimeoutMs)) {
            uint8_t r[64];
            const int n = ReadOnce(&d, r, sizeof(r), g_ioTimeoutMs, NULL);
            if (n <= 0) MarkFail("Configure@0x0C");
            printf("  %-30s n=%-3d :", "Configure@0x0C(2,[4,5])", n);
            for (int i = 0; i < n && i < 8; ++i) printf(" %02X", r[i]);
            printf("\n");
        } else {
            printf("  Configure@0x0C write FAILED\n");
            MarkFail("Configure@0x0C");
        }
    }
    {
        const uint8_t irs[2] = { 4, 5 };
        JtagConfigureN(&d, 2, irs, "Configure@0x15(2,[4,5])");
    }

    // 关键判定：0x0B（标准） vs 0x14（本套件）能否回 TDO
    JtagSeqRaw(&d, 0x0B, 0x03, 9, nav, 2,  "0x0B nav(info=0x03)");
    JtagSeqRaw(&d, 0x0B, 0x01, 32, zero, 4, "0x0B shift32+TDO");
    JtagSeqRaw(&d, 0x14, 0x03, 9, nav, 2,  "0x14 nav(info=0x03)");
    JtagSeqRaw(&d, 0x14, 0x01, 32, zero, 4, "0x14 shift32+TDO");

    // 两条 IDCODE 命令对照
    {
        const uint8_t ops[2] = { 0x0D, 0x16 };
        for (int i = 0; i < 2; ++i) {
            if (Aborted()) { printf("  IDCODE@0x%02X SKIP（已失败停止）\n", ops[i]); break; }
            uint8_t tx[64];
            memset(tx, 0, sizeof(tx));
            tx[0] = ops[i];
            if (!WriteOnce(&d, tx, 64, g_ioTimeoutMs)) {
                printf("  IDCODE@0x%02X write FAILED\n", ops[i]);
                MarkFail("IDCODE(quick)");
                continue;
            }
            uint8_t r[64];
            const int n = ReadOnce(&d, r, sizeof(r), g_ioTimeoutMs, NULL);
            if (n <= 0) MarkFail("IDCODE(quick)");
            printf("  %-30s n=%-3d :", (ops[i] == 0x0D) ? "IDCODE@0x0D" : "IDCODE@0x16", n);
            for (int k = 0; k < n && k < 10; ++k) printf(" %02X", r[k]);
            printf("\n");
        }
    }

    ConnectSwdIfAllowed(&d, 1, "Connect(1 SWD restore)");
    V2Close(&d);
    printf("=== quick done ===\n");
    return 0;
}

// ★ 按 orbtrace 固件源码的正确帧读 JTAG 链路
//   请求 [0x14][count=1][info][data nB]   info = 位数(0..63,0=64) | TMS?0x40 | TDO捕获?0x80
//   响应 [0x14][status][TDO nB]          数据字节数 = ceil(位数/8)，LSB first
static void JtagSeq1(V2Dev* d, int bits, int tmsLevel, int capture, uint8_t* tdoOut, const char* name)
{
    if (Aborted()) { printf("  %-26s SKIP（已失败停止）\n", name); return; }
    uint8_t tx[64];
    memset(tx, 0, sizeof(tx));
    const int nbytes = (bits + 7) / 8;
    tx[0] = 0x14;
    tx[1] = 1;                                                  // 段数
    tx[2] = (uint8_t)((bits & 0x3F) | (tmsLevel ? 0x40 : 0) | (capture ? 0x80 : 0));
    if (!WriteOnce(d, tx, 64, g_ioTimeoutMs)) {
        printf("  %-26s write FAILED\n", name);
        MarkFail(name);
        return;
    }
    uint8_t r[64];
    const int n = ReadOnce(d, r, sizeof(r), g_ioTimeoutMs, NULL);
    if (n <= 0) MarkFail(name);
    printf("  %-26s n=%-3d :", name, n);
    for (int i = 0; i < n && i < 12; ++i) printf(" %02X", r[i]);
    printf("\n");
    if (tdoOut) {
        memset(tdoOut, 0, (size_t)nbytes);
        if (n > 2) {
            const int avail = n - 2;                            // 跳过 [cmdId][status]
            memcpy(tdoOut, &r[2], (size_t)((avail < nbytes) ? avail : nbytes));
        }
    }
}

static int JtagPathTest(void)
{
    V2Dev d;
    if (!V2Open(&d)) { printf("open FAILED\n"); return 1; }
    printf("opened: in=0x%02X out=0x%02X\n", d.inPipe, d.outPipe);
    DrainIn(&d, 200);
    Connect2(&d, 2, "Connect(2 JTAG)");

    // 链上 2 个 TAP：cpu(IR=4, 0x4BA00477) + bs(IR=5, 0x06413041)
    {
        const uint8_t irs[2] = { 4, 5 };
        JtagConfigureN(&d, 2, irs, "JTAG_Configure(2,[4,5])");
    }

    // TMS 恒定值 => 状态迁移一段一段发
    JtagSeq1(&d, 6, 1, 0, NULL, "TMS reset (6)");
    JtagSeq1(&d, 1, 0, 0, NULL, "-> RTI");
    JtagSeq1(&d, 1, 1, 0, NULL, "-> Select-DR");
    JtagSeq1(&d, 1, 0, 0, NULL, "-> Capture-DR");
    JtagSeq1(&d, 1, 0, 0, NULL, "-> Shift-DR");

    uint8_t tdo[8] = {0};
    JtagSeq1(&d, 64, 0, 1, tdo, "shift 64 bits + TDO");        // TDO 从链尾先出
    JtagSeq1(&d, 1, 1, 0, NULL, "-> Exit1-DR");

    uint32_t first = 0, second = 0;
    memcpy(&first, &tdo[0], 4);
    memcpy(&second, &tdo[4], 4);
    printf("  => TDO[0..3]=0x%08X   TDO[4..7]=0x%08X\n", first, second);
    if (first == 0x06413041u || second == 0x4BA00477u ||
        first == 0x4BA00477u || second == 0x06413041u) {
        printf("  => [OK] JTAG 链路读通（cpu 0x4BA00477 / bs 0x06413041）\n");
    } else {
        printf("  => [BAD] 仍不是 IDCODE\n");
    }

    ConnectSwdIfAllowed(&d, 1, "Connect(1 SWD restore)");
    V2Close(&d);
    printf("=== path done ===\n");
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // 关缓冲：即使中途崩溃也能看到已打出的步骤
    bool quick = false;
    bool path = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && strcmp(argv[i], "--no-swd") == 0) {
            g_allowSwd = false;
        }
        if (argv[i] && strcmp(argv[i], "--quick") == 0) {
            quick = true;
        }
        if (argv[i] && strcmp(argv[i], "--path") == 0) {
            path = true;
        }
        // ---- 加固相关开关（见文件头 ⚠️⚠️） ----
        if (argv[i] && strcmp(argv[i], "--no-stop") == 0) {
            g_stopOnFail = false;                 // 失败后仍继续下发（默认不会）
        }
        if (argv[i] && strcmp(argv[i], "--bitbang") == 0) {
            g_doBitBang = true;                   // 打开 [13]/[14] 引脚位拷
        }
        if (argv[i] && strcmp(argv[i], "--mode-switch") == 0) {
            g_doModeSwitch = true;                // 打开"来回切模式"的段
        }
        if (argv[i] && strcmp(argv[i], "--retry") == 0 && i + 1 < argc) {
            g_maxAttempts = atoi(argv[++i]);
            if (g_maxAttempts < 1) g_maxAttempts = 1;
        }
        if (argv[i] && strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            g_ioTimeoutMs = atoi(argv[++i]);
            if (g_ioTimeoutMs < 50) g_ioTimeoutMs = 50;
        }
    }
    if (path) {
        return JtagPathTest();
    }
    if (quick) {
        return QuickOpcodeTest();
    }
    printf("=== JTAG 通路直连探测（原始 CMSIS-DAP 帧，出包固定 64 字节短包）===\n");
    printf("模式：%s\n", g_allowSwd ? "完整（会先做 Connect(0)，仅供对照）"
                                   : "JTAG-only（--no-swd：绝不先碰 SWD，用于上电后的首次连接）");
    printf("策略：%s | 重发 %d 次 | 超时 %d ms | 引脚位拷[13/14] %s | 模式来回切段 %s\n",
           g_stopOnFail ? "任一步失败即停" : "失败继续（--no-stop）",
           g_maxAttempts, g_ioTimeoutMs,
           g_doBitBang ? "开（--bitbang）" : "关",
           g_doModeSwitch ? "开（--mode-switch）" : "关");
    printf("注意：本程序**不发送** cmd=0x06（ID_DAP_TRANSFER_BLOCK）—— 该命令在 JTAG 模式下\n");
    printf("      会让 orbtrace 挂死、须重新插拔（COMPAT_ANALYSIS §18.9 第十步）。\n");
    V2Dev d;
    if (!V2Open(&d)) { printf("打开设备失败\n"); return 1; }
    printf("opened: in=0x%02X out=0x%02X\n", d.inPipe, d.outPipe);
    DrainIn(&d, 200);

    ShowCaps(&d);

    printf("\n[1] 基线：Connect(0) 自动\n");
    ConnectSwdIfAllowed(&d, 0, "Connect(0 auto)");

    printf("\n[2] 关键判定：Connect(2) = JTAG\n");
    const int m2 = Connect2(&d, 2, "Connect(2 JTAG)");

    if (g_doModeSwitch) {
        printf("\n[3] 若上面回的是 SWD，再试 Connect(1) 作对照\n");
        ConnectSwdIfAllowed(&d, 1, "Connect(1 SWD)");
    } else {
        printf("\n[3] SKIP（来回切模式的段，需 --mode-switch）\n");
    }

    printf("\n[4] Connect(2) 之后再问一次模式（看是否可切）\n");
    Connect2(&d, 2, "Connect(2 JTAG) #2");

    printf("\n[5] JTAG_Configure(count=1, ir=4)\n");
    JtagConfigure(&d, 1, 4);

    printf("\n[6] JTAG_IDCODE 扫链（未做 TMS 复位）\n");
    JtagIdcode(&d, "JTAG_IDCODE #1");

    printf("\n[7] TAP 复位（JTAG_Sequence 的 TMS 序列）后重扫\n");
    JtagTmsReset(&d);
    JtagIdcode(&d, "JTAG_IDCODE #2");

    printf("\n[8] 换 SWJ_Sequence 发 TMS 复位后重扫\n");
    SwjJtagReset(&d);
    JtagIdcode(&d, "JTAG_IDCODE #3");

    printf("\n[9] 变体：ir=5 / count=1、ir=4 / count=2\n");
    JtagConfigure(&d, 1, 5);
    JtagIdcode(&d, "JTAG_IDCODE ir=5");
    JtagConfigure(&d, 2, 4);
    JtagIdcode(&d, "JTAG_IDCODE cnt=2");
    JtagConfigure(&d, 1, 4);

    printf("\n[10] TDO 线特征（TMS 进 Shift-DR 后移 32 位读回 TDO）\n");
    printf("  变体 A: TMS info=0x03 + 移位 info=0x01\n");
    CharacterizeTdo(&d, 0x03, 0x01);
    JtagConfigure(&d, 1, 4);
    printf("  变体 B: TMS info=0x02 + 移位 info=0x01\n");
    CharacterizeTdo(&d, 0x02, 0x01);
    JtagConfigure(&d, 1, 4);

    printf("\n[10b] 接口引脚电平（判断 TDO/TDI 线是否被驱动）\n");
    JtagPinRead(&d, "SWJ_Pins(read) A");
    {
        // 把 TDI 置 1（pinSelect bit2）再看 TDO 是否有上拉/响应
        uint8_t r[64];
        uint8_t c[7] = {0x07, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00};
        Cmd(&d, c, sizeof(c), r, sizeof(r), 1500, "SWJ_Pins(TDI=1)");
    }
    JtagPinRead(&d, "SWJ_Pins(read) B");

    printf("\n[10c] info 位组合穷举（找 TDO 回读的编码）\n");
    InfoMatrix(&d);

    printf("\n[10d] 多段读：TDO 会不会出现在「第二段」响应里？\n");
    {
        uint8_t prep[5] = {CMD_JTAG_SEQUENCE, 0x02, 9, 0x5F, 0x00};
        MultiSeg(&d, prep, sizeof(prep), "TMS->ShiftDR", 3);

        uint8_t sh1[7] = {CMD_JTAG_SEQUENCE, 0x01, 32, 0, 0, 0, 0};
        MultiSeg(&d, sh1, sizeof(sh1), "shift32 info=0x01", 4);

        uint8_t sh3[7] = {CMD_JTAG_SEQUENCE, 0x03, 32, 0, 0, 0, 0};
        MultiSeg(&d, sh3, sizeof(sh3), "shift32 info=0x03", 4);

        uint8_t idc[1] = {CMD_JTAG_IDCODE};
        MultiSeg(&d, idc, sizeof(idc), "JTAG_IDCODE", 3);

        uint8_t cap[2] = {CMD_INFO, 0xF0};
        MultiSeg(&d, cap, sizeof(cap), "Info(0xF0) 对照", 3);
    }

    if (g_doModeSwitch) {
        printf("\n[10e] ★ 补发 SWD->JTAG 切换序列(0xE73C) 后重扫 —— SWJ-DP 模式记忆假说\n");
        Connect2(&d, 2, "Connect(2 JTAG)");
        SwjSwitchToJtag(&d);
        Connect2(&d, 2, "Connect(2 JTAG) 再选");
        JtagConfigure(&d, 1, 4);
        JtagIdcode(&d, "JTAG_IDCODE 切换后");

        printf("\n[10f] 变体：先发切换序列、再 Connect(2)，然后扫链\n");
        ConnectSwdIfAllowed(&d, 1, "Connect(1 SWD)");
        SwjSwitchToJtag(&d);
        Connect2(&d, 2, "Connect(2 JTAG)");
        JtagConfigure(&d, 1, 4);
        JtagIdcode(&d, "JTAG_IDCODE 变体");
    } else {
        printf("\n[10e]/[10f] SKIP（来回切模式的段，需 --mode-switch）\n");
    }

    printf("\n[12] ★ 引脚电平读取（判定 TDO/nRESET 的真实状态）\n");
    PinsRead(&d, 1000, 1, "Pins read #1");
    {
        // 驱动 TMS=1 / TDI=1 再读，看 TDO 是否有反应（有源器件会跟随/驱动）
        PinsDriveQuiet(&d, PIN_TMS | PIN_TDI, PIN_TMS | PIN_TDI);
        PinsRead(&d, 1000, 1, "Pins read #2 (TMS=TDI=1)");
        PinsDriveQuiet(&d, 0x00, PIN_TMS | PIN_TDI);
        PinsRead(&d, 1000, 1, "Pins read #3 (TMS=TDI=0)");
    }

    if (g_doBitBang) {
        printf("\n[13] ★ 引脚位拷扫 IDCODE（完全绕开 gateware 的 JTAG 引擎）\n");
        Connect2(&d, 2, "Connect(2 JTAG)");
        BitBangIdcode(&d);

        printf("\n[14] ★ 拉低 nRESET（复位态下扫链；目标固件复用 JTAG 引脚时的标准解法）\n");
        {
            PinsDriveQuiet(&d, 0x00, PIN_NRESET);          // nRESET=0，保持复位
            PinsRead(&d, 1000, 1, "Pins read @reset");
            Connect2(&d, 2, "Connect(2 JTAG)");
            JtagConfigure(&d, 1, 4);
            JtagIdcode(&d, "JTAG_IDCODE @reset");
            BitBangIdcode(&d);
            PinsDriveQuiet(&d, PIN_NRESET, PIN_NRESET);    // 释放复位
            PinsRead(&d, 1000, 1, "Pins read @release");
        }
    } else {
        printf("\n[13]/[14] SKIP（引脚位拷约 80 条引脚命令，最易把探针怼死；需 --bitbang）\n");
    }

    if (g_doModeSwitch) {
        printf("\n[15] ★ JTAG 时钟扫描（orbtrace 官方：JTAG 10-12Mbps 就到顶，Keil 传的是 10MHz）\n");
        {
            const uint32_t clocks[5] = { 100000u, 500000u, 1000000u, 4000000u, 10000000u };
            for (int i = 0; i < 5; ++i) {
                if (Aborted()) { printf("  [15] 提前收尾（已失败停止）\n"); break; }
                printf("  --- SWJ_Clock = %u Hz ---\n", (unsigned)clocks[i]);
                SwjClock(&d, clocks[i], 1);
                Connect2(&d, 2, "  Connect(2 JTAG)");
                JtagConfigure(&d, 1, 4);
                JtagIdcode(&d, "  JTAG_IDCODE");
                if (g_doBitBang) BitBangIdcode(&d);
                ConnectSwdIfAllowed(&d, 1, "  Connect(1 SWD 复位模式)");
            }
        }
    } else {
        printf("\n[15] SKIP（时钟扫描里含来回切模式，需 --mode-switch；引脚位拷另需 --bitbang）\n");
    }

    printf("\n[16] ★ 链配置候选扫描（OpenOCD 报本板是 2 个 TAP：cpu IR=4 + bs IR=5）\n");
    {
        // 之前所有尝试都按"1 个器件 / IR=4"配 —— 而链上是 2 个 TAP，
        // 单器件配置会让固件只移 32 位，拿到混合数据（bit0 不是 1）→ 报 0 个器件。
        const uint8_t c1 [1] = { 4 };
        const uint8_t c2a[2] = { 4, 5 };      // cpu, bs（OpenOCD 的链顺序）
        const uint8_t c2b[2] = { 5, 4 };
        const uint8_t c3 [3] = { 4, 5, 4 };
        struct Cand { int n; const uint8_t* irs; const char* tag; };
        const Cand cands[4] = {
            { 1, c1,  "  JTAG_Configure(1, [4])    " },
            { 2, c2a, "  JTAG_Configure(2, [4,5])  " },
            { 2, c2b, "  JTAG_Configure(2, [5,4])  " },
            { 3, c3,  "  JTAG_Configure(3, [4,5,4])" },
        };
        for (int i = 0; i < 4; ++i) {
            if (Aborted()) { printf("  [16] 提前收尾（已失败停止）\n"); break; }
            printf("  --- 候选 %d: count=%d ---\n", i + 1, cands[i].n);
            Connect2(&d, 2, "  Connect(2 JTAG)");
            JtagConfigureN(&d, cands[i].n, cands[i].irs, cands[i].tag);
            JtagIdcode(&d, "  JTAG_IDCODE");
        }
    }

    printf("\n[17] ★ 真通路验证：配链后用 DAP_Transfer 直接读 DP/AP（OpenOCD 就是这么干的）\n");
    {
        const uint8_t n4[] = { 4 };
        const uint8_t n45[2] = { 4, 5 };
        const struct { int n; const uint8_t* irs; const char* tag; } cfgs[2] = {
            { 2, n45, "JTAG_Configure(2,[4,5])" },
            { 1, n4,  "JTAG_Configure(1,[4])  " },
        };
        for (int c = 0; c < 2; ++c) {
            if (Aborted()) { printf("  [17] 提前收尾（已失败停止）\n"); break; }
            printf("  === 链配置：%s ===\n", cfgs[c].tag);
            Connect2(&d, 2, "Connect(2 JTAG)");
            JtagConfigureN(&d, cfgs[c].n, cfgs[c].irs, cfgs[c].tag);
            int st = -1;
            const uint32_t dpidr = DapRead(&d, 0x02, &st, "DPIDR");
            DapWrite(&d, 0x04, 0x50000000, "CTRL/STAT <- 0x50000000");
            const uint32_t ctrl = DapRead(&d, 0x06, &st, "CTRL/STAT read");
            DapWrite(&d, 0x08, 0x00000000, "SELECT <- 0");
            const uint32_t csw = DapRead(&d, 0x03, &st, "AP CSW read");
            DapWrite(&d, 0x05, 0xE000ED00, "AP TAR <- 0xE000ED00");
            DapRead(&d, 0x0F, &st, "AP DRW (posted r1)");
            const uint32_t cpuid = DapRead(&d, 0x0F, &st, "SCB->CPUID");
            printf("      >> DPIDR=0x%08X CTRL=0x%08X CSW=0x%08X CPUID=0x%08X\n",
                   dpidr, ctrl, csw, cpuid);
            if (dpidr == 0x4BA00477u || cpuid == 0x410FC241u) {
                printf("      >> [OK] JTAG 通路可用！\n");
            } else {
                printf("      >> [BAD] 该链配置下读不到 DP\n");
            }
        }
    }

    printf("\n[18] ★ 试标准操作码：0x0C=JTAG_Configure / 0x0B=JTAG_Sequence（OpenOCD 用这套）\n");
    {
        // 本套件的 ORBMDK_DAP.h 里 JTAG 是 0x14/0x15/0x16；CMSIS-DAP 标准是 0x0B/0x0C/0x0D。
        // OpenOCD 能读到 0x4ba00477，说明它用的那套是有效的 —— 逐一对照。
        Connect2(&d, 2, "Connect(2 JTAG)");
        {
            uint8_t tx[64];
            memset(tx, 0, sizeof(tx));
            tx[0] = 0x0C; tx[1] = 2; tx[2] = 4; tx[3] = 5;      // 标准 JTAG_Configure
            if (!Aborted() && WriteOnce(&d, tx, 64, g_ioTimeoutMs)) {
                uint8_t r[64];
                const int n = ReadOnce(&d, r, sizeof(r), g_ioTimeoutMs, NULL);
                if (n <= 0) MarkFail("JTAG_Configure@0x0C");
                printf("  %-30s n=%-3d :", "JTAG_Configure@0x0C(2,[4,5])", n);
                for (int i = 0; i < n && i < 8; ++i) printf(" %02X", r[i]);
                printf("\n");
            } else {
                printf("  JTAG_Configure@0x0C write FAILED\n");
                MarkFail("JTAG_Configure@0x0C");
            }
        }

        // TAP 复位 + 进 Shift-DR：TMS 序列 9 位 = 1,1,1,1,1,0,1,0,0 -> 字节 0x5F,0x00
        const uint8_t nav[2] = { 0x5F, 0x00 };
        const uint8_t zero[4] = { 0, 0, 0, 0 };

        // 标准 0x0B，info: bit0=TDO 捕获, bit1=TMS 序列
        JtagSeqRaw(&d, 0x0B, 0x03, 9, nav, 2, "0x0B nav->ShiftDR(info=0x03)");
        JtagSeqRaw(&d, 0x0B, 0x01, 32, zero, 4, "0x0B shift32+TDO(info=0x01)");
        // 本套件映射 0x14，做对照
        JtagSeqRaw(&d, 0x14, 0x03, 9, nav, 2, "0x14 nav->ShiftDR(info=0x03)");
        JtagSeqRaw(&d, 0x14, 0x01, 32, zero, 4, "0x14 shift32+TDO(info=0x01)");

        // 标准 0x0D = DAP_JTAG_IDCODE（看它和 0x16 是否一样）
        {
            uint8_t tx[64];
            memset(tx, 0, sizeof(tx));
            tx[0] = 0x0D;
            if (!Aborted() && WriteOnce(&d, tx, 64, g_ioTimeoutMs)) {
                uint8_t r[64];
                const int n = ReadOnce(&d, r, sizeof(r), g_ioTimeoutMs, NULL);
                if (n <= 0) MarkFail("JTAG_IDCODE@0x0D");
                printf("  %-30s n=%-3d :", "JTAG_IDCODE@0x0D", n);
                for (int i = 0; i < n && i < 10; ++i) printf(" %02X", r[i]);
                printf("\n");
            }
        }
    }

    printf("\n[11] 收尾：恢复 SWD + Disconnect（不把设备留在 JTAG 态）\n");
    ConnectSwdIfAllowed(&d, 1, "Connect(1 SWD restore)");
    {
        uint8_t r[64];
        uint8_t c[1] = {CMD_DISCONNECT};
        Cmd(&d, c, sizeof(c), r, sizeof(r), 1000, "Disconnect");
    }

    V2Close(&d);
    if (g_failed) {
        printf("\n[!!] 本次已出现首个失败并按策略停止。若探针此后仍无响应 -> 重新插拔 USB 再跑；\n");
        printf("     继续原地重试只会重复失败（orbtrace 的挂死不会自己恢复）。\n");
    }
    printf("\n=== done ===\n");
    if (!g_allowSwd) {
        printf("\n[提醒] 本次是 JTAG-only 模式。若上面 IDCODE 仍为 0，请给**目标断电重新上电**\n");
        printf("       后再跑一次：本目标的 SWJ-DP 只能 JTAG->SWD 单向切换，一旦执行过 SWD\n");
        printf("       （任何 Connect(0)/Connect(1)，包括 Keil 的对话框扫描）就回不到 JTAG，\n");
        printf("       补发 ADIv5 的 SWD->JTAG 序列(0xE73C) 也无效。\n");
    }
    return 0;
}
