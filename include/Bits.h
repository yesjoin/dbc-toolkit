#pragma once
// ============================================================================
// Bits.h —— 位引擎（M1）
//
// 这一层的唯一职责：把 (startBit, length, order) 翻译成"帧里哪几个 bit"，
// 然后在这几个 bit 上取/放一个整数。既不碰物理量，也不碰多路复用。
//
// 帧内位号约定（DBC 通用）：
//   帧 8 字节 = 64 个 bit，编号 0..63
//   bit 号 b 所在字节 = b / 8，在字节内的位置 = b % 8（0 是这一字节的最低位）
//
// 两种字节序的差别只有一条 —— startBit 指的是哪一头：
//   Intel(@1)    startBit 是信号的 LSB，往后位号 +1（0,1,2,...）
//   Motorola(@0) startBit 是信号的 MSB，位号先 -1 走，到字节最低位后跳 +15
//                （也就是"进到下一字节的最高位"）
//
// 那个 +15 是整个项目最不像人话的一条规则，见 Bits.cpp 里的讲解。
// ============================================================================

#include "Dbc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dbc {

// 信号的第 i 个位权落在帧的哪个 bit 上
struct BitSlot {
    std::uint8_t byte = 0;   // 第几个数据字节（0..7）
    std::uint8_t bit  = 0;   // 字节内第几位（0 = 最低位）
    std::uint8_t mask = 0;   // 1u << bit，省得每次再算
};

// 一个信号展开后的位表。slot[0] 是信号的 LSB，slot[length-1] 是 MSB。
struct BitLayout {
    bool          ok = false;
    std::string   error;         // ok == false 时的人话
    std::uint32_t length = 0;
    BitSlot       slot[64] {};
};

// 帧位号序列，按"从 MSB 到 LSB"的顺序给出。非法声明也会返回序列，
// 越界的位号由 layoutOf() 负责拦（这里只做纯数学）。
std::vector<int> bitSequence(std::uint32_t startBit, std::uint32_t length, ByteOrder order);

// 展开 + 校验。越出 64 位帧、长度为 0、起点 > 63 都会在这里被拦下。
BitLayout layoutOf(const Signal& s);

// 从 8 字节数据里抠出原始无符号值（不做符号扩展，那是 M2）
std::uint64_t decodeRaw(const BitLayout& lay, const std::uint8_t* data);

// 放进 8 字节数据里。只改这个信号自己的位，其余位原样保留。
// raw 超出位宽时返回 false，且一个字节都不动（要么全写要么不写）。
bool encodeRaw(const BitLayout& lay, std::uint64_t raw, std::uint8_t* data);

// 带校验的便捷版：layout 非法时返回 0 / false，并在 err 里说明原因
std::uint64_t decodeRaw(const Signal& s, const std::uint8_t* data, std::string* err = nullptr);
bool          encodeRaw(const Signal& s, std::uint64_t raw, std::uint8_t* data,
                        std::string* err = nullptr);

// 同一个报文里两条信号抢同一个 bit。真实 DBC 里这类错误很常见，
// 解析阶段永远发现不了（解析不管位），只有位引擎算出来才看得见。
struct Overlap {
    std::string a;
    std::string b;
    int         sharedBits = 0;
};

// 多路复用的不同分支本来就共用位（这是设计，不是错误），所以：
// 两条都是 mN 分支且 N 不同 → 豁免。其余一律上报。
std::vector<Overlap> findOverlaps(const Message& m);

// "byte4.7-0 + byte5.7-4" 这种人看得懂的描述
std::string describeBits(const Signal& s);

} // namespace dbc
