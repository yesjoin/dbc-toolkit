// ============================================================================
// dbc_gen.cpp —— 把 DBC 编译成 C++17 头文件（M4）
//
// 产出物是一个**自包含**的头文件：只依赖 <cstdint>，不 include 本工程的任何东西。
// 这样别的 ECU 项目直接把 header 拷进去 #include 就能用，不用改 CMake，
// 也不用把整个 dbc-toolkit 拖过去。这是"代码生成"和"库"的根本区别。
//
// 生成的东西有三层：
//   ① 描述层  SignalDesc 表（名字/位段/字节序/符号/factor/offset/量程）
//   ② 数据层  struct Value（每个信号一个字段，用刚好装得下的整型）
//   ③ 动作层  decode / encode / <信号>_toPhysical / <信号>_fromPhysical
//
// 最要紧的是第四样：**static_assert 布局检查**。
// 生成的头文件在编译期就会用那个经典的 +15 锯齿规则把所有信号摊开，
// 只要 DBC 里有两条同时有效的信号抢同一个 bit，或者有信号冲出 64 位，
// 这个头文件直接编不过 —— 坏 DBC 进不了构建系统。
//
// 这是代码生成相对"手抄字段"最硬的价值：人手抄会抄错，编译器不会放水。
// ============================================================================

#include "Codec.h"
#include "Dbc.h"
#include "Mux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 名字消毒：DBC 的 \w+ 名字基本能用，但要挡住"数字开头"和"C++ 关键字"
// ---------------------------------------------------------------------------
bool isCppKeyword(const std::string& s) {
    static const char* kws[] = {
        "alignas","alignof","and","asm","auto","bool","break","case","catch","char",
        "class","const","constexpr","continue","default","delete","do","double",
        "else","enum","explicit","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept","not",
        "nullptr","operator","or","private","protected","public","register",
        "return","short","signed","sizeof","static","struct","switch","template",
        "this","throw","true","try","typedef","typeid","typename","union",
        "unsigned","using","virtual","void","volatile","while","xor"
    };
    for (const char* k : kws) if (s == k) return true;
    return false;
}

std::string ident(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '_';
        out += ok ? c : '_';
    }
    if (out.empty()) out = "_";
    if (out[0] >= '0' && out[0] <= '9') out = "_" + out;
    if (isCppKeyword(out)) out += "_";
    return out;
}

std::string cstr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

// 浮点字面量：%.15g 能保住 DBC 里写的那几个有效数字，又不会出现 0.125000000000001
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

// 刚好装得下 length 位的整型
const char* rawType(unsigned length, bool isSigned) {
    if (isSigned) {
        if (length <= 8)  return "int8_t";
        if (length <= 16) return "int16_t";
        if (length <= 32) return "int32_t";
        return "int64_t";
    }
    if (length <= 8)  return "uint8_t";
    if (length <= 16) return "uint16_t";
    if (length <= 32) return "uint32_t";
    return "uint64_t";
}

int branchOf(const std::string& mux) {
    if (!dbc::isBranchMux(mux)) return 0;
    return dbc::branchNumber(mux);
}

// ---------------------------------------------------------------------------
// 头文件里那段"只写一次"的公共代码
// ---------------------------------------------------------------------------
const char* kPrelude = R"RX(
namespace dbc_gen {
namespace detail {

// 一条信号的静态描述。生成器把 DBC 里的每个字段都塞进来了，
// 这样运行时（和编译期）都不需要再回头读 DBC 文件。
struct SignalDesc {
    const char* name;
    const char* unit;
    unsigned    startBit;
    unsigned    length;
    bool        bigEndian;     // true = Motorola(@0)，startBit 是 MSB
    bool        isSigned;
    int         branch;        // 0 = 普通信号或复用开关；N = 第 N 支
    double      factor;
    double      offset;
    double      minimum;
    double      maximum;
};

// Motorola 的那个锯齿：走到字节最低位就跳下一字节的最高位，也就是 +15
constexpr int nextBit(int b, bool bigEndian) {
    return bigEndian ? ((b % 8 == 0) ? (b + 15) : (b - 1)) : (b + 1);
}

// 把一条信号摊成 64 位掩码；越出帧就返回 0（0 是"非法"的信号，正常信号至少有 1 位）
constexpr unsigned long long bitMaskOf(const SignalDesc& s) {
    unsigned long long m = 0ull;
    int b = static_cast<int>(s.startBit);
    for (unsigned k = 0; k < s.length; ++k) {
        if (b < 0 || b > 63) return 0ull;
        m |= (1ull << b);
        b = nextBit(b, s.bigEndian);
    }
    return m;
}

// 两条信号会不会"同时有效"。不同复用分支本来就共用位，不算冲突。
constexpr bool canCoexist(const SignalDesc& a, const SignalDesc& b) {
    if (a.branch > 0 && b.branch > 0 && a.branch != b.branch) return false;
    return true;
}

// 编译期布局检查：越界、长度非法、同时有效的信号互相抢位 → 全在这里被拦住
constexpr bool layoutOk(const SignalDesc* sigs, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (sigs[i].length == 0u || sigs[i].length > 64u) return false;
        if (bitMaskOf(sigs[i]) == 0ull) return false;
    }
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned j = i + 1; j < n; ++j) {
            if (canCoexist(sigs[i], sigs[j]) &&
                (bitMaskOf(sigs[i]) & bitMaskOf(sigs[j])) != 0ull) {
                return false;
            }
        }
    }
    return true;
}

