/**
 * @file ORBMDK_OrbTrace.cpp
 * @brief orbtrace 调试器配置接口 —— orbuculum `Src/orbtraceIf.c` 的 WinUSB 移植。
 *
 * 见 include/ORBMDK_OrbTrace.h 的说明（接口分类、控制请求、与 orbuculum 的差异）。
 *
 * 设计要点：
 *   - 与 CMSIS-DAP 通路**并存**：DAP 走 ORBMDK_USB_Bulk.cpp 的接口句柄，本模块另开
 *     trace / power 两个接口句柄，互不干扰。
 *   - 只做"配置 + 搬运"：不做 OFLOW/COBS 解帧，也不做任何 trace 语义解码。
 *   - 所有失败都软失败：找不到 orbtrace、接口打不开时返回非 0，不影响 DAP 通路。
 */
#include "pch.h"
#include "ORBMDK.h"
#include "ORBMDK_OrbTrace.h"
#include "ORBMDK_Log.h"

#include <string.h>
#include <stdio.h>

// Windows SDK (WinUSB)
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#pragma comment(lib, "setupapi.lib")

#include <initguid.h>
#include <usbiodef.h>
#pragma comment(lib, "uuid.lib")

#define WINUSB_NO_DEFAULT_LIB
#include <winusb.h>
#include <winusbio.h>
#pragma comment(lib, "winusb.lib")

#define ORBMDK_LOG_MODULE "ORBTRACE"
#define TrcTrace(...) ORBMDK_LOG_INFO(__VA_ARGS__)
#define TrcWarn(...)  ORBMDK_LOG_WARN(__VA_ARGS__)

namespace {

// ---------------------------------------------------------------------------
// 常量（对齐 orbuculum orbtraceIf.c）
// ---------------------------------------------------------------------------

// vendor 控制请求
const uint8_t  kRqClass     = 0x41;   // bmRequestType = RQ_CLASS | RQ_INTERFACE
const uint8_t  kRqSetTWidth = 1;      // Trace: 宽度 / SWO 模式
const uint8_t  kRqSetSpeed  = 2;      // Trace: 波特率
const uint8_t  kRqSetEnable = 1;      // Power: 使能
const uint8_t  kRqSetVoltage= 2;      // Power: 电压

// 接口子类（bInterfaceSubClass，按字符）
const uint8_t  kSubClassPower   = 0x50;  // 'P'
const uint8_t  kSubClassTrace   = 0x54;  // 'T'
const uint8_t  kSubClassVersion = 0x56;  // 'V'

// 电压范围（orbuculum MIN/MAX + 容差）
const int kMinVoltageMv = 900;
const int kMaxVoltageMv = 5000 + 10;

const char* const kVersionFrontMatter = "Version: ";

// orbtrace VID/PID（与 ORBMDK.h 一致；mini 为 0x3442）
struct VidPid { uint16_t vid; uint16_t pid; };
const VidPid kOrbTraceDevices[] = {
    { ORBMDK_ORBTRACE_VID, 0x3443 },   // ORBTrace
    { ORBMDK_ORBTRACE_VID, 0x3442 },   // ORBTrace mini
};

// WinUSB 设备接口 GUID（与 ORBMDK_USB_Bulk.cpp 同一份值；SDK 未给常量名）
const GUID kWinUsbGuid =
    { 0xDEE824EF, 0x729B, 0x4A0E, { 0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17 } };

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
struct OrbTraceState {
    bool detected;
    bool opened;

    uint16_t vid;
    uint16_t pid;

    int traceIf;
    int powerIf;
    int versionIf;
    int traceProtocol;
    bool supportsOFLOW;

    char product[128];
    char serial[64];
    char version[64];

    HANDLE                  traceFile;
    WINUSB_INTERFACE_HANDLE traceWinUsb;
    UCHAR                   traceEp;

    HANDLE                  powerFile;
    WINUSB_INTERFACE_HANDLE powerWinUsb;

