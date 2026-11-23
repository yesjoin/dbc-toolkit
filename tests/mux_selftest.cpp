// ============================================================================
// mux_selftest.cpp —— M3 验收夹具
//
// 核心验收点不是"能选出分支"，而是**未选中的分支必须被判为"不存在"**。
// 所以下面每一条路由用例都同时检查三项：
//   ① 开关值读对了没
//   ② 该 active 的 active 了吗
//   ③ 不该 active 的，是不是老老实实 active == false
// ============================================================================

#include "Mux.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
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

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

const dbc::SignalValue* findValue(const dbc::DecodedMessage& dm, const char* name) {
    for (const auto& sv : dm.signals) {
        if (sv.name == name) return &sv;
    }
    return nullptr;
}

dbc::Signal mkSignal(const char* name, std::uint32_t start, std::uint32_t len,
                     const char* mux) {
    dbc::Signal s;
    s.name = name;
    s.startBit = start;
    s.length = len;
    s.order = dbc::ByteOrder::Intel;
    s.factor = 1;
    s.offset = 0;
    s.mux = mux;
    return s;
}

// ---------------------------------------------------------------------------
// 1) M / mN 的语法识别
// ---------------------------------------------------------------------------
void testSyntax() {
    std::printf("\n== 1. 开关与分支的写法 ==\n");

    check(dbc::isBranchMux("m1"), "\"m1\" 是分支");
    check(!dbc::isBranchMux("M"), "\"M\" 是开关，不是分支（只差一个大小写）");
    check(!dbc::isBranchMux(""), "空串不是分支（普通信号）");
    check(!dbc::isBranchMux("m"), "\"m\" 后面没数字，不是分支");
    check(!dbc::isBranchMux("mx"), "\"mx\" 不是分支");
    check(!dbc::isBranchMux("MuxA"), "信号名 \"MuxA\" 不是分支标记");
    check(dbc::branchNumber("m1") == 1 && dbc::branchNumber("m10") == 10,
          "分支号解析：m1→1，m10→10");
    check(dbc::branchNumber("M") == -1 && dbc::branchNumber("") == -1,
          "不是分支的一律返回 -1");
}

// ---------------------------------------------------------------------------
// 2) 报文结构分析
// ---------------------------------------------------------------------------
void testAnalyze(const dbc::Database& db) {
    std::printf("\n== 2. 报文结构 ==\n");

    const dbc::Message* muxFrame = db.find(0x300, false);
    check(muxFrame != nullptr, "找到 0x300 MuxFrame");
    if (muxFrame == nullptr) return;

    dbc::MuxInfo info = dbc::analyzeMux(*muxFrame);
    check(info.ok, "MuxFrame 结构合法");
    check(info.selector != nullptr && info.selector->name == "MuxSel",
          "找到开关信号 MuxSel");
    check(info.maxBranch == 2, "最大分支号是 2");
    check(static_cast<int>(info.branches.size()) == 2 && info.branches[0] == 1 && info.branches[1] == 2,
          "分支列表 = [1, 2]（排序去重后）");
    check(info.warnings.empty(), "没有任何告警");

    // 有分支没开关 → 死信号
    dbc::Message bad1;
    bad1.name = "NoSelector";
    bad1.signals = { mkSignal("A", 0, 8, "m1"), mkSignal("B", 8, 8, "m2") };
    dbc::MuxInfo i1 = dbc::analyzeMux(bad1);
    check(!i1.ok && contains(i1.error, "M 开关"),
          "有 m1/m2 却没 M 开关 → 报错（分支永远选不中）");

    // 两个开关 → 不支持
    dbc::Message bad2;
    bad2.name = "TwoSelectors";
    bad2.signals = { mkSignal("S1", 0, 8, "M"), mkSignal("S2", 8, 8, "M"),
                     mkSignal("A", 16, 8, "m1") };
    dbc::MuxInfo i2 = dbc::analyzeMux(bad2);
    check(!i2.ok && contains(i2.error, "扩展"),
          "两个 M 开关 → 明确拒绝，并点明这是扩展多路复用");

    // 分支号超出开关位宽
    dbc::Message bad3;
    bad3.name = "BranchTooBig";
    bad3.signals = { mkSignal("S", 0, 8, "M"), mkSignal("A", 8, 8, "m300") };
    dbc::MuxInfo i3 = dbc::analyzeMux(bad3);
    check(i3.ok && !i3.warnings.empty() && contains(i3.warnings[0], "选不中"),
          "8 位开关配 m300 → 告警（300 装不进 0..255）");

    dbc::Message bad4;
    bad4.name = "WeirdMux";
    bad4.signals = { mkSignal("S", 0, 8, "M"), mkSignal("A", 8, 8, "x1") };
    dbc::MuxInfo i4 = dbc::analyzeMux(bad4);
    check(!i4.warnings.empty() && contains(i4.warnings[0], "认不出来"),
          "认不出的复用标记 → 告警，不假装没看见");

    dbc::Message bad5;
    bad5.name = "ZeroBranch";
    bad5.signals = { mkSignal("S", 0, 8, "M"), mkSignal("A", 8, 8, "m0") };
    dbc::MuxInfo i5 = dbc::analyzeMux(bad5);
    check(!i5.warnings.empty() && contains(i5.warnings[0], "m0"),
          "m0 这种分支号 → 告警（分支号从 1 开始）");
}