// 取：帧位 → 值位。Motorola 的 seq 从 MSB 起排，Intel 从 LSB 起排。
inline unsigned long long decodeRaw(const SignalDesc& s, const uint8_t* data) {
    unsigned long long raw = 0ull;
    int b = static_cast<int>(s.startBit);
    for (unsigned k = 0; k < s.length; ++k) {
        const bool one = ((data[b / 8] >> (b % 8)) & 1u) != 0u;
        const unsigned vbit = s.bigEndian ? (s.length - 1u - k) : k;
        if (one) raw |= (1ull << vbit);
        b = nextBit(b, s.bigEndian);
    }
    return raw;
}

// 放：只动这条信号自己的位，别人的位一个也不碰
inline void encodeRaw(const SignalDesc& s, unsigned long long raw, uint8_t* data) {
    int b = static_cast<int>(s.startBit);
    for (unsigned k = 0; k < s.length; ++k) {
        const unsigned vbit = s.bigEndian ? (s.length - 1u - k) : k;
        const uint8_t mask = static_cast<uint8_t>(1u << (b % 8));
        if (((raw >> vbit) & 1ull) != 0ull) data[b / 8] |= mask;
        else                                data[b / 8] &= static_cast<uint8_t>(~mask);
        b = nextBit(b, s.bigEndian);
    }
}

// 补码符号扩展：12 位全 1 是 -1，不是 4095
inline long long signExtend(unsigned long long raw, unsigned length) {
    if (length == 0u) return 0;
    if (length >= 64u) return static_cast<long long>(raw);
    const unsigned long long signbit = 1ull << (length - 1u);
    if ((raw & signbit) != 0ull) {
        return static_cast<long long>(raw | ~((1ull << length) - 1ull));
    }
    return static_cast<long long>(raw);
}

inline double toPhysical(const SignalDesc& s, long long raw) {
    return static_cast<double>(raw) * s.factor + s.offset;
}

// 反算。装不下返回 false；saturate = true 时改为夹到边界并返回 true。
inline bool fromPhysical(const SignalDesc& s, double v, long long* out, bool saturate) {
    if (out == nullptr || s.factor == 0.0) return false;

    const double raws = (v - s.offset) / s.factor;
    if (!(raws > -9.0e18 && raws < 9.0e18)) return false;   // 挡住 inf / NaN / 离谱值

    long long lo = 0;
    long long hi = 0;
    if (s.isSigned) {
        lo = (s.length >= 64u) ? (-9223372036854775807LL - 1) : -(1LL << (s.length - 1u));
        hi = (s.length >= 64u) ? 9223372036854775807LL : ((1LL << (s.length - 1u)) - 1);
    } else {
        lo = 0;
        hi = (s.length >= 64u) ? 9223372036854775807LL : ((1LL << s.length) - 1);
    }

    const long long need = static_cast<long long>((raws >= 0.0) ? (raws + 0.5) : (raws - 0.5));
    if (need < lo || need > hi) {
        if (!saturate) return false;
        *out = (need < lo) ? lo : hi;
        return true;
    }
    *out = need;
    return true;
}

// 复用分支：branch == 0 的信号（含开关本身）永远有效
inline bool branchActive(int branch, long long selector) {
    return (branch == 0) || (branch == static_cast<int>(selector));
}

} // namespace detail
} // namespace dbc_gen
)RX";

