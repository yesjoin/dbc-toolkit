// ============================================================================
// Codec.cpp —— 编解码实现（M2）
//
// ----------------------------------------------------------------------------
// 三个必须先想清楚的事
// ----------------------------------------------------------------------------
//
// 一、raw 是"位模式"，不是"数"
//
//    BrakePress 12 位，帧里那 12 个 bit 全是 1，raw 就是 0xFFF。
//    如果声明是无符号 → 它表示 4095。
//    如果声明是有符号 → 它表示 -1（补码：最高位是符号位，全 1 = 负一）。
//    同一堆 bit，两种解释，值差 4096。所以 extendSign 是独立的一步，
//    绝不能"顺手"在 decodeRaw 里做掉。
//
// 二、换算方向只有一个
//
//        物理值 = raw * factor + offset
//        raw   = (物理值 - offset) / factor     ← 反算，注意先减 offset 再除
//
//    写成 (物理值 / factor) - offset 就错了。factor 是"一个 raw 单位值多少物理量"，
//    从 DBC 的 (factor,offset) 原样读出来的就是这个意思。
//    EngineSpeed: (0.125, 0) → raw 4660 → 4660*0.125 = 582.5 rpm
//    EngineTemp : (1, -40)   → raw 0    → 0*1 - 40 = -40 degC（offset 是零点偏移）
//
// 三、位宽 ≠ 量程
//
//    12 位无符号：能装 0..4095
//    BrakePress 声明 [0|400]：物理上合理的是 0..400 bar，对应 raw 0..4000
//    raw = 4095 → 409.5 bar → **装得下，但超量程** → 这是"传感器坏了"的信号，
//    不是"数据坏了"。两者要分开报告，否则排查时根本不知道该怀疑谁。
//
// ----------------------------------------------------------------------------
// 四、四舍五入的方向是个决定，不是巧合
//
//    physicalToInt 用 std::round（.5 远离 0）：0.05/0.1 = 0.5 → 1 → 0.1 bar
//    用 floor 就会变成 0 → 0.0 bar。发报文时差一个 LSB 就是差一个精度台阶，
//    选哪个都行，但必须写死并测出来，不能"随编译器"。
//
// ============================================================================

#include "Codec.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace dbc {

