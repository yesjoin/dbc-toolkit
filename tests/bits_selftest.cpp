// ============================================================================
// bits_selftest.cpp —— M1 验收夹具
//
// 这一层的验收标准只有一条：**手算得出的位号序列，代码必须一字不差**。
// 所以下面每一种 case 都是先在纸上算好的常数，不是从代码里抄回来的。
//
// 检查顺序刻意跟人学的顺序一致：
//   1) 位号序列对不对（数学）
//   2) 序列落到哪个字节的哪一位（翻译）
//   3) 取出来的数对不对（应用）
//   4) 放回去对不对（逆运算）
//   5) 非法声明有没有被拦住（防守）
//   6) 抢位能不能查出来（增值）
// ============================================================================

#include "Bits.h"
#include "Dbc.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#ifndef DBC_SAMPLE
#define DBC_SAMPLE "dbc/sample.dbc"
#endif

namespace {

int gPass = 0;
int gFail = 0;

void check(bool cond, const char* what) {
    if (cond) {
        ++gPass;
        std::printf("[PASS] %s\n", what);
    } else {
        ++gFail;
        std::printf("[FAIL] %s\n", what);
    }
}

dbc::Signal mk(std::uint32_t start, std::uint32_t len, dbc::ByteOrder order) {
    dbc::Signal s;
    s.name = "probe";
    s.startBit = start;
    s.length = len;
    s.order = order;
    return s;
}

bool seqEq(const std::vector<int>& got, std::initializer_list<int> want) {
    if (got.size() != want.size()) return false;
    std::size_t i = 0;
    for (int v : want) {
        if (got[i] != v) return false;
        ++i;
    }
    return true;
}

bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1) 位号序列 —— 纯数学，手算过的常数
// ---------------------------------------------------------------------------
void testSequence() {
    std::printf("\n== 1. 位号序列 ==\n");

    {
        auto got = dbc::bitSequence(0, 16, dbc::ByteOrder::Intel);
        check(seqEq(got, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}),
              "Intel 0|16 → 0,1,…,15 一路递增");
    }
    {
        auto got = dbc::bitSequence(7, 16, dbc::ByteOrder::Motorola);
        check(seqEq(got, {7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8}),
              "Motorola 7|16 → 7..0 然后跳到 15..8（+15 那条规则的经典形态）");
    }
    {
        auto got = dbc::bitSequence(39, 12, dbc::ByteOrder::Motorola);
        check(seqEq(got, {39, 38, 37, 36, 35, 34, 33, 32, 47, 46, 45, 44}),
              "Motorola 39|12 → 39..32 然后 +15 到 47..44");
    }
    {
        auto got = dbc::bitSequence(55, 16, dbc::ByteOrder::Motorola);
        check(seqEq(got, {55, 54, 53, 52, 51, 50, 49, 48,
                          63, 62, 61, 60, 59, 58, 57, 56}),
              "Motorola 55|16 → 整占 byte6 + byte7");
    }
    {
        auto got = dbc::bitSequence(63, 8, dbc::ByteOrder::Motorola);
        check(seqEq(got, {63, 62, 61, 60, 59, 58, 57, 56}),
              "Motorola 63|8 → 正好最后一字节，不越界");
    }
    {
        auto got = dbc::bitSequence(40, 4, dbc::ByteOrder::Intel);
        check(seqEq(got, {40, 41, 42, 43}), "Intel 40|4 → 40,41,42,43（byte5 低半字节）");
    }
    {
        auto got = dbc::bitSequence(5, 1, dbc::ByteOrder::Intel);
        check(seqEq(got, {5}), "单 bit：Intel 5|1 → {5}");
    }
    {
        auto got = dbc::bitSequence(7, 1, dbc::ByteOrder::Motorola);
        check(seqEq(got, {7}), "单 bit：Motorola 7|1 → {7}");
    }
}

