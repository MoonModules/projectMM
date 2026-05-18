#include "platform/TcpServer.h"
#include <cerrno>

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace mm::platform {

TcpServerHandle tcpListen(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return INVALID_TCP_HANDLE;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    // Set non-blocking
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return INVALID_TCP_HANDLE;
    }

    if (listen(sock, 8) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return INVALID_TCP_HANDLE;
    }

    return sock;
}

TcpClientHandle tcpAccept(TcpServerHandle server) {
    struct sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int client = accept(server, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
    if (client < 0) return INVALID_TCP_HANDLE;
    return client;
}

int tcpRead(TcpClientHandle client, char* buf, size_t maxLen) {
    auto n = recv(client, buf, maxLen, 0);
    if (n < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
        return -1;
    }
    return static_cast<int>(n);
}

void tcpWrite(TcpClientHandle client, const void* data, size_t len) {
    send(client, reinterpret_cast<const char*>(data), len, 0);
}

void tcpClose(TcpClientHandle client) {
    if (client >= 0) {
#ifdef _WIN32
        closesocket(client);
#else
        close(client);
#endif
    }
}

void tcpCloseServer(TcpServerHandle server) {
    if (server >= 0) {
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
    }
}

} // namespace mm::platform
