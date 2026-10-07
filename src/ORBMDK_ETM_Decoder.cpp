/**
 * @file ORBMDK_ETM_Decoder.cpp
 * @brief ETM (Embedded Trace Macrocell) 解码器实现
 *
 * 支持 ETM v3.5 和 v4.x 协议
 * 参考: ARM Embedded Trace Macrocell Architecture Specification
 */

#include "pch.h"
#include "ORBMDK_ETM_Decoder.h"

#include <cstring>

// ============================================================================
// ETM 协议常量 (参考 ARM ETM 规范和 orbuculum 实现)
// ============================================================================

// ETM v3.5 关键 Opcode
#define ETM_V35_ASYNC         0x00  // A-SYNC - 同步包
#define ETM_V35_CYCCNT        0x04  // Cycle Count
#define ETM_V35_ISYNC         0x08  // Instruction Sync
#define ETM_V35_ISYNC_CYC     0x70  // ISYNC + Cycle Count
#define ETM_V35_TRIGGER       0x0C  // Trigger
#define ETM_V35_VMID          0x3C  // VMID
#define ETM_V35_TIMESTAMP     0x42  // Timestamp
#define ETM_V35_IGNORE        0x66  // Ignore
#define ETM_V35_CONTEXTID     0x6E  // Context ID
#define ETM_V35_EXCEPT_EXIT   0x76  // Exception Exit
#define ETM_V35_EXCEPT_ENTRY  0x7E  // Exception Entry
#define ETM_V35_BRANCH        0x80  // Branch (bit0=1)

// ETM v4 关键 Opcode
#define ETM_V4_EXT            0x00  // Extension
#define ETM_V4_INFO           0x01  // Info (PLCTL)
#define ETM_V4_TRACE_ON       0x04  // Trace On
#define ETM_V4_FUNC_RETURN    0x05  // Function Return
#define ETM_V4_EXCEPTION      0x06  // Exception
#define ETM_V4_EXCEPTION_RET  0x07  // Exception Return
#define ETM_V4_RESYNC         0x08  // Resync
#define ETM_V4_TSTAMP         0x02  // Timestamp (bits 1:0 = 0b10)
#define ETM_V4_TSTAMP_2       0x03  // Timestamp 2-byte
#define ETM_V4_TSTAMP_3       0x82  // Timestamp 3-byte
#define ETM_V4_TSTAMP_4       0x83  // Timestamp 4-byte
#define ETM_V4_CONTEXT        0x80  // Context (no payload)
#define ETM_V4_CONTEXT_P      0x81  // Context (with payload)
#define ETM_V4_SHORT_ADDR_IS0 0x95  // Short address, IS0
#define ETM_V4_SHORT_ADDR_IS1 0x96  // Short address, IS1
#define ETM_V4_LONG_ADDR_IS0  0x9A  // 32-bit address, IS0
#define ETM_V4_LONG_ADDR_IS1  0x9B  // 32-bit address, IS1
#define ETM_V4_LONG64_ADDR_IS0 0x9D  // 64-bit address, IS0
#define ETM_V4_LONG64_ADDR_IS1 0x9E  // 64-bit address, IS1
#define ETM_V4_ATOM_FMT1_1    0xF6  // Atom Format 1 (1E)
#define ETM_V4_ATOM_FMT1_2    0xF7  // Atom Format 1 (NE)
#define ETM_V4_ATOM_FMT2      0xD8  // Atom Format 2 (range)
#define ETM_V4_ATOM_FMT3      0xF8  // Atom Format 3 (range)
#define ETM_V4_EVENT          0x71  // Event (range 01110001-01111111)
#define ETM_V4_PREFIX         0xE8  // v4 Prefix

// ETM v4 原子指令编码
#define ATOM_E    0x01  // Execute always
#define ATOM_N    0x02  // Not execute
#define ATOM_W    0x03  // Wait for interrupt (commit on interrupt)
#define ATOM_C    0x04  // Commit only

