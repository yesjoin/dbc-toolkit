// ============================================================================
// gen_selftest.cpp —— M4 验收：生成的头文件和本工程解码器必须**逐字节一致**
//
// 这是差分测试（differential test）：两种实现（生成的代码 / 库里的运行时）
// 处理同一份字节，结果必须完全相同。它们走的是同一套规则，但代码是分别写的，
// 所以一方有 bug 才能被抓出来。
//
// 检查四件事：
//   ① 元数据（ID / dlc / 扩展标志 / 信号数）生成得对不对
//   ② 同一帧随机字节，两边解出来的 raw 是否一致
//   ③ 同一组 raw，两边编出来的 8 个字节是否一致
//   ④ 物理换算 和 复用分支判定 是否一致
// ============================================================================

#include "sample_dbc.h"        // ← 生成物，由 CMake 在构建时产到 build/gen/

#include "Codec.h"
#include "Dbc.h"
#include "Mux.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 线性同余：固定的种子 → 每次都跑出同一串字节，失败可复现
std::uint32_t gSeed = 0x2026u;
std::uint32_t rnd() {
    gSeed = gSeed * 1103515245u + 12345u;
    return gSeed >> 8;
}
void randomFrame(std::uint8_t* f) {
    for (int i = 0; i < 8; ++i) f[i] = static_cast<std::uint8_t>(rnd() & 0xFFu);
}

// 单条对照：生成侧给的 raw vs 库侧对这个字节段解出的 rawSigned
template <typename T>
bool sameSignal(const dbc::Message* m, const char* name, T genRaw, const std::uint8_t* f) {
    const dbc::Signal* s = m->find(name);
    if (s == nullptr) {
        std::printf("        ! DBC 里找不到 %s\n", name);
        return false;
    }
    dbc::DecodeResult r = dbc::decodeSignal(*s, f);
    if (!r.ok) {
        std::printf("        ! 库侧解码失败 %s：%s\n", name, r.error.c_str());
        return false;
    }
    const long long gen = static_cast<long long>(genRaw);
    if (gen != static_cast<long long>(r.rawSigned)) {
        std::printf("        ! %s：生成=%lld 库=%lld\n", name, gen,
                    static_cast<long long>(r.rawSigned));
        return false;
    }
    return true;
}

// 库侧按 DBC 顺序把一串 raw 写进 data（只写自己的位）
bool libEncode(const dbc::Message* m, const unsigned long long* raws, std::uint8_t* data) {
    std::memset(data, 0, 8);
    for (std::size_t i = 0; i < m->signals.size(); ++i) {
        dbc::BitLayout lay = dbc::layoutOf(m->signals[i]);
        if (!lay.ok) return false;
        dbc::encodeRaw(lay, raws[i], data);
    }
    return true;
}

// ---------------------------------------------------------------------------
// ① 元数据
// ---------------------------------------------------------------------------
void testMeta(const dbc::Database& db) {
    std::printf("\n== 1. 元数据 ==\n");

    const dbc::Message* m1 = db.find(0x100, false);
    const dbc::Message* m2 = db.find(0x200, false);
    const dbc::Message* m3 = db.find(0x300, false);
    const dbc::Message* m4 = db.find(0x1000, true);
    check(m1 && m2 && m3 && m4, "DBC 里 4 个报文都在");
    if (!(m1 && m2 && m3 && m4)) return;

    check(EngineData::ID == 0x100u && !EngineData::EXTENDED && EngineData::DLC == 8u &&
              EngineData::SIGNAL_COUNT == 6u,
          "EngineData：ID=0x100、标准帧、dlc=8、6 个信号");
    check(AbsStatus::ID == 0x200u && AbsStatus::SIGNAL_COUNT == 4u,
          "AbsStatus：ID=0x200、4 个信号");
    check(MuxFrame::ID == 0x300u && MuxFrame::SIGNAL_COUNT == 3u,
          "MuxFrame：ID=0x300、3 个信号");
    check(ExtDiag::ID == 0x1000u && ExtDiag::EXTENDED && ExtDiag::SIGNAL_COUNT == 1u,
          "ExtDiag：ID=0x1000、扩展帧");

    check(EngineData::SIGNALS[0].name == std::string("EngineSpeed") &&
              EngineData::SIGNALS[1].name == std::string("EngineTemp"),
          "描述表里的信号顺序与 DBC 一致（索引也要对得上）");
    check(MuxFrame::SIGNALS[1].branch == 1 && MuxFrame::SIGNALS[2].branch == 2,
          "分支号被写进了描述表：MuxA=1、MuxB=2");

    // static_assert 已经在编译期跑过一遍；这里再在运行期验一次，证明那条 constexpr 逻辑真的在算
    check(::dbc_gen::detail::layoutOk(EngineData::SIGNALS, EngineData::SIGNAL_COUNT),
          "EngineData 布局检查（运行期复核）通过");
    check(::dbc_gen::detail::layoutOk(MuxFrame::SIGNALS, MuxFrame::SIGNAL_COUNT),
          "MuxFrame 布局检查通过（m1/m2 同址不算冲突）");
}

