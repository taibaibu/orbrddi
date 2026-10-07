/**
 * @file ORBMDK.h
 * @brief ORBMDK - ORBTrace CMSIS-DAP AGDI 适配层
 *
 * ORBMDK provides AGDI (Application Generic Debug Interface) support for ORBTrace
 * debuggers using CMSIS-DAP protocol over USB HID.
 *
 * Architecture:
 *   Keil uVision (AGDI) -> ORBMDK_AGDI -> ORBMDK_RDDI -> USB HID -> ORBTrace
 */

#pragma once

// ORBMDK_NormalizeFwVersion 用到 sscanf_s / sprintf_s / strncpy_s / _TRUNCATE：
// 本头不依赖包含者先引过 stdio/string，自带这两条。
#include <stdio.h>
#include <string.h>

#define ORBMDK_EXPORT __declspec(dllexport)
#define ORBMDK_IMPORT __declspec(dllimport)

#ifdef ORBMDK_EXPORTS
#define ORBMDK_API      ORBMDK_EXPORT   // 构建 DLL：导出
#define ORBMDK_INTERNAL                 // 内部函数，不导出
#else
#define ORBMDK_API      ORBMDK_IMPORT   // 使用 DLL：导入
#define ORBMDK_INTERNAL                 // 静态库或直接包含时不导出
#endif

// Version info
#define ORBMDK_VERSION_MAJOR  1
#define ORBMDK_VERSION_MINOR  0
#define ORBMDK_VERSION_PATCH  0
#define ORBMDK_VERSION_STRING "1.0.0"

// ----------------------------------------------------------------------------
// 上报给宿主的固件版本串（Identify(idNo=4) / DAP_Info）
//
// 取值来自**设备自报的 CMSIS-DAP `DAP_Info(0x04)`**：打开设备后问一次、缓存进
// RDDI 上下文（见 src/ORBMDK_RDDI.cpp 的 RDDI_Open），每次 Identify(idNo=4) 都回
// 这一份。**不要**改用 USB 设备描述符的 bcdDevice 当版本：那是 USB 栈/引导程序写的
// 字段，与固件真实版本无关，已弃用。
//
// ★ 当前口径（2026-10-01 放开）：上报串的主版本**必须 ≥ 2**，次版本/修订号保留真实值。
//   原因：AGDI 把该串按 "%lu.%lu.%lu" 解析后 `cmp eax, 2`（0x1003CB97），主版本 < 2 时
//   它压根不会进入 streaming sink 注册分支，配置串里的 TraceTransport 也永远只写 `Read`。
//   要让 µVision 的 Trace 页真正走流式（`TraceTransport=Stream;`），这一半门控必须打开。
//   另一半门控是 `CMSIS_DAP_Capabilities` 里的 `INFO_CAPS_SWO_STREAMING_TRACE(0x40)`，
//   两道**必须同侧**（Todo.md §18.10-C「两条总原则」第一条）。
//
// ----------------------------------------------------------------------------
#define ORBMDK_FALLBACK_FWVER_STRING "1.0.0"

// 把**设备 DAP_Info(0x04) 回的**版本串归一化成"上报串"（唯一口径）：
//   - 空串 / 解析不出 -> ORBMDK_FALLBACK_FWVER_STRING
//   - 主版本 < 2      -> 提升为 2，次版本/修订号原样保留（1.1.0 -> 2.1.0）
//   - 主版本 ≥ 2      -> 原样上报
// 注意这里是"提升"而不是"照抄"：多数 orbtrace 固件自报 1.x，而门控只看主版本，
// 不提升则 streaming 分支永远不会被触发。
static inline void ORBMDK_NormalizeFwVersion(const char* raw, char* out, size_t outLen)
{
    if (!out || outLen == 0) {
        return;
    }

    unsigned major = 0, minor = 0, patch = 0;
    const bool parsed = (raw && raw[0] != '\0' &&
                         sscanf_s(raw, "%u.%u.%u", &major, &minor, &patch) >= 1);
    if (!parsed) {
        strncpy_s(out, outLen, ORBMDK_FALLBACK_FWVER_STRING, _TRUNCATE);
        return;
    }

    if (major < 2) {
        major = 2;   // AGDI「streaming sink」门控；与 caps 的 0x40 同侧，见上
    }
    sprintf_s(out, outLen, "%u.%u.%u", major, minor, patch);
}

// ----------------------------------------------------------------------------
// ORBTrace USB 身份 (VID/PID)
//
// 必须与 orbtrace 固件保持一致，否则 V1(HID)/V2(Bulk) 会匹配到不同设备：
//   - orbtrace 固件 (orbtrace/debug/cmsis_test.py): VENDOR_ID=0x1209, PRODUCT_ID=0x3443
//   - orbuculum (Src/orbtraceIf.c):               { 0x1209, 0x3443, DEVICE_ORBTRACE_MINI }
//   - orbtrace README udev 规则:                  1209:3443 / 1209:3442
//
// HID 层仅按 VID 匹配（PID 由设备枚举阶段宽松处理），Bulk 层按 VID+PID 精确匹配，
// 因此两处必须使用同一份常量，禁止各自定义。
// ----------------------------------------------------------------------------
#define ORBMDK_ORBTRACE_VID   0x1209U
#define ORBMDK_ORBTRACE_PID   0x3443U

// ----------------------------------------------------------------------------
// "首选"调试器身份 (VID/PID) —— V1(HID) 与 V2(Bulk) **共用这一份表**
//
// ⚠ 这不是准入白名单，只表示"优先级"（同机多台时先选表内设备）。
// 设备识别一律按能力特征：
//   V2(Bulk)：绑 WinUSB + 接口类 0xFF + Bulk IN/OUT 对 + DAP_Info 有应答
//   V1(HID) ：UsagePage 0xFF00 + OUT/IN 报告可用      + DAP_Info 有应答
// 因此任何合规范的第三方 CMSIS-DAP（不论 VID/PID）都能直接用，无需改代码。
//
//   X(VID, PID)
// 0x0D28:0x0204 = ARM 官方 CMSIS-DAP/DAPLink 身份，多数实现沿用。注意该
// VID/PID 的复合设备会暴露多个 0xFF 接口（MI_00 是 v2 DAP、MI_04 不是），
// 所以"表内命中"后仍必须过能力校验 —— 只有能力校验能区分是不是 DAP。
// ----------------------------------------------------------------------------
#define ORBMDK_DEVICE_VIDPID_LIST(X)          \
    X(0x1209U, 0x3443U) /* ORBTrace        */ \
    X(0x1209U, 0x3442U) /* ORBTrace mini   */ \
    X(0x0D28U, 0x0204U) /* ARM DAPLink/CMSIS-DAP */

struct ORBMDK_VidPidPair { uint16_t vid; uint16_t pid; };

/** 是否为首选（已知）设备 —— 只用于优先级，不构成准入条件（见上）。
 *  识别本身按能力特征；VID/PID 不能区分"是不是 DAP"。 */
static inline bool ORBMDK_IsPreferredDevice(uint16_t vid, uint16_t pid)
{
#define ORBMDK_VIDPID_ENTRY(v, p) { (v), (p) },
    static const ORBMDK_VidPidPair table[] = {
        ORBMDK_DEVICE_VIDPID_LIST(ORBMDK_VIDPID_ENTRY)
    };
#undef ORBMDK_VIDPID_ENTRY
    for (const ORBMDK_VidPidPair& d : table) {
        if (d.vid == vid && d.pid == pid) return true;
    }
    return false;
}
