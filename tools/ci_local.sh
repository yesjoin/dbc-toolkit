#!/usr/bin/env bash
# ============================================================================
# ci_local.sh —— 在本地把 GitHub Actions 的每一步原样跑一遍
#
# 为什么要有这个东西：
#   推上去才知道红的滋味不好受 —— 等 2 分钟、看 200 行日志、还不一定能复现。
#   CI 的价值不在"跑在云上"，而在"跑的是一组确定的步骤"。既然步骤是确定的，
#   就能在本地按同一顺序、同一参数跑一遍。这个脚本就是那组步骤。
#
# 用法：
#   bash tools/ci_local.sh            # 走 gcc 腿（带 -Werror，和 CI 的 gcc 腿一致）
#   bash tools/ci_local.sh clang      # 走 clang 腿（不开 -Werror）
#
# 环境变量：
#   BUILD=build-ci      构建目录，故意不叫 build/，免得覆盖你平时的增量构建
#   SKIP_BUS=1          跳过总线端到端（虚拟机没 vcan0 时用）
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

CC_NAME=${1:-gcc}
BUILD=${BUILD:-build-ci}

pass=0
fail=0
ok()  { echo "[PASS] $1"; pass=$((pass + 1)); }
bad() { echo "[FAIL] $1"; fail=$((fail + 1)); }
step() { printf '\n=== %s ===\n' "$1"; }

LOG=$(mktemp -d)
trap 'rm -rf "$LOG"' EXIT

# ---------------------------------------------------------------------------
# 1 选编译器：CI 的矩阵腿就是这么切的
# ---------------------------------------------------------------------------
step "1/7 编译器"
if [ "$CC_NAME" = clang ]; then
    export CC=clang CXX=clang++
    WERROR=0
else
    export CC=gcc CXX=g++
    WERROR=1     # CI 的 gcc 腿开 -Werror
fi
echo "      CC=$CC  CXX=$CXX"
if ! command -v "$CXX" > /dev/null 2>&1; then
    echo "[SKIP] 这台机器上没有 $CXX（GitHub 的 runner 上 clang 是预装的）。跳过这条腿。"
    exit 0
fi
$CXX --version | head -1 | sed 's/^/      /'

# ---------------------------------------------------------------------------
# 2 配置：删掉旧目录，模拟 CI 的"干净 checkout"
# ---------------------------------------------------------------------------
step "2/7 配置（干净目录，等价于 CI 全新克隆）"
rm -rf "$BUILD"
EXTRA=""
[ "$WERROR" -eq 1 ] && EXTRA="-DDBC_WERROR=ON"
echo "      cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Debug $EXTRA"
if cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug $EXTRA > "$LOG/cfg.log" 2>&1; then
    ok "cmake 配置成功"
else
    bad "cmake 配置失败"
    tail -n 15 "$LOG/cfg.log" | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
# 3 构建：警告在这里决定生死
# ---------------------------------------------------------------------------
step "3/7 构建 -Wall -Wextra -Wpedantic $([ "$WERROR" -eq 1 ] && echo '+ -Werror')"
if cmake --build "$BUILD" -j"$(nproc)" > "$LOG/build.log" 2>&1; then
    ok "构建成功（生成的 sample_dbc.h 也在这一步产出）"
else
    bad "构建失败"
    grep -m 10 -E "error:|Error " "$LOG/build.log" | sed 's/^/      /'
fi

W=$(grep -c "warning:" "$LOG/build.log" 2>/dev/null) || W=0
if [ "$W" -eq 0 ]; then
    ok "零编译警告"
else
    echo "[WARN] 有 $W 条编译警告$([ "$WERROR" -eq 1 ] && echo '（-Werror 已让构建失败）')"
    grep -m 5 "warning:" "$LOG/build.log" | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
# 4 单元测试：ctest 一网打尽 6 个 target
# ---------------------------------------------------------------------------
step "4/7 单元测试（ctest：4 套自测 + 1 套差分测试）"
if (cd "$BUILD" && ctest --output-on-failure > "$LOG/ctest.log" 2>&1); then
    ok "ctest 全绿"