// ---------------------------------------------------------------------------
// 3) 按开关值路由
// ---------------------------------------------------------------------------
void testRouting(const dbc::Database& db) {
    std::printf("\n== 3. 路由：哪一支有效 ==\n");

    const dbc::Message* m = db.find(0x300, false);
    if (m == nullptr) { check(false, "没找到 MuxFrame"); return; }

    // branch 1：byte0=1，byte1..2 = 100（MuxA 的 raw）
    std::uint8_t f1[8] = {0x01, 0x64, 0x00, 0, 0, 0, 0, 0};
    dbc::DecodedMessage d1 = dbc::decodeMessage(*m, f1);
    check(d1.ok && d1.hasMux, "MuxFrame 确实有多路复用");
    check(d1.selectorValue == 1, "开关值 = 1");
    {
        const dbc::SignalValue* a = findValue(d1, "MuxA");
        const dbc::SignalValue* b = findValue(d1, "MuxB");
        check(a && a->active, "MuxA（m1）被选中");
        check(b && !b->active, "MuxB（m2）未被选中 → active == false（值不存在，不是 0）");
        check(a && near(a->decoded.value, 10.0, 1e-9), "MuxA 物理值 10.0");
    }
    {
        const dbc::SignalValue* s = findValue(d1, "MuxSel");
        check(s && s->active, "开关自己永远有效（不属于任何分支）");
    }

    // branch 2：byte0=2，byte1..2 = 200
    std::uint8_t f2[8] = {0x02, 0xC8, 0x00, 0, 0, 0, 0, 0};
    dbc::DecodedMessage d2 = dbc::decodeMessage(*m, f2);
    {
        const dbc::SignalValue* a = findValue(d2, "MuxA");
        const dbc::SignalValue* b = findValue(d2, "MuxB");
        check(d2.selectorValue == 2 && a && !a->active && b && b->active,
              "开关值 2 → 换成 MuxB 有效、MuxA 无效");
        check(b && near(b->decoded.value, 20.0, 1e-9), "MuxB 物理值 20.0");
    }

    // branch 0 / 3：两支都不在
    {
        std::uint8_t f0[8] = {0x00, 0x64, 0x00, 0, 0, 0, 0, 0};
        dbc::DecodedMessage d0 = dbc::decodeMessage(*m, f0);
        const dbc::SignalValue* a = findValue(d0, "MuxA");
        const dbc::SignalValue* b = findValue(d0, "MuxB");
        check(a && b && !a->active && !b->active,
              "开关值 0 → 两支都无效（这块字节此刻不代表任何信号）");

        std::uint8_t f3[8] = {0x03, 0x64, 0x00, 0, 0, 0, 0, 0};
        dbc::DecodedMessage d3 = dbc::decodeMessage(*m, f3);
        const dbc::SignalValue* a3 = findValue(d3, "MuxA");
        const dbc::SignalValue* b3 = findValue(d3, "MuxB");
        check(a3 && b3 && !a3->active && !b3->active,
              "开关值 3（DBC 没定义这一支）→ 两支都无效");
    }
}

