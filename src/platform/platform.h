#pragma once

#include <atomic>
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

// This host's LAN IPv4 address as a dotted string, or "" if unavailable.
// Desktop: the outbound interface address. ESP32: empty — the device IP is
// owned by NetworkModule (WiFi/Ethernet), not the platform layer.
const char* hostIp();

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

// OTA — fetch a firmware image from `url` and flash it to the next OTA partition.
// ESP32: spawns a one-shot FreeRTOS task (the call returns immediately; the task
// runs to completion or error). The task uses `esp_https_ota`, which rolls the
// download + partition write + boot-pointer flip into one API; on success it
// calls platform::reboot() so the device boots the new image.
// Desktop: returns false (no OTA partition to write to); call sites guard with
// `if constexpr (mm::platform::hasOta)`.
//
// `statusBuf` is updated in place by the task with a short progress string
// (e.g. "downloading", "flashing", "error: HTTP 404"). `bytesReadOut` /
// `bytesTotalOut` advance as the download proceeds — the UI renders them as
// "X KB / Y KB". `bytesTotalOut` is 0 until esp_https_ota reports the image
// size (just after the HTTPS handshake), then holds the real value for the
// rest of the task's lifetime. FirmwareUpdateModule polls all three at 1 Hz
// and copies into its Control buffers so the WS state push surfaces progress
// to the UI without extra wiring.
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut);

// Improv WiFi provisioning over UART0.
// ESP32 only; desktop stub returns false. Spawns a FreeRTOS task that installs
// a UART driver on UART_NUM_0 (the same channel ESP-IDF logging writes to;
// they coexist because logging uses direct register writes, not the driver).
// The task feeds inbound bytes into the `improv/improv` parser.
//
// On a provision request the task validates state: if wifiStaConnected() is
// true it emits Improv's wrong-state error frame. Otherwise it copies the
// credentials into the caller-owned buffers `ssidOut` / `passwordOut` (sized
// to hold 33 + 64 bytes, matching NetworkModule's storage) and sets `*ready`
// to true. The caller's loop1s() polls `ready`, copies the buffers onward,
// and clears the flag.
//
// `statusBuf` mirrors http_fetch_to_ota's pattern: the task writes short
// strings ("listening", "received credentials", "connecting",
// "connected: <ssid>", "error: <reason>"). ImprovProvisioningModule polls
// it into a read-only Control.
//
// `info` is borrowed; the task copies the strings on init (pass static
// storage like `kVersion` and SystemModule::deviceName()).
struct ImprovDeviceInfo {
    const char* name;            // device hostname, e.g. "MM-3A7F"
    const char* chipFamily;      // "ESP32" / "ESP32-S3" / ...
    const char* firmwareVersion; // e.g. "1.0.0-rc2"
};
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen);

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