// ---------------------------------------------------------------------------
// 2) 序列 → 字节/位。核心是 MSB 与 LSB 各落在哪
// ---------------------------------------------------------------------------
void testSlots() {
    std::printf("\n== 2. 位到字节的翻译 ==\n");

    auto brake = dbc::layoutOf(mk(39, 12, dbc::ByteOrder::Motorola));
    check(brake.ok, "39|12@0 是合法声明");
    check(brake.slot[11].byte == 4 && brake.slot[11].bit == 7,
          "39|12@0 的 MSB(第11位) 落在 byte4.bit7");
    check(brake.slot[7].byte == 4 && brake.slot[7].bit == 3,
          "39|12@0 的第7位落在 byte4.bit3");
    check(brake.slot[4].byte == 4 && brake.slot[4].bit == 0,
          "39|12@0 的第4位落在 byte4.bit0（byte4 走到最低位了，再往下才跳到 byte5）");
    check(brake.slot[0].byte == 5 && brake.slot[0].bit == 4,
          "39|12@0 的 LSB(第0位) 落在 byte5.bit4");
    check(brake.slot[3].mask == 0x80, "byte4.bit7 的掩码是 0x80");

    auto speed = dbc::layoutOf(mk(0, 16, dbc::ByteOrder::Intel));
    check(speed.ok, "0|16@1 是合法声明");
    check(speed.slot[0].byte == 0 && speed.slot[0].bit == 0,
          "0|16@1 的 LSB 落在 byte0.bit0（和 Motorola 正好相反）");
    check(speed.slot[15].byte == 1 && speed.slot[15].bit == 7,
          "0|16@1 的 MSB 落在 byte1.bit7");

    auto steer = dbc::layoutOf(mk(55, 16, dbc::ByteOrder::Motorola));
    check(steer.slot[15].byte == 6 && steer.slot[15].bit == 7,
          "55|16@0 的 MSB 落在 byte6.bit7");
    check(steer.slot[0].byte == 7 && steer.slot[0].bit == 0,
          "55|16@0 的 LSB 落在 byte7.bit0");
}

// ---------------------------------------------------------------------------
// 3) 取数 —— 每个期望值都是手算的
// ---------------------------------------------------------------------------
void testDecode() {
    std::printf("\n== 3. 取值 ==\n");

    {
        std::uint8_t a[8] = {0x34, 0x12, 0, 0, 0, 0, 0, 0};
        dbc::Signal s = mk(0, 16, dbc::ByteOrder::Intel);
        check(dbc::decodeRaw(s, a) == 0x1234u, "Intel 0|16 读到 0x1234（byte0 是低位）");
    }
    {
        std::uint8_t a[8] = {0x12, 0x34, 0, 0, 0, 0, 0, 0};
        dbc::Signal s = mk(7, 16, dbc::ByteOrder::Motorola);
        check(dbc::decodeRaw(s, a) == 0x1234u, "Motorola 7|16 也读到 0x1234（byte0 是高位）");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0, 0, 0x12, 0x34};
        dbc::Signal s = mk(55, 16, dbc::ByteOrder::Motorola);
        check(dbc::decodeRaw(s, a) == 0x1234u, "Motorola 55|16 读到 0x1234（跨 byte6/byte7）");
        dbc::Signal intel = mk(48, 16, dbc::ByteOrder::Intel);   // Intel 的同一段是 48|16，不是 55
        check(dbc::decodeRaw(intel, a) == 0x3412u,
              "同样的两个字节，Motorola 55|16 读 0x1234，Intel 48|16 读 0x3412 —— 字节序真会改数");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0xAB, 0xCD, 0, 0};
        dbc::Signal s = mk(39, 12, dbc::ByteOrder::Motorola);
        check(dbc::decodeRaw(s, a) == 0xABCu,
              "Motorola 39|12 从 0xAB/0xCD 里抠出 0xABC（byte4 全用 + byte5 高 4 位）");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0, 0x0D, 0, 0};
        dbc::Signal s = mk(40, 4, dbc::ByteOrder::Intel);
        check(dbc::decodeRaw(s, a) == 0xDu, "Intel 40|4 读到 0x0D（byte5 低半字节）");
    }
    {
        std::uint8_t a[8] = {0x80, 0, 0, 0, 0, 0, 0, 0};
        dbc::Signal s = mk(0, 8, dbc::ByteOrder::Intel);
        check(dbc::decodeRaw(s, a) == 128u, "Intel 0|8 读到 128（byte0.bit7 一个 1）");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        dbc::Signal s = mk(55, 16, dbc::ByteOrder::Motorola);
        check(dbc::decodeRaw(s, a) == 0xFFFFu, "全 1 的 16 位读到 0xFFFF");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        dbc::Signal s = mk(48, 16, dbc::ByteOrder::Motorola);
        std::string err;
        check(dbc::decodeRaw(s, a, &err) == 0u && !err.empty(),
              "Motorola 48|16 这种声明被拦下并给出原因（它算到 71 位，出帧）");
        check(contains(err, "帧外"), "拦截原因里点明是“帧外”");
    }
}

