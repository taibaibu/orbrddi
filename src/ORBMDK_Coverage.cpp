/**
 * @file ORBMDK_Coverage.cpp
 * @brief 代码覆盖率分析实现
 *
 * 基于 ETM trace 数据实现代码覆盖率统计
 * 参考: orbuculum orbprofile/ext_fileformats 实现
 */

#include "pch.h"
#include "ORBMDK_Coverage.h"
#include "ORBMDK_ETM_Decoder.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// ============================================================================
// 内部数据结构
// ============================================================================

/**
 * @brief 指令哈希表条目
 */
struct ExecEntry {
    uint32_t addr;               // 内存地址
    uint32_t codes;             // 指令编码
    uint64_t count;             // 执行计数
    uint64_t scount;             // 源码行级计数
    bool isJump;                 // 是否为跳转指令
    bool is4Byte;                // 是否为 4 字节指令
    bool isSubCall;              // 是否为子程序调用
    bool isReturn;              // 是否为返回指令
    uint32_t jumpdest;           // 跳转目标地址
    uint32_t fileindex;          // 文件索引
    uint32_t functionindex;      // 函数索引
    uint32_t line;               // 源代码行号
    const char* assyText;       // 汇编文本

    ExecEntry* next;             // 链表下一项
};

/**
 * @brief 函数调用条目
 */
struct CallEntry {
    struct ORBMDK_Coverage_CallSig sig;  // 调用签名
    ExecEntry* caller;            // 调用方
    ExecEntry* callee;            // 被调用方
    uint64_t myCost;              // 此调用的包含成本
    uint64_t count;               // 执行次数
    uint64_t inTicks;             // 进入时的时间戳

    CallEntry* next;              // 链表下一项
};

/**
 * @brief 函数信息
 */
struct FuncInfo {
    uint32_t startAddr;
    uint32_t endAddr;
    uint32_t entryAddr;
    const char* name;
    const char* file;
    uint64_t totalCount;
    uint64_t totalCycles;
    uint32_t callCount;

    FuncInfo* next;
};

/**
 * @brief 基本块
 */
struct BlockInfo {
    uint32_t startAddr;
    uint32_t endAddr;
    uint64_t executeCount;
    bool isCovered;

    BlockInfo* next;
};

/**
 * @brief 调用栈条目
 */
struct CallStackEntry {
    uint32_t retAddr;
    uint32_t funcAddr;
    uint64_t entryTime;
};

/**
 * @brief 覆盖率分析器内部结构
 */
struct ORBMDK_Coverage_Analyzer {
    // 哈希表
    ExecEntry* execHash[1024];
    uint32_t execCount;

    // 函数调用列表
    CallEntry* callList;
    uint32_t callCount;

    // 函数信息
    FuncInfo* funcList;
    uint32_t funcCount;

    // 基本块
    BlockInfo* blockList;
    uint32_t blockCount;

    // 调用栈
    CallStackEntry callStack[32];
    uint32_t callStackLen;

    // 符号回调
    const char* (*symbolCallback)(uint32_t addr, void* userData);
    void (*lineCallback)(uint32_t addr, const char** file, uint32_t* line, void* userData);
    void* symbolUserData;
    void* lineUserData;

    // ELF 文件路径
    char elfPath[256];

    // 统计
    uint64_t totalInstructions;
    uint64_t totalBranches;
    uint64_t takenBranches;
    uint64_t totalCycles;
    uint32_t uniqueAddresses;

    // 最大条目限制
    uint32_t maxEntries;
};

// ============================================================================
// 哈希函数
// ============================================================================

static uint32_t _hashAddr(uint32_t addr)
{
    // 简单的哈希函数
    addr = ((addr >> 16) ^ addr) * 0x45d9f3b;
    addr = ((addr >> 16) ^ addr) * 0x45d9f3b;
    addr = (addr >> 16) ^ addr;
    return addr & 0x3FF;  // 1024 buckets
}

// ============================================================================
// 查找/创建函数
// ============================================================================

