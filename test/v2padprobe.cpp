// V2 帧格式 / 响应分片探测（**直接**走 WinUSB，不经过 ORBMDK 驱动层）
//
// 目的：orbtrace 的 CMSIS-DAP v2 接口（MI_05，EP 0x03/0x85，描述符 wMaxPacketSize=512）
//       ① OUT 包要按多少字节填充；② 响应是不是**多段**发送/滞后一拍。
//
// 上一轮（单读一次）的结论可疑：
//   pad=2 无响应；pad=64/128/256/512 各读到 2 字节 [00 00]；
//   同一句柄连发两条时出现"第 1 条超时、第 2 条成功"的滞后现象。
// 所以这轮改成：打开 -> 先排空 -> 每条命令后**循环读多段**并逐段打印。
//
// 构建： powershell -File test\build_test.ps1 -Source v2padprobe.cpp
// 运行： bin\v2padprobe.exe [pad]     pad 省略时扫 64 / 512

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <stdio.h>
#include <stdint.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")

static const GUID kIfGuid = {
    0xdee824ef, 0x729b, 0x4a0e,
    {0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17}
};

typedef struct {
    HANDLE  dev;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR   inPipe;
    UCHAR   outPipe;
} V2Dev;

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

// 单次写（不算响应）
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
// 注意：超时时**只** CancelIoEx，不 AbortPipe/ResetPipe —— 复位会把迟到的
// 响应一起冲掉，导致下一轮才看到数据（"滞后一拍"的假象就是这么来的）。
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
            ret = -2;
        }
    } else { ret = ok ? (int)rd : -1; }
    if (msOut) *msOut = GetTickCount64() - t0;
    CloseHandle(ov.hEvent);
    return ret;
}

static void Dump(const char* tag, const uint8_t* p, int n, int cap)
{
    printf("      %-8s len=%-3d :", tag, n);
    int lim = (n < cap) ? n : cap;
    for (int i = 0; i < lim; ++i) printf(" %02X", p[i]);
    if (n > cap) printf(" ...");
    printf("\n");
}

// 排空 IN：把设备可能"不请自来"的包读掉，返回读掉的总字节数
static int DrainIn(V2Dev* d, int ms)
{
    uint8_t tmp[1024];
    int total = 0;
    for (int i = 0; i < 6; ++i) {
        const int n = ReadOnce(d, tmp, sizeof(tmp), ms, NULL);
        if (n <= 0) break;
        printf("      [drain] 清掉 %d 字节:", n);
        for (int k = 0; k < n && k < 12; ++k) printf(" %02X", tmp[k]);
        printf("\n");
        total += n;
    }
    return total;
}

// 发一条命令 + 循环读多段；返回总字节数
static int Cmd(V2Dev* d, int infoId, ULONG pad, int perReadMs)
{
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x00;              // ID_DAP_INFO
    buf[1] = (uint8_t)infoId;
    ULONG len = (pad > 2) ? pad : 2;

    if (!WriteOnce(d, buf, len, 500)) { printf("    write FAILED\n"); return 0; }

    uint8_t out[1024];
    int total = 0;
    for (int seg = 0; seg < 4; ++seg) {
        const ULONG cap = (pad > 2 && pad <= 1024) ? pad : 64;
        ULONGLONG ms = 0;
        const int n = ReadOnce(d, out, cap, perReadMs, &ms);
        if (n > 0) {
            char tag[32];
            sprintf_s(tag, sizeof(tag), "seg%d", seg);
            Dump(tag, out, n, 16);
            printf("               (等待 %llu ms)\n", ms);
            total += n;
        } else {
            printf("      seg%d     rc=%d 等待 %llu ms (无更多数据)\n",
                   seg, n, ms);
            break;
        }
    }
    return total;
}

// 同一条命令连发 n 次：看"能不能持续一问一答"、响应慢不慢、有没有滞后
static void Repeat(V2Dev* d, int infoId, ULONG pad, int times, int perReadMs)
{
    printf("  >>> 连发 %d 次 DAP_Info 0x%02X (pad=%lu, 每次读 %d ms)\n",
           times, infoId, (unsigned long)pad, perReadMs);
    for (int k = 0; k < times; ++k) {
        printf("    #%d ", k + 1);
        fflush(stdout);

        uint8_t buf[1024];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x00; buf[1] = (uint8_t)infoId;
        ULONG len = (pad > 2) ? pad : 2;
        if (!WriteOnce(d, buf, len, 500)) { printf("write FAILED\n"); continue; }

        uint8_t out[1024];
        ULONGLONG ms = 0;
        const int n = ReadOnce(d, out, (pad > 2 && pad <= 1024) ? pad : 64, perReadMs, &ms);
        if (n > 0) {
            printf("resp len=%-3d (%llu ms) :", n, ms);
            for (int i = 0; i < n && i < 12; ++i) printf(" %02X", out[i]);
            printf("\n");
        } else {
            printf("NO RESPONSE (rc=%d, %llu ms)\n", n, ms);
        }
        fflush(stdout);
        Sleep(30);
    }
}

static void RunOne(ULONG pad)
{
    printf("\n================ pad=%lu ================\n", (unsigned long)pad);
    V2Dev d;
    if (!V2Open(&d)) { printf("打开设备失败\n"); return; }
    printf("  opened: in=0x%02X out=0x%02X\n", d.inPipe, d.outPipe);

    printf("  [0] 排空（看有没有不请自来的包）:\n");
    DrainIn(&d, 200);

    Repeat(&d, 0xF0, pad, 5, 2000);   // capabilities: 期望 00 01 03
    Repeat(&d, 0xFF, pad, 3, 2000);   // packet size : 期望 00 02 ...
    Repeat(&d, 0x01, pad, 2, 2000);   // vendor

    V2Close(&d);
}

int main(int argc, char** argv)
{
    if (argc > 1 && argv[1] && argv[1][0]) {
        RunOne((ULONG)atoi(argv[1]));
    } else {
        RunOne(64);
        RunOne(508);
        RunOne(512);
    }
    return 0;
}
