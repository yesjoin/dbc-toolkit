# dbc-toolkit

一个自研的 **DBC 解析 → 位运算 → 物理值 → 多路复用 → 真总线 → 代码生成** 工具链。
用 C++17 写的，不依赖任何第三方库；生成出来的头文件也不依赖本工程。

配套项目：[`uds-ecu`](../uds-ecu) —— 一个跑在虚拟 CAN 上的 UDS/ISO-TP 虚拟 ECU。
两个项目互为验收工具：`uds-ecu` 造帧，`dbc-toolkit` 读帧。

---

## 它解决什么问题

车上每一帧 CAN 报文都是 8 个字节、64 个 bit，**没有人看得懂**。
看懂它需要一份叫 DBC 的"网络字典"：哪个 ID 是什么报文、每一段 bit 是什么信号、
物理值怎么换算、多路复用怎么切支。

这个工具链把这份字典变成三样东西：

1. **人话**：`dbc-decode` / `dbc-read` 把一帧字节解成 `8000.0 rpm`、`120.4 bar`；
2. **能编译的接口**：`dbc-gen` 把 DBC 变成一个 C++17 头文件，业务代码 `#include` 就能用；
3. **体检报告**：位越界、信号抢位、分支死信号，在编译期就被拦下。

---

## 架构

```
                 dbc/sample.dbc
                      │
   ┌──────────────────┼──────────────────────────────────────────┐
   │ M0  Dbc.cpp      │ 文本 → Database{Message{Signal}}          │  错误带 1 基行号
   ▼                  │                                          │
 Database             │
   │ M1  Bits.cpp     │ (startBit,length,order) → 帧内位掩码 → raw │  Motorola +15 锯齿
   ▼                  │                                          │
 raw (uint64)         │
   │ M2  Codec.cpp    │ 补码扩展 → raw*factor+offset → 物理值      │  位宽 ≠ 量程
   ▼                  │                                          │
 物理值 (double)      │
   │ M3  Mux.cpp      │ 开关值 → 哪一支有效；未选中的 = 不存在     │  分支同址豁免
   ▼                  │                                          │
 DecodedMessage       │
   │ M3.5 CanRaw.cpp  │ SocketCAN: write/read struct can_frame    │  真总线闭环
   ▼                  │                                          │
   vcan0              │
   │ M4  dbc_gen.cpp  │ 同一套规则 → constexpr → static_assert     │  编译期体检
   ▼                  └──────────────────────────────────────────┘
 gen/sample_dbc.h  ← 自包含 C++17 头文件，任何工程可直接 include
```

一条规则贯穿全局：**Motorola（`@0`）的 startBit 是信号的最高位，位号走到字节 bit0 后跳 +15**
（下一字节的最高位）。M1 用它算位掩码，M4 用同一个函数在编译期复查。

---

## 目录

```
include/    Dbc.h  Bits.h  Codec.h  Mux.h  CanRaw.h      ← 五层接口
src/        Dbc.cpp Bits.cpp Codec.cpp Mux.cpp CanRaw.cpp ← 五层实现
tests/      5 个自测夹具（不依赖框架，纯 printf 断言）
tools/      dbc-dump  dbc-decode  dbc-send  dbc-read  dbc-gen
            m35_loop.sh  m4_check.sh                     ← 端到端自检脚本
dbc/        sample.dbc（4 报文 14 信号）  bad_overlap.dbc（故意写坏，验证编译期体检）
```

---

## 构建与测试

```bash
cmake -S . -B build
cmake --build build -j4

./build/dbc-selftest      # M0  64 项
./build/bits-selftest     # M1  56 项
./build/codec-selftest    # M2  73 项
./build/mux-selftest      # M3  55 项
./build/gen-selftest      # M4  31 项（差分：生成代码 vs 运行时库）
```

没有 gtest、没有 catch2。每个夹具就是 `check(cond, "人话描述")` 加最后一行的
`通过 N 项，失败 M 项`。好处是：编译即跑，失败信息直接就是那句话。

端到端（需要 `vcan0`，见配套项目 `uds-ecu/tools/vcanup.sh`）：

```bash
bash tools/m35_loop.sh vcan0   # 真总线闭环，10 项断言
bash tools/m4_check.sh         # 生成物能编译 / 坏 DBC 编不过
```

---

## 工具

### dbc-dump —— 看字典本身

```bash
./build/dbc-dump dbc/sample.dbc
```