// ============================================================================
// ETM v3.5 状态机
// ============================================================================
enum ETMv35_State {
    ETMV35_UNSYNCED,
    ETMV35_IDLE,
    ETMV35_COLLECT_BA_STD,      // 分支地址 - 标准格式
    ETMV35_COLLECT_BA_ALT,      // 分支地址 - 备选格式
    ETMV35_GET_IADDRESS,        // ISYNC 后获取指令地址
    ETMV35_GET_CYCLECOUNT,       // 获取周期计数
    ETMV35_GET_CONTEXTID,        // 获取上下文 ID
    ETMV35_GET_VMID,            // 获取 VMID
    ETMV35_GET_TSTAMP,          // 获取时间戳
    ETMV35_GET_TRIGGER          // 获取触发器
};

// ETM v4 状态机
enum ETMv4_State {
    ETMV4_UNSYNCED,
    ETMV4_IDLE,
    ETMV4_SHORT_IADDR_IS0,       // 短地址 IS0
    ETMV4_SHORT_IADDR_IS1,       // 短地址 IS1
    ETMV4_LONG_IADDR_IS0,        // 长地址 IS0
    ETMV4_LONG_IADDR_IS1,        // 长地址 IS1
    ETMV4_ATOM,                 // 原子指令
    ETMV4_CONTEXT,              // 上下文
    ETMV4_TSTAMP,              // 时间戳
    ETMV4_EVENT,               // 事件
    ETMV4_FUNC_RET,            // 函数返回
    ETMV4_EXCEP                // 异常
};

// ============================================================================
// 内部函数
// ============================================================================

/**
 * @brief 从字节流提取可变长度值 (小端序)
 */
static uint32_t _extractValue(const uint8_t* data, int len)
{
    uint32_t val = 0;
    for (int i = 0; i < len; i++) {
        val |= ((uint32_t)data[i]) << (i * 8);
    }
    return val;
}

/**
 * @brief 从字节流提取 Q1.7 格式值
 */
static int32_t _extractQ17(const uint8_t* data, int len)
{
    uint32_t val = _extractValue(data, len);
    // Q1.7 格式: 符号位 + 7 位小数
    if (len == 1) {
        int8_t s = (int8_t)val;
        return (int32_t)s;  // 直接作为 int8_t 返回
    }
    return (int32_t)val;
}

/**
 * @brief 发射分支事件
 */
static void _emitBranch(struct ORBMDK_ETM_Decoder* dec, uint32_t from, uint32_t to,
                        enum ORBMDK_ETM_BranchType type, bool conditional, bool taken)
{
    if (!dec) return;

    dec->prevAddr = from;
    dec->currentAddr = to;
    dec->stats.branchCount++;

    if (dec->branchCallback) {
        struct ORBMDK_ETM_Branch branch = {};
        branch.fromAddr = from;
        branch.toAddr = to;
        branch.type = type;
        branch.isConditional = conditional;
        branch.isTaken = taken;
        branch.timestamp = dec->cycleCount;
        dec->branchCallback(&branch, dec->userData);
    }
}

/**
 * @brief 发射同步事件
 */
static void _emitSync(struct ORBMDK_ETM_Decoder* dec, uint32_t addr)
{
    if (!dec) return;
    dec->stats.syncCount++;

    if (dec->syncCallback) {
        dec->syncCallback(addr, dec->userData);
    }
}

/**
 * @brief 发射异常事件
 */
static void _emitException(struct ORBMDK_ETM_Decoder* dec, uint8_t type, uint16_t number)
{
    if (!dec) return;
    dec->stats.exceptionCount++;

    if (dec->exceptionCallback) {
        dec->exceptionCallback(type, number, dec->userData);
    }
}

/**
 * @brief 发射指令执行事件
 */
static void _emitInstruction(struct ORBMDK_ETM_Decoder* dec, uint32_t addr, int count)
{
    if (!dec) return;
    dec->stats.instructionCount += count;
    dec->currentAddr = addr;
}