    OrbTraceState()
        : detected(false), opened(false), vid(0), pid(0),
          traceIf(-1), powerIf(-1), versionIf(-1),
          traceProtocol(ORBMDK_ORBTRACE_PROT_UNKNOWN), supportsOFLOW(false),
          traceFile(INVALID_HANDLE_VALUE), traceWinUsb(nullptr), traceEp(0),
          powerFile(INVALID_HANDLE_VALUE), powerWinUsb(nullptr) {
        product[0] = serial[0] = version[0] = '\0';
    }
};

OrbTraceState& St()
{
    static OrbTraceState s;   // 进程级单例（DLL 生命周期内）
    return s;
}

bool IsOrbTraceVidPid(uint16_t vid, uint16_t pid)
{
    for (const VidPid& d : kOrbTraceDevices) {
        if (d.vid == vid && d.pid == pid) return true;
    }
    return false;
}

// 从设备接口路径里取 vid / pid（路径形如 \\?\usb#vid_1209&pid_3443&mi_05#...）
bool ParseVidPid(const wchar_t* path, uint16_t* vid, uint16_t* pid)
{
    const wchar_t* p = wcsstr(path, L"vid_");
    if (!p) return false;
    unsigned v = 0, d = 0;
    if (swscanf_s(p, L"vid_%4x&pid_%4x", &v, &d) != 2) return false;
    *vid = static_cast<uint16_t>(v);
    *pid = static_cast<uint16_t>(d);
    return true;
}

// 读接口的 iInterface 字符串描述符（UTF-16 → ANSI）
bool ReadInterfaceString(WINUSB_INTERFACE_HANDLE h, UCHAR iInterface, char* out, size_t outLen)
{
    out[0] = '\0';
    if (iInterface == 0) return false;

    UCHAR buf[256] = {0};
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, iInterface, 0x0409,
                              buf, sizeof(buf), &got) || got < 2) {
        return false;
    }
    const int chars = static_cast<int>((got - 2) / 2);
    const int n = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(buf + 2), chars,
                                      out, static_cast<int>(outLen) - 1, nullptr, nullptr);
    out[(n > 0) ? n : 0] = '\0';
    return n > 0;
}

// 读设备描述符里的产品/序列号字符串
void ReadDeviceStrings(WINUSB_INTERFACE_HANDLE h, char* product, size_t plen,
                       char* serial, size_t slen)
{
    USB_DEVICE_DESCRIPTOR desc;
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(h, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0x0409,
                              reinterpret_cast<PUCHAR>(&desc), sizeof(desc), &got)) {
        return;
    }
    auto readStr = [&](UCHAR idx, char* out, size_t outLen) {
        out[0] = '\0';
        if (idx == 0) return;
        UCHAR buf[256] = {0};
        ULONG g = 0;
        if (!WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, idx, 0x0409,
                                  buf, sizeof(buf), &g) || g < 2) {
            return;
        }
        const int chars = static_cast<int>((g - 2) / 2);
        const int n = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(buf + 2), chars,
                                          out, static_cast<int>(outLen) - 1, nullptr, nullptr);
        out[(n > 0) ? n : 0] = '\0';
    };
    readStr(desc.iProduct, product, plen);
    readStr(desc.iSerialNumber, serial, slen);
}

// 打开一条设备接口路径（CreateFile + WinUsb_Initialize）。失败返回 false。
//
// FILE_FLAG_OVERLAPPED 是**必须**的：少了它，WinUsb_Initialize 在 orbtrace
// 复合设备的**每一个**接口上都返回 ERROR_INVALID_HANDLE(6) —— 包括本进程
// bulk 路径正在正常使用的 MI_05。结果就是 ScanForOrbTrace 一个接口都认不出，
// 日志只有一句 "no orbtrace device found"，看不出卡在哪一步。
// ORBMDK_USB_Bulk.cpp 的对应调用带了这个标志，所以那边一直是好的。
//
// 注意错误码具有误导性：6 是"句柄无效"，会把人引向"设备没绑 WinUSB 驱动"，
// 而绑定其实是好的。判据见 test/ORBAGDI_OrbTraceProbe（ORBAGDI 侧工具）。
bool OpenInterfacePath(const wchar_t* path, HANDLE* file, WINUSB_INTERFACE_HANDLE* winUsb)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    WINUSB_INTERFACE_HANDLE wu = nullptr;
    if (!WinUsb_Initialize(h, &wu)) {
        CloseHandle(h);
        return false;
    }
    *file = h;
    *winUsb = wu;
    return true;
}

