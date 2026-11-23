// ============================================================================
// dbc_decode.cpp —— 拿一帧真实字节，按 DBC 解出每个信号的物理值
//
// 用法：
//   ./build/dbc-decode dbc/sample.dbc 0x100 3412A0BC0D0F1234
//   ./build/dbc-decode dbc/sample.dbc 0x100 "34 12 A0 BC 0D 0F 12 34"
//
// 这个工具是 M3.5 的雏形：只要把 candump 抓到的 8 个字节喂进来，
// 它就是一个"不看文档也知道这帧在说什么"的解码器。
// ============================================================================

#include "Codec.h"
#include "Dbc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// "3412A0BC0D0F1234" 或 "34 12 A0 ..." → 字节数组
bool parseBytes(const char* text, std::uint8_t* out, int* count) {
    int n = 0;
    for (const char* p = text; *p != '\0';) {
        // 跳过空格、冒号、横线这类分隔符
        if (*p == ' ' || *p == '\t' || *p == ':' || *p == '-' || *p == '_') {
            ++p;
            continue;
        }
        int hi = -1, lo = -1;
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        hi = hexVal(p[0]);
        if (hi < 0) return false;
        if (p[1] != '\0') lo = hexVal(p[1]);
        if (lo < 0) return false;          // 必须两位一组
        if (n >= 8) return false;          // 只认 8 字节帧

        out[n++] = static_cast<std::uint8_t>((hi << 4) | lo);
        p += 2;
    }
    *count = n;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "用法：%s <文件.dbc> <报文ID，如 0x100> <8 字节十六进制>\n"
                     "例如：%s dbc/sample.dbc 0x100 3412A0BC0D0F1234\n",
                     argv[0], argv[0]);
        return 2;
    }

    dbc::ParseResult pr = dbc::parseFile(argv[1]);
    if (!pr.ok) {
        std::fprintf(stderr, "DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }

    const std::uint32_t id = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 0));

    const dbc::Message* msg = pr.db.find(id, false);
    if (msg == nullptr) msg = pr.db.find(id, true);
    if (msg == nullptr) {
        std::fprintf(stderr, "DBC 里没有 ID 0x%X 的报文\n", id);
        return 1;
    }

    std::uint8_t data[8] = {0};
    int count = 0;

    // 允许两种写法：整个字节串一个参数（"3412A0…"），或 8 个参数分开写
    std::string hexAll;
    for (int i = 3; i < argc; ++i) hexAll += argv[i];
    hexAll += "";

    if (!parseBytes(hexAll.c_str(), data, &count)) {
        std::fprintf(stderr, "字节串看不懂：%s（要 8 个两位十六进制，可带空格）\n", hexAll.c_str());
        return 1;
    }

    std::printf("帧 0x%03X %s  dlc=%u  发送=%s\n", msg->id, msg->name.c_str(), msg->dlc,
                msg->transmitter.c_str());
    std::printf("字节：");
    for (int i = 0; i < count; ++i) std::printf("%02X ", data[i]);
    std::printf("\n");
    if (count < static_cast<int>(msg->dlc)) {
        std::printf("注意：只给了 %d 字节，DBC 声明 %u 字节，缺的按 0 处理\n", count, msg->dlc);
    }
    std::printf("\n");

    for (const auto& s : msg->signals) {
        dbc::DecodeResult r = dbc::decodeSignal(s, data);
        if (!r.ok) {
            std::printf("  %-16s 解不出来：%s\n", s.name.c_str(), r.error.c_str());
            continue;
        }

        const int digits = static_cast<int>((s.length + 3) / 4);   // 十六进制位数
        std::printf("  %-16s raw=0x%0*llX(%8lld) → %10.3f %-6s",
                    s.name.c_str(),
                    digits > 0 ? digits : 1,
                    static_cast<long long>(r.raw),
                    static_cast<long long>(r.rawSigned),
                    r.value, s.unit.c_str());

        if (const std::string* txt = dbc::valueTextFor(s, r.rawSigned)) {
            std::printf("  \"%s\"", txt->c_str());
        } else if (!s.valueTable.empty()) {
            std::printf("  \"(值表里没有这个值)\"");
        }

        if (r.rangeDeclared) {
            if (r.inEngineeringRange) {
                std::printf("  [量程 %g..%g ✓]", s.minimum, s.maximum);
            } else {
                std::printf("  [量程 %g..%g  ✗ 超量程]", s.minimum, s.maximum);
            }
        }
        std::printf("\n");
    }
    return 0;
}
