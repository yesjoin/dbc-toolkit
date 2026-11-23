// ============================================================================
// dbc_dump.cpp —— 把解析结果打成一张能看的表
//
// 用法：./build/dbc-dump dbc/sample.dbc
//
// 这个工具的意义不是"好看"，是让 M1 调试时有参照物：
// 位算错了，先回来看看 startBit/length/order 是不是解析阶段就错了。
// ============================================================================

#include "Dbc.h"

#include <cstdio>
#include <string>

namespace {

void printSignal(const dbc::Signal& s) {
    char pos[32];
    std::snprintf(pos, sizeof(pos), "%2u|%-2u", s.startBit, s.length);

    std::printf("   %-16s %-9s %-11s %s  x%-8g %+g  [%g .. %g]",
                s.name.c_str(), pos, dbc::orderName(s.order),
                s.isSigned ? "有符号" : "无符号",
                s.factor, s.offset, s.minimum, s.maximum);

    if (!s.unit.empty()) std::printf("  \"%s\"", s.unit.c_str());
    if (!s.mux.empty()) {
        if (s.mux == "M") std::printf("  [复用开关]");
        else              std::printf("  [分支 %s]", s.mux.c_str());
    }
    if (!s.receivers.empty()) {
        std::printf("  <- ");
        for (std::size_t i = 0; i < s.receivers.size(); ++i) {
            std::printf("%s%s", i ? "," : "", s.receivers[i].c_str());
        }
    }
    std::printf("\n");

    if (!s.valueTable.empty()) {
        std::printf("   %-16s 值表：", "");
        for (std::size_t i = 0; i < s.valueTable.size(); ++i) {
            std::printf("%s%lld=%s", i ? ", " : "",
                        static_cast<long long>(s.valueTable[i].value),
                        s.valueTable[i].text.c_str());
        }
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法：%s <文件.dbc>\n", argv[0]);
        return 2;
    }

    dbc::ParseResult r = dbc::parseFile(argv[1]);
    if (!r.ok) {
        std::fprintf(stderr, "解析失败（第 %d 行）：%s\n", r.errorLine, r.error.c_str());
        return 1;
    }

    const dbc::Database& db = r.db;
    std::printf("文件：%s\n", argv[1]);
    std::printf("版本：%s\n", db.version.empty() ? "(没写)" : db.version.c_str());
    std::printf("节点：");
    for (std::size_t i = 0; i < db.nodes.size(); ++i) {
        std::printf("%s%s", i ? " " : "", db.nodes[i].c_str());
    }
    std::printf("\n报文 %zu 个，信号 %zu 个\n\n", db.messages.size(), db.signalCount());

    for (const auto& m : db.messages) {
        std::printf("BO_ 0x%03X%s  %-14s dlc=%u  发送=%s  （%zu 个信号）\n",
                    m.id, m.extended ? "(扩展)" : "", m.name.c_str(), m.dlc,
                    m.transmitter.c_str(), m.signals.size());
        for (const auto& s : m.signals) printSignal(s);
        std::printf("\n");
    }
    return 0;
}
