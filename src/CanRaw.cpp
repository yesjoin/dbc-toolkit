// ============================================================================
// CanRaw.cpp —— SocketCAN 原始收发
//
// 只有 Linux 有这套东西（PF_CAN 是内核协议族）。为了让工程在别的系统上
// 也能编译通过，非 Linux 平台给一套"永远失败"的桩，而不是让 CMake 直接挂掉。
// ============================================================================

#include "CanRaw.h"

#include <cstring>

#ifdef __linux__

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dbc {

int canOpen(const char* ifname, std::string* err) {
    if (err != nullptr) err->clear();

    const int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        if (err != nullptr) *err = std::string("socket() 失败：") + std::strerror(errno);
        return -1;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        if (err != nullptr) {
            *err = std::string("接口 ") + ifname + " 不存在：" + std::strerror(errno) +
                   "（虚拟总线先执行 bash ~/uds-ecu/tools/vcanup.sh）";
        }
        ::close(fd);
        return -1;
    }

    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (err != nullptr) *err = std::string("bind() 失败：") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

void canClose(int fd) {
    if (fd >= 0) ::close(fd);
}

bool canSend(int fd, std::uint32_t id, bool extended,
             const std::uint8_t* data, std::uint8_t dlc, std::string* err) {
    struct can_frame frame;
    std::memset(&frame, 0, sizeof(frame));

    frame.can_id  = id | (extended ? CAN_EFF_FLAG : 0u);
    frame.can_dlc = (dlc > 8) ? 8 : dlc;
    if (data != nullptr) std::memcpy(frame.data, data, frame.can_dlc);

    const ssize_t n = ::write(fd, &frame, sizeof(frame));
    if (n != static_cast<ssize_t>(sizeof(frame))) {
        if (err != nullptr) *err = std::string("write() 失败：") + std::strerror(errno);
        return false;
    }
    return true;
}

bool canRecv(int fd, std::uint32_t* id, bool* extended,
             std::uint8_t* data, std::uint8_t* dlc, int timeoutMs) {
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    const int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr <= 0) return false;               // 超时或出错，都当"没收到"

    struct can_frame frame;
    std::memset(&frame, 0, sizeof(frame));
    if (::read(fd, &frame, sizeof(frame)) < 0) return false;

    *extended = (frame.can_id & CAN_EFF_FLAG) != 0;
    *id = *extended ? (frame.can_id & CAN_EFF_MASK) : (frame.can_id & CAN_SFF_MASK);
    *dlc = static_cast<std::uint8_t>(frame.can_dlc > 8 ? 8 : frame.can_dlc);

    std::memset(data, 0, 8);
    std::memcpy(data, frame.data, *dlc);
    return true;
}

} // namespace dbc

#else  // 非 Linux：桩

namespace dbc {

int canOpen(const char* ifname, std::string* err) {
    (void)ifname;
    if (err != nullptr) *err = "SocketCAN 只在 Linux 上可用";
    return -1;
}

void canClose(int fd) { (void)fd; }

bool canSend(int, std::uint32_t, bool, const std::uint8_t*, std::uint8_t, std::string* err) {
    if (err != nullptr) *err = "SocketCAN 只在 Linux 上可用";
    return false;
}

bool canRecv(int, std::uint32_t*, bool*, std::uint8_t*, std::uint8_t*, int) { return false; }

} // namespace dbc

#endif
