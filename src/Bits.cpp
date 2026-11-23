// ============================================================================
// Bits.cpp —— 位引擎实现（M1）
//
// ----------------------------------------------------------------------------
// 先把那条最不像人话的规则讲透，因为后面每一行代码都是它的直接后果。
// ----------------------------------------------------------------------------
//
// 一帧 8 字节 = 64 bit，全局编号 0..63：
//
//     byte0        byte1        byte2      ...   byte7
//   7 6 5 4 3 2 1 0 | 15 14 ... 8 | 23 ... 16 | ... | 63 ... 56
//   ↑                                          ↑
//   这是 byte0 的最高位 7                    这是 byte7 的最高位 63
//
// 注意"位号越大在字节里越靠左（越高位）"，但"位号越大，字节越靠后"。
// 这两个方向是相反的，这就是所有混乱的来源。
//
//
// 二、Intel（@1，小端）：startBit 是信号的 LSB
//
//     EngineSpeed : 0|16@1
//     值 bit0 在帧 bit0，值 bit1 在帧 bit1 …… 值 bit15 在帧 bit15
//     一句话：位号一路 +1，从 byte0 的 bit0 数到 byte1 的 bit7。
//
//
// 三、Motorola（@0，大端）：startBit 是信号的 MSB，位号会"锯齿状"下降
//
//     BrakePress : 39|12@0
//     39 是这段 12 位的最高位。往下（往低位）走是 39→38→…→32，
//     走到某个字节的 bit0 之后，下一个是**下一字节的 bit7**，也就是 +15。
//
//     39 38 37 36 35 34 33 32 | 47 46 45 44
//     └──── byte4 整个字节 ───┘ └ byte5 高 4 位 ┘
//              ↑
//         32 是 byte4 的 bit0，下一个不是 31，是 32+15=47
//
//     为什么是 +15 而不是 -1？因为 bit0 是该字节的**最低位**，
//     信号还要继续往低位走，但本字节已经没了，只能去下一个数据字节，
//     而下一个字节的最低位是 (byte+1)*8+0 = b+8，最高位是 b+15。
//     信号的连续性要求先接最高位（MSB 在前），所以是 b+15。
//
//
// 四、由此得到一条判据，能救掉一半的错 DBC
//
//     Motorola 信号如果长度 >= 8，它的 startBit 必须落在某个字节的最高位，
//     也就是 startBit % 8 == 7。否则它会在字节中间起步、然后跳到下一字节，
//     十有八九冲出帧尾。
//     （我们的样例原来写的是 48|16@0，48%8==0，按这条规则算出 71 位，越界；
//       改成 55|16@0 才落在 byte6.bit7，正好占满 byte6+byte7。）
//
//     Intel 相反：长度 >= 8 时 startBit % 8 == 0 才是整字节对齐。
//
// ============================================================================

#include "Bits.h"

#include <cstdio>

