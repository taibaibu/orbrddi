/**
 * @file ORBMDK_Coverage.h
 * @brief 代码覆盖率分析接口
 *
 * 基于 ETM trace 数据实现代码覆盖率统计
 * 参考: orbuculum orbprofile 实现
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 覆盖率统计类型
// ============================================================================

/**
 * @brief 指令执行记录
 */
struct ORBMDK_Coverage_ExecEntry {
    uint32_t addr;              // 内存地址
    uint32_t codes;             // 指令编码
    uint64_t count;             // 执行计数
    uint64_t scount;            // 源码行级计数
    bool isJump;                // 是否为跳转指令
    bool is4Byte;               // 是否为 4 字节指令
    bool isSubCall;             // 是否为子程序调用 (BL/BLX)
    bool isReturn;              // 是否为返回指令
    uint32_t jumpdest;          // 跳转目标地址
    uint32_t fileindex;         // 文件索引
    uint32_t functionindex;     // 函数索引
    uint32_t line;              // 源代码行号
    const char* assyText;       // 汇编文本
};

/**
 * @brief 函数调用签名
 */
struct ORBMDK_Coverage_CallSig {
    uint32_t src;               // 调用来源地址
    uint32_t dst;               // 调用目标地址
};

/**
 * @brief 函数调用记录
 */
struct ORBMDK_Coverage_CallEntry {
    struct ORBMDK_Coverage_CallSig sig;  // 调用签名
    struct ORBMDK_Coverage_ExecEntry* caller;   // 调用方
    struct ORBMDK_Coverage_ExecEntry* callee;   // 被调用方
    uint64_t myCost;            // 此调用的包含成本
    uint64_t count;             // 执行次数
    uint64_t inTicks;           // 进入时的时间戳
};

/**
 * @brief 函数信息
 */
struct ORBMDK_Coverage_Function {
    uint32_t startAddr;         // 函数起始地址
    uint32_t endAddr;           // 函数结束地址
    uint32_t entryAddr;         // 函数入口地址
    const char* name;           // 函数名
    const char* file;           // 源文件
    uint64_t totalCount;        // 总执行计数
    uint64_t totalCycles;      // 总周期数
    uint32_t callCount;         // 调用次数
};

/**
 * @brief 基本块信息
 */
struct ORBMDK_Coverage_Block {
    uint32_t startAddr;         // 块起始地址
    uint32_t endAddr;           // 块结束地址
    uint64_t executeCount;     // 执行次数
    bool isCovered;             // 是否被执行过
};

/**
 * @brief 文件覆盖率统计
 */
struct ORBMDK_Coverage_FileStats {
    const char* filename;       // 文件名
    uint32_t totalLines;        // 总行数
    uint32_t coveredLines;     // 覆盖行数
    uint32_t functionCount;    // 函数数量
    uint32_t coveredFunctions; // 覆盖函数数量
    double lineCoverage;        // 行覆盖率 (0.0-1.0)
    double functionCoverage;    // 函数覆盖率 (0.0-1.0)
};

/**
 * @brief 覆盖率统计摘要
 */
struct ORBMDK_Coverage_Summary {
    uint64_t totalInstructions;    // 总指令数
    uint64_t executedInstructions; // 执行指令数
    uint64_t totalBranches;        // 总分支数
    uint64_t takenBranches;        // 执行分支数
    uint64_t totalFunctions;       // 总函数数
    uint64_t calledFunctions;      // 调用函数数
    uint64_t totalCycles;         // 总周期数
    uint32_t uniqueCodeRegions;    // 唯一代码区域数
    double instructionCoverage;    // 指令覆盖率
    double branchCoverage;         // 分支覆盖率
    double functionCoverage;       // 函数覆盖率
};

// ============================================================================
// 覆盖率分析器
// ============================================================================

/**
 * @brief 覆盖率分析器
 */
struct ORBMDK_Coverage_Analyzer;

// ============================================================================
// 创建/销毁
// ============================================================================

