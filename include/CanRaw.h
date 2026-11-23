#pragma once
// ============================================================================
// CanRaw.h —— 裸 SocketCAN 收发（M3.5）
//
// 这一层刻意不用 libsocketcan，也不依赖 can-utils：
// 直接对着 Linux 内核的 PF_CAN 套接字说话，这样才能看清"帧"到底是什么。
//
//   socket(PF_CAN, SOCK_RAW, CAN_RAW)   ← 建一个 CAN 原始套接字
//   ioctl(SIOCGIFINDEX)                 ← 用接口名（vcan0）换内核里的接口号
//   bind()                              ← 把这个套接字钉在 vcan0 上
//   write(can_frame) / read(can_frame)  ← 发/收一帧
//
// struct can_frame 就是 CAN 帧的全部真相：11/29 位 ID、dlc、8 个数据字节。
// 顺带说一句：dlc 能写 0..8，但 CAN 允许 dlc 字段填 9..15（真实长度仍是 8），
// 那是给 CAN FD 之前的老设备留的兼容位——本项目只用真实长度。
// ============================================================================

#include <cstdint>
#include <string>

namespace dbc {

// 返回 fd，失败返回 -1 并在 err 里写人话
int  canOpen(const char* ifname, std::string* err);
void canClose(int fd);

bool canSend(int fd, std::uint32_t id, bool extended,
             const std::uint8_t* data, std::uint8_t dlc, std::string* err);

// 等一帧，超时返回 false。data 总是 8 字节（不足补 0）
bool canRecv(int fd, std::uint32_t* id, bool* extended,
             std::uint8_t* data, std::uint8_t* dlc, int timeoutMs);

} // namespace dbc