static ExecEntry* _findOrCreateExec(struct ORBMDK_Coverage_Analyzer* a, uint32_t addr)
{
    uint32_t bucket = _hashAddr(addr);

    // 查找现有条目
    for (ExecEntry* e = a->execHash[bucket]; e; e = e->next) {
        if (e->addr == addr) {
            return e;
        }
    }

    // 创建新条目
    if (a->execCount >= a->maxEntries) {
        return nullptr;  // 达到上限
    }

    ExecEntry* e = new ExecEntry();
    if (!e) return nullptr;

    memset(e, 0, sizeof(*e));
    e->addr = addr;

    // 插入哈希表
    e->next = a->execHash[bucket];
    a->execHash[bucket] = e;
    a->execCount++;

    return e;
}

static CallEntry* _createCallEntry(struct ORBMDK_Coverage_Analyzer* a,
    uint32_t src, uint32_t dst)
{
    CallEntry* c = new CallEntry();
    if (!c) return nullptr;

    memset(c, 0, sizeof(*c));
    c->sig.src = src;
    c->sig.dst = dst;
    c->inTicks = a->totalCycles;

    // 添加到列表
    c->next = a->callList;
    a->callList = c;
    a->callCount++;

    return c;
}

// ============================================================================
// ARM Thumb 指令识别
// ============================================================================

/**
 * @brief 判断是否为 Thumb 分支指令
 */
static bool _isThumbBranch(uint16_t instr)
{
    // B (cond) 1110 ----- cond
    if ((instr & 0xF000) == 0xD000) return true;
    // B (uncond) 11100 offset
    if ((instr & 0xF800) == 0xE000) return true;
    // BL/BLX 11110S(1) H(1) offset
    if ((instr & 0xF800) == 0xF000) return true;
    // CBZ/CBNZ
    if ((instr & 0xF500) == 0xB100) return true;
    return false;
}

/**
 * @brief 判断是否为 Thumb 调用指令
 */
static bool _isThumbCall(uint16_t instr)
{
    // BL 11110 S(1) H(1) offset
    if ((instr & 0xF800) == 0xF000) {
        // 检查 H 位
        return true;
    }
    // BLX (encoded as BL with H=0 and later instruction)
    return false;
}

/**
 * @brief 判断是否为 Thumb 返回指令
 */
static bool _isThumbReturn(uint16_t instr)
{
    // BX LR, POP {..., PC}, LDMFD sp!, {..., PC}
    if ((instr & 0xFF87) == 0x4780) return true;  // BX LR / BLX LR
    if ((instr & 0xF700) == 0xBD00) return true;  // POP {..., PC}
    if ((instr & 0xF708) == 0xE8D0) return true;  // LDMFD sp!, {..., PC}
    return false;
}

/**
 * @brief 判断是否为 Thumb 跳转指令
 */
static bool _isThumbJump(uint16_t instr)
{
    return _isThumbBranch(instr);
}

// ============================================================================
// 公共 API 实现
// ============================================================================

struct ORBMDK_Coverage_Analyzer* ORBMDK_Coverage_Create(uint32_t maxEntries)
{
    struct ORBMDK_Coverage_Analyzer* a = new struct ORBMDK_Coverage_Analyzer;
    if (!a) return nullptr;

    memset(a, 0, sizeof(*a));
    a->maxEntries = (maxEntries > 0) ? maxEntries : 65536;

    return a;
}

void ORBMDK_Coverage_Destroy(struct ORBMDK_Coverage_Analyzer* a)
{
    if (!a) return;

    // 释放哈希表
    for (int i = 0; i < 1024; i++) {
        ExecEntry* e = a->execHash[i];
        while (e) {
            ExecEntry* next = e->next;
            delete e;
            e = next;
        }
    }

    // 释放调用列表
    CallEntry* c = a->callList;
    while (c) {
        CallEntry* next = c->next;
        delete c;
        c = next;
    }

    // 释放函数列表
    FuncInfo* f = a->funcList;
    while (f) {
        FuncInfo* next = f->next;
        delete f;
        f = next;
    }

    // 释放基本块列表
    BlockInfo* b = a->blockList;
    while (b) {
        BlockInfo* next = b->next;
        delete b;
        b = next;
    }

    delete a;
}