// ---------------------------------------------------------------------------
// 扫描：把 orbtrace 复合设备的三个 vendor 接口找出来
// ---------------------------------------------------------------------------
bool ScanForOrbTrace()
{
    OrbTraceState& s = St();

    HDEVINFO devInfo = SetupDiGetClassDevs(&kWinUsbGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        TrcWarn("SetupDiGetClassDevs failed, err=%lu", (unsigned long)GetLastError());
        return false;
    }

    bool found = false;
    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    DWORD index = 0;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kWinUsbGuid, index++, &did)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(malloc(need));
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, need, nullptr, nullptr)) {
            uint16_t vid = 0, pid = 0;
            if (ParseVidPid(detail->DevicePath, &vid, &pid) && IsOrbTraceVidPid(vid, pid)) {
                HANDLE file = INVALID_HANDLE_VALUE;
                WINUSB_INTERFACE_HANDLE wu = nullptr;
                if (OpenInterfacePath(detail->DevicePath, &file, &wu)) {
                    USB_INTERFACE_DESCRIPTOR ifd;
                    if (WinUsb_QueryInterfaceSettings(wu, 0, &ifd)) {
                        const bool isVendor = (ifd.bInterfaceClass == 0xFF);
                        if (isVendor) {
                            if (!s.detected) {
                                s.vid = vid;
                                s.pid = pid;
                                ReadDeviceStrings(wu, s.product, sizeof(s.product),
                                                  s.serial, sizeof(s.serial));
                            }
                            if (ifd.bInterfaceSubClass == kSubClassTrace && s.traceIf < 0) {
                                s.traceIf = ifd.bInterfaceNumber;
                                s.traceProtocol = ifd.bInterfaceProtocol;
                                s.supportsOFLOW =
                                    (ifd.bInterfaceProtocol == ORBMDK_ORBTRACE_PROT_OFLOWV1_0);
                                TrcTrace("found trace iface %d (proto=0x%02X, OFLOW=%d)",
                                         s.traceIf, s.traceProtocol, s.supportsOFLOW ? 1 : 0);
                            } else if (ifd.bInterfaceSubClass == kSubClassPower && s.powerIf < 0) {
                                s.powerIf = ifd.bInterfaceNumber;
                                TrcTrace("found power iface %d", s.powerIf);
                            } else if (ifd.bInterfaceSubClass == kSubClassVersion && s.versionIf < 0) {
                                s.versionIf = ifd.bInterfaceNumber;
                                char vstr[128] = {};
                                if (ReadInterfaceString(wu, ifd.iInterface, vstr, sizeof(vstr))) {
                                    // 去掉 "Version: " 前缀（orbuculum 同款处理）
                                    const char* v = strstr(vstr, kVersionFrontMatter);
                                    strncpy_s(s.version, sizeof(s.version),
                                              v ? v + strlen(kVersionFrontMatter) : vstr,
                                              _TRUNCATE);
                                }
                                TrcTrace("found version iface %d ('%s')", s.versionIf, s.version);
                            }
                            if (s.traceIf >= 0) found = true;
                        }
                    }
                    WinUsb_Free(wu);
                    CloseHandle(file);
                }
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

// ---------------------------------------------------------------------------
// 控制请求
// ---------------------------------------------------------------------------
bool ControlRequest(WINUSB_INTERFACE_HANDLE wu, uint8_t request, uint16_t value,
                    uint8_t indexUpperHalf, uint8_t iface, const void* data, uint8_t dlen)
{
    if (!wu) return false;

    WINUSB_SETUP_PACKET sp = {};
    sp.RequestType = kRqClass;
    sp.Request     = request;
    sp.Value       = value;
    sp.Index       = static_cast<USHORT>((indexUpperHalf << 8) | iface);
    sp.Length      = dlen;

    ULONG transferred = 0;
    const BOOL ok = WinUsb_ControlTransfer(wu, sp, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                                           dlen, &transferred, nullptr);
    if (!ok) {
        TrcWarn("control req %u value=0x%04X iface=%u failed, err=%lu",
                request, value, iface, (unsigned long)GetLastError());
    }
    return ok != FALSE;
}

bool TraceRequest(uint8_t request, uint16_t value, const void* data, uint8_t dlen)
{
    OrbTraceState& s = St();
    return ControlRequest(s.traceWinUsb, request, value, 0,
                          static_cast<uint8_t>(s.traceIf), data, dlen);
}

bool PowerRequest(uint8_t request, uint16_t value, uint8_t channel)
{
    OrbTraceState& s = St();
    return ControlRequest(s.powerWinUsb, request, value, channel,
                          static_cast<uint8_t>(s.powerIf), nullptr, 0);
}

// ---------------------------------------------------------------------------
// 异步读取管线
//
// 照 orbuculum 的结构（`Src/orbuculum.c:67` NUM_RAW_BLOCKS + `Src/orbtraceIf.c:611`）：
// 常驻 32 个 overlapped 读 × 64 KB = 2 MB 在飞，USB 总线不空转，探针随时有请求可推。
// 原先的同步串行读在两次读之间有整个往返的空窗，探针 FIFO 一浅就溢出（红灯）。
// 参数怎么调都补不上这个差距 —— 实测记录见 COMPAT_ANALYSIS.md §10.22 / §10.23。
//
//     Open  -> 起线程，提交 32 个 overlapped 读；完成即入环、立刻重投
//     Read  -> 从环里取（**导出面与语义不变**，调用方无需改动）
//     Close -> 取消传输、收线程
//
// 线程只在 trace 流打开期间存在，关流即停，不碰 DAP / Flash 通路。
// ---------------------------------------------------------------------------

const int    kPipeBlocks   = 32;                  // = orbuculum NUM_RAW_BLOCKS
const size_t kPipeBlockLen = 64 * 1024;           // = orbuculum USB_TRANSFER_SIZE
const size_t kRingSize     = 32u * 1024 * 1024;   // 环 32 MB，吸收 2 MB 突发绰绰有余
const DWORD  kPipeTimeout  = 100;                 // ms，= orbuculum 的非 hires 值

struct PipeBlock {
    OVERLAPPED ov;
    DWORD      got;
    uint8_t*   buf;
    bool       submitted;
};

struct TracePipe {
    PipeBlock      blk[kPipeBlocks];
    HANDLE         thr;
    volatile LONG  stop;
    volatile LONG  running;

    // 环形缓冲：生产者 = 读取线程，消费者 = ORBMDK_OrbTrace_ReadTrace
    std::mutex           lock;
    std::vector<uint8_t> ring;
    size_t               head;
    size_t               tail;
    size_t               used;
    unsigned long long   dropped;

    TracePipe() : thr(nullptr), stop(0), running(0),
                  head(0), tail(0), used(0), dropped(0) {
        memset(blk, 0, sizeof(blk));
    }
};

TracePipe& P()
{
    // 故意泄漏（与 St() 同理：DLL 卸载时可能还有回调）。
    static TracePipe* p = new TracePipe();
    return *p;
}

void RingPush(const uint8_t* data, size_t len)
{
    TracePipe& p = P();
    std::lock_guard<std::mutex> g(p.lock);
    if (p.ring.empty()) return;
    for (size_t i = 0; i < len; ++i) {
        if (p.used >= p.ring.size()) { ++p.dropped; continue; }
        p.ring[p.head] = data[i];
        p.head = (p.head + 1) % p.ring.size();
        ++p.used;
    }
}

size_t RingPop(uint8_t* out, size_t len)
{
    TracePipe& p = P();
    std::lock_guard<std::mutex> g(p.lock);
    const size_t n = (len < p.used) ? len : p.used;
    for (size_t i = 0; i < n; ++i) {
        out[i] = p.ring[p.tail];
        p.tail = (p.tail + 1) % p.ring.size();
    }
    p.used -= n;
    return n;
}

// 提交一个 overlapped 读。false = 硬错误（设备拔出 / 端点失效）。
bool PipeSubmit(PipeBlock& b)
{
    OrbTraceState& s = St();
    if (!s.traceWinUsb || !b.buf || !b.ov.hEvent) return false;

    ResetEvent(b.ov.hEvent);
    b.got       = 0;
    b.submitted = false;

    if (WinUsb_ReadPipe(s.traceWinUsb, s.traceEp, b.buf,
                        static_cast<ULONG>(kPipeBlockLen), &b.got, &b.ov)) {
        b.submitted = true;     // 同步完成，数据已在手里
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) { b.submitted = true; return true; }
    return false;
}

// 读取线程：等任意一个传输完成 -> 入环 -> 立刻重投。
DWORD WINAPI TracePipeThread(LPVOID)
{
    TracePipe& p = P();
    HANDLE ev[kPipeBlocks];

    for (;;) {
        if (InterlockedCompareExchange(&p.stop, 0, 0) != 0) break;

        for (int i = 0; i < kPipeBlocks; ++i) ev[i] = p.blk[i].ov.hEvent;

        const DWORD w = WaitForMultipleObjects(kPipeBlocks, ev, FALSE, 100);
        if (w == WAIT_TIMEOUT) continue;
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + kPipeBlocks) continue;

        PipeBlock& b = p.blk[static_cast<int>(w - WAIT_OBJECT_0)];
        if (!b.submitted) continue;

        DWORD got = 0;
        const BOOL ok = WinUsb_GetOverlappedResult(St().traceWinUsb, &b.ov, &got, FALSE);
        // ⚠ 不能用 `ok && got`：传输被中止（取消 / 硬错误）时它返回 FALSE，但
        // `got` 仍是**已经交给主机的字节数**，那些字节已经离开探针 FIFO，不入环
        // 就等于在流里打一个静默缺口 —— 而缺口正好落在 64 KB 块边界上。
        // 只推 0 < got <= kPipeBlockLen 的值：失败路径上 got 若是垃圾大数，
        // 会推入一段不属于这条流的字节，比丢更糟。
        (void)ok;
        if (got > 0 && got <= kPipeBlockLen) {
            RingPush(b.buf, got);
        }
        // 失败（含 100 ms 超时）照样重投 —— 超时只是"暂时没数据"，是正常路径。

        if (!PipeSubmit(b)) {
            Sleep(20);          // 硬错误：歇一下再试，别把 CPU 打满
            PipeSubmit(b);
        }
    }
    return 0;
}

bool PipeStart()
{
    TracePipe& p = P();
    OrbTraceState& s = St();
    if (p.running) return true;
    if (!s.traceWinUsb || !s.traceEp) return false;

    p.ring.assign(kRingSize, 0);
    p.head = p.tail = p.used = 0;
    p.dropped = 0;
    InterlockedExchange(&p.stop, 0);

    // ⚠ 管道超时 = 0（永不超时）。原来是 100 ms（orbuculum 的非 hires 值），
    // 那会让传输在"这 64 KB 还没收满"时被中止 —— 已由探针发出的那部分字节
    // （已在 b.buf 前部）随之丢掉，而丢的地方**永远是一个 64 KB 块的起点**，
    // 于是 COBS 流在块边界出现静默缺口：探针的 OFLOW 帧被截断、解帧器在那一点
    // 错位，而 TraceStream_DroppedBytes 因为没经过 RingPush 完全看不到（实测
    // `环溢出丢帧=0 B` 与"每隔 64 KB 边界解帧错一次"同时出现）。
    // 0 = 无限等待：传输要么收满 kPipeBlockLen，要么被 PipeStop 的 CancelIoEx
    // 取消，不存在"半块被丢弃"这个状态。
    ULONG tmo = 0;
    WinUsb_SetPipePolicy(s.traceWinUsb, s.traceEp, PIPE_TRANSFER_TIMEOUT, sizeof(tmo), &tmo);

    for (int i = 0; i < kPipeBlocks; ++i) {
        PipeBlock& b = p.blk[i];
        if (!b.buf)      b.buf = new uint8_t[kPipeBlockLen];
        if (!b.ov.hEvent) b.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!b.buf || !b.ov.hEvent) return false;
        b.ov.Offset     = 0;
        b.ov.OffsetHigh = 0;
        PipeSubmit(b);
    }

    p.thr = CreateThread(nullptr, 0, TracePipeThread, nullptr, 0, nullptr);
    if (!p.thr) return false;

    InterlockedExchange(&p.running, 1);
    TrcTrace("trace pipe started: %d blocks x %u KB = %u MB in flight, ring %u MB",
             kPipeBlocks, (unsigned)(kPipeBlockLen / 1024),
             (unsigned)(kPipeBlocks * kPipeBlockLen / (1024 * 1024)),
             (unsigned)(kRingSize / (1024 * 1024)));
    return true;
}

