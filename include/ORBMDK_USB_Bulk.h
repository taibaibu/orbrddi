/**
 * @file ORBMDK_USB_Bulk.h
 * @brief USB Bulk 传输层 (CMSIS-DAP V2)
 *
 * 使用 WinUSB 或 libusb 实现 USB Bulk 传输
 */

#pragma once

#include "ORBMDK.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB Bulk V2 初始化状态
typedef enum {
    USB_BULK_NOT_INITED   = 0,
    USB_BULK_HID_MODE     = 1,    // V1 HID 模式
    USB_BULK_BULK_MODE    = 2,    // V2 Bulk 模式
} USB_Bulk_Mode;

// 传输模式偏好
//
// 需求：V1(HID) 与 V2(Bulk) 两种都要能**显式选择使用**，而不是只能被动回退。
//   AUTO ：优先 V2，不可用才回退 V1（仅用于"用户尚未选定接口"的窗口期）
//   BULK ：只用 V2；不可用直接失败（不做静默降级）
//   HID  ：只用 V1
//
// 选择来源只有一处：CMSIS_DAP_ConfigureInterface(ifNo)。没有文件/环境变量开关。
typedef enum {
    USB_BULK_TRANSPORT_AUTO = 0,
    USB_BULK_TRANSPORT_BULK = 1,
    USB_BULK_TRANSPORT_HID  = 2,
} USB_Bulk_TransportPref;

// 连接信息
typedef struct {
    uint16_t vid;
    uint16_t pid;
    char serial[64];
    char product[128];
    uint8_t protocol_version;     // 1 = HID, 2 = Bulk
    uint32_t max_packet_size;
    uint32_t max_packet_count;
} USB_Bulk_Device_Info;

// ============================================================================
// USB Bulk API 函数
// ============================================================================

/**
 * @brief 初始化 USB Bulk 传输层
 * @param vid  Vendor ID
 * @param pid  Product ID
 * @param serial 设备序列号 (可选)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Init(uint16_t vid, uint16_t pid, const char* serial);

/**
 * @brief 关闭 USB Bulk 连接
 */
ORBMDK_INTERNAL void ORBMDK_USB_Bulk_Shutdown(void);

/**
 * @brief 检查是否已连接
 * @return 当前传输模式
 */
ORBMDK_INTERNAL USB_Bulk_Mode ORBMDK_USB_Bulk_GetMode(void);

/**
 * @brief 获取设备信息
 * @param info 输出设备信息
 * @return 0 成功
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetDeviceInfo(USB_Bulk_Device_Info* info);

/**
 * @brief 获取 V2 接口的产品名
 *
 * 来自 Bulk 接口自己的字符串描述符，通常为 "CMSIS-DAP v2"。
 * Keil 对话框里的适配器名字就是 CMSIS_DAP_Identify(idNo=2) 返回的产品名，
 * V2 模式下必须用这个而不是 HID 接口的产品名（否则显示 "CMSIS-DAP v1"）。
 *
 * @param buf 输出缓冲
 * @param len 缓冲长度
 * @return 0 成功（取到名字）, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetProductName(char* buf, size_t len);

/**
 * @brief 获取当前配置的传输模式偏好
 * @return USB_Bulk_TransportPref 取值
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetTransportPreference(void);

/**
 * @brief 按"接口序号"选定传输层（来自 AGDI 的 CMSIS_DAP_ConfigureInterface）
 *
 *   ifNo 0 -> CMSIS-DAP v2 (USB Bulk)
 *   ifNo 1 -> CMSIS-DAP v1 (HID)
 *
 * 这是传输层的**唯一**选择来源（没有文件/环境变量开关）。必要时现场切换；
 * 选中的通道打不开时返回非 0（上层据此如实报错，不做静默降级）。
 *
 * @return 0 成功
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_SelectInterface(int ifNo);

/**
 * @brief 某个接口序号对应的适配器名字（AGDI 对话框列表里显示的就是它）
 * @return 0 成功
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetInterfaceName(int ifNo, char* buf, size_t len);

/**
 * @brief 获取 V2 接口所属设备的序列号
 *
 * V2 模式下没有打开 HID 接口，HidD_GetSerialNumberString 不可用；
 * 序列号改从设备的 iSerialNumber 字符串描述符读，供 CMSIS_DAP_Identify(idNo=3)。
 *
 * @param buf 输出缓冲
 * @param len 缓冲长度
 * @return 0 成功（取到序列号）, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetSerialNumber(char* buf, size_t len);

/**
 * @brief 发送 DAP 命令并接收响应
 * @param cmd 命令数据
 * @param cmdLen 命令长度
 * @param resp 响应缓冲区
 * @param respLen 输入:缓冲区大小, 输出:实际响应长度
 * @param timeoutMs 超时时间(ms)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_DAPCommand(const uint8_t* cmd, size_t cmdLen,
                                uint8_t* resp, size_t* respLen, int timeoutMs);

/**
 * @brief 当前传输模式下"一条命令能占用的最大字节数"
 *
 *   - V2 Bulk：返回自标定得到的有效出包长度（见 _bulkPacketSize）。命令长度
 *     **不能超过它**，否则发出去的就不是"已验证过的那个长度"，可能踩中
 *     "恰为整包 → 固件等下一包 → 命令永不派发"的坑。
 *   - 非 Bulk（V1 HID / 未初始化）：返回 0，调用方据此走 HID 的 64 字节规则。
 *
 * 供 RDDI 层决定单次块传输（ID_DAP_TRANSFER_BLOCK）能带多少字：V2 下把每次
 * USB 往返的载荷从 14 字提升到 (出包-5)/4 字，这是烧录吞吐的主杠杆。
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetMaxCommandBytes(void);

/**
 * @brief 当前 V2 链路的端口速度（WinUsb DEVICE_SPEED 原值）
 * @return 1=Low 2=Full 3=High；0 = 未连接 / V1 HID
 *
 * 用于把实测速率与总线理论上限对比 —— 若已接近上限，说明瓶颈在物理层，
 * 驱动侧再怎么改也没用（只能改高速硬件）；若远低于上限，才值得查驱动。
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetDeviceSpeed(void);

/**
 * @brief 发送原始数据 (V2 Bulk 模式)
 * @param data 数据
 * @param len 数据长度
 * @param timeoutMs 超时
 * @return 发送的字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Write(const uint8_t* data, size_t len, int timeoutMs);

/**
 * @brief 接收原始数据 (V2 Bulk 模式)
 * @param data 接收缓冲区
 * @param maxLen 最大接收长度
 * @param timeoutMs 超时
 * @return 接收的字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Read(uint8_t* data, size_t maxLen, int timeoutMs);

/**
 * @brief 获取 V2 协议能力
 * @return 能力标志
 */
