#pragma once
// ============================================================================
// Codec.h —— 编解码层（M2）：raw ↔ 物理值
//
// M1 拿到的是"一串 0/1 拼出来的整数"，叫 raw。车上没人关心 raw，
// 人关心的是 582.5 rpm、274.8 bar。这两者之间隔了三层东西：
//
//   ① 符号       raw 是无符号位模式，有符号信号要把它解释成负数（补码）
//   ② 换算       物理值 = raw * factor + offset   ←  方向只有这一个
//   ③ 量程       位宽能表示的范围 ≠ DBC 声明的 [min|max]
//
// 第 ③ 条是新手最容易混的：12 位无符号能装 0..4095，
// 但 BrakePress 的 DBC 声明是 [0|400]，raw 4095 对应 409.5 bar ——
// **装得下，但物理上不合理**。这两件事必须分开报。
// ============================================================================

#include "Bits.h"
#include "Dbc.h"

#include <cstdint>
#include <string>

namespace dbc {

// ---------------------------------------------------------------------------
// 位宽能表示的原始值范围（和 DBC 的 [min|max] 无关）
// ---------------------------------------------------------------------------
void rawRange(const Signal& s, std::int64_t* lo, std::int64_t* hi);

// 补码符号扩展：把 length 位的无符号位模式当有符号整数解释
//   12 位 0xFFF → -1        16 位 0x8000 → -32768
std::int64_t extendSign(std::uint64_t raw, std::uint32_t length);

// 反方向：把有符号整数压回 length 位的无符号位模式（负数按补码截低位）
std::uint64_t packRaw(std::int64_t raw, std::uint32_t length);

// 单步换算，方便单独测
double intToPhysical(const Signal& s, std::int64_t raw);
bool   physicalToInt(const Signal& s, double value, std::int64_t* out, std::string* err);

// VAL_ 值表查询：raw 命中就返回文字，没有就返回 nullptr
const std::string* valueTextFor(const Signal& s, std::int64_t raw);

// ---------------------------------------------------------------------------
// 一次解码的全部结果
// ---------------------------------------------------------------------------
struct DecodeResult {
    bool          ok = false;
    std::string   error;
    std::uint64_t raw = 0;          // 位模式
    std::int64_t  rawSigned = 0;    // 符号扩展后的整数（无符号信号就是 raw 本身）
    double        value = 0.0;      // 物理值
    bool          inEngineeringRange = true;   // 是否落在 DBC 的 [min|max] 内
    bool          rangeDeclared = false;       // DBC 里到底写没写量程
};

DecodeResult decodeSignal(const Signal& s, const std::uint8_t* data);

// ---------------------------------------------------------------------------
// 编码
//   saturate = false（默认）：物理值超出位宽能表示的范围 → 拒绝，一个字节都不写
//   saturate = true         ：夹到边界并置 clamped，适合"发给 ECU 别让它崩"
// ---------------------------------------------------------------------------
struct EncodeResult {
    bool          ok = false;
    std::string   error;
    bool          clamped = false;
    std::uint64_t raw = 0;
};

EncodeResult encodeSignal(const Signal& s, double value, std::uint8_t* data,
                          bool saturate = false);

} // namespace dbc