// ---------------------------------------------------------------------------
// ② 解码对照（10 帧随机字节）
// ---------------------------------------------------------------------------
void testDecodeDiff(const dbc::Database& db) {
    std::printf("\n== 2. 解码对照（随机字节） ==\n");

    const dbc::Message* m1 = db.find(0x100, false);
    const dbc::Message* m2 = db.find(0x200, false);
    const dbc::Message* m3 = db.find(0x300, false);
    const dbc::Message* m4 = db.find(0x1000, true);
    if (!(m1 && m2 && m3 && m4)) { check(false, "报文没找全"); return; }

    const int rounds = 10;
    bool ok1 = true, ok2 = true, ok3 = true, ok4 = true;

    for (int r = 0; r < rounds; ++r) {
        std::uint8_t f[8];
        randomFrame(f);

        {
            EngineData::Value v{};
            EngineData::decode(f, v);
            ok1 = ok1 && sameSignal(m1, "EngineSpeed", v.EngineSpeed, f);
            ok1 = ok1 && sameSignal(m1, "EngineTemp", v.EngineTemp, f);
            ok1 = ok1 && sameSignal(m1, "ThrottlePos", v.ThrottlePos, f);
            ok1 = ok1 && sameSignal(m1, "BrakePress", v.BrakePress, f);
            ok1 = ok1 && sameSignal(m1, "GearPosition", v.GearPosition, f);
            ok1 = ok1 && sameSignal(m1, "SteeringAngle", v.SteeringAngle, f);
        }
        {
            AbsStatus::Value v{};
            AbsStatus::decode(f, v);
            ok2 = ok2 && sameSignal(m2, "WheelSpeedFL", v.WheelSpeedFL, f);
            ok2 = ok2 && sameSignal(m2, "WheelSpeedFR", v.WheelSpeedFR, f);
            ok2 = ok2 && sameSignal(m2, "AbsLamp", v.AbsLamp, f);
            ok2 = ok2 && sameSignal(m2, "AbsFaultCode", v.AbsFaultCode, f);
        }
        {
            MuxFrame::Value v{};
            MuxFrame::decode(f, v);
            ok3 = ok3 && sameSignal(m3, "MuxSel", v.MuxSel, f);
            ok3 = ok3 && sameSignal(m3, "MuxA", v.MuxA, f);
            ok3 = ok3 && sameSignal(m3, "MuxB", v.MuxB, f);
        }
        {
            ExtDiag::Value v{};
            ExtDiag::decode(f, v);
            ok4 = ok4 && sameSignal(m4, "ExtCounter", v.ExtCounter, f);
        }
    }

    check(ok1, "EngineData：10 帧 × 6 信号，生成代码与库解出的 raw 完全一致");
    check(ok2, "AbsStatus：10 帧 × 4 信号一致");
    check(ok3, "MuxFrame：10 帧 × 3 信号一致");
    check(ok4, "ExtDiag：10 帧 × 1 信号一致");
}