int ORBMDK_Coverage_SetELFFile(struct ORBMDK_Coverage_Analyzer* a, const char* elfPath)
{
    if (!a || !elfPath) return -1;
    strncpy(a->elfPath, elfPath, sizeof(a->elfPath) - 1);
    a->elfPath[sizeof(a->elfPath) - 1] = '\0';
    return 0;
}

void ORBMDK_Coverage_SetSymbolCallback(struct ORBMDK_Coverage_Analyzer* a,
    const char* (*callback)(uint32_t addr, void* userData),
    void* userData)
{
    if (!a) return;
    a->symbolCallback = callback;
    a->symbolUserData = userData;
}

void ORBMDK_Coverage_SetLineCallback(struct ORBMDK_Coverage_Analyzer* a,
    void (*callback)(uint32_t addr, const char** file, uint32_t* line, void* userData),
    void* userData)
{
    if (!a) return;
    a->lineCallback = callback;
    a->lineUserData = userData;
}

void ORBMDK_Coverage_RecordInstruction(struct ORBMDK_Coverage_Analyzer* a,
    uint32_t addr, bool isThumb)
{
    if (!a) return;

    ExecEntry* e = _findOrCreateExec(a, addr);
    if (!e) return;

    e->count++;
    a->totalInstructions++;

    // 更新唯一地址计数
    static uint32_t lastAddr = 0xFFFFFFFF;
    if (addr != lastAddr) {
        a->uniqueAddresses++;
        lastAddr = addr;
    }

    // 符号解析
    if (a->symbolCallback) {
        const char* name = a->symbolCallback(addr, a->symbolUserData);
        if (name) {
            // 可以存储符号名
        }
    }

    // 行号解析
    if (a->lineCallback) {
        const char* file = nullptr;
        uint32_t line = 0;
        a->lineCallback(addr, &file, &line, a->lineUserData);
        if (line > 0) {
            e->line = line;
            e->scount++;
        }
    }

    // 检查指令类型
    uint16_t instr = addr & 0xFFFFFFFE;  // Thumb 模式清除 LSB
    // 注意: 实际应该从内存读取指令，这里简化处理
    // e->isSubCall = _isThumbCall(instr);
    // e->isReturn = _isThumbReturn(instr);
    // e->isJump = _isThumbJump(instr);
}

void ORBMDK_Coverage_RecordBranch(struct ORBMDK_Coverage_Analyzer* a,
    uint32_t from, uint32_t to, bool taken, bool isConditional)
{
    if (!a) return;

    a->totalBranches++;
    if (taken) {
        a->takenBranches++;
    }

    // 记录分支源地址的执行
    ExecEntry* e = _findOrCreateExec(a, from);
    if (e) {
        e->count++;
        e->isJump = true;
        e->jumpdest = to;
    }

    // 记录分支目标
    ExecEntry* te = _findOrCreateExec(a, to);
    if (te) {
        te->count++;
    }

    a->totalInstructions += 2;
}

void ORBMDK_Coverage_RecordCall(struct ORBMDK_Coverage_Analyzer* a,
    uint32_t callAddr, uint32_t targetAddr)
{
    if (!a) return;

    // 创建调用记录
    CallEntry* c = _createCallEntry(a, callAddr, targetAddr);
    if (!c) return;

    // 记录指令
    ExecEntry* caller = _findOrCreateExec(a, callAddr);
    if (caller) {
        caller->isSubCall = true;
        caller->count++;
    }

    ExecEntry* callee = _findOrCreateExec(a, targetAddr);
    if (callee) {
        callee->count++;
    }

    // 压入调用栈
    if (a->callStackLen < 32) {
        a->callStack[a->callStackLen].retAddr = callAddr + 4;  // Thumb: 返回地址
        a->callStack[a->callStackLen].funcAddr = targetAddr;
        a->callStack[a->callStackLen].entryTime = a->totalCycles;
        a->callStackLen++;
    }
}

void ORBMDK_Coverage_RecordReturn(struct ORBMDK_Coverage_Analyzer* a,
    uint32_t from, uint32_t to)
{
    if (!a) return;

    // 记录指令
    ExecEntry* e = _findOrCreateExec(a, from);
    if (e) {
        e->isReturn = true;
        e->count++;
    }

    ExecEntry* te = _findOrCreateExec(a, to);
    if (te) {
        te->count++;
    }

    // 更新调用栈
    if (a->callStackLen > 0) {
        a->callStackLen--;
    }
}

