#pragma once

#include <cstdint>
#include <cstddef>

namespace mm::platform {

uint32_t millis();
uint32_t micros();

void* alloc(size_t bytes);
void free(void* ptr);

void yield();
size_t freeHeap();          // total free (internal + PSRAM if present)
size_t freeInternalHeap();  // internal RAM only (for stack/HTTP/WiFi reserve check)
size_t maxAllocBlock();     // largest contiguous block (any memory type)
size_t totalHeap();         // total heap capacity (internal + PSRAM)
size_t totalInternalHeap(); // total internal heap capacity

void getMacAddress(uint8_t mac[6]);
const char* chipModel();
const char* sdkVersion();
size_t firmwareSize();        // firmware image bytes
size_t firmwarePartition();   // app partition size (firmware capacity)
size_t flashChipSize();       // total flash chip capacity
size_t filesystemUsed();      // filesystem used bytes
size_t filesystemTotal();     // filesystem total bytes

// Network (ESP32 only, stubs on desktop)
bool ethInit();
bool ethLinkUp();       // PHY link detected (cable plugged, fast check)
bool ethConnected();    // IP assigned (DHCP complete)
void ethGetIP(char* buf, size_t len);

bool wifiStaInit(const char* ssid, const char* password);
bool wifiStaConnected();
void wifiStaGetIP(char* buf, size_t len);
void wifiStaStop();

bool wifiApInit(const char* apName, const char* ip);
bool wifiApConnected();
void wifiApStop();

bool mdnsInit(const char* deviceName);
void mdnsStop();

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

class TcpConnection {
public:
    TcpConnection() = default;
    explicit TcpConnection(int fd) : fd_(fd) {}
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    bool valid() const { return fd_ >= 0; }
    int read(uint8_t* buf, size_t maxLen);   // non-blocking: >0 data, 0 closed, -1 nothing
    bool write(const uint8_t* data, size_t len);
    void close();

private:
    int fd_ = -1;
};

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool open(uint16_t port);
    TcpConnection accept();  // non-blocking, returns invalid if none pending
    void close();

private:
    int fd_ = -1;
};

} // namespace mm::platform
