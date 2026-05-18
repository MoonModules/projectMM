#include "platform/UdpSocket.h"

#ifdef _WIN32
    #include <winsock2.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace mm::platform {

UdpSocketHandle udpOpen() {
    return socket(AF_INET, SOCK_DGRAM, 0);
}

void udpClose(UdpSocketHandle sock) {
    if (sock >= 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }
}

void udpEnableBroadcast(UdpSocketHandle sock) {
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
}

void udpSend(UdpSocketHandle sock, const char* destIP, uint16_t port,
             const void* data, size_t len) {
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, destIP, &dest.sin_addr);
    sendto(sock, reinterpret_cast<const char*>(data), len, 0,
           reinterpret_cast<const struct sockaddr*>(&dest), sizeof(dest));
}

} // namespace mm::platform