void ORBMDK_Coverage_RecordCycles(struct ORBMDK_Coverage_Analyzer* a, uint64_t cycles)
{
    if (!a) return;
    a->totalCycles += cycles;
}

void ORBMDK_Coverage_AddBranchRecord(struct ORBMDK_Coverage_Analyzer* a,
    const struct ORBMDK_ETM_Branch* branch)
{
    if (!a || !branch) return;

    ORBMDK_Coverage_RecordBranch(a, branch->fromAddr, branch->toAddr,
        branch->isTaken, branch->isConditional);
}

struct ORBMDK_Coverage_Summary ORBMDK_Coverage_GetSummary(struct ORBMDK_Coverage_Analyzer* a)
{
    struct ORBMDK_Coverage_Summary s = {};

    if (!a) return s;

    s.totalInstructions = a->execCount;  // 唯一指令数
    s.executedInstructions = a->uniqueAddresses;
    s.totalBranches = a->totalBranches;
    s.takenBranches = a->takenBranches;
    s.totalFunctions = a->funcCount;
    s.totalCycles = a->totalCycles;
    s.uniqueCodeRegions = a->execCount;

    // 计算覆盖率
    if (a->execCount > 0) {
        s.instructionCoverage = (double)a->uniqueAddresses / (double)a->execCount;
    }
    if (a->totalBranches > 0) {
        s.branchCoverage = (double)a->takenBranches / (double)a->totalBranches;
    }

    return s;
}

uint32_t ORBMDK_Coverage_GetUniqueAddresses(struct ORBMDK_Coverage_Analyzer* a)
{
    return a ? a->uniqueAddresses : 0;
}

uint32_t ORBMDK_Coverage_GetInstructionCount(struct ORBMDK_Coverage_Analyzer* a)
{
    return a ? a->execCount : 0;
}

uint32_t ORBMDK_Coverage_GetCallCount(struct ORBMDK_Coverage_Analyzer* a)
{
    return a ? a->callCount : 0;
}

uint64_t ORBMDK_Coverage_GetTotalCycles(struct ORBMDK_Coverage_Analyzer* a)
{
    return a ? a->totalCycles : 0;
}

void ORBMDK_Coverage_IterateInstructions(struct ORBMDK_Coverage_Analyzer* a,
    void (*callback)(const struct ORBMDK_Coverage_ExecEntry* entry, void* userData),
    void* userData)
{
    if (!a || !callback) return;

    for (int i = 0; i < 1024; i++) {
        for (ExecEntry* e = a->execHash[i]; e; e = e->next) {
            callback((const struct ORBMDK_Coverage_ExecEntry*)e, userData);
        }
    }
}

void ORBMDK_Coverage_IterateCalls(struct ORBMDK_Coverage_Analyzer* a,
    void (*callback)(const struct ORBMDK_Coverage_CallEntry* entry, void* userData),
    void* userData)
{
    if (!a || !callback) return;

    for (CallEntry* c = a->callList; c; c = c->next) {
        callback((const struct ORBMDK_Coverage_CallEntry*)c, userData);
    }
}

void ORBMDK_Coverage_IterateFunctions(struct ORBMDK_Coverage_Analyzer* a,
    void (*callback)(const struct ORBMDK_Coverage_Function* func, void* userData),
    void* userData)
{
    if (!a || !callback) return;

    for (FuncInfo* f = a->funcList; f; f = f->next) {
        callback((const struct ORBMDK_Coverage_Function*)f, userData);
    }
}

