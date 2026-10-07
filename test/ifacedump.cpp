// 接口清点：列出某 VID/PID 下**所有**可见接口（WinUSB / CMSIS-DAP / HID）及端点。
//
// 用途：判断复合设备到底有几个功能口、哪个才是 DAP 通道。
// 起因：0D28:0204 是 4 接口复合设备，而 ORBMDK 的枚举"打开成功即 break"，
//       MI_04（另一个 class 0xFF 的 WinUSB 接口）从未被看过一眼。
//
// 只读：仅打开接口句柄 + 查描述符，**不发任何 USB 命令**，不改设备状态。
//
// 构建： powershell -File test\build_test.ps1 -Source ifacedump.cpp
// 运行： bin\ifacedump.exe [vid] [pid]        默认 0x0D28 0x0204

#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "hid.lib")

static const GUID GUID_WINUSB = {0xDEE824EF, 0x729B, 0x4A0E,
                                 {0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17}};
static const GUID GUID_DAPV2  = {0xCDB3B5AD, 0x293B, 0x4663,
                                 {0xAA, 0x36, 0x1A, 0xAE, 0x46, 0x46, 0x37, 0x76}};

// 设备接口路径里取 vid/pid/mi，例如：
//   \\?\usb#vid_0d28&pid_0204&mi_04#6&c666865&2&0004#{dee824ef-...}
//   \\?\hid#vid_0d28&pid_0204&mi_03#7&10cb485a&0&0000#{4d1e55b2-...}
static bool ParsePath(const char* path, unsigned* vid, unsigned* pid, int* mi)
{
    const char* v = NULL;
    for (const char* p = path; *p; ++p) {
        if (_strnicmp(p, "vid_", 4) == 0) { v = p; break; }
    }
    if (!v) return false;

    unsigned vv = 0, pp = 0;
    if (sscanf_s(v, "vid_%4x&pid_%4x", &vv, &pp) != 2) return false;

    *mi = -1;
    const char* m = strstr(path, "mi_");
    if (m) {
        unsigned mm = 0;
        if (sscanf_s(m, "mi_%2x", &mm) == 1) *mi = (int)mm;
    }
    *vid = vv;
    *pid = pp;
    return true;
}

static const char* PipeTypeName(USBD_PIPE_TYPE t)
{
    switch (t) {
    case UsbdPipeTypeControl:     return "Control";
    case UsbdPipeTypeIsochronous: return "Iso";
    case UsbdPipeTypeBulk:        return "Bulk";
    case UsbdPipeTypeInterrupt:   return "Interrupt";
    default:                      return "?";
    }
}

// 枚举某个接口 GUID 下的所有实例，按 vid/pid 过滤，逐个查接口描述符与端点
static void DumpWinUsbInterfaces(const GUID& guid, const char* tag,
                                 unsigned wantVid, unsigned wantPid)
{
    HDEVINFO di = SetupDiGetClassDevsA(&guid, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) {
        printf("\n[%s] SetupDiGetClassDevs failed, err=%lu\n", tag, GetLastError());
        return;
    }

    SP_DEVICE_INTERFACE_DATA did = {0};
    did.cbSize = sizeof(did);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &guid, i, &did); ++i) {
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

        unsigned vid = 0, pid = 0;
        int mi = -1;
        if (!ParsePath(det->DevicePath, &vid, &pid, &mi) ||
            vid != wantVid || pid != wantPid) {
            free(det);
            continue;
        }

        printf("\n[%s] mi_%02d  %s\n", tag, mi < 0 ? -1 : mi, det->DevicePath);

        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        free(det);
        if (h == INVALID_HANDLE_VALUE) {
            printf("      CreateFile 失败, err=%lu (被占用/无权限)\n", GetLastError());
            continue;
        }

        WINUSB_INTERFACE_HANDLE wu = NULL;
        if (!WinUsb_Initialize(h, &wu)) {
            printf("      WinUsb_Initialize 失败, err=%lu (该接口未绑定 WinUSB?)\n",
                   GetLastError());
            CloseHandle(h);
            continue;
        }

        USB_INTERFACE_DESCRIPTOR idesc = {0};
        if (WinUsb_QueryInterfaceSettings(wu, 0, &idesc)) {
            printf("      class=0x%02X sub=0x%02X prot=0x%02X 端点数=%u iInterface=%u\n",
                   idesc.bInterfaceClass, idesc.bInterfaceSubClass,
                   idesc.bInterfaceProtocol, idesc.bNumEndpoints,
                   idesc.iInterface);

            for (UCHAR n = 0; n < idesc.bNumEndpoints; ++n) {
                WINUSB_PIPE_INFORMATION pi = {0};
                if (!WinUsb_QueryPipe(wu, 0, n, &pi)) continue;
                printf("        ep 0x%02X  %-9s maxPkt=%-4u interval=%u\n",
                       pi.PipeId, PipeTypeName(pi.PipeType),
                       pi.MaximumPacketSize, pi.Interval);
            }

            // 接口名（UTF-16 -> 窄字符，仅取 ASCII 部分）
            if (idesc.iInterface) {
                wchar_t ws[256] = {0};
                ULONG got = 0;
                if (WinUsb_GetDescriptor(wu, USB_STRING_DESCRIPTOR_TYPE,
                                         (UCHAR)idesc.iInterface, 0x0409,
                                         (PUCHAR)ws, sizeof(ws), &got)) {
                    char narrow[256] = {0};
                    int k = 0;
                    for (int j = 1; j < (int)(got / 2) && k < 255; ++j) {
                        narrow[k++] = (char)(ws[j] & 0xFF);
                    }
                    printf("      iInterface 名: '%s'\n", narrow);
                }
            }
        } else {
            printf("      WinUsb_QueryInterfaceSettings 失败, err=%lu\n", GetLastError());
        }

        WinUsb_Free(wu);
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(di);
}

