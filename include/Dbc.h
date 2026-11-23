#pragma once
// ============================================================================
// Dbc.h —— DBC 内存模型（M0）
//
// DBC 是一份"网络字典"：哪个 ID 是什么报文、每个 bit 段是什么信号、
// 物理值怎么换算。这个头文件只定义"解析出来长什么样"，不做任何位运算。
//
// 边界：
//   · 只管 8 字节帧（CAN FD 的 64 字节留了口子，但 M0 只保证 8）
//   · 只管整数信号（factor/offset 是 double，但信号本身是定点整数）
//   · 多路复用只"识别并记住"，不做运行时路由（那是 M3）
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace dbc {

// DBC 里 @0 是 Motorola（大端，默认），@1 是 Intel（小端）。
// 这两个名字最容易记反，所以枚举顺序刻意按 DBC 里的数值排。
enum class ByteOrder {
    Motorola = 0,   // @0，大端，start bit 是信号的 MSB
    Intel    = 1,   // @1，小端，start bit 是信号的 LSB
};

const char* orderName(ByteOrder o);

// VAL_ 里的一条：原始值 → 文字
struct ValueDescription {
    std::int64_t value = 0;
    std::string  text;
};

struct Signal {
    std::string name;

    // 位段。startBit 的含义随 order 变：Intel 是 LSB，Motorola 是 MSB。
    // 这个差别是整个项目的核心坑，M1 会专门处理。
    std::uint32_t startBit = 0;
    std::uint32_t length   = 0;

    ByteOrder order    = ByteOrder::Motorola;
    bool      isSigned = false;

    double factor = 1.0;    // 物理值 = 原始值 * factor + offset
    double offset = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;

    std::string unit;
    std::vector<std::string> receivers;   // 接收节点名

    // 多路复用："" = 普通信号，"M" = 复用开关（决定当前是哪一支），
    // "mN" = 属于第 N 支。M3 才用它做路由，M0 只负责存对。
    std::string mux;

    std::vector<ValueDescription> valueTable;
};

struct Message {
    std::uint32_t id = 0;          // 已剥掉扩展帧标志的纯净 ID
    bool          extended = false;
    std::string   name;
    std::uint32_t dlc = 8;
    std::string   transmitter;
    std::vector<Signal> signals;

    const Signal* find(const std::string& signalName) const;
};

struct Database {
    std::string version;
    std::vector<std::string> nodes;       // BU_ 里列出的节点
    std::vector<Message>     messages;

    const Message* find(std::uint32_t id, bool extended) const;
    const Message* find(const std::string& name) const;
    std::size_t    signalCount() const;
};

// 解析结果。失败时 errorLine 是 1 基行号，error 是人话。
struct ParseResult {
    bool        ok = false;
    int         errorLine = 0;
    std::string error;
    Database    db;
};

ParseResult parseText(const std::string& text);
ParseResult parseFile(const char* path);

} // namespace dbc
