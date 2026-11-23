// ============================================================================
// codec_selftest.cpp —— M2 验收夹具
//
// 验收标准：raw 和物理值之间来回走一趟，必须回到原处；
//           走不过去的时候，必须**说清楚是"装不下"还是"不合理"**。
//
// 所有期望值都是手算的常数。例如：
//   EngineSpeed (0.125,0)，raw 4660 → 4660 * 0.125 = 582.5 rpm
//   EngineTemp  (1,-40)  ，raw 0    → 0 * 1 - 40   = -40 degC
//   SteeringAngle 有符号，raw 0xFFFF → 补码 -1 → -1 * 0.1 = -0.1 deg
// ============================================================================

#include "Codec.h"
#include "Dbc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

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

bool near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

dbc::Signal mk(std::uint32_t start, std::uint32_t len, dbc::ByteOrder order,
               bool isSigned, double factor, double offset,
               double minV = 0.0, double maxV = 0.0) {
    dbc::Signal s;
    s.name = "probe";
    s.startBit = start;
    s.length = len;
    s.order = order;
    s.isSigned = isSigned;
    s.factor = factor;
    s.offset = offset;
    s.minimum = minV;
    s.maximum = maxV;
    return s;
}

const dbc::Signal* sig(const dbc::Database& db, std::uint32_t id, const char* name) {
    const dbc::Message* m = db.find(id, false);
    if (m == nullptr) m = db.find(id, true);
    return m == nullptr ? nullptr : m->find(name);
}