ORBMDK_INTERNAL uint32_t ORBMDK_USB_Bulk_GetCapabilities(void);

/**
 * @brief 设置 V2 协议超时
 * @param timeoutMs 超时时间(ms)
 */
ORBMDK_INTERNAL void ORBMDK_USB_Bulk_SetTimeout(int timeoutMs);

// ============================================================================
// CMSIS-DAP V2 专用 API
// ============================================================================

/**
 * @brief 获取 V2 协议信息
 * @param info_id 信息 ID
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return >0 成功 (返回长度), <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_GetInfo(uint8_t info_id, uint8_t* buffer, size_t buffer_len);

/**
 * @brief 启动 V2 时间戳
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStart(void);

/**
 * @brief 读取 V2 时间戳
 * @param timestamp 时间戳输出
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerRead(uint32_t* timestamp);

/**
 * @brief 停止 V2 时间戳
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStop(void);

/**
 * @brief 设置 V2 超时
 * @param timeout_ms 超时(ms)
 * @return 0 成功
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_SetTimeout(int timeout_ms);

/**
 * @brief 获取 V2 协议版本
 * @return 协议版本 (1=HID, 2=Bulk)
 */
ORBMDK_INTERNAL uint8_t CMSIS_DAP_V2_GetProtocolVersion(void);

/**
 * @brief 获取 V2 能力标志
 * @return 能力位掩码
 */
ORBMDK_INTERNAL uint32_t CMSIS_DAP_V2_GetCapabilities(void);

// ============================================================================
// USB Bulk 直接读写 (V2 模式)
// ============================================================================

/**
 * @brief 打开 USB Bulk 设备
 * @param vid Vendor ID
 * @param pid Product ID
 * @param serial 序列号 (可选)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Open(uint16_t vid, uint16_t pid, const char* serial);

/**
 * @brief 关闭 USB Bulk 设备
 */
ORBMDK_INTERNAL void ORBMDK_Bulk_Close(void);

/**
 * @brief USB Bulk 写入
 * @param data 数据
 * @param len 长度
 * @param timeout_ms 超时
 * @return 写入字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Write(const uint8_t* data, size_t len, int timeout_ms);

/**
 * @brief USB Bulk 读取
 * @param data 接收缓冲区
 * @param max_len 最大长度
 * @param timeout_ms 超时
 * @return 读取字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Read(uint8_t* data, size_t max_len, int timeout_ms);

#ifdef __cplusplus
}
#endif