// ---------------------------------------------------------------------------
// 单个报文
// ---------------------------------------------------------------------------
void emitMessage(std::ostringstream& o, const dbc::Message& m) {
    const std::string ns = ident(m.name);
    const unsigned n = static_cast<unsigned>(m.signals.size());

    o << "// ==========================================================================\n";
    o << "// 报文 " << m.name << "  id=0x" << std::hex << m.id << std::dec
      << (m.extended ? "（扩展帧）" : "") << "  dlc=" << m.dlc
      << "  发送=" << (m.transmitter.empty() ? "?" : m.transmitter) << "\n";
    o << "// ==========================================================================\n";
    o << "namespace " << ns << " {\n\n";

    o << "constexpr unsigned ID = 0x" << std::hex << m.id << std::dec << "u;\n";
    o << "constexpr bool     EXTENDED = " << (m.extended ? "true" : "false") << ";\n";
    o << "constexpr uint8_t  DLC = " << m.dlc << "u;\n";
    o << "constexpr const char* NAME = " << cstr(m.name) << ";\n\n";

    // ---- 描述表 ----
    o << "constexpr ::dbc_gen::detail::SignalDesc SIGNALS[" << n << "] = {\n";
    for (unsigned i = 0; i < n; ++i) {
        const dbc::Signal& s = m.signals[i];
        o << "    { " << cstr(s.name) << ", " << cstr(s.unit) << ", "
          << s.startBit << "u, " << s.length << "u, "
          << (s.order == dbc::ByteOrder::Motorola ? "true" : "false") << ", "
          << (s.isSigned ? "true" : "false") << ", "
          << branchOf(s.mux) << ", "
          << num(s.factor) << ", " << num(s.offset) << ", "
          << num(s.minimum) << ", " << num(s.maximum) << " },";
        o << "   // " << s.startBit << "|" << s.length << "@"
          << (s.order == dbc::ByteOrder::Motorola ? "0" : "1")
          << (s.isSigned ? "-" : "+");
        if (branchOf(s.mux) > 0) o << "  m" << branchOf(s.mux);
        o << "\n";
    }
    o << "};\n";
    o << "constexpr unsigned SIGNAL_COUNT = " << n << "u;\n\n";

    o << "// 编译期体检：位越界 / 长度非法 / 同时有效的信号抢位 —— 只要有一条，这行就编不过\n";
    o << "static_assert(::dbc_gen::detail::layoutOk(SIGNALS, SIGNAL_COUNT),\n";
    o << "              \"" << m.name << "：信号位段越界或互相重叠，修 DBC\");\n\n";

    // ---- 数据体 ----
    o << "struct Value {\n";
    for (unsigned i = 0; i < n; ++i) {
        const dbc::Signal& s = m.signals[i];
        char line[512];
        // 负 offset 写成 " - 40.0" 而不是 " + -40.0"，注释是给人看的
        const std::string offPart =
            (s.offset < 0) ? (" - " + num(-s.offset)) : (" + " + num(s.offset));
        std::snprintf(line, sizeof(line),
                      "    %-9s %-13s // %u|%u@%c%c raw；物理 = raw * %s%s",
                      rawType(s.length, s.isSigned), (ident(s.name) + ";").c_str(),
                      s.startBit, s.length,
                      s.order == dbc::ByteOrder::Motorola ? '0' : '1',
                      s.isSigned ? '-' : '+',
                      num(s.factor).c_str(), offPart.c_str());
        o << line << "\n";
    }
    o << "};\n\n";

    // ---- 取 ----
    o << "inline void decode(const uint8_t* data, Value& out) {\n";
    for (unsigned i = 0; i < n; ++i) {
        const dbc::Signal& s = m.signals[i];
        const std::string f = ident(s.name);
        const char* ty = rawType(s.length, s.isSigned);
        if (s.isSigned) {
            o << "    out." << f << " = static_cast<" << ty
              << ">(::dbc_gen::detail::signExtend("
              << "::dbc_gen::detail::decodeRaw(SIGNALS[" << i << "], data), "
              << s.length << "u));\n";
        } else {
            o << "    out." << f << " = static_cast<" << ty
              << ">(::dbc_gen::detail::decodeRaw(SIGNALS[" << i << "], data));\n";
        }
    }
    o << "}\n\n";

    // ---- 放 ----
    o << "// 只写本报文信号占的位，data 里其他位原样保留（多报文拼一帧时用得上）\n";
    o << "inline void encode(const Value& in, uint8_t* data) {\n";
    for (unsigned i = 0; i < n; ++i) {
        const dbc::Signal& s = m.signals[i];
        const std::string f = ident(s.name);
        if (s.isSigned) {
            o << "    ::dbc_gen::detail::encodeRaw(SIGNALS[" << i << "], "
              << "static_cast<unsigned long long>(in." << f << "), data);\n";
        } else {
            o << "    ::dbc_gen::detail::encodeRaw(SIGNALS[" << i << "], "
              << "static_cast<unsigned long long>(in." << f << "), data);\n";
        }
    }
    o << "}\n\n";

    // ---- 物理换算 ----
    for (unsigned i = 0; i < n; ++i) {
        const dbc::Signal& s = m.signals[i];
        const std::string f = ident(s.name);
        const char* ty = rawType(s.length, s.isSigned);

        o << "inline double " << f << "_toPhysical(" << ty << " raw) {\n";
        if (s.isSigned) {
            o << "    return ::dbc_gen::detail::toPhysical(SIGNALS[" << i
              << "], static_cast<long long>(raw));\n";
        } else {
            o << "    return ::dbc_gen::detail::toPhysical(SIGNALS[" << i
              << "], static_cast<long long>(raw));\n";
        }
        o << "}\n";

        o << "inline bool " << f << "_fromPhysical(double v, " << ty
          << "* out, bool saturate = false) {\n";
        o << "    long long raw = 0;\n";
        o << "    if (!::dbc_gen::detail::fromPhysical(SIGNALS[" << i
          << "], v, &raw, saturate)) return false;\n";
        o << "    *out = static_cast<" << ty << ">(raw);\n";
        o << "    return true;\n";
        o << "}\n\n";
    }

    // ---- 复用 ----
    unsigned selectorIndex = n;                 // n 表示"没找到开关"
    for (unsigned i = 0; i < n; ++i) {
        if (m.signals[i].mux == "M") { selectorIndex = i; break; }
    }
    if (selectorIndex < n) {
        const std::string selField = ident(m.signals[selectorIndex].name);
        o << "// 多路复用：开关是 " << m.signals[selectorIndex].name
          << "，它的值等于下面这个数字时，这条信号才有效。\n";
        o << "// 未选中的分支字段里仍是位上的原值，**不是 0** —— 用之前先判 xxx_active。\n";
        for (unsigned i = 0; i < n; ++i) {
            const int br = branchOf(m.signals[i].mux);
            if (br <= 0) continue;
            o << "inline bool " << ident(m.signals[i].name)
              << "_active(Value const& v) {\n";
            o << "    return ::dbc_gen::detail::branchActive(" << br
              << ", static_cast<long long>(v." << selField << "));\n";
            o << "}\n";
        }
        o << "\n";
    }

    o << "} // namespace " << ns << "\n\n";
}

} // namespace

