#pragma once

#include <cstddef>
#include <cstdint>

namespace mm::platform {

using TcpServerHandle = int;
using TcpClientHandle = int;
constexpr TcpServerHandle INVALID_TCP_HANDLE = -1;

// Open a non-blocking TCP listener on the given port.
TcpServerHandle tcpListen(uint16_t port);

// Accept a pending connection (non-blocking). Returns INVALID_TCP_HANDLE if none.
TcpClientHandle tcpAccept(TcpServerHandle server);

// Read from client (non-blocking). Returns bytes read, 0 if no data, -1 on error/closed.
int tcpRead(TcpClientHandle client, char* buf, size_t maxLen);

// Write to client.
void tcpWrite(TcpClientHandle client, const void* data, size_t len);

// Close a client connection.
void tcpClose(TcpClientHandle client);

// Close the server listener.
void tcpCloseServer(TcpServerHandle server);

} // namespace mm::platform
