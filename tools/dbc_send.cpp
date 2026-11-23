// ============================================================================
// dbc_send.cpp —— 按 DBC 编一帧，真发到 CAN 总线上（M3.5）
//
// 用法：
//   ./build/dbc-send dbc/sample.dbc vcan0 EngineData -1
//        EngineSpeed=8000 EngineTemp=90 BrakePress=120.4
//   ./build/dbc-send dbc/sample.dbc vcan0 MuxFrame 1 MuxA=45.5
//
// 第三个参数是"报文名"，第四个是分支号（-1 表示这个报文没有多路复用）。
//
// 这一步的意义：帧不再是我们自己拼的字节数组，而是真的进了内核、上了总线。
// 抓包工具能看到它，对面节点也能看到它 —— 这才是"真帧"。
// ============================================================================

#include "CanRaw.h"
#include "Codec.h"
#include "Dbc.h"
#include "Mux.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

bool parseAssignment(const char* arg, std::string* name, double* value) {
    const char* eq = std::strchr(arg, '=');
    if (eq == nullptr) return false;
    *name = std::string(arg, static_cast<std::size_t>(eq - arg));
    *value = std::strtod(eq + 1, nullptr);
    return !name->empty();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "用法：%s <dbc> <接口名> <报文名> <分支号|-1> [信号=值 ...]\n"
                     "例如：%s dbc/sample.dbc vcan0 EngineData -1 EngineSpeed=8000\n",
                     argv[0], argv[0]);
        return 2;
    }

    dbc::ParseResult pr = dbc::parseFile(argv[1]);
    if (!pr.ok) {
        std::fprintf(stderr, "DBC 解析失败（第 %d 行）：%s\n", pr.errorLine, pr.error.c_str());
        return 1;
    }

    const dbc::Message* msg = pr.db.find(argv[3]);
    if (msg == nullptr) {
        std::fprintf(stderr, "DBC 里没有叫 %s 的报文\n", argv[3]);
        return 1;
    }

    const int branch = std::atoi(argv[4]);

    std::map<std::string, double> values;
    for (int i = 5; i < argc; ++i) {
        std::string name;
        double v = 0;
        if (!parseAssignment(argv[i], &name, &v)) {
            std::fprintf(stderr, "看不懂这个参数：%s（要写成 信号名=值）\n", argv[i]);
            return 1;
        }
        values[name] = v;
    }

    if (values.empty()) {
        std::fprintf(stderr, "没给任何信号值，发出去全是 0，没意义\n");
        return 1;
    }

    std::uint8_t data[8] = {0};
    dbc::EncodeMessageResult er = dbc::encodeMessage(*msg, branch, values, data);
    if (!er.ok) {
        std::fprintf(stderr, "编不出来：%s\n", er.error.c_str());
        return 1;
    }

    std::printf("报文 0x%03X %s%s  分支 %d  字节：",
                msg->id, msg->name.c_str(), msg->extended ? "(扩展帧)" : "", branch);
    for (int i = 0; i < 8; ++i) std::printf("%02X ", data[i]);
    std::printf("\n");

    for (const auto& skipped : er.skipped) {
        std::printf("  跳过 %s（不属于分支 %d）\n", skipped.c_str(), branch);
    }

    // 发之前先自己解一遍：这是"写错了立刻知道"的低成本保险
    dbc::DecodedMessage dm = dbc::decodeMessage(*msg, data);
    if (dm.ok) {
        for (const auto& sv : dm.signals) {
            if (!sv.active) continue;
            if (sv.decoded.ok) {
                const dbc::Signal* s = msg->find(sv.name);
                std::printf("  %-14s = %10.3f %s\n", sv.name.c_str(), sv.decoded.value,
                            s != nullptr ? s->unit.c_str() : "");
            }
        }
    }

    std::string err;
    const int fd = dbc::canOpen(argv[2], &err);
    if (fd < 0) {
        std::fprintf(stderr, "打不开总线 %s：%s\n", argv[2], err.c_str());
        return 1;
    }

    if (!dbc::canSend(fd, msg->id, msg->extended, data, static_cast<std::uint8_t>(msg->dlc), &err)) {
        std::fprintf(stderr, "发不出去：%s\n", err.c_str());
        dbc::canClose(fd);
        return 1;
    }
    dbc::canClose(fd);

    std::printf("已发送到 %s。\n", argv[2]);
    return 0;
}
