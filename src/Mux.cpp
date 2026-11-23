// ============================================================================
// Mux.cpp —— 多路复用实现（M3）
//
// ----------------------------------------------------------------------------
// 多路复用到底在解决什么问题
// ----------------------------------------------------------------------------
//
// 一个报文 8 个字节 = 64 bit，很贵（总线上每 1 ms 就占一次带宽）。
// 但如果"方向盘角度"只在低速时有用、"电机转速"只在高速时有用，
// 就没必要各占一个报文 —— 把同一块字节分时复用：
//
//     MuxSel = 1  →  byte1..byte2 这段 16 bit 含义是 MuxA（方向盘角度）
//     MuxSel = 2  →  同一段 16 bit 含义是 MuxB（电机转速）
//
// 这就是为什么 MuxA 和 MuxB 在 DBC 里声明完全一样的 8|16@1 —— 它们本来就
// 是同一块物理位置，只是含义随时切换。M1 的抢位检查为此专门豁免了"不同分支"。
//
// ----------------------------------------------------------------------------
// 三条最容易踩的
// ----------------------------------------------------------------------------
// ① 未选中的分支，值是"不存在"，不是"0"。
//    给它塞 0.0 的后果：下游看到"刹车压力 0 bar"，以为是松刹车，
//    实际是"这帧根本没在发刹车压力"。这是把"没有"伪装成"是"。
//
// ② 开关必须是 M（大写），分支必须是 mN（小写 m + 数字）。
//    DBC 里 "M" 和 "m1" 只差一个大小写，含义完全不同，看漏一个就是全盘错。
//
// ③ 一个报文只能有一个开关。两个开关就是"扩展多路复用"（SG_MUL_VAL_），
//    表达的是"开关套开关"的二维选择，本工具不支持 —— 遇到就必须拒绝，
//    不能猜一个近似解出来，那种"差不多对"的解最难查。
//
// ============================================================================

#include "Mux.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dbc {