/**
 * @brief 发射上下文切换事件
 */
static void _emitContext(struct ORBMDK_ETM_Decoder* dec, uint32_t contextId, int len)
{
    if (!dec) return;
    dec->contextID = contextId;
    dec->contextIDlen = len;
    dec->stats.contextCount++;
}

// ============================================================================
// ETM v3.5 解码 (参考 orbuculum traceDecoder_etm35.c)
// ============================================================================

/**
 * @brief ETM v3.5 A-SYNC 同步检测
 * 需要连续收到 5 个或更多的 0x00 后跟 0x80
 */
static bool _detectETMv35Sync(struct ORBMDK_ETM_Decoder* dec, uint8_t byte)
{
    if (!dec) return false;

    if (byte == 0x00) {
        dec->asyncCount++;
    } else if (byte == 0x80 && dec->asyncCount >= 5) {
        dec->asyncCount = 0;
        return true;
    } else {
        dec->asyncCount = 0;
    }
    return false;
}

/**
 * @brief 解码 ETM v3.5 分支地址 (标准格式)
 * 格式: 0b1xxxxxxx (bit0=1 表示分支)
 */
static int _decodeETMv35Branch(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len < 1 || !consumed) return -1;

    uint8_t header = data[0];
    *consumed = 1;

    // 检查分支位 (bit 0)
    if ((header & 0x01) == 0) {
        // 非分支指令
        return 0;
    }

    uint32_t from = dec->currentAddr;

    // 分析地址编码方式
    // bit[3:1] = 00: 8 位偏移
    // bit[3:1] = 01: 11 位偏移
    // bit[3:1] = 10: 19 位偏移 (标准)
    // bit[3:1] = 11: 条件分支
    uint8_t addrBits = (header >> 1) & 0x03;

    switch (addrBits) {
    case 0: { // 8 位偏移
        if (len < 2) return 1;
        int8_t offset = (int8_t)(data[1]);
        uint32_t target = dec->currentAddr + offset;
        dec->currentAddr += 2;  // 分支本身占 2 字节 (Thumb)
        _emitBranch(dec, from, target, ETM_BRANCH_NORMAL, false, true);
        *consumed = 2;
        break;
    }
    case 1: { // 11 位偏移
        if (len < 2) return 1;
        int16_t offset = (int16_t)((data[1] << 5) | ((header >> 3) & 0x1F));
        offset = (offset << 4) >> 4;  // 符号扩展
        uint32_t target = dec->currentAddr + offset;
        _emitBranch(dec, from, target, ETM_BRANCH_NORMAL, false, true);
        *consumed = 2;
        break;
    }
    case 2: { // 19 位偏移 (Thumb 标准)
        if (len < 3) return 2;
        int32_t offset = ((int32_t)data[2] << 12) | ((int32_t)data[1] << 4) | ((header >> 3) & 0x0F);
        offset = (offset << 12) >> 12;  // 符号扩展到 32 位
        uint32_t target = dec->currentAddr + offset;
        dec->currentAddr += 4;  // Thumb 分支指令
        _emitBranch(dec, from, target, ETM_BRANCH_NORMAL, false, true);
        *consumed = 3;
        break;
    }
    case 3: { // 条件分支
        if (len < 2) return 1;
        bool taken = (data[1] & 0x01) != 0;
        int8_t offset = (int8_t)(data[1] >> 1);
        uint32_t target = dec->currentAddr + offset;
        _emitBranch(dec, from, target, ETM_BRANCH_NORMAL, true, taken);
        *consumed = 2;
        break;
    }
    }

    return 0;
}

/**
 * @brief 解码 ETM v3.5 ISYNC 包
 * ISYNC 包含执行后的地址信息: 0x08 后跟 addrBytes 个地址字节
 */
