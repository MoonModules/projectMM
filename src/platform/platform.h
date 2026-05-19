#pragma once

#include <cstdint>
#include <cstddef>

namespace mm::platform {

uint32_t millis();
uint32_t micros();

void* alloc(size_t bytes);
void free(void* ptr);

void yield();
size_t freeHeap();
size_t maxAllocBlock();

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    bool send(const char* ip, uint16_t port, const uint8_t* data, size_t len);
    void close();

private:
    int fd_ = -1;
};

} // namespace mm::platform