void PipeStop()
{
    TracePipe& p = P();
    if (!p.running) return;

    InterlockedExchange(&p.stop, 1);
    // ⚠ 先撤销挂起的传输，再等线程。管道超时改成"无限"之后，32 个传输在目标
    //   不输出时会一直挂着，不先 CancelIoEx 线程就得等 2000 ms 超时才回来；
    //   反过来先取消，WaitForMultipleObjects 立刻全部唤醒，线程当轮就退出。
    //   事件与缓冲都留着复用（下次 Open 还要用）。
    if (St().traceFile != INVALID_HANDLE_VALUE) CancelIoEx(St().traceFile, nullptr);
    if (p.thr) {
        WaitForSingleObject(p.thr, 2000);
        CloseHandle(p.thr);
        p.thr = nullptr;
    }
    for (int i = 0; i < kPipeBlocks; ++i) p.blk[i].submitted = false;

    InterlockedExchange(&p.running, 0);
    if (p.dropped) {
        TrcWarn("trace pipe: ring overflowed, %llu bytes dropped", p.dropped);
    }
}

} // namespace

// ===========================================================================
// 公共 API
// ===========================================================================

int ORBMDK_OrbTrace_Detect(void)
{
    OrbTraceState& s = St();
    if (s.detected) return 1;

    if (ScanForOrbTrace()) {
        s.detected = true;
        TrcTrace("orbtrace detected: vid=%04X pid=%04X product='%s' serial='%s' version='%s' "
                 "(traceIf=%d powerIf=%d versionIf=%d OFLOW=%d)",
                 s.vid, s.pid, s.product, s.serial, s.version,
                 s.traceIf, s.powerIf, s.versionIf, s.supportsOFLOW ? 1 : 0);
        return 1;
    }
    TrcTrace("no orbtrace device found");
    return 0;
}

