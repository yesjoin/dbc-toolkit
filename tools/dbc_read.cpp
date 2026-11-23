// ============================================================================
// dbc_read.cpp —— 挂在真总线上，收到 DBC 认得的帧就解出来（M3.5）
//
// 用法：
//   ./build/dbc-read dbc/sample.dbc vcan0 5        # 跑 5 秒
//
// 这是整个项目的收口：帧从总线来（不是从命令行来），
// 一路走 M1 位引擎 → M2 补码换算 → M3 分支路由，最后变成人话。
//
// 认不出的 ID 不报错、也不猜 —— 只计数。真实车上 80% 的帧都不在你的 DBC 里，
// 一个解码器必须能"安静地忽略"绝大多数流量。
// ============================================================================

#include "CanRaw.h"
#include "Codec.h"
#include "Dbc.h"
#include "Mux.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "用法：%s <dbc> <接口名> [运行秒数，默认 5]\n", argv[0]);
        return 2;
    }

    dbc::ParseResult pr = dbc::parseFile(argv[1]);
    if (!pr.ok) {
        std::fprintf(stderr, "DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }

    const double seconds = (argc >= 4) ? std::atof(argv[3]) : 5.0;

    std::string err;
    const int fd = dbc::canOpen(argv[2], &err);
    if (fd < 0) {
        std::fprintf(stderr, "打不开总线 %s：%s\n", argv[2], err.c_str());
        return 1;
    }

    std::printf("监听 %s，%g 秒…（DBC：%zu 个报文）\n\n", argv[2], seconds, pr.db.messages.size());

    int known = 0;
    int unknown = 0;
    auto t0 = std::chrono::steady_clock::now();

    while (true) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= seconds) break;

        std::uint32_t id = 0;
        bool extended = false;
        std::uint8_t data[8] = {0};
        std::uint8_t dlc = 0;

        if (!dbc::canRecv(fd, &id, &extended, data, &dlc, 200)) continue;

        const dbc::Message* msg = pr.db.find(id, extended);
        if (msg == nullptr) { ++unknown; continue; }

        ++known;
        std::printf("[%7.3f] 0x%03X %s%s  [", elapsed, id, msg->name.c_str(),
                    extended ? " 扩展" : "");
        for (int i = 0; i < 8; ++i) std::printf("%02X%s", data[i], i + 1 < 8 ? " " : "");
        std::printf("] dlc=%u\n", dlc);

        dbc::DecodedMessage dm = dbc::decodeMessage(*msg, data);
        if (!dm.ok) {
            std::printf("        解不了：%s\n", dm.error.c_str());
            continue;
        }
        if (dm.hasMux) {
            std::printf("        开关 → %lld\n", static_cast<long long>(dm.selectorValue));
        }

        for (const auto& sv : dm.signals) {
            if (!sv.decoded.ok) {
                std::printf("        %-14s 解不出来：%s\n", sv.name.c_str(),
                            sv.decoded.error.c_str());
                continue;
            }
            if (!sv.active) {
                std::printf("        %-14s （分支未选中）\n", sv.name.c_str());
                continue;
            }
            const dbc::Signal* s = msg->find(sv.name);
            std::printf("        %-14s = %10.3f %-6s", sv.name.c_str(), sv.decoded.value,
                        s != nullptr ? s->unit.c_str() : "");

            if (const std::string* txt = dbc::valueTextFor(*s, sv.decoded.rawSigned)) {
                std::printf(" \"%s\"", txt->c_str());
            }
            if (sv.decoded.rangeDeclared && !sv.decoded.inEngineeringRange) {
                std::printf("  ✗ 超量程(%g..%g)", s->minimum, s->maximum);
            }
            std::printf("\n");
        }
        std::printf("\n");
    }

    dbc::canClose(fd);

    std::printf("--------------------------------\n");
    std::printf("DBC 认得的帧 %d 个，不认识的 %d 个（已安静忽略）\n", known, unknown);
    if (unknown > 0 && known > 0) {
        std::printf("总线是共享的：别的节点的帧不归你管，能忽略才算能用。\n");
    }
    return 0;
}