static int _decodeETMv35ISYNC(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || !consumed) return -1;

    // 地址宽度由 ETM 配置决定 (addrBytes)，默认 4 字节 (32-bit ARM)。
    // 修复: 旧实现按 2 字节地址消费 4 字节数据，但调用方却以 len<3 判定，
    //       两者口径不一致，会在数据不足时把后续字节误当作地址。
    int addrBytes = dec->addrBytes ? dec->addrBytes : 4;
    size_t need = 1 + (size_t)addrBytes;

    if (len < need) {
        // 需要更多字节: 返回还差的字节数
        return (int)(need - len);
    }

    uint32_t addr = _extractValue(data + 1, addrBytes);

    // Thumb 模式下 bit[0] = 1
    if (dec->isThumb) {
        addr |= 0x01;
    }

    dec->currentAddr = addr;
    _emitSync(dec, addr);

    *consumed = need;
    return 0;
}

/**
 * @brief 解码 ETM v3.5 周期计数
 */
static int _decodeETMv35CycleCount(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len < 1 || !consumed) return -1;

    // CYCCNT 可以后跟可变数量的字节
    // 简化处理: 单字节周期计数
    dec->cycleDelta = data[0];
    dec->cycleCount += dec->cycleDelta;

    *consumed = 1;
    return 0;
}

/**
 * @brief 解码 ETM v3.5 CONTEXTID
 */
static int _decodeETMv35ContextID(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len < 4 || !consumed) return -1;

    // CONTEXTID: 4 字节上下文 ID
    uint32_t ctxId = _extractValue(data, 4);
    _emitContext(dec, ctxId, 4);

    *consumed = 4;
    return 0;
}

/**
 * @brief ETM v3.5 主解码函数
 */
static int _decodeETMv35(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len == 0 || !consumed) return -1;

    uint8_t byte = data[0];
    *consumed = 1;

    // A-SYNC 检测
    if (byte == 0x00) {
        if (dec->asyncCount >= 4) {
            // 准备进入同步
            dec->asyncCount++;
        } else {
            dec->asyncCount++;
        }
        return 0;
    }

    if (dec->asyncCount >= 5 && byte == 0x80) {
        // 同步成功
        dec->state = ETM_STATE_IDLE;
        dec->asyncCount = 0;
        _emitSync(dec, dec->currentAddr);
        return 0;
    }

    dec->asyncCount = 0;

    // 未同步时忽略
    if (dec->state == ETM_STATE_UNSYNCED) {
        return 0;
    }

    // 检查 ISYNC (0x08)
    if (byte == ETM_V35_ISYNC) {
        return _decodeETMv35ISYNC(dec, data, len, consumed);
    }

    // 检查 CYCCNT (0x04)
    if (byte == ETM_V35_CYCCNT) {
        return _decodeETMv35CycleCount(dec, data, len, consumed);
    }

    // 检查 CONTEXTID (0x6E)
    if (byte == ETM_V35_CONTEXTID) {
        if (len < 4) return 3;
        return _decodeETMv35ContextID(dec, data, len, consumed);
    }

    // 检查分支 (bit0=1)
    if (byte & 0x01) {
        return _decodeETMv35Branch(dec, data, len, consumed);
    }

    // 其他指令 - 可能是立即数更新地址
    // 0x02: 5 位立即数增量
    // 0x03: 11 位立即数增量
    if ((byte & 0x03) == 0x02) {
        uint8_t inc = (data[0] >> 2) & 0x1F;
        if (inc > 0) {
            dec->currentAddr += inc;
            _emitInstruction(dec, dec->currentAddr, 1);
        }
    }

    return 0;
}

// ============================================================================
// ETM v4 解码 (参考 orbuculum traceDecoder_etm4.c)
// ============================================================================

/**
 * @brief 解码 ETM v4 短地址 (IS0/IS1)
 */