void ORBMDK_Coverage_Reset(struct ORBMDK_Coverage_Analyzer* a)
{
    if (!a) return;

    // 清除哈希表
    for (int i = 0; i < 1024; i++) {
        ExecEntry* e = a->execHash[i];
        while (e) {
            ExecEntry* next = e->next;
            delete e;
            e = next;
        }
        a->execHash[i] = nullptr;
    }

    // 清除调用列表
    CallEntry* c = a->callList;
    while (c) {
        CallEntry* next = c->next;
        delete c;
        c = next;
    }
    a->callList = nullptr;
    a->callCount = 0;

    // 清除函数列表
    FuncInfo* f = a->funcList;
    while (f) {
        FuncInfo* next = f->next;
        delete f;
        f = next;
    }
    a->funcList = nullptr;
    a->funcCount = 0;

    // 清除基本块
    BlockInfo* b = a->blockList;
    while (b) {
        BlockInfo* next = b->next;
        delete b;
        b = next;
    }
    a->blockList = nullptr;
    a->blockCount = 0;

    // 重置统计
    a->execCount = 0;
    a->callStackLen = 0;
    a->totalInstructions = 0;
    a->totalBranches = 0;
    a->takenBranches = 0;
    a->totalCycles = 0;
    a->uniqueAddresses = 0;
}

// ============================================================================
// 报告输出
// ============================================================================

/**
 * @brief KCacheGrind 格式输出
 *
 * 格式说明:
 * event="Instructions" 0
 * event="Cycles" 0
 * fi=文件索引
 * fn=函数索引 函数名
 * 0x地址 地址计数 函数计数 行号
 * cfni=调用方函数索引
 * cf=调用函数索引 调用计数
 */
int ORBMDK_Coverage_OutputKCacheGrind(struct ORBMDK_Coverage_Analyzer* a,
    const char* outputPath, bool includeVisits)
{
    if (!a || !outputPath) return -1;

    FILE* f = fopen(outputPath, "w");
    if (!f) return -1;

    // 输出头部
    fprintf(f, "# KCacheGrind profile data\n");
    fprintf(f, "version: 1\n");
    fprintf(f, "creator: ORBMDK_Coverage/1.0\n");
    fprintf(f, "pid: 0\n");
    fprintf(f, "cmd: %s\n", a->elfPath[0] ? a->elfPath : "unknown");
    fprintf(f, "\n");

    // 输出事件类型
    fprintf(f, "events: Instructions %llu\n", (unsigned long long)a->totalInstructions);
    fprintf(f, "events: Cycles %llu\n", (unsigned long long)a->totalCycles);
    fprintf(f, "events: Branches %llu\n", (unsigned long long)a->totalBranches);
    fprintf(f, "\n");

    // 输出文件列表
    uint32_t fileIdx = 0;
    fprintf(f, "fl=%u\n", fileIdx);
    fprintf(f, "fn=%u %s\n", fileIdx, "???");

    // 输出指令覆盖数据
    for (int i = 0; i < 1024; i++) {
        for (ExecEntry* e = a->execHash[i]; e; e = e->next) {
            fprintf(f, "0x%08X %llu %llu %u\n",
                e->addr,
                (unsigned long long)e->count,
                (unsigned long long)e->scount,
                e->line);
        }
    }
    fprintf(f, "\n");

    // 输出调用关系
    uint32_t callIdx = 0;
    for (CallEntry* c = a->callList; c; c = c->next) {
        fprintf(f, "cob=%u\n", callIdx);
        fprintf(f, "cfi=%u %u\n", fileIdx, callIdx);
        fprintf(f, "cfn=%u 0x%08X\n", fileIdx, c->sig.src);
        fprintf(f, "calls=%llu 0x%08X\n", (unsigned long long)c->count, c->sig.dst);
        fprintf(f, "%u %llu\n", c->sig.src, (unsigned long long)c->myCost);
        callIdx++;
    }

    // 输出摘要
    fprintf(f, "\n# Summary\n");
    fprintf(f, "# Total Instructions: %llu\n", (unsigned long long)a->totalInstructions);
    fprintf(f, "# Unique Addresses: %u\n", a->uniqueAddresses);
    fprintf(f, "# Total Branches: %llu\n", (unsigned long long)a->totalBranches);
    fprintf(f, "# Taken Branches: %llu\n", (unsigned long long)a->takenBranches);
    fprintf(f, "# Total Cycles: %llu\n", (unsigned long long)a->totalCycles);

    fclose(f);
    return 0;
}

/**
 * @brief 文本格式覆盖率报告
 */