int ORBMDK_OrbTrace_GetInfo(ORBMDK_OrbTrace_Info* info)
{
    if (!info) return -1;
    memset(info, 0, sizeof(*info));

    OrbTraceState& s = St();
    if (!s.detected && !ORBMDK_OrbTrace_Detect()) {
        info->present = 0;
        return 0;   // 没找到不是错误
    }

    info->present       = 1;
    info->opened        = s.opened ? 1 : 0;
    info->vid           = s.vid;
    info->pid           = s.pid;
    info->traceIf       = s.traceIf;
    info->powerIf       = s.powerIf;
    info->versionIf     = s.versionIf;
    info->traceProtocol = s.traceProtocol;
    info->supportsOFLOW = s.supportsOFLOW ? 1 : 0;
    strncpy_s(info->product, sizeof(info->product), s.product, _TRUNCATE);
    strncpy_s(info->serial,  sizeof(info->serial),  s.serial,  _TRUNCATE);
    strncpy_s(info->version, sizeof(info->version), s.version, _TRUNCATE);
    return 0;
}

int ORBMDK_OrbTrace_Open(void)
{
    OrbTraceState& s = St();
    if (s.opened) return 0;
    if (!s.detected && !ORBMDK_OrbTrace_Detect()) return -1;

    // 重新枚举一次拿路径：接口号已知，但路径要现场找（设备可能重插）
    HDEVINFO devInfo = SetupDiGetClassDevs(&kWinUsbGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return -1;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);
    DWORD index = 0;
    bool traceOpened = false, powerOpened = false;

    while (SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kWinUsbGuid, index++, &did)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(malloc(need));
        if (!detail) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &did, detail, need, nullptr, nullptr)) {
            uint16_t vid = 0, pid = 0;
            if (ParseVidPid(detail->DevicePath, &vid, &pid) && IsOrbTraceVidPid(vid, pid)) {
                HANDLE file = INVALID_HANDLE_VALUE;
                WINUSB_INTERFACE_HANDLE wu = nullptr;
                if (OpenInterfacePath(detail->DevicePath, &file, &wu)) {
                    USB_INTERFACE_DESCRIPTOR ifd;
                    if (WinUsb_QueryInterfaceSettings(wu, 0, &ifd) &&
                        ifd.bInterfaceClass == 0xFF) {
                        if (ifd.bInterfaceSubClass == kSubClassTrace && !traceOpened) {
                            // 取该接口的第一个 bulk IN 端点
                            WINUSB_PIPE_INFORMATION pipe;
                            if (WinUsb_QueryPipe(wu, 0, 0, &pipe) &&
                                pipe.PipeType == UsbdPipeTypeBulk &&
                                (pipe.PipeId & 0x80) != 0) {   // IN 方向
                                s.traceFile  = file;
                                s.traceWinUsb = wu;
                                s.traceEp    = pipe.PipeId;
                                traceOpened  = true;
                                TrcTrace("trace interface %d opened (ep=0x%02X)",
                                         ifd.bInterfaceNumber, pipe.PipeId);
                                continue;   // 句柄已留用
                            }
                        } else if (ifd.bInterfaceSubClass == kSubClassPower && !powerOpened) {
                            s.powerFile  = file;
                            s.powerWinUsb = wu;
                            powerOpened  = true;
                            TrcTrace("power interface %d opened", ifd.bInterfaceNumber);
                            continue;
                        }
                    }
                    WinUsb_Free(wu);
                    CloseHandle(file);
                }
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);

    if (!traceOpened) {
        TrcWarn("orbtrace trace interface could not be opened");
        return -1;
    }

    // 起异步读取管线（32 个 overlapped 读常驻在飞 + 环形缓冲）。
    // 起不来就直接失败：读路径完全依赖它，开着一个读不出东西的流只会让人误判。
    if (!PipeStart()) {
        TrcWarn("orbtrace trace pipe could not be started");
        return -1;
    }

    s.opened = true;
    (void)powerOpened;   // 没有 power 接口也能跑 trace
    return 0;
}

