#pragma once

#include <cstdint>
#include <cstddef>

namespace mm::platform {

uint32_t millis();
uint32_t micros();

void* alloc(size_t bytes);
void free(void* ptr);

void yield();
void delayMs(uint32_t ms);  // blocking sleep; only use outside the hot path
size_t freeHeap();          // total free (internal + PSRAM if present)
size_t freeInternalHeap();  // internal RAM only (for stack/HTTP/WiFi reserve check)
size_t maxAllocBlock();     // largest contiguous block (any memory type)
size_t totalHeap();         // total heap capacity (internal + PSRAM)
size_t totalInternalHeap(); // total internal heap capacity

void getMacAddress(uint8_t mac[6]);
const char* chipModel();
const char* sdkVersion();

// Human-readable reset reason: "POWERON", "SW", "PANIC", "INT_WDT", "TASK_WDT",
// "BROWNOUT", "DEEPSLEEP", or "UNKNOWN". On desktop always returns "OK". UI uses
// this to flag a "crashed" prior boot (PANIC / INT_WDT / TASK_WDT / BROWNOUT).
const char* resetReason();
size_t firmwareSize();        // firmware image bytes
size_t firmwarePartition();   // app partition size (firmware capacity)
size_t flashChipSize();       // total flash chip capacity
size_t filesystemUsed();      // filesystem used bytes
size_t filesystemTotal();     // filesystem total bytes

// Filesystem — LittleFS on ESP32, std::filesystem on desktop (rooted at ./.config/'s parent).
// Paths are absolute-looking (start with '/'); desktop strips the leading '/' so
// "/.config/System.json" maps to "<root>/.config/System.json".
//
// fsSetRoot redirects the desktop root from CWD to an absolute path. Used by unit tests
// to give each TEST_CASE an isolated working directory without chdir. No-op on ESP32
// (LittleFS is mounted at a fixed partition). Must be called BEFORE fsMount; defaults
// to ".".
void fsSetRoot(const char* path);
bool fsMount();                                              // idempotent; safe to call multiple times
void fsUnmount();
bool fsMkdir(const char* path);                              // mkdir -p; no error if exists
bool fsExists(const char* path);
bool fsRemove(const char* path);                             // file or empty dir
int  fsRead(const char* path, char* buf, size_t maxLen);     // bytes read; -1 on error; null-terminated on success
bool fsWriteAtomic(const char* path, const char* data, size_t len);
                                                              // writes <path>.tmp, fsync, rename. Caller ensures parent dir exists.
using FsListCb = void(*)(const char* name, bool isDir, void* user);
void fsList(const char* dir, FsListCb cb, void* user);       // single-level listing

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
    // Bind a fixed destination so each sendTo() skips the per-packet address
    // parse + route lookup. Returns false on a bad IP.
    bool connect(const char* ip, uint16_t port);
    bool sendTo(const uint8_t* data, size_t len);  // uses the connect()ed destination
    void close();

private:
    int fd_ = -1;
};

// One contiguous span for a scatter-gather write.
struct WriteChunk { const uint8_t* data; size_t len; };

// Outcome of a non-blocking scatter-gather write (TcpConnection::writeChunks).
enum class WriteResult {
    Complete,    // every byte across all chunks was sent
    WouldBlock,  // socket buffer full, NOTHING was sent — caller may retry later
    Partial,     // some bytes sent — the message is truncated, caller MUST close()
    Error        // socket error — caller MUST close()
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
    bool write(const uint8_t* data, size_t len);  // blocking — retries until all sent

    // Single non-blocking scatter-gather write (one writev). Never blocks.
    // Used for the preview broadcast so a backpressured browser cannot stall
    // the render task. `count` must be 1..MAX_WRITE_CHUNKS.
    static constexpr int MAX_WRITE_CHUNKS = 3;
    WriteResult writeChunks(const WriteChunk* chunks, int count);

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

// Restart the device. On ESP32: hardware reset (esp_restart). On desktop: process exit.
// Does not return.
[[noreturn]] void reboot();

} // namespace mm::platform