static int _decodeETMv4ShortAddr(struct ORBMDK_ETM_Decoder* dec, uint8_t header, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len < 2 || !consumed) return -1;

    bool isIS1 = (header == ETM_V4_SHORT_ADDR_IS1);
    uint8_t fmt = (data[0] >> 6) & 0x03;
    uint8_t addrBits = ((data[0] >> 3) & 0x07) | ((header & 0x01) << 3);

    uint32_t addr = 0;
    int numBytes = 0;

    // 根据格式决定地址字节数
    // fmt=0: 1 字节, fmt=1: 2 字节, fmt=2: 3 字节
    switch (fmt) {
    case 0: numBytes = 1; break;
    case 1: numBytes = 2; break;
    case 2: numBytes = 3; break;
    default: return -1;  // 未知格式
    }

    if ((int)len < numBytes + 1) {
        return static_cast<int>(numBytes + 1 - len);  // 需要更多字节
    }

    addr = _extractValue(data + 1, numBytes);

    // IS1 高半字与前一个地址相关
    if (isIS1) {
        uint32_t high = (dec->currentAddr >> 16) & 0xFFFF;
        addr = (high << 16) | (addr << 1);
    } else {
        addr = addr << 1;  // Thumb 模式
    }

    dec->currentAddr = addr;
    _emitInstruction(dec, addr, 1);

    *consumed = 1 + numBytes;
    return 0;
}

/**
 * @brief 解码 ETM v4 长地址 (IS0/IS1)
 */
static int _decodeETMv4LongAddr(struct ORBMDK_ETM_Decoder* dec, uint8_t header, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len < 2 || !consumed) return -1;

    bool isIS1 = (header == ETM_V4_LONG_ADDR_IS1);
    bool is64bit = (header == ETM_V4_LONG64_ADDR_IS0 || header == ETM_V4_LONG64_ADDR_IS1);

    int numBytes = is64bit ? 6 : 4;
    if ((int)len < numBytes + 1) {
        return static_cast<int>(numBytes + 1 - len);
    }

    uint64_t addr = _extractValue(data + 1, numBytes);

    // Thumb 模式
    if (!is64bit) {
        addr |= 0x01;
    }

    dec->currentAddr = (uint32_t)addr;
    _emitInstruction(dec, (uint32_t)addr, 1);

    *consumed = 1 + numBytes;
    return 0;
}

/**
 * @brief 解码 ETM v4 原子指令
 */
static int _decodeETMv4Atom(struct ORBMDK_ETM_Decoder* dec, uint8_t header, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || !consumed) return -1;

    *consumed = 1;

    // Atom 序列由 header 高位编码
    // Format 1: 0b1111011x - 短序列
    // Format 2: 0b11011xxx - 可变长度
    // Format 3: 0b11111xxx - 长序列

    uint8_t n, e;
    int count = 0;

    if ((header & 0xF6) == ETM_V4_ATOM_FMT1_1) {
        // Format 1: 1-2 个原子
        n = (header >> 1) & 1;
        e = header & 1;
        count = n + 1;
    } else if ((header & 0xF8) == ETM_V4_ATOM_FMT2) {
        // Format 2: 1-8 个原子
        n = (header >> 3) & 0x07;
        e = 0;
        count = n + 1;
        if ((int)len < 1) return 1;
        e = data[0] & 0x01;
        *consumed = 2;
    } else if ((header & 0xF8) == ETM_V4_ATOM_FMT3) {
        // Format 3: 1-8 个原子 (inline)
        n = (header >> 3) & 0x07;
        e = 0;
        count = n + 1;
        if ((int)len < count) return static_cast<int>(count - len + 1);
        // 每个原子 1 位，0=commit, 1=discard
        for (int i = 0; i < count; i++) {
            bool exec = ((data[i] >> (7 - (i % 8))) & 1) == 0;
            if (exec) {
                dec->currentAddr += 2;  // Thumb 指令
                _emitInstruction(dec, dec->currentAddr, 1);
            }
        }
        *consumed = 1 + ((count + 7) / 8);
        return 0;
    } else {
        // 未知格式
        return 0;
    }

    // 执行原子指令
    for (int i = 0; i < count; i++) {
        if (i == count - 1 && e) {
            // 最后一个原子例外执行
            dec->currentAddr += 2;
            _emitInstruction(dec, dec->currentAddr, 1);
        } else if (i == 0 && !e) {
            // 第一个原子正常执行
            dec->currentAddr += 2;
            _emitInstruction(dec, dec->currentAddr, 1);
        }
    }

    return 0;
}

