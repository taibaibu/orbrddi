/**
 * @file ORBMDK_DAPV2.h
 * @brief CMSIS-DAP V2 协议定义
 *
 * 基于 CMSIS-DAP V2 规范，支持 USB Bulk 传输
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CMSIS-DAP V2 Protocol Version
// ============================================================================
#define DAP_V2_VERSION_MAJOR     2
#define DAP_V2_VERSION_MINOR     0

// ============================================================================
// CMSIS-DAP V2 Command IDs
// ============================================================================
enum ORBMDK_DAPV2_Command {
    // Vendor Commands (0x80-0x9F)
    ID_DAP_VENDOR_START          = 0x80U,
    ID_DAP_VENDOR_END            = 0x9FU,

    // Test Domain Timer Commands (0xA0-0xAF)
    ID_DAP_TIMER_START           = 0xA0U,
    ID_DAP_TIMER_READ            = 0xA1U,
    ID_DAP_TIMER_STOP            = 0xA2U,

    // SWO Trace Commands (0xB0-0xBF)
    ID_DAP_SWO_DATA_START        = 0xB0U,
    ID_DAP_SWO_DATA_STATUS       = 0xB1U,
    ID_DAP_SWO_DATA_READ         = 0xB2U,
    ID_DAP_SWO_DATA_STOP         = 0xB3U,

    // Access Port Commands (0xC0-0xCF)
    ID_DAP_AP_ABORT              = 0xC0U,
    ID_DAP_AP_BD0R               = 0xC1U,
    ID_DAP_AP_BD1R               = 0xC2U,
    ID_DAP_AP_BD2R               = 0xC3U,
    ID_DAP_AP_BD3R               = 0xC4U,
    ID_DAP_AP_REG_RW             = 0xCFU,

    // Memory Access Commands (0xD0-0xDF)
    ID_DAP_MEM_AP_BANKED_RW      = 0xD0U,
    ID_DAP_MEM_AP_REG_RW         = 0xD1U,
    ID_DAP_MEM_AP_64BIT_RW       = 0xD2U,
    ID_DAP_MEM_AP_64BIT_REG_RW   = 0xD3U,

    // Queue Transfer Commands (0xE0-0xEF)
    ID_DAP_QUEUE_RWW             = 0xE0U,
    ID_DAP_QUEUE_RWW_REG         = 0xE1U,
    ID_DAP_EXECUTE_RWW           = 0xE2U,
    ID_DAP_EXECUTE_RWW_REG       = 0xE3U,

    // V2 Info Commands
    ID_DAP_V2_INFO               = 0xF0U,
    ID_DAP_V2_CAPABILITIES_1     = 0xF1U,
    ID_DAP_V2_CAPABILITIES_2     = 0xF2U,
    ID_DAP_V2_TEST_DOMAIN_TIMER  = 0xF3U,
    ID_DAP_V2_PRODUCT_NAME       = 0xF4U,

    // Reserved for future use
    ID_DAP_V2_RESERVED           = 0xFFU,
};

// ============================================================================
// CMSIS-DAP V2 Capabilities
// ============================================================================
enum ORBMDK_DAPV2_Capabilities_1 {
    DAPV2_CAP_SWD                = (1U << 0),   // SWD implemented
    DAPV2_CAP_JTAG               = (1U << 1),   // JTAG implemented
    DAPV2_CAP_SWO_UART          = (1U << 2),   // SWO UART transport
    DAPV2_CAP_SWO_MANCHESTER    = (1U << 3),   // SWO Manchester transport
    DAPV2_CAP_SWO_SERIAL        = (1U << 4),   // SWO Serial transport
    DAPV2_CAP_SWO_MASK          = (0x7U << 2), // SWO mask
    DAPV2_CAP_ATOMIC            = (1U << 8),   // Atomic commands
    DAPV2_CAP_SWO_STREAMING     = (1U << 9),   // SWO streaming trace
    DAPV2_CAP_UART              = (1U << 10),  // UART communication
    DAPV2_CAP_UART_PORT         = (1U << 11),  // UART port configuration
    DAPV2_CAP_USB               = (1U << 12),  // USB communication
    DAPV2_CAP_FLASH_PG          = (1U << 13),  // Flash Patch / Breakpoints
    DAPV2_CAP_SEMIHOST          = (1U << 14),  // Semihosting
    DAPV2_CAP_TPIU              = (1U << 15),  // TPIU trace unit
    DAPV2_CAP_ITM               = (1U << 16),  // ITM trace unit
    DAPV2_CAP_PC_SAMPLE         = (1U << 17),  // PC Sample profiling
    DAPV2_CAP_SYNC_CLOCK        = (1U << 18),  // Synchronous clock mode
    DAPV2_CAP_DAP_DFU           = (1U << 19),  // DAP DFU supported
    DAPV2_CAP_RTT               = (1U << 20),  // RTT communication
    DAPV2_CAP_OSPI_FLASH        = (1U << 21),  // OctoSPI/HyperBus
    DAPV2_CAP_APCV2              = (1U << 22),  // APC (Arm Performance Counters)
    DAPV2_CAP_JTAG_2G           = (1U << 23),  // JTAG 2-pin support
};

enum ORBMDK_DAPV2_Capabilities_2 {
    DAPV2_CAP2_SWD_2G           = (1U << 0),   // SWD 2-pin support
    DAPV2_CAP2_EVENT_OUTPUT     = (1U << 1),   // Event Output
    DAPV2_CAP2_EVENT_INPUT      = (1U << 2),   // Event Input
    DAPV2_CAP2_TRACE_PORT       = (1U << 3),   // Trace Port
    DAPV2_CAP2_SWO_64BIT        = (1U << 4),    // SWO 64-bit data
    DAPV2_CAP2_PICOCOM          = (1U << 5),   // Picocom port
};

// ============================================================================
// CMSIS-DAP V2 Transfer Modes
// ============================================================================
enum ORBMDK_DAPV2_Transfer_Mode {
    DAPV2_MODE_DEFAULT           = 0,
    DAPV2_MODE_SWD               = 1,
    DAPV2_MODE_JTAG               = 2,
    DAPV2_MODE_SWD_MULTI_DROP    = 3,  // SWD with multiple targets
};

// ============================================================================
// CMSIS-DAP V2 Packet Types
// ============================================================================
enum ORBMDK_DAPV2_Packet_Type {
    DAPV2_PACKET_CMD              = 0x00U,  // Request
    DAPV2_PACKET_RESP             = 0x01U,  // Response
    DAPV2_PACKET_CMD_RT          = 0x02U,  // Request with timestamp
    DAPV2_PACKET_RESP_RT         = 0x03U,  // Response with timestamp
    DAPV2_PACKET_EVENT           = 0x04U,  // Event (async)
    DAPV2_PACKET_EVENT_RT        = 0x05U,  // Event with timestamp
};

// ============================================================================
// CMSIS-DAP V2 Event Flags
// ============================================================================
enum ORBMDK_DAPV2_Event {
    DAPV2_EVENT_SWJ_PINS         = (1U << 0),  // SWJ Pin changed
    DAPV2_EVENT_SWJ_COMMANDS     = (1U << 1),  // SWJ sequence executed
    DAPV2_EVENT_TARGET_RUNNING    = (1U << 2),  // Target is running
    DAPV2_EVENT_TARGET_HALTED     = (1U << 3),  // Target halted
    DAPV2_EVENT_TARGET_RESET     = (1U << 4),  // Target reset
    DAPV2_EVENT_SWO_DATA         = (1U << 5),  // SWO data available
    DAPV2_EVENT_SWO_BUFFER_OVER  = (1U << 6),  // SWO buffer overflow
    DAPV2_EVENT_DAP_ERROR        = (1U << 7),  // DAP error
};

// ============================================================================
// DAP V2 Info IDs
//
// ⚠️ 下面这套编号**与 CMSIS-DAP 规范的 DAP_Info ID 完全不对齐**（同样例：规范里
//    0x03 是序列号、0x04 是协议版本、0x09 才是产品固件版本），**不要**拿它当命令参数
//    发给设备。发 DAP_Info 一律用 ORBMDK_DAP.h 的 `DAP_INFO_*`（绕行写法见
//    src/ORBMDK_USB_Bulk.cpp 的 CMSIS_DAP_V2_GetInfo）。这里只定义本层 API 的参数编号。
// ============================================================================
enum ORBMDK_DAPV2_Info {
    DAPV2_INFO_PRODUCT_NAME      = 0x01U,  // Product name string
    DAPV2_INFO_SERIAL_NUMBER     = 0x02U,  // Serial number string
    DAPV2_INFO_FIRMWARE_VERSION  = 0x03U,  // Firmware version string
    DAPV2_INFO_VENDOR_STRING     = 0x04U,  // Vendor name string
    DAPV2_INFO_FAMILY_ID         = 0x05U,  // Device family ID
    DAPV2_INFO_PRODUCT_ID        = 0x06U,  // Product ID
    DAPV2_INFO_CAPABILITIES      = 0x07U,  // Capabilities bitmap
    DAPV2_INFO_TEST_DOMAIN_TIMER = 0x08U, // Test Domain Timer
    DAPV2_INFO_INTEL_HEX        = 0x09U,  // Intel HEX support
    DAPV2_INFO_MCUCID           = 0x0AU,  // MCU Chip ID
    DAPV2_INFO_CAPABILITIES_1    = 0xF1U, // Capabilities 1 (extended)
    DAPV2_INFO_CAPABILITIES_2    = 0xF2U, // Capabilities 2 (extended)
};

// ============================================================================
// Transfer Timing (V2 only)
// ============================================================================
#define DAPV2_TIMESTAMP_FREQ_MIN    1000000U    // 1 MHz minimum
#define DAPV2_TIMESTAMP_FREQ_MAX    100000000U  // 100 MHz maximum

// ============================================================================
// USB Bulk Transfer Support (V2)
// ============================================================================
typedef struct {
    uint8_t protocol_version;
    uint8_t capability_flags;
    uint8_t reserved[2];
    uint32_t timestamp_clock;         // Test domain timer frequency
    uint32_t max_packet_count;        // Max packet count
    uint32_t max_packet_size;         // Max packet size
} ORBMDK_DAPV2_Info_Block;

// ============================================================================
// V2 Transfer Header (for timestamped transfers)
// ============================================================================
typedef struct {
    uint32_t timestamp;               // Timestamp in test domain clock
    uint16_t command_count;          // Number of commands
    uint8_t  reserved;
    uint8_t  response_count;         // Expected response count
} ORBMDK_DAPV2_Transfer_Header;

#ifdef __cplusplus
}
#endif
