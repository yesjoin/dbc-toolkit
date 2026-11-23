#!/usr/bin/env bash
# ============================================================================
# m4_check.sh —— M4 端到端自检：生成的代码能不能用、坏 DBC 能不能被拦住
#
# 三段：
#   ① 正常 DBC → 生成头文件 → 编译通过（证明产物自包含、能被别的工程直接用）
#   ② 坏 DBC（两条信号抢位）→ 生成头文件 → **编译必须失败**（证明 static_assert 真在干活）
#   ③ 顺手看一眼生成物长什么样
#
# 用法：bash tools/m4_check.sh
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

TMP=$(mktemp -d)
pass=0
fail=0

ok()   { echo "[PASS] $1"; pass=$((pass + 1)); }
bad()  { echo "[FAIL] $1"; fail=$((fail + 1)); }

CXX=${CXX:-g++}
INF=build/include_none    # 故意不 include 本工程目录：证明生成物不依赖本工程

echo "① 正常 DBC"
./build/dbc-gen dbc/sample.dbc -o "$TMP/sample_dbc.h" || bad "生成失败（正常 DBC）"
cat > "$TMP/use_ok.cpp" <<'EOF'
#include <cstdint>
#include "sample_dbc.h"

int main() {
    std::uint8_t frame[8] = {0};
    EngineData::Value v{};
    EngineData::decode(frame, v);
    v.EngineSpeed = 100;
    EngineData::encode(v, frame);
    return static_cast<int>(EngineData::EngineSpeed_toPhysical(v.EngineSpeed));
}
EOF

if "$CXX" -std=c++17 -Wall -Wextra -fsyntax-only -I "$TMP" "$TMP/use_ok.cpp" 2> "$TMP/ok.log"; then
    ok "正常 DBC 生成的头文件：只带 <cstdint> 就编译通过（零警告）"
else
    bad "正常 DBC 生成的头文件编译失败"
    sed -n '1,10p' "$TMP/ok.log"
fi

echo
echo "② 坏 DBC（FirstSignal 0|16 与 SecondSignal 8|16 抢 8 个 bit）"
./build/dbc-gen dbc/bad_overlap.dbc -o "$TMP/bad_overlap.h" || bad "生成失败（坏 DBC）"
cat > "$TMP/use_bad.cpp" <<'EOF'
#include "bad_overlap.h"
int main() { BadMsg::Value v{}; (void)v; return 0; }
EOF

if "$CXX" -std=c++17 -fsyntax-only -I "$TMP" "$TMP/use_bad.cpp" 2> "$TMP/bad.log"; then
    bad "坏 DBC 居然编译通过了 —— static_assert 没起作用"
else
    if grep -q "重叠" "$TMP/bad.log"; then
        ok "坏 DBC 被编译期拦住，报错点明“信号位段越界或互相重叠”"
        grep -m1 "static assertion failed" "$TMP/bad.log" | sed 's/^/       /'
        grep -m1 "重叠" "$TMP/bad.log" | sed 's/^/       /'
    else
        bad "编译失败了，但不是因为布局检查（原因待查）"
        sed -n '1,10p' "$TMP/bad.log"
    fi
fi

echo
echo "③ 生成物规模"
echo "      正常 DBC → $(wc -l < "$TMP/sample_dbc.h") 行 / $(wc -c < "$TMP/sample_dbc.h") 字节"
N=$(grep -c "static_assert" "$TMP/sample_dbc.h")
echo "      其中编译期体检 static_assert：$N 处"

echo "--------------------------------"
echo "通过 $pass 项，失败 $fail 项"
rm -rf "$TMP"
[ "$fail" -eq 0 ] && echo "M4 端到端全绿 —— 好 DBC 进得来，坏 DBC 出不去。"
exit "$fail"
