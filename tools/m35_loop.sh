#!/usr/bin/env bash
# ============================================================================
# m35_loop.sh —— M3.5 真总线闭环自检
#
# 干的事：在一条虚拟 CAN 总线上，先挂监听，再发 4 帧，然后逐条比对解出来的值。
# 这不是单元测试（单元测试在 tests/ 里），这是"端到端"：
#   帧真的进过内核、真的走过总线、真的被抓回来。
#
# 用法：bash tools/m35_loop.sh [接口名，默认 vcan0]
# 前置：vcan0 已 up（bash ~/uds-ecu/tools/vcanup.sh）
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

IFACE=${1:-vcan0}
BUILD=${BUILD:-build}    # 换构建目录（CI 用的是 build-ci，不碰你平时的 build/）
LOG=$(mktemp)
pass=0
fail=0

ok() {
    if grep -qF "$1" "$LOG"; then
        echo "[PASS] $2"
        pass=$((pass + 1))
    else
        echo "[FAIL] $2"
        fail=$((fail + 1))
    fi
}

"$BUILD/dbc-read" dbc/sample.dbc "$IFACE" 4 > "$LOG" 2>&1 &
READER=$!
sleep 0.6

"$BUILD/dbc-send" dbc/sample.dbc "$IFACE" EngineData -1 \
    EngineSpeed=8000 EngineTemp=90 BrakePress=120.4 > /dev/null
"$BUILD/dbc-send" dbc/sample.dbc "$IFACE" MuxFrame 1 MuxA=45.5 > /dev/null
"$BUILD/dbc-send" dbc/sample.dbc "$IFACE" MuxFrame 2 MuxB=88.8 > /dev/null
"$BUILD/dbc-send" dbc/sample.dbc "$IFACE" ExtDiag -1 ExtCounter=7 > /dev/null

wait $READER

ok "EngineSpeed    =   8000.000 rpm"  "0x100 EngineSpeed 解回 8000 rpm"
ok "EngineTemp     =     90.000 degC" "0x100 EngineTemp 解回 90 degC（offset -40 生效）"
ok "BrakePress     =    120.400 bar"  "0x100 BrakePress 解回 120.4 bar（Motorola 跨字节）"
ok "MuxA           =     45.500"      "分支 1 的帧里 MuxA = 45.5"
ok "MuxB           （分支未选中）"     "分支 1 的帧里 MuxB 被判为不存在"
ok "MuxB           =     88.800"      "分支 2 的帧里 MuxB = 88.8"
ok "MuxA           （分支未选中）"     "分支 2 的帧里 MuxA 被判为不存在"
ok "0x1000 ExtDiag 扩展"              "扩展帧 0x1000 被认出（29 位 ID）"
ok "ExtCounter     =      7.000"      "扩展帧里的 ExtCounter = 7"
ok "DBC 认得的帧 4 个"                 "4 帧全部被 DBC 认出来"

echo "--------------------------------"
echo "通过 $pass 项，失败 $fail 项"
if [ "$fail" -ne 0 ]; then
    echo "--- 监听原始输出 ---"
    cat "$LOG"
fi
rm -f "$LOG"
[ "$fail" -eq 0 ] && echo "M3.5 全绿 —— 帧从总线来，一路解成人话。"
exit "$fail"