namespace {

std::string itoa(int v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return buf;
}

std::uint64_t maskOf(const BitLayout& lay) {
    std::uint64_t m = 0;
    for (std::uint32_t i = 0; i < lay.length; ++i) {
        m |= (1ull << (lay.slot[i].byte * 8 + lay.slot[i].bit));
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
bool isBranchMux(const std::string& mux) {
    if (mux.size() < 2 || mux[0] != 'm') return false;
    for (std::size_t i = 1; i < mux.size(); ++i) {
        if (mux[i] < '0' || mux[i] > '9') return false;
    }
    return true;
}

int branchNumber(const std::string& mux) {
    if (!isBranchMux(mux)) return -1;
    return std::atoi(mux.c_str() + 1);
}

// ---------------------------------------------------------------------------
MuxInfo analyzeMux(const Message& m) {
    MuxInfo info;

    int selectors = 0;
    for (const auto& s : m.signals) {
        if (s.mux == "M") {
            ++selectors;
            if (info.selector == nullptr) info.selector = &s;
            continue;
        }
        if (isBranchMux(s.mux)) {
            const int n = branchNumber(s.mux);
            info.branches.push_back(n);
            if (n > info.maxBranch) info.maxBranch = n;
            if (n <= 0) {
                info.warnings.push_back("信号 " + s.name + " 的分支号是 " + s.mux +
                                        "，分支号应该从 1 开始（m0 没有意义）");
            }
            continue;
        }
        if (!s.mux.empty()) {
            info.warnings.push_back("信号 " + s.name + " 的复用标记 “" + s.mux +
                                    "” 认不出来，已当成普通信号处理");
        }
    }

    std::sort(info.branches.begin(), info.branches.end());
    info.branches.erase(std::unique(info.branches.begin(), info.branches.end()),
                        info.branches.end());

    if (selectors > 1) {
        info.ok = false;
        info.error = "报文 " + m.name + " 里声明了 " + itoa(selectors) +
                     " 个开关（M）；本工具只支持单开关，多开关属于扩展多路复用（SG_MUL_VAL_）";
        return info;
    }
    if (selectors == 0 && !info.branches.empty()) {
        info.ok = false;
        info.error = "报文 " + m.name + " 有 " + itoa(static_cast<int>(info.branches.size())) +
                     " 个分支信号，但没有 M 开关；这些分支永远选不中（等于死信号）";
        return info;
    }

    // 分支号必须装得进开关的位宽
    if (info.selector != nullptr && info.maxBranch > 0) {
        std::int64_t lo = 0, hi = 0;
        rawRange(*info.selector, &lo, &hi);
        if (info.maxBranch > hi) {
            info.warnings.push_back("最大分支号 " + itoa(info.maxBranch) + " 超过开关 " +
                                    info.selector->name + " 能表示的 " + itoa(static_cast<int>(hi)) +
                                    "，这一支永远选不中");
        }
    }

    return info;
}

// ---------------------------------------------------------------------------
DecodedMessage decodeMessage(const Message& m, const std::uint8_t* data) {
    DecodedMessage out;

    MuxInfo info = analyzeMux(m);
    if (!info.ok) {
        out.error = info.error;
        return out;
    }

    out.hasMux = (info.selector != nullptr);
    if (out.hasMux) {
        DecodeResult sel = decodeSignal(*info.selector, data);
        if (!sel.ok) {
            out.error = "开关 " + info.selector->name + " 解不出来：" + sel.error;
            return out;
        }
        out.selectorValue = sel.rawSigned;
    }

    for (const auto& s : m.signals) {
        SignalValue sv;
        sv.name = s.name;

        // 未选中的分支 → active = false。注意仍然做了解码，
        // 但值只作为"如果它是这一支会长什么样"的参考，调用方不许用。
        sv.decoded = decodeSignal(s, data);

        if (isBranchMux(s.mux)) {
            sv.active = (branchNumber(s.mux) == out.selectorValue);
        } else {
            sv.active = true;      // 开关本身 + 普通信号，永远有效
        }
        out.signals.push_back(sv);
    }

    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// 两步走：先全部算成 raw，一个都不写；全部成功才落笔。
// 这样"某个信号值非法"时帧还是原样，不会留半写状态。
// ---------------------------------------------------------------------------
EncodeMessageResult encodeMessage(const Message& m, int branch,
                                  const std::map<std::string, double>& values,
                                  std::uint8_t* data) {
    EncodeMessageResult r;

    MuxInfo info = analyzeMux(m);
    if (!info.ok) {
        r.error = info.error;
        return r;
    }

    if (info.selector != nullptr && branch < 0) {
        r.error = "报文 " + m.name + " 有多路复用，必须指定分支号（0 也算）";
        return r;
    }
    if (info.selector == nullptr && branch >= 0) {
        r.error = "报文 " + m.name + " 没有多路复用，不该指定分支号";
        return r;
    }

    // ---- 阶段 1：算出要写哪些 bit，装不下就直接退出 ----
    struct Planned {
        BitLayout     lay;
        std::uint64_t packed;
    };
    std::vector<Planned> plan;

    if (info.selector != nullptr) {
        const Signal& sel = *info.selector;
        double selValue = static_cast<double>(branch);
        auto it = values.find(sel.name);
        if (it != values.end()) selValue = it->second;    // 想让开关取别的值（少用）

        std::int64_t raw = 0;
        std::string err;
        if (!physicalToInt(sel, selValue, &raw, &err)) {
            r.error = "开关 " + sel.name + "：" + err;
            return r;
        }
        BitLayout lay = layoutOf(sel);
        if (!lay.ok) {
            r.error = "开关 " + sel.name + "：" + lay.error;
            return r;
        }
        plan.push_back(Planned{ lay, packRaw(raw, sel.length) });
    }

    for (const auto& s : m.signals) {
        if (s.mux == "M") continue;                        // 已经安排过

        if (isBranchMux(s.mux) && branchNumber(s.mux) != branch) {
            r.skipped.push_back(s.name);                   // 不是这一支，不许写
            continue;
        }

        auto it = values.find(s.name);
        if (it == values.end()) continue;                  // 没给值就不动它

        std::int64_t raw = 0;
        std::string err;
        if (!physicalToInt(s, it->second, &raw, &err)) {
            r.error = s.name + "：" + err;
            return r;
        }
        BitLayout lay = layoutOf(s);
        if (!lay.ok) {
            r.error = s.name + "：" + lay.error;
            return r;
        }
        plan.push_back(Planned{ lay, packRaw(raw, s.length) });
    }

    // ---- 阶段 2：全部合法了，才真写 ----
    for (auto& p : plan) {
        if (!encodeRaw(p.lay, p.packed, data)) {
            r.error = "写位失败（理论到不了这里）";
            return r;
        }
    }

    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
std::vector<Overlap> findActiveOverlaps(const Message& m, const std::uint8_t* data) {
    std::vector<Overlap> out;

    MuxInfo info = analyzeMux(m);
    std::int64_t sel = 0;
    if (info.selector != nullptr) {
        DecodeResult r = decodeSignal(*info.selector, data);
        if (!r.ok) return out;
        sel = r.rawSigned;
    }

    struct Item {
        const char*   name;
        std::uint64_t mask;
    };
    std::vector<Item> actives;

    for (const auto& s : m.signals) {
        if (isBranchMux(s.mux) && branchNumber(s.mux) != sel) continue;   // 没被选中
        BitLayout lay = layoutOf(s);
        if (!lay.ok) continue;
        actives.push_back(Item{ s.name.c_str(), maskOf(lay) });
    }

    for (std::size_t i = 0; i < actives.size(); ++i) {
        for (std::size_t j = i + 1; j < actives.size(); ++j) {
            std::uint64_t shared = actives[i].mask & actives[j].mask;
            if (shared == 0) continue;
            int nbits = 0;
            for (std::uint64_t v = shared; v != 0; v &= (v - 1)) ++nbits;
            out.push_back(Overlap{ actives[i].name, actives[j].name, nbits });
        }
    }
    return out;
}

} // namespace dbc