void ORBMDK_OrbTrace_Close(void)
{
    OrbTraceState& s = St();

    // 先停管线：撤掉挂起的传输、收掉读线程。**必须在释放接口句柄之前**，
    // 否则线程还在用 s.traceWinUsb 就会踩空。
    PipeStop();

    if (s.traceWinUsb) { WinUsb_Free(s.traceWinUsb); s.traceWinUsb = nullptr; }
    if (s.traceFile != INVALID_HANDLE_VALUE) { CloseHandle(s.traceFile); s.traceFile = INVALID_HANDLE_VALUE; }
    if (s.powerWinUsb) { WinUsb_Free(s.powerWinUsb); s.powerWinUsb = nullptr; }
    if (s.powerFile != INVALID_HANDLE_VALUE) { CloseHandle(s.powerFile); s.powerFile = INVALID_HANDLE_VALUE; }
    s.opened = false;
}

int ORBMDK_OrbTrace_IsOpen(void)
{
    return St().opened ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

int ORBMDK_OrbTrace_SetTraceWidth(int width)
{
    if (!St().opened) return -1;
    // orbuculum: 4 → 3（1/2/4-bit 编码里 4-bit 记 3）
    uint16_t d = static_cast<uint16_t>((width != 4) ? width : 3);
    if (d < 1 || d > 3) return -1;
    return TraceRequest(kRqSetTWidth, d, nullptr, 0) ? 0 : -1;
}

int ORBMDK_OrbTrace_SetTraceSWO(int isManchester, int useTPIU)
{
    if (!St().opened) return -1;
    const uint16_t v = static_cast<uint16_t>((isManchester ? 0x10 : 0x12) | (useTPIU ? 0x01 : 0x00));
    return TraceRequest(kRqSetTWidth, v, nullptr, 0) ? 0 : -1;
}

int ORBMDK_OrbTrace_SetSWOBaudrate(uint32_t baud)
{
    if (!St().opened) return -1;
    // 4 字节小端（orbuculum 原样搬运）
    uint8_t le[4] = {
        static_cast<uint8_t>(baud & 0xFF),
        static_cast<uint8_t>((baud >> 8) & 0xFF),
        static_cast<uint8_t>((baud >> 16) & 0xFF),
        static_cast<uint8_t>((baud >> 24) & 0xFF),
    };
    return TraceRequest(kRqSetSpeed, 0, le, 4) ? 0 : -1;
}

int ORBMDK_OrbTrace_SetVoltage(int channel, int millivolts)
{
    if (!St().opened || !St().powerWinUsb) return -1;
    if (channel < 0 || channel >= ORBMDK_ORBTRACE_CH_MAX) return -1;
    if (millivolts < kMinVoltageMv || millivolts > kMaxVoltageMv) return -1;   // 同 orbuculum 校验
    return PowerRequest(kRqSetVoltage, static_cast<uint16_t>(millivolts),
                        static_cast<uint8_t>(channel)) ? 0 : -1;
}

int ORBMDK_OrbTrace_SetVoltageEnable(int channel, int on)
{
    if (!St().opened || !St().powerWinUsb) return -1;
    if (channel >= ORBMDK_ORBTRACE_CH_MAX && channel != ORBMDK_ORBTRACE_CH_ALL) return -1;
    return PowerRequest(kRqSetEnable, static_cast<uint16_t>(on ? 1 : 0),
                        static_cast<uint8_t>(channel)) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// trace 数据（raw）
// ---------------------------------------------------------------------------

int ORBMDK_OrbTrace_ReadTrace(uint8_t* buf, size_t len, int timeoutMs)
{
    TracePipe& p = P();
    if (!p.running || !buf || len == 0) return -1;

    // 有环之后这里只是 memcpy，不再有 USB 往返。数据由读取线程按 32 个常驻传输
    // 搬进环，本函数只负责取：
    //   timeoutMs == 0 -> 环空即返回 0（非阻塞）；< 0 -> 等到有数据；> 0 -> 最多等这么久
    const DWORD t0 = GetTickCount();
    for (;;) {
        const size_t n = RingPop(buf, len);
        if (n) return static_cast<int>(n);

        if (timeoutMs == 0) return 0;
        if (InterlockedCompareExchange(&p.stop, 0, 0) != 0) return 0;
        if (timeoutMs > 0 && (GetTickCount() - t0) >= static_cast<DWORD>(timeoutMs)) return 0;

        Sleep(1);   // 环是空的：让出 CPU，等读取线程填
    }
}

int ORBMDK_OrbTrace_GetDropped(unsigned long long* dropped)
{
    if (!dropped) return -1;
    *dropped = 0;

    TracePipe& p = P();
    std::lock_guard<std::mutex> g(p.lock);
    *dropped = p.dropped;
    return 0;
}