// ---------------------------------------------------------------------------
// ③ 编码对照
// ---------------------------------------------------------------------------
void testEncodeDiff(const dbc::Database& db) {
    std::printf("\n== 3. 编码对照 ==\n");

    const dbc::Message* m1 = db.find(0x100, false);
    const dbc::Message* m3 = db.find(0x300, false);
    if (!(m1 && m3)) { check(false, "报文没找全"); return; }

    bool ok1 = true;
    bool ok3 = true;

    for (int r = 0; r < 5; ++r) {
        {
            EngineData::Value v{};
            v.EngineSpeed    = static_cast<std::uint16_t>(rnd());
            v.EngineTemp     = static_cast<std::uint8_t>(rnd());
            v.ThrottlePos    = static_cast<std::uint8_t>(rnd());
            v.BrakePress     = static_cast<std::uint16_t>(rnd() & 0x0FFFu);
            v.GearPosition   = static_cast<std::uint8_t>(rnd() & 0x0Fu);
            v.SteeringAngle  = static_cast<std::int16_t>(static_cast<std::int16_t>(rnd()));

            std::uint8_t a[8] = {0};
            EngineData::encode(v, a);

            const unsigned long long raws[6] = {
                v.EngineSpeed, v.EngineTemp, v.ThrottlePos,
                v.BrakePress, v.GearPosition,
                static_cast<unsigned long long>(static_cast<std::uint16_t>(v.SteeringAngle))
            };
            std::uint8_t b[8] = {0};
            if (!libEncode(m1, raws, b)) { ok1 = false; continue; }

            if (std::memcmp(a, b, 8) != 0) {
                ok1 = false;
                std::printf("        ! EngineData 编码不一致\n");
                for (int i = 0; i < 8; ++i) std::printf("          生成 %02X / 库 %02X\n", a[i], b[i]);
            }
        }
        {
            MuxFrame::Value v{};
            v.MuxSel = static_cast<std::uint8_t>(rnd());
            v.MuxA   = static_cast<std::uint16_t>(rnd());
            v.MuxB   = static_cast<std::uint16_t>(rnd());

            std::uint8_t a[8] = {0};
            MuxFrame::encode(v, a);

            const unsigned long long raws[3] = { v.MuxSel, v.MuxA, v.MuxB };
            std::uint8_t b[8] = {0};
            if (!libEncode(m3, raws, b)) { ok3 = false; continue; }
            if (std::memcmp(a, b, 8) != 0) ok3 = false;
        }
    }

    check(ok1, "EngineData：5 组随机值，两边编出的 8 字节完全相同");
    check(ok3, "MuxFrame：5 组随机值（含两支同址覆盖），两边编出的 8 字节相同");
}