/**
 * @brief ETM v4 主解码函数
 */
static int _decodeETMv4(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len, size_t* consumed)
{
    if (!dec || !data || len == 0 || !consumed) return -1;

    uint8_t byte = data[0];
    *consumed = 1;

    // 未同步时忽略
    if (dec->state == ETM_STATE_UNSYNCED) {
        return 0;
    }

    // Trace On (0x04)
    if (byte == ETM_V4_TRACE_ON) {
        _emitSync(dec, dec->currentAddr);
        return 0;
    }

    // Context (0x80/0x81)
    if (byte == ETM_V4_CONTEXT || byte == ETM_V4_CONTEXT_P) {
        if (byte == ETM_V4_CONTEXT_P) {
            // 带 payload 的上下文
            if (len < 2) return 1;
            uint32_t ctxId = data[1];
            _emitContext(dec, ctxId, 1);
            *consumed = 2;
        }
        return 0;
    }

    // Function Return (0x05)
    if (byte == ETM_V4_FUNC_RETURN) {
        // 需要返回地址 (从栈)
        // 简化处理
        return 0;
    }

    // Exception (0x06)
    if (byte == ETM_V4_EXCEPTION) {
        if (len < 2) return 1;
        uint8_t exceptNum = data[1] & 0x1F;
        _emitException(dec, ETM_V35_EXCEPT_ENTRY, exceptNum);
        *consumed = 2;
        return 0;
    }

    // Exception Return (0x07)
    if (byte == ETM_V4_EXCEPTION_RET) {
        return 0;
    }

    // 短地址 (0x95/0x96)
    if (byte == ETM_V4_SHORT_ADDR_IS0 || byte == ETM_V4_SHORT_ADDR_IS1) {
        return _decodeETMv4ShortAddr(dec, byte, data, len, consumed);
    }

    // 长地址 (0x9A/0x9B/0x9D/0x9E)
    if (byte == ETM_V4_LONG_ADDR_IS0 || byte == ETM_V4_LONG_ADDR_IS1 ||
        byte == ETM_V4_LONG64_ADDR_IS0 || byte == ETM_V4_LONG64_ADDR_IS1) {
        return _decodeETMv4LongAddr(dec, byte, data, len, consumed);
    }

    // 原子指令
    if ((byte & 0xF8) == ETM_V4_ATOM_FMT2 || (byte & 0xF8) == ETM_V4_ATOM_FMT3 ||
        (byte & 0xF6) == ETM_V4_ATOM_FMT1_1) {
        return _decodeETMv4Atom(dec, byte, data, len, consumed);
    }

    // Resync (0x08)
    if (byte == ETM_V4_RESYNC) {
        _emitSync(dec, dec->currentAddr);
        return 0;
    }

    // v4 Prefix (0xE8)
    if (byte == ETM_V4_PREFIX) {
        if (len < 2) return 1;
        // 前缀处理
        *consumed = 2;
        return 0;
    }

    return 0;
}

// ============================================================================
// API 实现
// ============================================================================

struct ORBMDK_ETM_Decoder* ORBMDK_ETM_Create(enum ORBMDK_ETM_Version version)
{
    struct ORBMDK_ETM_Decoder* dec = new struct ORBMDK_ETM_Decoder;
    if (dec) {
        memset(dec, 0, sizeof(*dec));
        dec->version = version;
        dec->state = ETM_STATE_UNSYNCED;
        dec->isThumb = true;
        dec->asyncCount = 0;
        dec->addrBytes = 4;   // 默认 32-bit 地址
    }
    return dec;
}

void ORBMDK_ETM_Destroy(struct ORBMDK_ETM_Decoder* dec)
{
    if (dec) {
        delete dec;
    }
}