// M0 的 0x100 帧：手算好的 8 字节
//   EngineSpeed raw=100  → byte0=0x64
//   EngineTemp  raw=100  → byte2=0x64（物理 60 degC）
//   ThrottlePos raw=0    → byte3=0x00
//   BrakePress  raw=0xABC→ byte4=0xAB, byte5 高 4 位 = 0xC
//   GearPosition raw=3   → byte5 低 4 位 = 0x3  → byte5 = 0xC3
//   SteeringAngle raw=0xFFFF → byte6=0xFF, byte7=0xFF（补码 -1 → -0.1 deg）
const std::uint8_t kFrame[8] = {0x64, 0x00, 0x64, 0x00, 0xAB, 0xC3, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// 1) 补码
// ---------------------------------------------------------------------------
void testSign() {
    std::printf("\n== 1. 符号扩展与压回 ==\n");

    check(dbc::extendSign(0xFFFF, 16) == -1, "16 位 0xFFFF → -1");
    check(dbc::extendSign(0x8000, 16) == -32768, "16 位 0x8000 → -32768（负区间的起点）");
    check(dbc::extendSign(0x7FFF, 16) == 32767, "16 位 0x7FFF → 32767（正区间的终点）");
    check(dbc::extendSign(0xFFF, 12) == -1, "12 位 0xFFF → -1");
    check(dbc::extendSign(0x800, 12) == -2048, "12 位 0x800 → -2048");
    check(dbc::extendSign(0xF, 4) == -1, "4 位 0xF → -1（4 位信号的 -1 是 0xF）");
    check(dbc::extendSign(0x8, 4) == -8, "4 位 0x8 → -8");
    check(dbc::extendSign(0x1, 1) == -1 && dbc::extendSign(0x0, 1) == 0,
          "1 位信号：1 → -1，0 → 0");
    check(dbc::extendSign(0xFFFFFFFFFFFFFFFFull, 64) == -1, "64 位全 1 → -1");

    check(dbc::packRaw(-1, 12) == 0xFFFull, "packRaw(-1,12) → 0xFFF");
    check(dbc::packRaw(-2048, 12) == 0x800ull, "packRaw(-2048,12) → 0x800");
    check(dbc::packRaw(0xABC, 12) == 0xABCull, "正数原样通过");
    check(dbc::packRaw(-1, 64) == 0xFFFFFFFFFFFFFFFFull, "64 位 -1 压回全 1");
}

// ---------------------------------------------------------------------------
// 2) 位宽范围（和 DBC 的量程无关）
// ---------------------------------------------------------------------------
void testRawRange() {
    std::printf("\n== 2. 位宽能表示的范围 ==\n");

    std::int64_t lo = 0, hi = 0;

    dbc::rawRange(mk(0, 12, dbc::ByteOrder::Intel, false, 1, 0), &lo, &hi);
    check(lo == 0 && hi == 4095, "12 位无符号 → [0, 4095]");

    dbc::rawRange(mk(0, 12, dbc::ByteOrder::Intel, true, 1, 0), &lo, &hi);
    check(lo == -2048 && hi == 2047, "12 位有符号 → [-2048, 2047]");

    dbc::rawRange(mk(0, 8, dbc::ByteOrder::Intel, true, 1, 0), &lo, &hi);
    check(lo == -128 && hi == 127, "8 位有符号 → [-128, 127]");

    dbc::rawRange(mk(0, 16, dbc::ByteOrder::Intel, false, 1, 0), &lo, &hi);
    check(lo == 0 && hi == 65535, "16 位无符号 → [0, 65535]");

    dbc::rawRange(mk(0, 16, dbc::ByteOrder::Intel, true, 1, 0), &lo, &hi);
    check(lo == -32768 && hi == 32767, "16 位有符号 → [-32768, 32767]");
}

// ---------------------------------------------------------------------------
// 3) raw → 物理值
// ---------------------------------------------------------------------------
void testRawToPhysical() {
    std::printf("\n== 3. raw → 物理值 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    check(pr.ok, "样例 DBC 解析成功");
    const dbc::Database& db = pr.db;

    const dbc::Signal* spd = sig(db, 0x100, "EngineSpeed");
    const dbc::Signal* tmp = sig(db, 0x100, "EngineTemp");
    const dbc::Signal* brk = sig(db, 0x100, "BrakePress");
    const dbc::Signal* thr = sig(db, 0x100, "ThrottlePos");
    const dbc::Signal* st  = sig(db, 0x100, "SteeringAngle");
    check(spd && tmp && brk && thr && st, "5 个被测信号都拿到了");
    if (!(spd && tmp && brk && thr && st)) return;

    check(near(dbc::intToPhysical(*spd, 100), 12.5, 1e-12), "EngineSpeed raw=100 → 12.5 rpm");
    check(near(dbc::intToPhysical(*spd, 4660), 582.5, 1e-9), "EngineSpeed raw=4660 → 582.5 rpm");
    check(near(dbc::intToPhysical(*tmp, 0), -40.0, 1e-12),
          "EngineTemp raw=0 → -40 degC（offset 是零点偏移，不是加在 factor 后面）");
    check(near(dbc::intToPhysical(*tmp, 215), 175.0, 1e-12), "EngineTemp raw=215 → 175 degC");
    check(near(dbc::intToPhysical(*st, -1), -0.1, 1e-12), "SteeringAngle raw=-1 → -0.1 deg");
    check(near(dbc::intToPhysical(*brk, 2748), 274.8, 1e-9), "BrakePress raw=2748 → 274.8 bar");
    check(near(dbc::intToPhysical(*thr, 255), 100.0, 1e-6),
          "ThrottlePos raw=255 → 100%（factor 是截断的十进制，差在小数第 9 位，容差 1e-6）");
}

// ---------------------------------------------------------------------------
// 4) 物理值 → raw（含四舍五入方向和拒绝条件）
// ---------------------------------------------------------------------------
void testPhysicalToRaw() {
    std::printf("\n== 4. 物理值 → raw ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;
    const dbc::Signal* spd = sig(db, 0x100, "EngineSpeed");
    const dbc::Signal* brk = sig(db, 0x100, "BrakePress");
    const dbc::Signal* st  = sig(db, 0x100, "SteeringAngle");
    const dbc::Signal* tmp = sig(db, 0x100, "EngineTemp");
    if (!(spd && brk && st && tmp)) { check(false, "信号没拿到，后面跳过"); return; }

    std::int64_t raw = 0;
    std::string err;

    check(dbc::physicalToInt(*spd, 12.5, &raw, &err) && raw == 100, "12.5 rpm → raw 100");
    check(dbc::physicalToInt(*brk, 274.8, &raw, &err) && raw == 2748, "274.8 bar → raw 2748");
    check(dbc::physicalToInt(*st, -0.1, &raw, &err) && raw == -1, "-0.1 deg → raw -1（负数）");
    check(dbc::physicalToInt(*tmp, -40.0, &raw, &err) && raw == 0, "-40 degC → raw 0");

    check(dbc::physicalToInt(*brk, 0.05, &raw, &err) && raw == 1,
          "0.05 bar → 0.5 个 LSB → 舍成 1（.5 远离 0，这个方向是写死的）");
    check(dbc::physicalToInt(*brk, 0.04, &raw, &err) && raw == 0, "0.04 bar → 0.4 个 LSB → 舍成 0");

    check(!dbc::physicalToInt(*brk, 500.0, &raw, &err) && contains(err, "超出"),
          "500 bar 需要 raw 5000，12 位装不下 → 拒绝，并说“超出”");
    check(!dbc::physicalToInt(*brk, -1.0, &raw, &err),
          "-1 bar 需要 raw -10，无符号装不下 → 拒绝");
    check(!dbc::physicalToInt(*st, -4000.0, &raw, &err),
          "-4000 deg 需要 raw -40000，16 位有符号装不下 → 拒绝");

    dbc::Signal zero = mk(0, 8, dbc::ByteOrder::Intel, false, 0.0, 0.0);
    check(!dbc::physicalToInt(zero, 1.0, &raw, &err) && contains(err, "factor"),
          "factor 为 0 直接拒绝（除零），并点明是 factor 的问题");
}

// ---------------------------------------------------------------------------
// 5) 真帧解码
// ---------------------------------------------------------------------------
void testDecodeFrame() {
    std::printf("\n== 5. 一帧 8 字节解出 6 个信号 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;
    const dbc::Message* m = db.find(0x100, false);
    check(m != nullptr, "找到 0x100 报文");
    if (m == nullptr) return;

    const dbc::Signal* spd = m->find("EngineSpeed");
    const dbc::Signal* tmp = m->find("EngineTemp");
    const dbc::Signal* thr = m->find("ThrottlePos");
    const dbc::Signal* brk = m->find("BrakePress");
    const dbc::Signal* gr  = m->find("GearPosition");
    const dbc::Signal* st  = m->find("SteeringAngle");
    if (!(spd && tmp && thr && brk && gr && st)) { check(false, "6 个信号没拿全"); return; }

    dbc::DecodeResult r1 = dbc::decodeSignal(*spd, kFrame);
    check(r1.ok && r1.raw == 100 && near(r1.value, 12.5, 1e-9),
          "EngineSpeed：raw 100，物理 12.5 rpm");

    dbc::DecodeResult r2 = dbc::decodeSignal(*tmp, kFrame);
    check(r2.ok && r2.raw == 100 && near(r2.value, 60.0, 1e-9),
          "EngineTemp：raw 100，物理 60 degC（100 - 40）");

    dbc::DecodeResult r3 = dbc::decodeSignal(*thr, kFrame);
    check(r3.ok && r3.raw == 0 && near(r3.value, 0.0, 1e-9), "ThrottlePos：raw 0，物理 0%");

    dbc::DecodeResult r4 = dbc::decodeSignal(*brk, kFrame);
    check(r4.ok && r4.raw == 0xABC && near(r4.value, 274.8, 1e-9),
          "BrakePress：raw 0xABC，物理 274.8 bar（Motorola 跨字节）");

    dbc::DecodeResult r5 = dbc::decodeSignal(*gr, kFrame);
    check(r5.ok && r5.raw == 3, "GearPosition：raw 3");
    check(dbc::valueTextFor(*gr, r5.rawSigned) != nullptr &&
              *dbc::valueTextFor(*gr, r5.rawSigned) == "D",
          "GearPosition raw 3 → 值表文字 \"D\"");

    dbc::DecodeResult r6 = dbc::decodeSignal(*st, kFrame);
    check(r6.ok && r6.raw == 0xFFFF && r6.rawSigned == -1,
          "SteeringAngle：位模式 0xFFFF，解释成 -1");
    check(near(r6.value, -0.1, 1e-9), "SteeringAngle 物理 -0.1 deg（不是 6553.5）");

    check(r1.inEngineeringRange && r2.inEngineeringRange && r3.inEngineeringRange &&
              r4.inEngineeringRange && r5.inEngineeringRange && r6.inEngineeringRange,
          "6 个信号的物理值全部落在 DBC 声明的量程内");
}

// ---------------------------------------------------------------------------
// 6) 超量程 —— 装得下但不合理
// ---------------------------------------------------------------------------
void testRangeCheck() {
    std::printf("\n== 6. 装得下 ≠ 合理 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;
    const dbc::Signal* brk = sig(db, 0x100, "BrakePress");
    const dbc::Signal* thr = sig(db, 0x100, "ThrottlePos");
    if (!(brk && thr)) { check(false, "信号没拿到"); return; }

    std::uint8_t bad[8] = {0x64, 0x00, 0x64, 0x00, 0xFF, 0xF0, 0xFF, 0xFF};
    dbc::DecodeResult r = dbc::decodeSignal(*brk, bad);
    check(r.ok && r.raw == 0xFFF, "12 位全 1 → raw 0xFFF，位宽完全装得下");
    check(near(r.value, 409.5, 1e-9), "物理 409.5 bar");
    check(!r.inEngineeringRange, "但超出 DBC 量程 [0..400] → 标记为不合理（这是传感器坏了，不是数据坏了）");

    const dbc::Signal* spd = sig(db, 0x100, "EngineSpeed");
    dbc::DecodeResult rs = dbc::decodeSignal(*spd, bad);
    check(rs.inEngineeringRange, "同一帧里的 EngineSpeed 不受影响，仍在量程内");

    dbc::DecodeResult rt = dbc::decodeSignal(*thr, bad);   // 这里 byte3 = 0x00
    check(rt.inEngineeringRange, "ThrottlePos 也不会被别人的超量程带偏");

    dbc::Signal noRange = mk(0, 8, dbc::ByteOrder::Intel, false, 1, 0, 0, 0);
    dbc::DecodeResult rn = dbc::decodeSignal(noRange, bad);
    check(rn.ok && !rn.rangeDeclared && rn.inEngineeringRange,
          "DBC 里 [0|0] 视为“没写量程”，不当作“只能取 0”");

    std::uint8_t full[8] = {0, 0, 0, 0xFF, 0, 0, 0, 0};
    dbc::DecodeResult rf = dbc::decodeSignal(*thr, full);
    check(rf.inEngineeringRange,
          "ThrottlePos raw=255 → 99.99999999%，容差之内算在量程内（不能被浮点误差误判）");
}

// ---------------------------------------------------------------------------
// 7) 值表
// ---------------------------------------------------------------------------
void testValueTable() {
    std::printf("\n== 7. 值表 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;
    const dbc::Signal* gr = sig(db, 0x100, "GearPosition");
    const dbc::Signal* flt = sig(db, 0x200, "AbsFaultCode");
    const dbc::Signal* spd = sig(db, 0x100, "EngineSpeed");
    if (!(gr && flt && spd)) { check(false, "信号没拿到"); return; }

    const std::string* t0 = dbc::valueTextFor(*gr, 0);
    const std::string* t3 = dbc::valueTextFor(*gr, 3);
    check(t0 != nullptr && *t0 == "P", "GearPosition 0 → P");
    check(t3 != nullptr && *t3 == "D", "GearPosition 3 → D");
    check(dbc::valueTextFor(*gr, 5) == nullptr, "GearPosition 5 值表里没有 → nullptr（不能瞎编文字）");

    const std::string* tn = dbc::valueTextFor(*flt, 255);
    check(tn != nullptr && *tn == "Unknown", "AbsFaultCode 255 → Unknown");

    check(dbc::valueTextFor(*spd, 100) == nullptr, "没有值表的信号 → nullptr");
}

// ---------------------------------------------------------------------------
// 8) 编码：全写或不写，不碰别人的位
// ---------------------------------------------------------------------------
void testEncodeSignal() {
    std::printf("\n== 8. 编码 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;
    const dbc::Signal* spd = sig(db, 0x100, "EngineSpeed");
    const dbc::Signal* st  = sig(db, 0x100, "SteeringAngle");
    const dbc::Signal* brk = sig(db, 0x100, "BrakePress");
    const dbc::Signal* gr  = sig(db, 0x100, "GearPosition");
    if (!(spd && st && brk && gr)) { check(false, "信号没拿到"); return; }

    {
        std::uint8_t f[8] = {0};
        dbc::EncodeResult e = dbc::encodeSignal(*spd, 12.5, f);
        check(e.ok && e.raw == 100, "写 12.5 rpm → raw 100");
        check(dbc::decodeSignal(*spd, f).raw == 100, "读回来还是 100（往返成立）");
    }
    {
        std::uint8_t f[8] = {0};
        dbc::EncodeResult e = dbc::encodeSignal(*st, -0.1, f);
        check(e.ok && e.raw == 0xFFFF, "写 -0.1 deg → 位模式 0xFFFF");
        check(near(dbc::decodeSignal(*st, f).value, -0.1, 1e-9), "读回来 -0.1 deg");
    }
    {
        std::uint8_t f[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        dbc::EncodeResult e = dbc::encodeSignal(*gr, 0.0, f);
        check(e.ok && f[5] == 0xF0,
              "在满 0xFF 的脏帧上写 Gear=0，只清掉 byte5 低 4 位，高 4 位（BrakePress 的）原样留着");
        check(f[0] == 0xFF && f[7] == 0xFF, "相邻字节一个都没动");
    }
    {
        std::uint8_t f[8] = {0};
        dbc::EncodeResult e = dbc::encodeSignal(*brk, 500.0, f);
        check(!e.ok && contains(e.error, "超出"), "500 bar 不饱和 → 拒绝，报“超出”");
        bool untouched = true;
        for (int i = 0; i < 8; ++i) untouched = untouched && f[i] == 0;
        check(untouched, "被拒的写入连一个字节都没动（不会留半写状态）");
    }
    {
        std::uint8_t f[8] = {0};
        dbc::EncodeResult e = dbc::encodeSignal(*brk, 500.0, f, /*saturate=*/true);
        check(e.ok && e.clamped && e.raw == 4095, "饱和模式下 500 bar 夹到 4095");
        check(near(dbc::decodeSignal(*brk, f).value, 409.5, 1e-9),
              "夹住之后物理值是 409.5 bar（超出量程，但至少是合法帧）");
    }
    {
        std::uint8_t f[8] = {0};
        dbc::EncodeResult e = dbc::encodeSignal(*st, -4000.0, f, /*saturate=*/true);
        check(e.ok && e.clamped && e.raw == 0x8000,
              "SteeringAngle 饱和到 -32768（位模式 0x8000）");
        check(near(dbc::decodeSignal(*st, f).value, -3276.8, 1e-9), "夹住之后是 -3276.8 deg");
    }
}

// ---------------------------------------------------------------------------
// 9) 全库往返 —— 14 个信号，每个取 3 个值
// ---------------------------------------------------------------------------
void testRoundTripAll() {
    std::printf("\n== 9. 全库往返（14 个信号 × 3 个值）==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    const dbc::Database& db = pr.db;

    int signals = 0;
    int trips = 0;
    bool allOk = true;
    std::string firstBad;

    for (const auto& m : db.messages) {
        for (const auto& s : m.signals) {
            ++signals;

            const double vmin = s.minimum;
            const double vmax = s.maximum;
            const double vmid = (vmin + vmax) / 2.0;
            const double vals[3] = { vmin, vmid, vmax };

            const double tol = std::fabs(s.factor) / 2.0 + 1e-6 * std::fmax(1.0, std::fabs(vmax));

            for (int i = 0; i < 3; ++i) {
                std::uint8_t f[8] = {0};
                dbc::EncodeResult e = dbc::encodeSignal(s, vals[i], f, /*saturate=*/true);
                if (!e.ok) {
                    allOk = false;
                    if (firstBad.empty()) firstBad = s.name + "：写不进去（" + e.error + "）";
                    continue;
                }
                dbc::DecodeResult d = dbc::decodeSignal(s, f);
                if (!d.ok || !near(d.value, vals[i], tol)) {
                    allOk = false;
                    if (firstBad.empty()) {
                        firstBad = s.name + "：写 " + std::to_string(vals[i]) + " 读回 " +
                                   std::to_string(d.value);
                    }
                    continue;
                }
                ++trips;
            }
        }
    }

    check(signals == 14, "遍历到 14 个信号");
    check(trips == 42, "42 次往返全部走通（每个信号 min / 中值 / max）");
    if (!allOk) std::printf("        首个失败：%s\n", firstBad.c_str());
}

} // namespace

int main() {
    std::printf("M2 编解码自测\n");

    testSign();
    testRawRange();
    testRawToPhysical();
    testPhysicalToRaw();
    testDecodeFrame();
    testRangeCheck();
    testValueTable();
    testEncodeSignal();
    testRoundTripAll();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) {
        std::printf("M2 全绿 —— 数字变成人话，才谈得上选哪一支信号。\n");
    }
    return gFail == 0 ? 0 : 1;
}