namespace dbc {

namespace {

std::string itoa(std::uint32_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u", v);
    return buf;
}

std::string itoa(int v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

// "m1"/"m2" 这种多路复用分支（注意 "M" 是开关本身，不算分支）
bool isBranch(const std::string& mux) {
    return !mux.empty() && mux[0] == 'm' && mux != "M";
}

std::uint64_t frameBitMask(const BitLayout& lay) {
    std::uint64_t m = 0;
    for (std::uint32_t i = 0; i < lay.length; ++i) {
        m |= (1ull << (lay.slot[i].byte * 8 + lay.slot[i].bit));
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// 帧位号序列：MSB 在前。整个 M1 只有这一个函数带"知识"，
// 其余都是它的下游。
// ---------------------------------------------------------------------------
std::vector<int> bitSequence(std::uint32_t startBit, std::uint32_t length, ByteOrder order) {
    std::vector<int> seq;
    seq.reserve(length);

    int b = static_cast<int>(startBit);
    for (std::uint32_t k = 0; k < length; ++k) {
        seq.push_back(b);
        if (order == ByteOrder::Intel) {
            b += 1;                                    // 小端：位号递增
        } else {
            b = (b % 8 == 0) ? (b + 15) : (b - 1);     // 大端：锯齿下降
        }
    }
    return seq;
}

// ---------------------------------------------------------------------------
// 展开成位表。seq[k] 是第 k 高位，值里的位置是 length-1-k。
// 所以 slot[length-1] 落第一个（MSB），slot[0] 落最后一个（LSB）。
// ---------------------------------------------------------------------------
BitLayout layoutOf(const Signal& s) {
    BitLayout lay;
    lay.length = s.length;

    if (s.length == 0) {
        lay.error = "信号长度为 0";
        return lay;
    }
    if (s.length > 64) {
        lay.error = "信号长度 " + itoa(s.length) + " 位超过 64 位（8 字节帧最多 64 位）";
        return lay;
    }
    if (s.startBit > 63) {
        lay.error = "起始位 " + itoa(s.startBit) + " 不在 0..63 之内";
        return lay;
    }

    std::vector<int> seq = bitSequence(s.startBit, s.length, s.order);
    for (std::uint32_t k = 0; k < s.length; ++k) {
        int fb = seq[k];
        if (fb < 0 || fb > 63) {
            std::string hint;
            if (s.order == ByteOrder::Motorola && (s.startBit % 8) != 7) {
                hint = "；Motorola 的起点应是某字节的最高位（startBit%8==7）";
            }
            lay.error = "第 " + itoa(static_cast<int>(k) + 1) + " 个位落到帧外（帧位号 " +
                        itoa(fb) + "，帧只有 0..63）" + hint;
            return lay;
        }

        // 关键：seq 的叫法两种字节序不一样，所以"第 k 个位置对应值的哪一位"也不同。
        //   Intel    ：seq 从 LSB 起排，seq[k] 就是值的第 k 位
        //   Motorola ：seq 从 MSB 起排，seq[k] 是值的第 (length-1-k) 位
        std::uint32_t valueBit = (s.order == ByteOrder::Intel)
                               ? k
                               : (s.length - 1 - k);
        lay.slot[valueBit].byte = static_cast<std::uint8_t>(fb / 8);
        lay.slot[valueBit].bit  = static_cast<std::uint8_t>(fb % 8);
        lay.slot[valueBit].mask = static_cast<std::uint8_t>(1u << (fb % 8));
    }

    lay.ok = true;
    return lay;
}

// ---------------------------------------------------------------------------
// 取：按位权把散在各字节的位拼回一个整数
// ---------------------------------------------------------------------------
std::uint64_t decodeRaw(const BitLayout& lay, const std::uint8_t* data) {
    if (!lay.ok || data == nullptr) return 0;

    std::uint64_t raw = 0;
    for (std::uint32_t i = 0; i < lay.length; ++i) {
        if (data[lay.slot[i].byte] & lay.slot[i].mask) {
            raw |= (1ull << i);
        }
    }
    return raw;
}

// ---------------------------------------------------------------------------
// 放：只改自己的位，别人的位一个也不许动
//
// 先整段校验位宽再动手，是为了保证"要么全写、要么不写" ——
// 半写状态下的帧是最难查的 bug。
// ---------------------------------------------------------------------------
bool encodeRaw(const BitLayout& lay, std::uint64_t raw, std::uint8_t* data) {
    if (!lay.ok || data == nullptr) return false;
    if (lay.length < 64 && (raw >> lay.length) != 0) return false;   // 值装不下

    for (std::uint32_t i = 0; i < lay.length; ++i) {
        if ((raw >> i) & 1ull) {
            data[lay.slot[i].byte] |= lay.slot[i].mask;
        } else {
            data[lay.slot[i].byte] &= static_cast<std::uint8_t>(~lay.slot[i].mask);
        }
    }
    return true;
}

std::uint64_t decodeRaw(const Signal& s, const std::uint8_t* data, std::string* err) {
    BitLayout lay = layoutOf(s);
    if (!lay.ok) {
        if (err != nullptr) *err = lay.error;
        return 0;
    }
    return decodeRaw(lay, data);
}

bool encodeRaw(const Signal& s, std::uint64_t raw, std::uint8_t* data, std::string* err) {
    BitLayout lay = layoutOf(s);
    if (!lay.ok) {
        if (err != nullptr) *err = lay.error;
        return false;
    }
    if (lay.length < 64 && (raw >> lay.length) != 0) {
        if (err != nullptr) {
            *err = "原始值 " + itoa(static_cast<int>(raw)) + " 装不进 " +
                   itoa(lay.length) + " 位";
        }
        return false;
    }
    return encodeRaw(lay, raw, data);
}

// ---------------------------------------------------------------------------
// 抢位检查
// ---------------------------------------------------------------------------
std::vector<Overlap> findOverlaps(const Message& m) {
    std::vector<Overlap> out;

    struct Item {
        const char* name;
        std::uint64_t mask;
        std::string   mux;
    };
    std::vector<Item> items;

    for (const auto& s : m.signals) {
        BitLayout lay = layoutOf(s);
        if (!lay.ok) continue;                     // 非法声明另有报错渠道
        items.push_back(Item{ s.name.c_str(), frameBitMask(lay), s.mux });
    }

    for (std::size_t i = 0; i < items.size(); ++i) {
        for (std::size_t j = i + 1; j < items.size(); ++j) {
            std::uint64_t shared = items[i].mask & items[j].mask;
            if (shared == 0) continue;

            // 不同复用分支本来就共用位，这是设计
            if (isBranch(items[i].mux) && isBranch(items[j].mux) &&
                items[i].mux != items[j].mux) {
                continue;
            }

            int nbits = 0;
            for (std::uint64_t v = shared; v != 0; v &= (v - 1)) ++nbits;

            out.push_back(Overlap{ items[i].name, items[j].name, nbits });
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 描述："byte4.7-0 + byte5.7-4" —— 按 MSB 到 LSB 的顺序，同一字节合并成区间
// ---------------------------------------------------------------------------
std::string describeBits(const Signal& s) {
    BitLayout lay = layoutOf(s);
    if (!lay.ok) return "非法（" + lay.error + "）";

    std::vector<int> seq = bitSequence(s.startBit, s.length, s.order);

    std::string out;
    int curByte = -1;
    int hi = 0;   // 区间起点（先到的那个，也就是高的一头）
    int lo = 0;   // 区间走到的最远一位

    for (std::size_t k = 0; k <= seq.size(); ++k) {
        const bool flush = (k == seq.size());

        int by = -1;
        int bi = -1;
        if (!flush) {
            by = seq[k] / 8;
            bi = seq[k] % 8;
        }

        if (!flush && by == curByte) {
            lo = bi;
            continue;
        }

        if (curByte >= 0) {
            if (!out.empty()) out += " + ";
            out += "byte" + itoa(static_cast<std::uint32_t>(curByte)) + "." + itoa(hi);
            if (lo != hi) out += "-" + itoa(lo);
        }
        if (flush) break;

        curByte = by;
        hi = bi;
        lo = bi;
    }
    return out;
}

} // namespace dbc
