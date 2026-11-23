// ============================================================================
// dbc_dump.cpp —— 把解析结果打成一张能看的表
//
// 用法：./build/dbc-dump dbc/sample.dbc
//
// 这个工具的意义不是"好看"，是让 M1 调试时有参照物：
// 位算错了，先回来看看 startBit/length/order 是不是解析阶段就错了。
// ============================================================================

#include "Bits.h"
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

    // M1：位引擎的结论 —— 这段声明实际吃掉了帧里哪几位
    const std::string bits = dbc::describeBits(s);
    if (dbc::layoutOf(s).ok) {
        std::printf("   %-16s 位段：%s\n", "", bits.c_str());
    } else {
        std::printf("   %-16s 位段：%s   <<< 声明有问题\n", "", bits.c_str());
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

        // M1：抢位检查。解析阶段永远发现不了这类错，只有把位摊开才算得出
        const auto overlaps = dbc::findOverlaps(m);
        if (!overlaps.empty()) {
            std::printf("   !! %zu 处抢位：\n", overlaps.size());
            for (const auto& o : overlaps) {
                std::printf("      %s × %s 争用 %d 位\n",
                            o.a.c_str(), o.b.c_str(), o.sharedBits);
            }
        }
        std::printf("\n");
    }
    return 0;
}