// ---------------------------------------------------------------------------
// 4) 放数 —— 逆运算 + 不许碰别人的位
// ---------------------------------------------------------------------------
void testEncode() {
    std::printf("\n== 4. 放值 ==\n");

    {
        std::uint8_t a[8] = {0};
        dbc::Signal s = mk(39, 12, dbc::ByteOrder::Motorola);
        check(dbc::encodeRaw(s, 0xABC, a), "39|12 写入 0xABC 成功");
        check(a[4] == 0xAB, "写入后 byte4 = 0xAB（信号的高 8 位）");
        check(a[5] == 0xC0, "写入后 byte5 = 0xC0（低 4 位被顶到高半字节）");
    }
    {
        std::uint8_t a[8] = {0, 0, 0, 0, 0, 0x0F, 0, 0};
        dbc::Signal s = mk(39, 12, dbc::ByteOrder::Motorola);
        check(dbc::encodeRaw(s, 0x0, a), "39|12 写入 0 成功");
        check(a[5] == 0x0F, "只清掉自己的 4 位，byte5 低半字节的 0x0F 原样留着");
        check(a[4] == 0x00, "byte4 的 8 位全清（那 8 位都属于这个信号）");
    }
    {
        dbc::Signal s = mk(39, 12, dbc::ByteOrder::Motorola);
        std::uint32_t values[5] = {0u, 1u, 0xABCu, 0x800u, 0xFFFu};
        bool allOk = true;
        for (int i = 0; i < 5; ++i) {
            std::uint8_t a[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            allOk = allOk && dbc::encodeRaw(s, values[i], a) &&
                    dbc::decodeRaw(s, a) == values[i];
        }
        check(allOk, "39|12 写入→读出往返 5 个值都一致（在满 0xFF 的脏帧上）");
    }
    {
        std::uint8_t a[8] = {0};
        dbc::Signal s = mk(39, 12, dbc::ByteOrder::Motorola);
        std::uint8_t before[8] = {0};
        bool r = dbc::encodeRaw(s, 0x1000, a);   // 12 位装不下 0x1000
        bool untouched = true;
        for (int i = 0; i < 8; ++i) untouched = untouched && a[i] == before[i];
        check(!r && untouched, "装不下的值直接拒写，而且一个字节都没动");
    }
    {
        std::uint8_t a[8] = {0};
        dbc::Signal s = mk(0, 64, dbc::ByteOrder::Intel);
        std::uint64_t v = 0xDEADBEEFCAFEBABEull;
        check(dbc::encodeRaw(s, v, a) && dbc::decodeRaw(s, a) == v,
              "Intel 0|64 能把一个 64 位常数原样写入再读出");
    }
    {
        std::uint8_t a[8] = {0};
        dbc::Signal s = mk(7, 64, dbc::ByteOrder::Motorola);
        std::uint64_t v = 0x0123456789ABCDEFull;
        check(dbc::encodeRaw(s, v, a) && dbc::decodeRaw(s, a) == v,
              "Motorola 7|64 同样能往返（8 字节整帧的大端写法）");
        check(a[0] == 0x01 && a[7] == 0xEF,
              "大端落点正确：最高字节进 byte0，最低字节留在 byte7");
    }
    {
        std::uint8_t a[8] = {0};
        std::string err;
        check(!dbc::encodeRaw(mk(48, 16, dbc::ByteOrder::Motorola), 1, a, &err) &&
              contains(err, "帧外"),
              "带校验的写入版对非法声明也报“帧外”");
    }
}

// ---------------------------------------------------------------------------
// 5) 非法声明的防守
// ---------------------------------------------------------------------------
void testErrors() {
    std::printf("\n== 5. 非法声明 ==\n");

    check(!dbc::layoutOf(mk(0, 0, dbc::ByteOrder::Intel)).ok, "长度为 0 被拒");
    check(!dbc::layoutOf(mk(0, 65, dbc::ByteOrder::Intel)).ok, "长度 65 被拒（超过 64 位）");
    check(!dbc::layoutOf(mk(64, 8, dbc::ByteOrder::Intel)).ok, "起始位 64 被拒");
    check(!dbc::layoutOf(mk(63, 2, dbc::ByteOrder::Intel)).ok,
          "Intel 63|2 被拒（第 2 位就出 64 了）");
    check(dbc::layoutOf(mk(0, 64, dbc::ByteOrder::Intel)).ok,
          "Intel 0|64 合法（正好铺满一帧，不越界）");
    check(dbc::layoutOf(mk(39, 12, dbc::ByteOrder::Motorola)).ok,
          "Motorola 39|12 合法");
    check(!dbc::layoutOf(mk(48, 16, dbc::ByteOrder::Motorola)).ok,
          "Motorola 48|16 非法 —— startBit 不在字节最高位，算下去会出帧");
}

// ---------------------------------------------------------------------------
// 6) 抢位检查 + 位段描述
// ---------------------------------------------------------------------------
void testOverlaps() {
    std::printf("\n== 6. 抢位与位段描述 ==\n");

    {
        dbc::Message m;
        m.name = "T";
        dbc::Signal a = mk(0, 16, dbc::ByteOrder::Intel);
        a.name = "A";
        dbc::Signal b = mk(8, 16, dbc::ByteOrder::Intel);
        b.name = "B";
        m.signals = {a, b};                      // 0..15 与 8..23 重叠 8 位
        auto ov = dbc::findOverlaps(m);
        check(ov.size() == 1 && ov[0].sharedBits == 8,
              "A(0..15) 与 B(8..23) 抢 8 位，被查出来");
    }
    {
        dbc::Message m;
        dbc::Signal sel = mk(0, 8, dbc::ByteOrder::Intel);
        sel.name = "MuxSel";
        sel.mux = "M";
        dbc::Signal a = mk(8, 16, dbc::ByteOrder::Intel);
        a.name = "MuxA";
        a.mux = "m1";
        dbc::Signal b = mk(8, 16, dbc::ByteOrder::Intel);
        b.name = "MuxB";
        b.mux = "m2";
        m.signals = {sel, a, b};                 // A/B 同址，但不同分支
        check(dbc::findOverlaps(m).empty(), "不同复用分支共用位不算错误（m1 与 m2 豁免）");

        b.mux = "m1";                            // 都改成 m1 → 真抢
        m.signals[2].mux = "m1";                 // 注意改副本没用，得改报文里那一条
        check(dbc::findOverlaps(m).size() == 1, "同一分支的两条信号抢位仍然会被报出来");
    }
    {
        dbc::ParseResult r = dbc::parseFile(DBC_SAMPLE);
        check(r.ok, "样例 DBC 解析成功");

        bool allClean = true;
        for (const auto& m : r.db.messages) {
            if (!dbc::findOverlaps(m).empty()) allClean = false;
        }
        check(allClean, "样例里 4 个报文全部零抢位（8 字节铺得刚刚好）");
    }
    {
        check(dbc::describeBits(mk(39, 12, dbc::ByteOrder::Motorola)) ==
                  "byte4.7-0 + byte5.7-4",
              "39|12@0 描述成 byte4.7-0 + byte5.7-4");
        check(dbc::describeBits(mk(55, 16, dbc::ByteOrder::Motorola)) ==
                  "byte6.7-0 + byte7.7-0",
              "55|16@0 描述成 byte6.7-0 + byte7.7-0");
        check(dbc::describeBits(mk(0, 16, dbc::ByteOrder::Intel)) ==
                  "byte0.0-7 + byte1.0-7",
              "0|16@1 描述成 byte0.0-7 + byte1.0-7（小端从小位往大写）");
    }
}

} // namespace

int main() {
    std::printf("M1 位引擎自测\n");

    testSequence();
    testSlots();
    testDecode();
    testEncode();
    testErrors();
    testOverlaps();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) {
        std::printf("M1 全绿 —— 位能取了，才谈得上物理值。\n");
    }
    return gFail == 0 ? 0 : 1;
}