else
    bad "ctest 有失败"
    tail -n 30 "$LOG/ctest.log" | sed 's/^/      /'
fi
echo "      用例明细："
grep -E "^ *[0-9]+/[0-9]+ Test" "$LOG/ctest.log" | sed 's/^/      /' || true

# ---------------------------------------------------------------------------
# 5 生成物可复现
#   三个变量各换一次，字节都得一样：时区、locale、**入参路径的写法**。
#   第三条是 CI 接进来当天抓到的真问题：生成物注释里原来写的是完整路径，
#   用 `dbc/sample.dbc` 和用绝对路径生成出来差 21 个字节，diff 里凭空一片红。
#   修完就把它钉成测试，防止有人改回去。
# ---------------------------------------------------------------------------
step "5/7 生成物可复现（换时区 / 换 locale / 换路径写法，逐字节比）"
"$BUILD/dbc-gen" dbc/sample.dbc -o "$LOG/a.h" > /dev/null 2>&1
TZ=America/New_York LANG=C LC_ALL=C "$BUILD/dbc-gen" dbc/sample.dbc -o "$LOG/b.h" > /dev/null 2>&1
if cmp -s "$LOG/a.h" "$LOG/b.h"; then
    ok "换时区 + 换 locale 后逐字节相同：$(md5sum "$LOG/a.h" | cut -d' ' -f1)"
else
    bad "换时区/locale 后不一致 —— 生成器混进了环境依赖"
    diff "$LOG/a.h" "$LOG/b.h" | head -n 10 | sed 's/^/      /'
fi

"$BUILD/dbc-gen" "$PWD/dbc/sample.dbc" -o "$LOG/c.h" > /dev/null 2>&1
if cmp -s "$LOG/a.h" "$LOG/c.h"; then
    ok "用绝对路径入参也逐字节相同（生成物只记文件名，不记路径写法）"
else
    bad "绝对路径 vs 相对路径生成不一致 —— 注释里混进了调用路径"
    diff "$LOG/a.h" "$LOG/c.h" | head -n 4 | sed 's/^/      /'
fi
echo "      规模：$(wc -l < "$LOG/a.h") 行 / $(wc -c < "$LOG/a.h") 字节"

# ---------------------------------------------------------------------------
# 6 端到端：好 DBC 能编、坏 DBC 编不过（不需要总线）
# ---------------------------------------------------------------------------
step "6/7 端到端：生成物 + 坏 DBC 拦截"
if BUILD="$BUILD" bash tools/m4_check.sh; then
    ok "m4_check 全绿"
else
    bad "m4_check 有失败"
fi

# ---------------------------------------------------------------------------
# 7 端到端：真总线（CI 里是一个独立 job，拿不到 vcan 就跳过）
# ---------------------------------------------------------------------------
step "7/7 端到端：真 SocketCAN（vcan0）"
if [ "${SKIP_BUS:-0}" = 1 ]; then
    echo "[SKIP] SKIP_BUS=1"
elif ! ip link show vcan0 > /dev/null 2>&1; then
    echo "[SKIP] 没有 vcan0（起一条：bash ~/uds-ecu/tools/vcanup.sh）"
else
    if BUILD="$BUILD" bash tools/m35_loop.sh vcan0; then
        ok "m35_loop 全绿（4 帧从总线回来全部解对）"
    else
        bad "m35_loop 有失败"
    fi
fi

# ---------------------------------------------------------------------------
echo
echo "--------------------------------"
echo "通过 $pass 项，失败 $fail 项"
if [ "$fail" -ne 0 ]; then
    echo "先修红的，再推 Git —— CI 上会遇到的是同一批问题。"
    exit 1
fi
echo "本地这条腿全绿。推上去 CI 应该也是绿的；要是红了，说明差异在环境上（编译器版本 / 内核模块 / 权限），不是代码。"
exit 0