// ---------------------------------------------------------------------------
// 4) 无复用报文也要照常解
// ---------------------------------------------------------------------------
void testPlain(const dbc::Database& db) {
    std::printf("\n== 4. 无复用报文 ==\n");

    const dbc::Message* m = db.find(0x100, false);
    if (m == nullptr) { check(false, "没找到 EngineData"); return; }

    std::uint8_t f[8] = {0x64, 0x00, 0x64, 0x00, 0xAB, 0xC3, 0xFF, 0xFF};
    dbc::DecodedMessage dm = dbc::decodeMessage(*m, f);
    check(dm.ok && !dm.hasMux, "EngineData 没有多路复用");

    bool allActive = true;
    for (const auto& sv : dm.signals) {
        if (!sv.active) allActive = false;
    }
    check(allActive && static_cast<int>(dm.signals.size()) == 6, "6 个信号全部有效");

    const dbc::SignalValue* spd = findValue(dm, "EngineSpeed");
    check(spd && near(spd->decoded.value, 12.5, 1e-9),
          "decodeMessage 的数值与单信号 decode 一致（12.5 rpm）");
}

// ---------------------------------------------------------------------------
// 5) 编码：只能写自己那一支
// ---------------------------------------------------------------------------
void testEncode(const dbc::Database& db) {
    std::printf("\n== 5. 编码：只写当前分支 ==\n");

    const dbc::Message* m = db.find(0x300, false);
    if (m == nullptr) { check(false, "没找到 MuxFrame"); return; }

    {
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"MuxA", 10.0} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, 1, vals, f);
        check(e.ok, "分支 1 写 MuxA=10.0 成功");
        check(f[0] == 0x01, "开关字节被写成 1（不是 0）");
        check(f[1] == 0x64 && f[2] == 0x00, "MuxA 的 raw 100 落在 byte1..byte2");
        check(static_cast<int>(e.skipped.size()) == 1 && e.skipped[0] == "MuxB",
              "MuxB 被跳过（不属于分支 1，一个字节都没碰）");

        dbc::DecodedMessage d = dbc::decodeMessage(*m, f);
        const dbc::SignalValue* a = findValue(d, "MuxA");
        const dbc::SignalValue* b = findValue(d, "MuxB");
        check(d.selectorValue == 1 && a && a->active && b && !b->active,
              "写完之后读回来：分支 1 有效，MuxB 无效（自洽）");
    }
    {
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"MuxB", 20.0} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, 2, vals, f);
        check(e.ok && f[0] == 0x02, "分支 2 自动把开关写成 2");
        dbc::DecodedMessage d = dbc::decodeMessage(*m, f);
        const dbc::SignalValue* b = findValue(d, "MuxB");
        check(b && b->active && near(b->decoded.value, 20.0, 1e-9),
              "读回来 MuxB = 20.0");
    }
    {
        // 想让"分支 1"的帧里写 MuxB —— 必须拒绝写
        std::uint8_t f[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
        std::map<std::string, double> vals{ {"MuxB", 20.0} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, 1, vals, f);
        check(e.ok && f[1] == 0xBB && f[2] == 0xCC,
              "在分支 1 的帧里给 MuxB 赋值 → 被跳过，byte1..2 原样保留");
    }
    {
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"MuxA", 7000.0} };   // 16 位装不下
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, 1, vals, f);
        bool untouched = true;
        for (int i = 0; i < 8; ++i) untouched = untouched && f[i] == 0;
        check(!e.ok && contains(e.error, "MuxA") && untouched,
              "值非法 → 整帧不动（连开关都没写，原子性守住）");
    }
    {
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"MuxA", 10.0} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, -1, vals, f);
        check(!e.ok && contains(e.error, "分支号"),
              "复用报文不指定分支号 → 报错（不知道该写哪一支）");
    }
    {
        const dbc::Message* plain = db.find(0x100, false);
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"EngineSpeed", 12.5} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*plain, -1, vals, f);
        check(e.ok && f[0] == 0x64, "无复用报文：branch = -1 正常写入");
        dbc::EncodeMessageResult e2 = dbc::encodeMessage(*plain, 0, vals, f);
        check(!e2.ok, "无复用报文指定分支号 → 报错（多此一举就是错）");
    }
    {
        // 写一个 DBC 里不存在的分支
        std::uint8_t f[8] = {0};
        std::map<std::string, double> vals{ {"MuxA", 10.0} };
        dbc::EncodeMessageResult e = dbc::encodeMessage(*m, 5, vals, f);
        check(e.ok && f[0] == 0x05 && static_cast<int>(e.skipped.size()) == 2,
              "写未定义的分支 5：开关照写 5，两支信号全跳过（不猜）");
    }
}