// ---------------------------------------------------------------------------
// ④ 物理换算 与 复用判定
// ---------------------------------------------------------------------------
void testPhysicalAndMux(const dbc::Database& db) {
    std::printf("\n== 4. 物理换算与复用判定 ==\n");

    check(near(EngineData::EngineSpeed_toPhysical(100), 12.5, 1e-9),
          "EngineSpeed_toPhysical(100) = 12.5 rpm");
    check(near(EngineData::EngineTemp_toPhysical(0), -40.0, 1e-12),
          "EngineTemp_toPhysical(0) = -40 degC");
    check(near(EngineData::ThrottlePos_toPhysical(255), 99.99999999, 1e-6),
          "ThrottlePos_toPhysical(255) ≈ 100%（factor 是截断的十进制）");
    check(near(EngineData::BrakePress_toPhysical(2748), 274.8, 1e-9),
          "BrakePress_toPhysical(2748) = 274.8 bar");
    check(near(EngineData::SteeringAngle_toPhysical(-1), -0.1, 1e-12),
          "SteeringAngle_toPhysical(-1) = -0.1 deg（有符号字段）");
    check(near(AbsStatus::WheelSpeedFL_toPhysical(1000), 10.0, 1e-9),
          "WheelSpeedFL_toPhysical(1000) = 10 km/h");

    {
        std::uint16_t raw16 = 0;
        std::uint8_t  raw8 = 0;
        check(EngineData::EngineSpeed_fromPhysical(12.5, &raw16) && raw16 == 100,
              "EngineSpeed_fromPhysical(12.5) → raw 100");
        check(!EngineData::BrakePress_fromPhysical(500.0, &raw16),
              "BrakePress_fromPhysical(500) 装不下 → false");
        check(EngineData::BrakePress_fromPhysical(500.0, &raw16, true) && raw16 == 4095,
              "同一件事，saturate=true 时夹到 4095");
        check(!AbsStatus::AbsLamp_fromPhysical(70.0, &raw8),
              "AbsLamp（1 位）装不下 70 → false");
        check(EngineData::EngineTemp_fromPhysical(-40.0, &raw8) && raw8 == 0,
              "EngineTemp_fromPhysical(-40) → raw 0（offset 参与反算）");
    }

    {
        const dbc::Message* m3 = db.find(0x300, false);
        check(m3 != nullptr, "找到 MuxFrame");
        if (m3 != nullptr) {
            std::uint8_t f1[8] = {0x01, 0x64, 0x00, 0, 0, 0, 0, 0};
            MuxFrame::Value v1{};
            MuxFrame::decode(f1, v1);
            check(MuxFrame::MuxA_active(v1) && !MuxFrame::MuxB_active(v1),
                  "开关=1：MuxA_active 真、MuxB_active 假");

            std::uint8_t f2[8] = {0x02, 0xC8, 0x00, 0, 0, 0, 0, 0};
            MuxFrame::Value v2{};
            MuxFrame::decode(f2, v2);
            check(!MuxFrame::MuxA_active(v2) && MuxFrame::MuxB_active(v2),
                  "开关=2：反过来");
            check(near(MuxFrame::MuxA_toPhysical(v1.MuxA), 10.0, 1e-9) &&
                      near(MuxFrame::MuxB_toPhysical(v2.MuxB), 20.0, 1e-9),
                  "两支各自的物理值都对（10.0 / 20.0）");
        }
    }
}

// ---------------------------------------------------------------------------
// ⑤ 用 M3 的真帧再核一遍：生成代码的 active 判定必须和库的路由一致
// ---------------------------------------------------------------------------
void testSameRouting(const dbc::Database& db) {
    std::printf("\n== 5. 和库的路由结果对齐 ==\n");

    const dbc::Message* m3 = db.find(0x300, false);
    if (m3 == nullptr) { check(false, "没找到 MuxFrame"); return; }

    bool allOk = true;
    for (int r = 0; r < 6; ++r) {
        std::uint8_t f[8];
        randomFrame(f);
        f[0] = static_cast<std::uint8_t>(r % 4);      // 开关值 0..3 轮着来

        dbc::DecodedMessage dm = dbc::decodeMessage(*m3, f);
        MuxFrame::Value v{};
        MuxFrame::decode(f, v);

        const bool libA = [&] {
            for (const auto& sv : dm.signals) if (sv.name == "MuxA") return sv.active;
            return false;
        }();
        const bool libB = [&] {
            for (const auto& sv : dm.signals) if (sv.name == "MuxB") return sv.active;
            return false;
        }();

        if (MuxFrame::MuxA_active(v) != libA) allOk = false;
        if (MuxFrame::MuxB_active(v) != libB) allOk = false;
    }
    check(allOk, "6 帧（开关 0/1/2/3）× 两支：生成代码的 active 判定与库完全一致");
}

} // namespace

int main() {
    std::printf("M4 代码生成自测（差分：生成代码 vs 运行时库）\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    if (!pr.ok) {
        std::printf("[FAIL] DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }
    const dbc::Database& db = pr.db;

    testMeta(db);
    testDecodeDiff(db);
    testEncodeDiff(db);
    testPhysicalAndMux(db);
    testSameRouting(db);

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) {
        std::printf("M4 全绿 —— DBC 直接编译成代码，业务不用再手抄字段。\n");
    }
    return gFail == 0 ? 0 : 1;
}