/**
 * @brief 创建覆盖率分析器
 * @param maxEntries 最大条目数
 * @return 分析器句柄，失败返回 NULL
 */
struct ORBMDK_Coverage_Analyzer* ORBMDK_Coverage_Create(uint32_t maxEntries);

/**
 * @brief 销毁覆盖率分析器
 * @param analyzer 分析器句柄
 */
void ORBMDK_Coverage_Destroy(struct ORBMDK_Coverage_Analyzer* analyzer);

// ============================================================================
// 配置
// ============================================================================

/**
 * @brief 设置 ELF 文件路径（用于符号解析）
 * @param analyzer 分析器句柄
 * @param elfPath ELF 文件路径
 * @return 0 成功，非 0 失败
 */
int ORBMDK_Coverage_SetELFFile(struct ORBMDK_Coverage_Analyzer* analyzer, const char* elfPath);

/**
 * @brief 设置符号查找回调
 * @param analyzer 分析器句柄
 * @param callback 回调函数 (地址 -> 函数名)
 * @param userData 用户数据
 */
void ORBMDK_Coverage_SetSymbolCallback(struct ORBMDK_Coverage_Analyzer* analyzer,
    const char* (*callback)(uint32_t addr, void* userData),
    void* userData);

/**
 * @brief 设置行号查找回调
 * @param analyzer 分析器句柄
 * @param callback 回调函数 (地址 -> 文件:行号)
 * @param userData 用户数据
 */
void ORBMDK_Coverage_SetLineCallback(struct ORBMDK_Coverage_Analyzer* analyzer,
    void (*callback)(uint32_t addr, const char** file, uint32_t* line, void* userData),
    void* userData);

// ============================================================================
// 跟踪数据输入
// ============================================================================

/**
 * @brief 记录指令执行
 * @param analyzer 分析器句柄
 * @param addr 指令地址
 * @param isThumb 是否为 Thumb 指令
 */
void ORBMDK_Coverage_RecordInstruction(struct ORBMDK_Coverage_Analyzer* analyzer,
    uint32_t addr, bool isThumb);

/**
 * @brief 记录分支跳转
 * @param analyzer 分析器句柄
 * @param from 跳转源地址
 * @param to 跳转目标地址
 * @param taken 是否执行跳转
 * @param isConditional 是否为条件跳转
 */
void ORBMDK_Coverage_RecordBranch(struct ORBMDK_Coverage_Analyzer* analyzer,
    uint32_t from, uint32_t to, bool taken, bool isConditional);

/**
 * @brief 记录函数调用
 * @param analyzer 分析器句柄
 * @param callAddr 调用指令地址
 * @param targetAddr 目标函数地址
 */
void ORBMDK_Coverage_RecordCall(struct ORBMDK_Coverage_Analyzer* analyzer,
    uint32_t callAddr, uint32_t targetAddr);

/**
 * @brief 记录函数返回
 * @param analyzer 分析器句柄
 * @param from 返回源地址
 * @param to 返回目标地址
 */
void ORBMDK_Coverage_RecordReturn(struct ORBMDK_Coverage_Analyzer* analyzer,
    uint32_t from, uint32_t to);

/**
 * @brief 记录周期计数
 * @param analyzer 分析器句柄
 * @param cycles 周期数增量
 */
void ORBMDK_Coverage_RecordCycles(struct ORBMDK_Coverage_Analyzer* analyzer, uint64_t cycles);

/**
 * @brief 从 ETM 分支记录批量导入
 * @param analyzer 分析器句柄
 * @param branch 指向分支记录的指针
 */
void ORBMDK_Coverage_AddBranchRecord(struct ORBMDK_Coverage_Analyzer* analyzer,
    const struct ORBMDK_ETM_Branch* branch);

// ============================================================================
// 统计分析
// ============================================================================

/**
 * @brief 获取覆盖率摘要
 * @param analyzer 分析器句柄
 * @return 覆盖率摘要
 */
