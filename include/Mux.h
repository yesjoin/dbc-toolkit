#pragma once
// ============================================================================
// Mux.h —— 多路复用路由（M3）
//
// 一句话：一个报文里的 8 个字节，可以按"开关值"切成好几种互不相同的含义。
//
//   BO_ 768 MuxFrame: 8 GATEWAY
//    SG_ MuxSel M  : 0|8@1+ ...      ← 开关（"M"）。它的值决定下面读哪一支
//    SG_ MuxA   m1 : 8|16@1+ ...     ← 第 1 支。只有 MuxSel == 1 时才有意义
//    SG_ MuxB   m2 : 8|16@1+ ...     ← 第 2 支。只有 MuxSel == 2 时才有意义
//
// 三条铁律：
//   ① 开关必须显式声明为 "M"，且一个报文最多一个 —— 两个开关等于两套坐标系，
//      DBC 表达不了，那是扩展多路复用（SG_MUL_VAL_），本工具不支持。
//   ② 有 mN 分支却没有 M 开关 → 分支永远选不中，等于死信号，必须报错。
//   ③ 未选中的分支**不是 0**，是"不存在"。给它编个 0.0 是典型的错，
//      下游会把它当成"刹车压力为 0"，而不是"这帧没发刹车压力"。
//
// ============================================================================

#include "Codec.h"
#include "Dbc.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dbc {

// "m1"/"m2" 这类分支标记
bool isBranchMux(const std::string& mux);
int  branchNumber(const std::string& mux);      // "m1" → 1；不是分支 → -1

// ---------------------------------------------------------------------------
// 报文的多路复用结构
// ---------------------------------------------------------------------------
struct MuxInfo {
    bool        ok = true;
    std::string error;              // 结构性问题：有两个开关 / 有分支没开关
    const Signal* selector = nullptr;
    int         maxBranch = 0;      // 出现过的最大分支号
    std::vector<int> branches;      // 出现过的分支号（已排序去重）
    std::vector<std::string> warnings;
};

MuxInfo analyzeMux(const Message& m);

// ---------------------------------------------------------------------------
// 解一帧：每条信号给出"该不该读"
// ---------------------------------------------------------------------------
struct SignalValue {
    std::string   name;
    bool          active = false;     // false = 这条分支没被选中，值是"不存在"
    DecodeResult  decoded;
};

struct DecodedMessage {
    bool        ok = false;
    std::string error;
    bool        hasMux = false;
    std::int64_t selectorValue = 0;   // 开关读到的原始值（分支号就是它）
    std::vector<SignalValue> signals;
};

DecodedMessage decodeMessage(const Message& m, const std::uint8_t* data);

// ---------------------------------------------------------------------------
// 编一帧：指定分支号，写开关 + 只写这一支的信号
//   values 里给谁写谁；没给的不动（保留帧里原值）。
//   branch < 0 表示这个报文没有多路复用。
// ---------------------------------------------------------------------------
struct EncodeMessageResult {
    bool        ok = false;
    std::string error;
    std::vector<std::string> skipped;   // 因为分支不对被跳过的信号名
};

EncodeMessageResult encodeMessage(const Message& m, int branch,
                                  const std::map<std::string, double>& values,
                                  std::uint8_t* data);

// ---------------------------------------------------------------------------
// M3 版的抢位检查：只看"此刻同时有效"的信号。
// M1 的 findOverlaps 是静态两两比（靠 mux 字符串豁免），
// 这一版是真的按当前帧算一遍 —— 如果两支同时被选中还重叠，才是真错。
// ---------------------------------------------------------------------------
std::vector<Overlap> findActiveOverlaps(const Message& m, const std::uint8_t* data);

} // namespace dbc
