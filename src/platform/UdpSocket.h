#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform {

// Opaque UDP socket handle
using UdpSocketHandle = int;
constexpr UdpSocketHandle INVALID_SOCKET_HANDLE = -1;

UdpSocketHandle udpOpen();
void udpClose(UdpSocketHandle sock);
void udpEnableBroadcast(UdpSocketHandle sock);
void udpSend(UdpSocketHandle sock, const char* destIP, uint16_t port,
             const void* data, size_t len);

} // namespace mm::platform