struct ORBMDK_Coverage_Summary ORBMDK_Coverage_GetSummary(struct ORBMDK_Coverage_Analyzer* analyzer);

/**
 * @brief 获取执行的唯一地址数
 * @param analyzer 分析器句柄
 * @return 唯一地址数
 */
uint32_t ORBMDK_Coverage_GetUniqueAddresses(struct ORBMDK_Coverage_Analyzer* analyzer);

/**
 * @brief 获取指令执行记录数
 * @param analyzer 分析器句柄
 * @return 记录数
 */
uint32_t ORBMDK_Coverage_GetInstructionCount(struct ORBMDK_Coverage_Analyzer* analyzer);

/**
 * @brief 获取函数调用记录数
 * @param analyzer 分析器句柄
 * @return 调用记录数
 */
uint32_t ORBMDK_Coverage_GetCallCount(struct ORBMDK_Coverage_Analyzer* analyzer);

/**
 * @brief 获取总周期数
 * @param analyzer 分析器句柄
 * @return 总周期数
 */
uint64_t ORBMDK_Coverage_GetTotalCycles(struct ORBMDK_Coverage_Analyzer* analyzer);

/**
 * @brief 迭代执行记录
 * @param analyzer 分析器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Coverage_IterateInstructions(struct ORBMDK_Coverage_Analyzer* analyzer,
    void (*callback)(const struct ORBMDK_Coverage_ExecEntry* entry, void* userData),
    void* userData);

/**
 * @brief 迭代函数调用
 * @param analyzer 分析器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Coverage_IterateCalls(struct ORBMDK_Coverage_Analyzer* analyzer,
    void (*callback)(const struct ORBMDK_Coverage_CallEntry* entry, void* userData),
    void* userData);

/**
 * @brief 迭代覆盖的函数
 * @param analyzer 分析器句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_Coverage_IterateFunctions(struct ORBMDK_Coverage_Analyzer* analyzer,
    void (*callback)(const struct ORBMDK_Coverage_Function* func, void* userData),
    void* userData);

// ============================================================================
// 报告输出
// ============================================================================

/**
 * @brief 输出 KCacheGrind 格式覆盖率报告
 * @param analyzer 分析器句柄
 * @param outputPath 输出文件路径
 * @param includeVisits 包含访问计数
 * @return 0 成功，非 0 失败
 */
int ORBMDK_Coverage_OutputKCacheGrind(struct ORBMDK_Coverage_Analyzer* analyzer,
    const char* outputPath, bool includeVisits);

/**
 * @brief 输出简化覆盖率报告 (文本格式)
 * @param analyzer 分析器句柄
 * @param outputPath 输出文件路径
 * @return 0 成功，非 0 失败
 */
int ORBMDK_Coverage_OutputText(struct ORBMDK_Coverage_Analyzer* analyzer,
    const char* outputPath);

/**
 * @brief 输出 GCOV 格式覆盖率数据
 * @param analyzer 分析器句柄
 * @param outputPath 输出目录路径
 * @param sourceFiles 源文件列表 (逗号分隔)
 * @return 0 成功，非 0 失败
 */
int ORBMDK_Coverage_OutputGCOV(struct ORBMDK_Coverage_Analyzer* analyzer,
    const char* outputPath, const char* sourceFiles);

/**
 * @brief 生成 JSON 格式覆盖率报告
 * @param analyzer 分析器句柄
 * @param buffer 输出缓冲区
 * @param bufferSize 缓冲区大小
 * @return 写入的字节数
 */
size_t ORBMDK_Coverage_OutputJSON(struct ORBMDK_Coverage_Analyzer* analyzer,
    char* buffer, size_t bufferSize);

// ============================================================================
// 复位
// ============================================================================

/**
 * @brief 重置覆盖率统计
 * @param analyzer 分析器句柄
 */
void ORBMDK_Coverage_Reset(struct ORBMDK_Coverage_Analyzer* analyzer);

#ifdef __cplusplus
}
#endif