int main(int argc, char** argv) {
    const char* inPath = nullptr;
    const char* outPath = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (argv[i][0] != '-') inPath = argv[i];
    }

    if (inPath == nullptr || outPath == nullptr) {
        std::fprintf(stderr, "用法：%s <文件.dbc> -o <输出.h>\n", argv[0]);
        return 2;
    }

    dbc::ParseResult pr = dbc::parseFile(inPath);
    if (!pr.ok) {
        std::fprintf(stderr, "DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }

    // 结构错（两个开关 / 有分支没开关）不该带病生成
    for (const auto& m : pr.db.messages) {
        dbc::MuxInfo mi = dbc::analyzeMux(m);
        if (!mi.ok) {
            std::fprintf(stderr, "报文 %s 的多路复用结构有问题：%s\n", m.name.c_str(), mi.error.c_str());
            return 1;
        }
        for (const auto& w : mi.warnings) {
            std::fprintf(stderr, "警告（%s）：%s\n", m.name.c_str(), w.c_str());
        }
    }

    std::ostringstream o;
    o << "// ============================================================================\n";
    o << "// 本文件由 dbc-gen 从 " << inPath << " 自动生成，请勿手改。\n";
    o << "// 改 DBC 之后重新生成；生成器不带时间戳，同样的 DBC 永远生成同样的字节，\n";
    o << "// 这样 diff 里只会出现真正的改动，CI 里也不会因为时间戳天天报变更。\n";
    o << "//\n";
    o << "// 依赖：只有 <cstdint>。本文件可以被任何 C++17 工程直接 #include。\n";
    o << "// ============================================================================\n";
    o << "#pragma once\n\n";
    o << "#include <cstdint>\n";
    o << kPrelude << "\n";
    o << "// ============================================================================\n";
    o << "// 全局统计：" << pr.db.messages.size() << " 个报文，"
      << pr.db.signalCount() << " 个信号\n";
    o << "// ============================================================================\n\n";

    if (pr.db.hasExtendedMux) {
        o << "// 注意：源 DBC 用了扩展多路复用（SG_MUL_VAL_），本生成器只处理单开关，\n";
        o << "// 生成结果可能不完整。\n\n";
    }

    for (const auto& m : pr.db.messages) emitMessage(o, m);

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "写不了 %s\n", outPath);
        return 1;
    }
    out << o.str();

    std::printf("已生成 %s：%zu 个报文，%zu 个信号\n",
                outPath, pr.db.messages.size(), pr.db.signalCount());
    return 0;
}
