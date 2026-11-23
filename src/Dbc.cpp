#include "Dbc.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace dbc {

const char* orderName(ByteOrder o) {
    return (o == ByteOrder::Intel) ? "Intel(@1)" : "Motorola(@0)";
}

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : t) {
        if (c == '\r') continue;
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// 关键字必须"整词"匹配：BO_ 不能顺带匹配到 BO_TX_BU_，VAL_ 不能匹配到 VAL_TABLE_
bool startsToken(const std::string& s, const char* kw) {
    const std::size_t n = std::strlen(kw);
    if (s.size() < n || s.compare(0, n, kw) != 0) return false;
    return s.size() == n || std::isspace(static_cast<unsigned char>(s[n]));
}

// 这些语句允许跨行，必须先攒到分号
//
// 注意：NS_ 段里逐行列着 VAL_ / CM_ 这些"关键字名"，那些是目录不是语句。
// 所以这里要求关键字后面必须还有内容（裸关键字不算），否则会把
// 整个 NS_ 段当成一条没写完的语句，一路吞到文件里第一个分号。
bool needsSemicolon(const std::string& s) {
    static const char* kws[] = {"VAL_", "CM_", "BA_DEF_", "BA_", "BA_DEF_DEF_",
                                "SIG_VALTYPE_", "SG_MUL_VAL_", "VAL_TABLE_"};
    for (auto kw : kws) {
        if (startsToken(s, kw) && s.size() > std::strlen(kw)) return true;
    }
    return false;
}

double num(const std::string& s) {
    return std::strtod(s.c_str(), nullptr);
}

std::uint32_t u32(const std::string& s) {
    return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
}

std::uint64_t u64(const std::string& s) {
    return static_cast<std::uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    const std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 查询辅助
// ---------------------------------------------------------------------------
const Signal* Message::find(const std::string& signalName) const {
    for (const auto& s : signals) {
        if (s.name == signalName) return &s;
    }
    return nullptr;
}

const Message* Database::find(std::uint32_t id, bool extended) const {
    for (const auto& m : messages) {
        if (m.id == id && m.extended == extended) return &m;
    }
    return nullptr;
}

const Message* Database::find(const std::string& name) const {
    for (const auto& m : messages) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

std::size_t Database::signalCount() const {
    std::size_t n = 0;
    for (const auto& m : messages) n += m.signals.size();
    return n;
}

// ---------------------------------------------------------------------------
// 解析主线
//
// DBC 是"行式"格式：绝大多数语句一行一条，只有 CM_/VAL_/BA_ 类允许折行。
// 所以不做全量词法分析，只做：逐行 → 攒分号 → 按关键字分发 → 正则切字段。
// 好处是错误能精确报到行号，坏处是没法处理极端畸形文件（我们不处理）。
// ---------------------------------------------------------------------------
ParseResult parseText(const std::string& text) {
    ParseResult r;
    const std::vector<std::string> lines = splitLines(text);

    // BO_ 256 EngineData: 8 ECU
    // 原始字符串用 rx 当分隔符 —— 正则里本身会出现 )" 序列，默认 R"( )" 会被截断
    const std::regex reBo(R"rx(^\s*BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\S+)\s*$)rx");

    // SG_ Name [M|mN] : start|len@order sign (factor,offset) [min|max] "unit" receivers
    const std::regex reSg(
        R"rx(^\s*SG_\s+(\w+)\s*(M|m\d+)?\s*:\s*(\d+)\s*\|\s*(\d+)\s*@\s*([01])\s*([+-])\s*)rx"
        R"rx(\(\s*([^,\s)]+)\s*,\s*([^)\s]+)\s*\)\s*\[\s*([^|\s]+)\s*\|\s*([^\]\s]+)\s*\]\s*)rx"
        R"rx("([^"]*)"\s*(.*)$)rx");

    const std::regex reVal(R"rx(^\s*VAL_\s+(\d+)\s+(\w+)\s+(.*);\s*$)rx");
    const std::regex rePair(R"rx(([+-]?\d+)\s+"([^"]*)")rx");
    const std::regex reVersion(R"rx(^\s*VERSION\s+"([^"]*)")rx");
    const std::regex reBu(R"rx(^\s*BU_\s*:\s*(.*)$)rx");

    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) continue;
        if (line[0] == '#') continue;                    // 允许 # 注释行

        // ---- 跨行语句：把整条攒起来，行号仍算起始行 ----
        if (needsSemicolon(line) && line.find(';') == std::string::npos) {
            std::size_t j = i;
            while (line.find(';') == std::string::npos && j + 1 < lines.size()) {
                ++j;
                const std::string t = trim(lines[j]);
                if (!t.empty()) line += " " + t;
            }
            i = j;
        }

        std::smatch m;

        // ---- VERSION ----
        if (startsToken(line, "VERSION")) {
            if (std::regex_search(line, m, reVersion)) r.db.version = m[1];
            continue;
        }

        // ---- BU_ : 节点列表 ----
        if (startsToken(line, "BU_")) {
            if (std::regex_match(line, m, reBu)) {
                const std::string rest = trim(m[1]);
                std::size_t p = 0;
                while (p < rest.size()) {
                    while (p < rest.size() && std::isspace(static_cast<unsigned char>(rest[p]))) ++p;
                    std::size_t b = p;
                    while (p < rest.size() && !std::isspace(static_cast<unsigned char>(rest[p]))) ++p;
                    if (p > b) r.db.nodes.push_back(rest.substr(b, p - b));
                }
            }
            continue;
        }

        // ---- BO_ ----
        if (startsToken(line, "BO_")) {
            if (!std::regex_match(line, m, reBo)) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "BO_ 行格式不对：" + line;
                return r;
            }
            Message msg;
            const std::uint64_t rawId = u64(m[1].str());
            // 扩展帧在 DBC 里的约定：bit31 打标志，真实 ID 在低 29 位
            msg.extended = (rawId & 0x80000000ull) != 0;
            msg.id = static_cast<std::uint32_t>(msg.extended ? (rawId & 0x1FFFFFFFull) : rawId);
            msg.name = m[2].str();
            msg.dlc  = u32(m[3].str());
            msg.transmitter = m[4].str();

            if (msg.dlc < 1 || msg.dlc > 8) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "DLC 只支持 1..8，收到 " + std::to_string(msg.dlc);
                return r;
            }
            if (msg.id > (msg.extended ? 0x1FFFFFFFu : 0x7FFu)) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "报文 ID 超出范围：" + m[1].str();
                return r;
            }
            r.db.messages.push_back(msg);
            continue;
        }

        // ---- SG_ ----
        if (startsToken(line, "SG_")) {
            if (r.db.messages.empty()) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "SG_ 出现在任何 BO_ 之前";
                return r;
            }
            if (!std::regex_match(line, m, reSg)) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "SG_ 行格式不对：" + line;
                return r;
            }

            Signal s;
            s.name     = m[1].str();
            s.mux      = m[2].matched ? m[2].str() : "";
            s.startBit = u32(m[3].str());
            s.length   = u32(m[4].str());
            s.order    = (m[5].str() == "1") ? ByteOrder::Intel : ByteOrder::Motorola;
            s.isSigned = (m[6].str() == "-");
            s.factor   = num(m[7].str());
            s.offset   = num(m[8].str());
            s.minimum  = num(m[9].str());
            s.maximum  = num(m[10].str());
            s.unit     = m[11].str();
            s.receivers = splitComma(m[12].str());

            Message& msg = r.db.messages.back();

            if (s.length == 0 || s.length > 64) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "信号 " + s.name + " 长度非法：" + std::to_string(s.length);
                return r;
            }
            if (s.startBit >= msg.dlc * 8) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "信号 " + s.name + " 起始位超出 DLC：" + std::to_string(s.startBit);
                return r;
            }
            if (msg.find(s.name) != nullptr) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "报文 " + msg.name + " 里信号名重复：" + s.name;
                return r;
            }
            msg.signals.push_back(s);
            continue;
        }

        // ---- VAL_ 值表 ----
        // 裸 "VAL_" 是 NS_ 目录里的关键字名，不是语句，必须跳过
        if (startsToken(line, "VAL_") && line.size() > 4) {
            if (!std::regex_match(line, m, reVal)) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "VAL_ 行格式不对：" + line;
                return r;
            }
            const std::uint32_t msgId = u32(m[1].str());
            const std::string   sigName = m[2].str();

            Message* target = nullptr;
            for (auto& mm : r.db.messages) {
                const std::uint64_t raw = msgId;
                const bool ext = (raw & 0x80000000u) != 0;
                const std::uint32_t pure = ext ? (raw & 0x1FFFFFFFu) : raw;
                if (mm.id == pure && mm.extended == ext) { target = &mm; break; }
                // DBC 里 VAL_ 常常直接写裸 ID，不带扩展标志
                if (mm.id == msgId) { target = &mm; break; }
            }
            if (target == nullptr) {
                std::string ids;
                for (const auto& mm : r.db.messages) {
                    ids += std::to_string(mm.id) + (mm.extended ? "(ext) " : " ");
                }
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "VAL_ 引用了不存在的报文 ID：" + m[1].str() +
                          "（已解析的 ID：" + ids + "）";
                return r;
            }

            Signal* sig = const_cast<Signal*>(target->find(sigName));
            if (sig == nullptr) {
                r.errorLine = static_cast<int>(i) + 1;
                r.error = "VAL_ 引用了 " + target->name + " 里不存在的信号：" + sigName;
                return r;
            }

            const std::string body = m[3].str();
            for (std::sregex_iterator it(body.begin(), body.end(), rePair), end; it != end; ++it) {
                ValueDescription vd;
                vd.value = std::strtoll((*it)[1].str().c_str(), nullptr, 10);
                vd.text  = (*it)[2].str();
                sig->valueTable.push_back(vd);
            }
            continue;
        }

        // ---- 扩展多路复用（多开关）本工具不支持，但要记下来告警 ----
        // 注意：NS_ 段的目录里也有一行裸的 "SG_MUL_VAL_"，那是关键字名不是语句。
        // 判据和 needsSemicolon 一样 —— 关键字后面必须还有内容。
        if (startsToken(line, "SG_MUL_VAL_") && line.size() > 11) {
            r.db.hasExtendedMux = true;
            continue;
        }

        // ---- 其余语句一律跳过（CM_ / BA_ / NS_ / BS_ / SIG_GROUP_ ...）----
    }

    r.ok = true;
    return r;
}

ParseResult parseFile(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        ParseResult r;
        r.errorLine = 0;
        r.error = std::string("打不开文件：") + path;
        return r;
    }
    std::string text;
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), f) != nullptr) {
        text += buf;
    }
    std::fclose(f);
    return parseText(text);
}

} // namespace dbc