int ORBMDK_Coverage_OutputText(struct ORBMDK_Coverage_Analyzer* a,
    const char* outputPath)
{
    if (!a || !outputPath) return -1;

    FILE* f = fopen(outputPath, "w");
    if (!f) return -1;

    struct ORBMDK_Coverage_Summary s = ORBMDK_Coverage_GetSummary(a);

    fprintf(f, "========================================\n");
    fprintf(f, "       ORBMDK Coverage Report\n");
    fprintf(f, "========================================\n\n");

    fprintf(f, "Coverage Summary:\n");
    fprintf(f, "----------------------------------------\n");
    fprintf(f, "  Unique Addresses:      %u\n", a->uniqueAddresses);
    fprintf(f, "  Total Instructions:    %llu\n", (unsigned long long)a->totalInstructions);
    fprintf(f, "  Total Branches:       %llu\n", (unsigned long long)a->totalBranches);
    fprintf(f, "  Taken Branches:       %llu (%.2f%%)\n",
        (unsigned long long)a->takenBranches,
        a->totalBranches > 0 ? 100.0 * a->takenBranches / a->totalBranches : 0.0);
    fprintf(f, "  Total Cycles:         %llu\n", (unsigned long long)a->totalCycles);
    fprintf(f, "  Function Calls:       %u\n", a->callCount);
    fprintf(f, "\n");

    // 输出覆盖率百分比
    fprintf(f, "Coverage Rates:\n");
    fprintf(f, "----------------------------------------\n");
    fprintf(f, "  Instruction Coverage: %.2f%%\n", 100.0 * s.instructionCoverage);
    fprintf(f, "  Branch Coverage:      %.2f%%\n", 100.0 * s.branchCoverage);
    fprintf(f, "\n");

    // 输出执行最多的地址
    fprintf(f, "Top Executed Addresses:\n");
    fprintf(f, "----------------------------------------\n");

    // 收集前 20 个
    struct ExecEntry* top[20] = {nullptr};
    int topCount = 0;
    uint64_t minCount = 0;

    for (int i = 0; i < 1024; i++) {
        for (ExecEntry* e = a->execHash[i]; e; e = e->next) {
            if (topCount < 20) {
                top[topCount++] = e;
                if (e->count < minCount || topCount == 1) minCount = e->count;
            } else if (e->count > minCount) {
                // 替换最小的
                for (int j = 0; j < 20; j++) {
                    if (top[j] && top[j]->count == minCount) {
                        top[j] = e;
                        break;
                    }
                }
                minCount = e->count;
                for (int j = 0; j < 20; j++) {
                    if (top[j] && top[j]->count < minCount) {
                        minCount = top[j]->count;
                    }
                }
            }
        }
    }

    // 排序
    for (int i = 0; i < topCount - 1; i++) {
        for (int j = i + 1; j < topCount; j++) {
            if (top[j] && top[i] && top[j]->count > top[i]->count) {
                ExecEntry* tmp = top[i];
                top[i] = top[j];
                top[j] = tmp;
            }
        }
    }

    for (int i = 0; i < topCount && top[i]; i++) {
        const char* funcName = "???";
        if (a->symbolCallback) {
            const char* name = a->symbolCallback(top[i]->addr, a->symbolUserData);
            if (name) funcName = name;
        }
        fprintf(f, "  0x%08X  %10llu  %s\n",
            top[i]->addr,
            (unsigned long long)top[i]->count,
            funcName);
    }

    fprintf(f, "\n========================================\n");

    fclose(f);
    return 0;
}

/**
 * @brief GCOV 格式输出 (简化版)
 *
 * GCOV 使用 .gcda 和 .gcno 文件
 * 这里输出简化的文本格式，可用于后续转换
 */