void ORBMDK_ETM_Init(struct ORBMDK_ETM_Decoder* dec)
{
    if (!dec) return;
    memset(&dec->stats, 0, sizeof(dec->stats));
    dec->state = ETM_STATE_UNSYNCED;
    dec->currentAddr = 0;
    dec->prevAddr = 0;
    dec->nextAddr = 0;
    dec->cycleCount = 0;
    dec->cycleDelta = 0;
    dec->addrInc = 2;
    dec->contextID = 0;
    dec->contextIDlen = 0;
    dec->exceptionPending = false;
    dec->inDelaySlot = false;
    dec->asyncCount = 0;
    dec->addrBytes = 4;   // 默认 32-bit 地址
}

void ORBMDK_ETM_SetBranchCallback(struct ORBMDK_ETM_Decoder* dec,
    void (*callback)(const struct ORBMDK_ETM_Branch* branch, void* userData),
    void* userData)
{
    if (dec) {
        dec->branchCallback = callback;
        dec->userData = userData;
    }
}

void ORBMDK_ETM_SetSyncCallback(struct ORBMDK_ETM_Decoder* dec,
    void (*callback)(uint32_t addr, void* userData),
    void* userData)
{
    if (dec) {
        dec->syncCallback = callback;
    }
    (void)userData;
}

void ORBMDK_ETM_SetExceptionCallback(struct ORBMDK_ETM_Decoder* dec,
    void (*callback)(uint8_t type, uint16_t number, void* userData),
    void* userData)
{
    if (dec) {
        dec->exceptionCallback = callback;
    }
    (void)userData;
}

void ORBMDK_ETM_ForceSync(struct ORBMDK_ETM_Decoder* dec, bool synced)
{
    if (!dec) return;
    if (synced) {
        dec->state = ETM_STATE_IDLE;
        dec->stats.syncCount++;
    } else {
        dec->state = ETM_STATE_UNSYNCED;
        dec->stats.lostSyncCount++;
    }
}

bool ORBMDK_ETM_IsSynced(struct ORBMDK_ETM_Decoder* dec)
{
    return dec && dec->state != ETM_STATE_UNSYNCED;
}

uint32_t ORBMDK_ETM_GetCurrentAddress(struct ORBMDK_ETM_Decoder* dec)
{
    return dec ? dec->currentAddr : 0;
}

uint64_t ORBMDK_ETM_GetCycleCount(struct ORBMDK_ETM_Decoder* dec)
{
    return dec ? dec->cycleCount : 0;
}

struct ORBMDK_ETM_Stats* ORBMDK_ETM_GetStats(struct ORBMDK_ETM_Decoder* dec)
{
    return dec ? &dec->stats : nullptr;
}

int ORBMDK_ETM_Pump(struct ORBMDK_ETM_Decoder* dec, const uint8_t* data, size_t len)
{
    if (!dec || !data || len == 0) return -1;

    size_t offset = 0;
    int result;

    while (offset < len) {
        size_t consumed = 0;

        // 根据版本解码
        if (dec->version == ETM_PV_ETM35) {
            result = _decodeETMv35(dec, data + offset, len - offset, &consumed);
        } else {
            result = _decodeETMv4(dec, data + offset, len - offset, &consumed);
        }

        if (result < 0) {
            return -1;  // 错误
        } else if (result > 0) {
            return result;  // 需要更多字节
        }

        offset += consumed;
        dec->cycleCount++;
    }

    return 0;
}

// ============================================================================
// 兼容性 API (保留原有接口)
// ============================================================================

void ORBMDK_ETM_SetAltAddrEncode(struct ORBMDK_ETM_Decoder* dec, bool altEncode)
{
    if (dec) {
        dec->usingAltAddrEncode = altEncode;
    }
}

bool ORBMDK_ETM_GetAltAddrEncode(struct ORBMDK_ETM_Decoder* dec)
{
    return dec ? dec->usingAltAddrEncode : false;
}