// HID 类接口（对照用：确认某个 mi_ 到底是 HID 还是 WinUSB）
static void DumpHidInterfaces(unsigned wantVid, unsigned wantPid)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO di = SetupDiGetClassDevsA(&hidGuid, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) {
        printf("\n[HID] SetupDiGetClassDevs failed, err=%lu\n", GetLastError());
        return;
    }

    SP_DEVICE_INTERFACE_DATA did = {0};
    did.cbSize = sizeof(did);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &hidGuid, i, &did); ++i) {
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

        unsigned vid = 0, pid = 0;
        int mi = -1;
        if (!ParsePath(det->DevicePath, &vid, &pid, &mi) ||
            vid != wantVid || pid != wantPid) {
            free(det);
            continue;
        }

        printf("\n[HID] mi_%02d  %s\n", mi < 0 ? -1 : mi, det->DevicePath);

        HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        free(det);
        if (h == INVALID_HANDLE_VALUE) {
            printf("      CreateFile 失败, err=%lu\n", GetLastError());
            continue;
        }

        HIDD_ATTRIBUTES attrs = {0};
        attrs.Size = sizeof(attrs);
        if (HidD_GetAttributes(h, &attrs)) {
            wchar_t prod[256] = {0};
            char    prodNarrow[256] = {0};
            if (HidD_GetProductString(h, prod, sizeof(prod))) {
                int k = 0;
                for (int j = 0; prod[j] && k < 255; ++j) {
                    prodNarrow[k++] = (char)(prod[j] & 0xFF);
                }
            }
            printf("      VID=0x%04X PID=0x%04X ver=0x%04X product='%s'\n",
                   attrs.VendorID, attrs.ProductID, attrs.VersionNumber, prodNarrow);
        }

        PHIDP_PREPARSED_DATA pp = NULL;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps = {0};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                printf("      UsagePage=0x%04X Usage=0x%04X\n",
                       caps.UsagePage, caps.Usage);
                printf("      InputReportByteLength=%u OutputReportByteLength=%u "
                       "FeatureReportByteLength=%u\n",
                       caps.InputReportByteLength, caps.OutputReportByteLength,
                       caps.FeatureReportByteLength);
                printf("      NumberInputValueCaps=%u NumberOutputValueCaps=%u\n",
                       caps.NumberInputValueCaps, caps.NumberOutputValueCaps);
            }
            HidD_FreePreparsedData(pp);
        }

        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(di);
}

int main(int argc, char** argv)
{
    unsigned vid = 0x0D28, pid = 0x0204;
    if (argc > 2) {
        vid = (unsigned)strtoul(argv[1], NULL, 0);
        pid = (unsigned)strtoul(argv[2], NULL, 0);
    }

    printf("=== 接口清点 vid=0x%04X pid=0x%04X ===\n", vid, pid);
    printf("（只读：只列描述符/端点，不发任何 USB 命令）\n");

    DumpWinUsbInterfaces(GUID_WINUSB, "WinUSB-GUID", vid, pid);
    DumpWinUsbInterfaces(GUID_DAPV2,  "DAPv2-GUID",  vid, pid);
    DumpHidInterfaces(vid, pid);

    printf("\n=== 结束 ===\n");
    return 0;
}