int ORBMDK_Coverage_OutputGCOV(struct ORBMDK_Coverage_Analyzer* a,
    const char* outputPath, const char* sourceFiles)
{
    if (!a || !outputPath) return -1;

    // 构建文件名
    char gcdaPath[512];
    snprintf(gcdaPath, sizeof(gcdaPath), "%s.gcda", outputPath);

    FILE* f = fopen(gcdaPath, "wb");
    if (!f) return -1;

    // GCOV 文件格式是 GCC 特定的
    // 这里输出简化版本供参考

    // 输出 Magic Number (简化)
    uint32_t magic = 0x67636461;  // "gcda"
    fwrite(&magic, 4, 1, f);

    // 输出版本
    uint32_t version = 0x34313032;  // "0143" (版本信息)
    fwrite(&version, 4, 1, f);

    // 输出功能标志
    uint32_t stamp = (uint32_t)(a->totalCycles & 0xFFFFFFFF);
    fwrite(&stamp, 4, 1, f);

    // 输出函数信息块
    uint32_t functions = a->funcCount > 0 ? a->funcCount : 1;
    fwrite(&functions, 4, 1, f);

    // 简化: 输出一个默认函数
    if (a->funcCount == 0) {
        fprintf(f, "function %u: %llu\n", 0, (unsigned long long)a->totalInstructions);
    }

    // 输出基本块计数
    uint32_t blocks = a->execCount > 0 ? a->execCount : 1;
    fwrite(&blocks, 4, 1, f);

    // 输出每个地址的计数
    for (int i = 0; i < 1024 && a->execCount > 0; i++) {
        for (ExecEntry* e = a->execHash[i]; e; e = e->next) {
            uint64_t count = e->count;
            fwrite(&count, 8, 1, f);
        }
    }

    fclose(f);
    return 0;
}

/**
 * @brief JSON 格式输出
 */
size_t ORBMDK_Coverage_OutputJSON(struct ORBMDK_Coverage_Analyzer* a,
    char* buffer, size_t bufferSize)
{
    if (!a || !buffer || bufferSize == 0) return 0;

    struct ORBMDK_Coverage_Summary s = ORBMDK_Coverage_GetSummary(a);

    int len = snprintf(buffer, bufferSize,
        "{\n"
        "  \"summary\": {\n"
        "    \"uniqueAddresses\": %u,\n"
        "    \"totalInstructions\": %llu,\n"
        "    \"totalBranches\": %llu,\n"
        "    \"takenBranches\": %llu,\n"
        "    \"branchCoverage\": %.4f,\n"
        "    \"totalCycles\": %llu,\n"
        "    \"functionCalls\": %u\n"
        "  },\n"
        "  \"instructions\": [",
        a->uniqueAddresses,
        (unsigned long long)a->totalInstructions,
        (unsigned long long)a->totalBranches,
        (unsigned long long)a->takenBranches,
        s.branchCoverage,
        (unsigned long long)a->totalCycles,
        a->callCount);

    // 添加指令数据
    size_t offset = len;
    bool first = true;
    for (int i = 0; i < 1024; i++) {
        for (ExecEntry* e = a->execHash[i]; e; e = e->next) {
            if (!first) {
                if (offset < bufferSize) buffer[offset++] = ',';
            }
            first = false;

            int written = snprintf(buffer + offset, bufferSize - offset,
                "%s\n    {\"addr\":\"0x%08X\",\"count\":%llu,\"line\":%u}",
                first ? "" : "",
                e->addr,
                (unsigned long long)e->count,
                e->line);
            if (written > 0) offset += written;
            if (offset >= bufferSize) break;
        }
        if (offset >= bufferSize) break;
    }

    // 添加调用数据
    len = snprintf(buffer + offset, bufferSize - offset,
        "\n  ],\n"
        "  \"calls\": [");

    if (len > 0) offset += len;

    first = true;
    for (CallEntry* c = a->callList; c; c = c->next) {
        if (!first) {
            if (offset < bufferSize) buffer[offset++] = ',';
        }
        first = false;

        int written = snprintf(buffer + offset, bufferSize - offset,
            "%s\n    {\"from\":\"0x%08X\",\"to\":\"0x%08X\",\"count\":%llu}",
            first ? "" : "",
            c->sig.src,
            c->sig.dst,
            (unsigned long long)c->count);
        if (written > 0) offset += written;
        if (offset >= bufferSize) break;
    }

    // 结束 JSON
    len = snprintf(buffer + offset, bufferSize - offset,
        "\n  ]\n"
        "}\n");

    if (len > 0) offset += len;

    return offset;
}
