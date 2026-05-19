#include "platform/platform.h"

#include <chrono>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace mm::platform {

static auto startTime = std::chrono::steady_clock::now();

uint32_t millis() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );
}

uint32_t micros() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count()
    );
}

void* alloc(size_t bytes) {
    return std::malloc(bytes);
}

void free(void* ptr) {
    std::free(ptr);
}

void yield() {
    // No-op on desktop — OS scheduler handles threading
}

size_t freeHeap() {
    return 0; // Not meaningful on desktop
}

size_t maxAllocBlock() {
    return 0; // Not meaningful on desktop
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    return fd_ >= 0;
}

bool UdpSocket::send(const char* ip, uint16_t port, const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;

    auto sent = ::sendto(fd_, data, len, 0,
                         reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace mm::platform