namespace {

std::string ftoa(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string ltoa(std::int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return buf;
}

// 量程判断带一点容差：factor 是 0.1 这种非二进制小数，
// 算出来可能是 99.99999999999999，不能因此判成超量程。
bool within(double v, double lo, double hi) {
    double scale = std::fmax(1.0, std::fmax(std::fabs(lo), std::fabs(hi)));
    double eps = 1e-9 * scale;
    return v >= lo - eps && v <= hi + eps;
}

} // namespace

// ---------------------------------------------------------------------------
void rawRange(const Signal& s, std::int64_t* lo, std::int64_t* hi) {
    if (lo == nullptr || hi == nullptr) return;

    if (s.isSigned) {
        if (s.length >= 64) {
            *lo = std::numeric_limits<std::int64_t>::min();
            *hi = std::numeric_limits<std::int64_t>::max();
        } else {
            *lo = -(static_cast<std::int64_t>(1) << (s.length - 1));
            *hi = (static_cast<std::int64_t>(1) << (s.length - 1)) - 1;
        }
    } else {
        *lo = 0;
        *hi = (s.length >= 64) ? std::numeric_limits<std::int64_t>::max()
                               : ((static_cast<std::int64_t>(1) << s.length) - 1);
    }
}

// ---------------------------------------------------------------------------
// 符号扩展。核心就一句：最高位是 1 的话，把上面所有空位全填 1。
// ---------------------------------------------------------------------------
std::int64_t extendSign(std::uint64_t raw, std::uint32_t length) {
    if (length == 0) return 0;
    if (length >= 64) return static_cast<std::int64_t>(raw);

    const std::uint64_t signBit = 1ull << (length - 1);
    if ((raw & signBit) == 0) {
        return static_cast<std::int64_t>(raw & ((1ull << length) - 1));
    }
    // 负数：把 length 位以上的位全部铺成 1
    return static_cast<std::int64_t>(raw | ~((1ull << length) - 1));
}

std::uint64_t packRaw(std::int64_t raw, std::uint32_t length) {
    if (length >= 64) return static_cast<std::uint64_t>(raw);
    const std::uint64_t mask = (1ull << length) - 1;
    return static_cast<std::uint64_t>(raw) & mask;   // 负数按补码截低位
}

// ---------------------------------------------------------------------------
double intToPhysical(const Signal& s, std::int64_t raw) {
    return static_cast<double>(raw) * s.factor + s.offset;
}

bool physicalToInt(const Signal& s, double value, std::int64_t* out, std::string* err) {
    if (out == nullptr) return false;

    if (s.factor == 0.0) {
        if (err != nullptr) *err = "factor 为 0，物理值无法反算成 raw（除零）";
        return false;
    }

    double rawD = (value - s.offset) / s.factor;

    // 大得离谱的数先挡掉，避免 llround 溢出
    const double limit = 9.0e18;
    if (!std::isfinite(rawD) || rawD < -limit || rawD > limit) {
        if (err != nullptr) *err = "物理值 " + ftoa(value) + " 反算出的 raw 不是有限数";
        return false;
    }

    const std::int64_t raw = static_cast<std::int64_t>(std::round(rawD));   // .5 远离 0

    std::int64_t lo = 0, hi = 0;
    rawRange(s, &lo, &hi);
    if (raw < lo || raw > hi) {
        if (err != nullptr) {
            *err = "物理值 " + ftoa(value) + " 需要 raw=" + ltoa(raw) +
                   "，超出 " + std::to_string(s.length) + " 位的表示范围 [" +
                   ltoa(lo) + ", " + ltoa(hi) + "]";
        }
        return false;
    }

    *out = raw;
    return true;
}

// ---------------------------------------------------------------------------
const std::string* valueTextFor(const Signal& s, std::int64_t raw) {
    for (const auto& v : s.valueTable) {
        if (v.value == raw) return &v.text;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
DecodeResult decodeSignal(const Signal& s, const std::uint8_t* data) {
    DecodeResult r;

    BitLayout lay = layoutOf(s);
    if (!lay.ok) {
        r.error = lay.error;
        return r;
    }

    r.raw = decodeRaw(lay, data);
    r.rawSigned = s.isSigned ? extendSign(r.raw, s.length)
                             : static_cast<std::int64_t>(r.raw);
    r.value = intToPhysical(s, r.rawSigned);

    // DBC 里 [0|0] 通常表示"没写量程"，别拿它当"只能取 0"
    r.rangeDeclared = !(s.minimum == 0.0 && s.maximum == 0.0);
    r.inEngineeringRange = !r.rangeDeclared || within(r.value, s.minimum, s.maximum);

    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// 先算、先校验，最后才动 data —— 保证"要么全写、要么不写"
// ---------------------------------------------------------------------------
EncodeResult encodeSignal(const Signal& s, double value, std::uint8_t* data,
                          bool saturate) {
    EncodeResult r;

    BitLayout lay = layoutOf(s);
    if (!lay.ok) {
        r.error = lay.error;
        return r;
    }
    if (s.factor == 0.0) {
        r.error = "factor 为 0，物理值无法反算成 raw（除零）";
        return r;
    }

    double rawD = (value - s.offset) / s.factor;
    const double limit = 9.0e18;
    if (!std::isfinite(rawD) || rawD < -limit || rawD > limit) {
        r.error = "物理值 " + ftoa(value) + " 反算出的 raw 不是有限数";
        return r;
    }

    std::int64_t lo = 0, hi = 0;
    rawRange(s, &lo, &hi);

    double rounded = std::round(rawD);
    if (rounded < static_cast<double>(lo) || rounded > static_cast<double>(hi)) {
        if (!saturate) {
            r.error = "物理值 " + ftoa(value) + " 需要 raw=" + ltoa(static_cast<std::int64_t>(rounded)) +
                      "，超出 " + std::to_string(s.length) + " 位的表示范围 [" +
                      ltoa(lo) + ", " + ltoa(hi) + "]";
            return r;
        }
        rounded = (rounded < static_cast<double>(lo)) ? static_cast<double>(lo)
                                                      : static_cast<double>(hi);
        r.clamped = true;
    }

    const std::uint64_t packed =
        packRaw(static_cast<std::int64_t>(rounded), s.length);

    if (!encodeRaw(lay, packed, data)) {
        r.error = "写位失败";
        return r;
    }

    r.ok = true;
    r.raw = packed;
    return r;
}

} // namespace dbc