```
BO_ 0x100  EngineData     dlc=8  发送=ECU  （6 个信号）
   BrakePress       39|12     Motorola(@0) 无符号  x0.1   [0 .. 400]  "bar"
                    位段：byte4.7-0 + byte5.7-4
```

每个信号底下那行 **位段** 就是 M1 的结论：这段声明到底吃掉了帧里哪几位。
写错的声明会标 `<声明有问题`；同一个报文里抢位的信号会被点名。

### dbc-decode —— 解一帧给定的字节

```bash
./build/dbc-decode dbc/sample.dbc 0x100 64 00 64 00 AB C3 FF FF
```

```
  EngineSpeed    raw=0x0064(  100) →   12.500 rpm     [量程 0..8031.88 ✓]
  BrakePress     raw=0xABC ( 2748) →  274.800 bar     [量程 0..400 ✓]
  GearPosition   raw=0x3   (   3) →    3.000         "D"
  SteeringAngle  raw=0xFFFF(  -1) →   -0.100 deg     [量程 -3276.8..3276.7 ✓]
```

### dbc-read / dbc-send —— 真总线

```bash
./build/dbc-send dbc/sample.dbc vcan0 EngineData -1 EngineSpeed=8000 EngineTemp=90
./build/dbc-send dbc/sample.dbc vcan0 MuxFrame 1 MuxA=45.5
./build/dbc-read dbc/sample.dbc vcan0 5
```

`dbc-read` 对不认识的 ID 只计数、不报错 —— 真实车上大多数流量不归你管。

### dbc-gen —— DBC 变头文件

```bash
./build/dbc-gen dbc/sample.dbc -o gen/sample_dbc.h
```

```cpp
#include "sample_dbc.h"

std::uint8_t frame[8] = {0};
EngineData::Value v{};
EngineData::decode(frame, v);
v.EngineSpeed = 8000;
EngineData::encode(v, frame);
double rpm = EngineData::EngineSpeed_toPhysical(v.EngineSpeed);
```

生成物只依赖 `<cstdint>`，可以被任何 C++17 工程直接 include，不用改它的构建系统。
生成器不写时间戳，同一份 DBC 永远生成同样的字节。

---

## 里程碑

| 里程碑 | 内容 | 测试 | commit |
|---|---|---|---|
| M0 | DBC 解析器（BO_/SG_/VAL_/BU_/VERSION，错误带行号）+ 查看器 | 64 | `ccbcb33` |
| M1 | 位引擎（Intel 递增 / Motorola +15 锯齿，decode/encode，抢位检查） | 56 | `d0051f9` |
| M2 | 编解码层（补码、`raw*factor+offset`、位宽 vs 量程、值表、饱和） | 73 | `9ddbcce` |
| M3 | 多路复用路由（开关识别、分支有效判定、只写当前分支） | 55 | `9f27bbc` |
| M3.5 | SocketCAN 收发，真总线闭环 + 与 `uds-ecu` 共存 | 10 | `7144d20` |
| M4 | 代码生成（自包含头文件 + constexpr 编译期布局检查） | 31 | `f307f72` |
| M5 | 验收报告 + README | — | — |

单元 279 项 + 端到端 12 项 = **291 项断言，全绿**。

---

## 能力边界（明确不做的事）

| 不做 | 原因 |
|---|---|
| CAN FD（64 字节帧） | 当前模型写死 8 字节；FD 需要改帧结构和 dlc 语义 |
| 浮点信号（`SIG_VALTYPE_`） | 只做整数定点信号；浮点会让"raw 是整数"这个前提失效 |
| 环境变量（`EV_`） | 工况相关的信号，车载网络里用得少 |
| 扩展多路复用（`SG_MUL_VAL_`，多开关） | 单开关模型装不下二维选择；遇到此类 DBC 会明确告警而不是猜 |
| 发送周期、超时监控、网关路由 | 那是通信矩阵的事，不是 DBC 解析器的事 |

解析器对 `CM_` / `BA_` / `SIG_GROUP_` 等语句是**跳过**的（读得懂就跳），
只对信号定义相关的关键字做解析 —— 这是"够用"和"完整实现"之间的有意识取舍。

---

## 已知局限

- 多路复用只支持单开关；`SG_MUL_VAL_` 出现时只置位告警，不做解析。
- `parseText` 是行式扫描 + 正则，不处理极端畸形文件（比如字符串里含换行的单位）。
- `Value` 字段存的是 raw。物理值请走 `_toPhysical`，不要自己乘 factor —— 那样 offset 一定会漏。
