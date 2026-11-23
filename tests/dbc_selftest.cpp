// ============================================================================
// dbc_selftest.cpp —— M0 验收夹具
//
// 只测"文本 → 内存模型"这一段：字段有没有切对、坏输入有没有报对行号。
// 不测位运算（那是 M1 的事），所以这里一条 bit 也不碰。
// ============================================================================

#include "Dbc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#ifndef DBC_SAMPLE
#define DBC_SAMPLE "dbc/sample.dbc"
#endif

namespace {

int gPass = 0;
int gFail = 0;

void check(bool cond, const char* what) {
    if (cond) {
        ++gPass;
        std::printf("[PASS] %s\n", what);
    } else {
        ++gFail;
        std::printf("[FAIL] %s\n", what);
    }
}

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

bool eqStr(const std::string& a, const char* b) {
    return a == b;
}

void testParseSample() {
    dbc::ParseResult r = dbc::parseFile(DBC_SAMPLE);
    check(r.ok, "样例 DBC 能解析");
    if (!r.ok) {
        std::printf("       第 %d 行：%s\n", r.errorLine, r.error.c_str());
        return;
    }

    const dbc::Database& db = r.db;

    check(eqStr(db.version, "2.0"), "VERSION 抓到了");
    check(db.nodes.size() == 4, "BU_ 解析出 4 个节点");
    check(db.nodes.size() == 4 && eqStr(db.nodes[0], "ECU"), "节点顺序按原文（第一个是 ECU）");
    check(db.messages.size() == 4, "解析出 4 个报文");
    check(db.signalCount() == 14, "一共 14 个信号");

    const dbc::Message* engine = db.find("EngineData");
    check(engine != nullptr, "按名字能查到 EngineData");
    if (engine == nullptr) return;

    check(engine->id == 256, "EngineData 的 ID = 256");
    check(!engine->extended, "EngineData 是标准帧");
    check(engine->dlc == 8, "EngineData 的 DLC = 8");
    check(eqStr(engine->transmitter, "ECU"), "EngineData 发送节点 = ECU");
    check(engine->signals.size() == 6, "EngineData 有 6 个信号");

    const dbc::Signal* speed = engine->find("EngineSpeed");
    check(speed != nullptr, "查到 EngineSpeed");
    if (speed != nullptr) {
        check(speed->startBit == 0, "EngineSpeed 起始位 = 0");
        check(speed->length == 16, "EngineSpeed 长度 = 16");
        check(speed->order == dbc::ByteOrder::Intel, "EngineSpeed 是 Intel(@1)");
        check(!speed->isSigned, "EngineSpeed 无符号");
        check(near(speed->factor, 0.125), "EngineSpeed factor = 0.125");
        check(near(speed->offset, 0.0), "EngineSpeed offset = 0");
        check(near(speed->maximum, 8031.875), "EngineSpeed max = 8031.875");
        check(eqStr(speed->unit, "rpm"), "EngineSpeed 单位 = rpm");
        check(speed->receivers.size() == 2 &&
              eqStr(speed->receivers[0], "TCU") &&
              eqStr(speed->receivers[1], "ABS"), "EngineSpeed 接收节点 = TCU,ABS");
        check(speed->mux.empty(), "EngineSpeed 不参与多路复用");
    }

    const dbc::Signal* temp = engine->find("EngineTemp");
    check(temp != nullptr && near(temp->offset, -40.0), "EngineTemp 的 offset = -40（负数）");
    check(temp != nullptr && near(temp->minimum, -40.0), "EngineTemp 的 min = -40（负数）");

    const dbc::Signal* brake = engine->find("BrakePress");
    check(brake != nullptr, "查到 BrakePress");
    if (brake != nullptr) {
        check(brake->startBit == 39, "BrakePress 起始位 = 39");
        check(brake->length == 12, "BrakePress 长度 = 12（跨字节）");
        check(brake->order == dbc::ByteOrder::Motorola, "BrakePress 是 Motorola(@0)");
        check(!brake->isSigned, "BrakePress 无符号");
    }

    const dbc::Signal* steering = engine->find("SteeringAngle");
    check(steering != nullptr && steering->isSigned, "SteeringAngle 是有符号");
    check(steering != nullptr && steering->order == dbc::ByteOrder::Motorola,
          "SteeringAngle 是 Motorola(@0)");

    const dbc::Message* abs = db.find("AbsStatus");
    check(abs != nullptr && abs->signals.size() == 4, "AbsStatus 有 4 个信号");
    if (abs != nullptr) {
        const dbc::Signal* fl = abs->find("WheelSpeedFL");
        check(fl != nullptr && near(fl->factor, 0.01), "WheelSpeedFL factor = 0.01");
        const dbc::Signal* lamp = abs->find("AbsLamp");
        check(lamp != nullptr && lamp->length == 1, "AbsLamp 是 1 bit 信号");
    }

    const dbc::Message* mux = db.find("MuxFrame");
    check(mux != nullptr && mux->signals.size() == 3, "MuxFrame 有 3 个信号");
    if (mux != nullptr) {
        const dbc::Signal* sel = mux->find("MuxSel");
        check(sel != nullptr && eqStr(sel->mux, "M"), "MuxSel 是复用开关（M）");
        const dbc::Signal* a = mux->find("MuxA");
        check(a != nullptr && eqStr(a->mux, "m1"), "MuxA 属于第 1 支（m1）");
        const dbc::Signal* b = mux->find("MuxB");
        check(b != nullptr && eqStr(b->mux, "m2"), "MuxB 属于第 2 支（m2）");
    }

    const dbc::Message* ext = db.find("ExtDiag");
    check(ext != nullptr, "查到 ExtDiag");
    if (ext != nullptr) {
        check(ext->extended, "ExtDiag 是扩展帧");
        check(ext->id == 0x1000, "扩展帧 ID 剥掉 bit31 标志后 = 0x1000");
    }

    check(db.find(0x1000, true) != nullptr, "按 (id, extended) 能查到扩展帧");
    check(db.find(0x1000, false) == nullptr, "同一 ID 按标准帧查不到扩展帧（标志位有区分作用）");

    const dbc::Signal* gear = engine->find("GearPosition");
    check(gear != nullptr && gear->valueTable.size() == 4, "GearPosition 的 VAL_ 有 4 条");
    if (gear != nullptr && gear->valueTable.size() == 4) {
        check(gear->valueTable[0].value == 0 && eqStr(gear->valueTable[0].text, "P"),
              "值表第 1 条：0 = P");
        check(gear->valueTable[3].value == 3 && eqStr(gear->valueTable[3].text, "D"),
              "值表第 4 条：3 = D");
    }

    if (abs != nullptr) {
        const dbc::Signal* fault = abs->find("AbsFaultCode");
        check(fault != nullptr && fault->valueTable.size() == 4, "AbsFaultCode 的 VAL_ 有 4 条");
        if (fault != nullptr && fault->valueTable.size() == 4) {
            check(fault->valueTable[3].value == 255 && eqStr(fault->valueTable[3].text, "Unknown"),
                  "值表末条：255 = Unknown");
        }
    }
}

void testBadInput() {
    // 1) 信号长度为 0
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 8 ECU\n"
            " SG_ Bad : 0|0@1+ (1,0) [0|1] \"\" ECU\n");
        check(!r.ok && r.errorLine == 2, "长度为 0 的信号被拒，行号 2");
    }

    // 2) SG_ 出现在 BO_ 之前
    {
        dbc::ParseResult r = dbc::parseText(
            " SG_ X : 0|8@1+ (1,0) [0|1] \"\" ECU\n");
        check(!r.ok && r.errorLine == 1, "没有 BO_ 就写 SG_ 被拒，行号 1");
    }

    // 3) DLC 超过 8
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 9 ECU\n"
            " SG_ X : 0|8@1+ (1,0) [0|1] \"\" ECU\n");
        check(!r.ok && r.errorLine == 1, "DLC=9 被拒，行号 1");
    }

    // 4) BO_ 行本身不合法
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M 8 ECU\n");
        check(!r.ok && r.errorLine == 1, "BO_ 缺冒号被拒，行号 1");
    }

    // 5) 起始位超出 DLC
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 8 ECU\n"
            " SG_ X : 64|8@1+ (1,0) [0|1] \"\" ECU\n");
        check(!r.ok && r.errorLine == 2, "起始位 64 超出 8 字节帧，行号 2");
    }

    // 6) VAL_ 引用了不存在的信号
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 8 ECU\n"
            " SG_ X : 0|8@1+ (1,0) [0|1] \"\" ECU\n"
            "VAL_ 256 Nope 0 \"A\" ;\n");
        check(!r.ok && r.errorLine == 3, "VAL_ 引用不存在信号被拒，行号 3");
    }

    // 7) 信号重名
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 8 ECU\n"
            " SG_ X : 0|8@1+ (1,0) [0|1] \"\" ECU\n"
            " SG_ X : 8|8@1+ (1,0) [0|1] \"\" ECU\n");
        check(!r.ok && r.errorLine == 3, "同报文信号重名被拒，行号 3");
    }
}

