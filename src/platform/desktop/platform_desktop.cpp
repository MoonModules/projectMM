#include "platform/platform.h"

#include <chrono>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

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
    // No-op on desktop — OS scheduler handles threading.
    // Socket reads use SO_RCVTIMEO for blocking with timeout.
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

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    auto n = ::read(fd_, buf, maxLen);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0; // peer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1; // nothing available
    return 0; // error → treat as closed
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    size_t sent = 0;
    while (sent < len) {
        auto n = ::write(fd_, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue; // interrupted by signal, retry
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, nullptr);
        } else {
            return false;
        }
    }
    return true;
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (::listen(fd_, 8) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();

    // Set read timeout (2 seconds) instead of non-blocking
    struct timeval tv = {2, 0};
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace mm::platform