// ---------------------------------------------------------------------------
// 6) 只按"此刻有效"的信号查抢位
// ---------------------------------------------------------------------------
void testActiveOverlaps(const dbc::Database& db) {
    std::printf("\n== 6. 同时有效的信号才查抢位 ==\n");

    const dbc::Message* m = db.find(0x300, false);
    if (m == nullptr) { check(false, "没找到 MuxFrame"); return; }

    std::uint8_t f1[8] = {0x01, 0x64, 0x00, 0, 0, 0, 0, 0};
    check(dbc::findActiveOverlaps(*m, f1).empty(),
          "开关值 1：MuxA 有效、MuxB 不算 → 零抢位");

    std::uint8_t f2[8] = {0x02, 0xC8, 0x00, 0, 0, 0, 0, 0};
    check(dbc::findActiveOverlaps(*m, f2).empty(), "开关值 2：同样零抢位");

    // 造一个真错的：同一支里两条信号抢位
    dbc::Message bad;
    bad.name = "BadMux";
    bad.signals = { mkSignal("S", 0, 8, "M"),
                    mkSignal("A", 8, 8, "m1"),
                    mkSignal("B", 8, 8, "m1") };
    std::uint8_t fb[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    auto ov = dbc::findActiveOverlaps(bad, fb);
    check(ov.size() == 1 && ov[0].sharedBits == 8,
          "同一分支 m1 的两条信号抢 8 位 → 查出来（这条在 M1 里也是报错，一致）");
}

// ---------------------------------------------------------------------------
// 7) 全库体检 + 扩展多路复用识别
// ---------------------------------------------------------------------------
void testWholeDb() {
    std::printf("\n== 7. 全库体检 ==\n");

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    check(pr.ok, "样例 DBC 解析成功");
    const dbc::Database& db = pr.db;

    bool allOk = true;
    bool noWarn = true;
    int muxMessages = 0;
    for (const auto& m : db.messages) {
        dbc::MuxInfo info = dbc::analyzeMux(m);
        if (!info.ok) allOk = false;
        if (!info.warnings.empty()) noWarn = false;
        if (info.selector != nullptr) ++muxMessages;
    }
    check(allOk, "4 个报文的结构全部合法");
    check(noWarn, "没有任何告警（分支号、开关位宽都没问题）");
    check(muxMessages == 1, "只有 1 个报文用了多路复用（MuxFrame）");
    check(!db.hasExtendedMux, "样例没有 SG_MUL_VAL_（单开关模型够用）");

    const char* withExt =
        "VERSION \"1.0\"\n"
        "NS_ :\n"
        "    SG_MUL_VAL_\n"
        "BS_:\n"
        "BU_ : ECU\n"
        "BO_ 256 EngineData: 8 ECU\n"
        " SG_ EngineSpeed : 0|16@1+ (1,0) [0|100] \"\" ECU\n"
        " SG_ EngineTemp : 16|8@1+ (1,0) [0|100] \"\" ECU\n"
        "SG_MUL_VAL_ 256 EngineTemp EngineSpeed 0 0;\n";
    dbc::ParseResult ext = dbc::parseText(withExt);
    check(ext.ok && ext.db.hasExtendedMux,
          "文件里出现 SG_MUL_VAL_ → 标记为扩展多路复用（能解析，但要告警）");
    check(ext.db.messages.size() == 1, "带 SG_MUL_VAL_ 的文件其余部分照常解析");
}

} // namespace

int main() {
    std::printf("M3 多路复用自测\n");

    testSyntax();

    dbc::ParseResult pr = dbc::parseFile(DBC_SAMPLE);
    if (!pr.ok) {
        std::printf("[FAIL] 样例 DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }
    const dbc::Database& db = pr.db;

    testAnalyze(db);
    testRouting(db);
    testPlain(db);
    testEncode(db);
    testActiveOverlaps(db);
    testWholeDb();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) {
        std::printf("M3 全绿 —— 分支会选了，才谈得上接真车。\n");
    }
    return gFail == 0 ? 0 : 1;
}