void testEdgeForms() {
    // 科学计数法：1E-1 / 1E+2
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 1 Sci: 8 ECU\n"
            " SG_ S : 0|8@1+ (1E-1,0) [0|1E+2] \"\" ECU\n");
        check(r.ok, "科学计数法能解析");
        if (r.ok && !r.db.messages.empty() && !r.db.messages[0].signals.empty()) {
            const dbc::Signal& s = r.db.messages[0].signals[0];
            check(near(s.factor, 0.1), "1E-1 解析成 0.1");
            check(near(s.maximum, 100.0), "1E+2 解析成 100");
        }
    }

    // 跨行的 CM_ / VAL_：必须攒到分号
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 256 M: 8 ECU\n"
            " SG_ X : 0|8@1+ (1,0) [0|1] \"\" ECU\n"
            "CM_ BO_ 256 \"first line\n"
            "second line\n"
            "third line\";\n"
            "VAL_ 256 X 0 \"Zero\"\n"
            " 1 \"One\" ;\n");
        check(r.ok, "跨行的 CM_ / VAL_ 能正确吞行");
        if (r.ok) {
            check(r.db.messages.size() == 1 && r.db.messages[0].signals.size() == 1,
                  "跨行语句没有破坏后续解析");
            check(r.db.messages[0].signals[0].valueTable.size() == 2,
                  "跨行 VAL_ 的两条值都收进了");
        }
    }

    // 全空文件
    {
        dbc::ParseResult r = dbc::parseText("\n\n\n");
        check(r.ok && r.db.messages.empty(), "空文件解析成功且 0 报文");
    }

    // 单位里带空格
    {
        dbc::ParseResult r = dbc::parseText(
            "BO_ 2 U: 8 ECU\n"
            " SG_ S : 0|8@1+ (1,0) [0|1] \"km per hour\" ECU\n");
        check(r.ok && eqStr(r.db.messages[0].signals[0].unit, "km per hour"),
              "带空格的单位完整保留");
    }
}

} // namespace

int main() {
    std::printf("=== DBC 解析器自测（M0）===\n");
    std::printf("样例文件：%s\n\n", DBC_SAMPLE);

    std::printf("-- 正常解析 --\n");
    testParseSample();

    std::printf("\n-- 坏输入 --\n");
    testBadInput();

    std::printf("\n-- 边界写法 --\n");
    testEdgeForms();

    std::printf("\n--------------------------------\n");
    std::printf("通过 %d 项，失败 %d 项\n", gPass, gFail);
    if (gFail == 0) std::printf("M0 全绿 —— 文件读得懂了，才能谈位怎么算。\n");
    return (gFail == 0) ? 0 : 1;
}
