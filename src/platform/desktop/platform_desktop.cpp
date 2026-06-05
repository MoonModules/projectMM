#include "platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

namespace mm::platform {

static auto startTime = std::chrono::steady_clock::now();
// Test-only override for millis(); 0 means "use the real clock". std::atomic so
// a test can set it from one thread while a tested module reads from another.
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

uint32_t millis() {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
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

void delayMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

size_t freeHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t freeInternalHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t maxAllocBlock() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t maxInternalAllocBlock() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t totalHeap() {
    return 0; // Not meaningful on desktop
}

size_t totalInternalHeap() {
    return 0; // Not meaningful on desktop
}

void getMacAddress(uint8_t mac[6]) {
    // Stable fake MAC for desktop (consistent deviceName across runs)
    mac[0] = 0xDE; mac[1] = 0xAD; mac[2] = 0xBE;
    mac[3] = 0xEF; mac[4] = 0xCA; mac[5] = 0xFE;
}

const char* chipModel() {
    return "desktop";
}

const char* hostIp() {
    // Resolve the outbound-interface address. A UDP socket connect() sends no
    // packet — it just selects the route — so getsockname() then yields this
    // host's LAN IP. Cached after the first call. "" if offline.
    static char ip[INET_ADDRSTRLEN] = {};
    if (ip[0]) return ip;
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        }
    }
    ::close(fd);
    return ip;
}

const char* sdkVersion() {
#ifdef __clang__
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

const char* resetReason() {
    // Desktop has no reset-reason concept; report a benign value the UI treats as "not crashed".
    return "OK";
}

size_t firmwareSize() { return 0; }
size_t firmwarePartition() { return 0; }
size_t flashChipSize() { return 0; }

// Filesystem — std::filesystem rooted at fsRoot_ (default "build", overridable via fsSetRoot).
// A leading '/' in the API path maps to root-relative. Default lives under build/ so the
// desktop-created .config/ is gitignored (along with the rest of build/) and doesn't clutter
// the repo root. Tests override this to a tmpdir via fsSetRoot for isolation.

namespace {
std::filesystem::path fsRoot_{"build"};

// Map "/.config/foo.json" → "<root>/.config/foo.json". Strips leading '/'s, normalizes
// the result, and rejects paths that escape fsRoot_ (e.g. "../../etc/passwd"). Returns
// an empty path on rejection; callers already treat empty/nonexistent as failure.
std::filesystem::path toFsPath(const char* path) {
    if (!path) return {};
    while (*path == '/') path++;  // strip any number of leading slashes
    std::filesystem::path candidate = (fsRoot_ / path).lexically_normal();
    std::filesystem::path rootNormal = fsRoot_.lexically_normal();
    // Prefix check on the normalized string: candidate must start with rootNormal followed
    // by either end-of-string or a separator. Iterator comparison is more robust against
    // trailing-slash quirks; mismatched_first signals an escape.
    auto [r, c] = std::mismatch(rootNormal.begin(), rootNormal.end(),
                                candidate.begin(), candidate.end());
    if (r != rootNormal.end()) return {};  // candidate diverges before consuming all of rootNormal
    return candidate;
}
}

void fsSetRoot(const char* path) {
    fsRoot_ = (path && *path) ? std::filesystem::path(path) : std::filesystem::path("build");
}

bool fsMount() {
    // No mount needed on desktop; OS handles it.
    return true;
}

void fsUnmount() {}

bool fsMkdir(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(path), ec);
    return !ec;
}

bool fsExists(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(path), ec);
}

bool fsRemove(const char* path) {
    std::error_code ec;
    return std::filesystem::remove(toFsPath(path), ec);
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!buf || maxLen == 0) return -1;
    FILE* f = std::fopen(toFsPath(path).c_str(), "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::fflush(f);
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
    std::fclose(f);

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!cb) return;
    std::error_code ec;
    auto p = toFsPath(dir);
    if (!std::filesystem::exists(p, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
        if (ec) break;
        cb(entry.path().filename().c_str(), entry.is_directory(ec), user);
    }
}

size_t filesystemUsed() {
    // Sum of file sizes under ./.config/
    std::error_code ec;
    auto root = toFsPath("/.config");
    if (!std::filesystem::exists(root, ec)) return 0;
    size_t total = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

size_t filesystemTotal() {
    // Desktop has no fixed quota; report a notional 384 KB to match the 4MB ESP32 partition.
    return 384 * 1024;
}

// Network stubs (desktop has no WiFi/Ethernet hardware)

bool ethInit() { return false; }
bool ethLinkUp() { return false; }
bool ethConnected() { return false; }
void ethGetIP(char* buf, size_t len) { if (len > 0) buf[0] = 0; }

bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) { return false; }
bool wifiStaConnected() { return false; }
void wifiStaGetIP(char* buf, size_t len) { if (len > 0) buf[0] = 0; }
void wifiStaStop() {}
int wifiStaRssi() { return 0; }

bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}
int wifiTxPower() { return 0; }
// Match the API contract: 0 is a successful no-op (matches ESP-IDF
// MM_NO_WIFI stub semantics). Any non-zero value returns false since
// there's no radio to set on the desktop. The 0-as-success branch
// matters because NetworkModule's syncTxPower passes the ESP-IDF
// "no override" sentinel (80 quarter-dBm → full power, which maps to
// txPowerSetting_==0 in user-facing dBm) through this setter to lift
// any prior cap; on desktop the radio doesn't exist so "the cap is
// lifted" is trivially true.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

bool mdnsInit(const char* /*deviceName*/) { return false; }
void mdnsStop() {}

// OTA — no-op on desktop (no OTA partition). The /api/firmware/url route
// guards with `if constexpr (mm::platform::hasOta)` and returns 501 here,
// so this stub exists for compile coverage only.
bool http_fetch_to_ota(const char* /*url*/,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

// Improv WiFi — no USB-serial path on desktop. The module gates with
// `if constexpr (mm::platform::hasImprov)` and never calls this on desktop;
// the stub exists for compile coverage.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/, size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen,
                            char* /*boardOut*/, size_t /*boardOutLen*/,
                            std::atomic<bool>* /*boardReady*/) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    return false;
}

void reboot() {
    // Desktop: the device is the host process. Exit cleanly; the OS user / supervisor
    // can restart it. Matches the "device disappeared from the network" semantics the
    // browser-side WS reconnect logic expects.
    std::printf("platform::reboot() — exiting\n");
    std::fflush(stdout);
    std::exit(0);
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

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(fd_, data, len, 0) >= 0;
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

WriteResult TcpConnection::writeChunks(const WriteChunk* chunks, int count) {
    if (fd_ < 0) return WriteResult::Error;
    if (count < 1 || count > MAX_WRITE_CHUNKS) return WriteResult::Error;
    struct iovec iov[MAX_WRITE_CHUNKS];
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        iov[i].iov_base = const_cast<uint8_t*>(chunks[i].data);
        iov[i].iov_len = chunks[i].len;
        total += chunks[i].len;
    }
    // sendmsg + MSG_DONTWAIT makes this single scatter-gather write non-blocking
    // regardless of the socket's blocking mode (the desktop client socket uses a
    // read timeout, not O_NONBLOCK, so writev alone would block).
    struct msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(count);
    ssize_t n = ::sendmsg(fd_, &msg, MSG_DONTWAIT);
    if (n < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK)
                   ? WriteResult::WouldBlock : WriteResult::Error;
    }
    if (n == 0) return WriteResult::WouldBlock;
    if (static_cast<size_t>(n) == total) return WriteResult::Complete;
    return WriteResult::Partial;
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

// -----------------------------------------------------------------------
// I2C — MPU6050-shaped simulation so GyroDriver and unit tests get live
// rotation values on desktop without hardware. Not a generic bus emulator.
// -----------------------------------------------------------------------

static bool i2cSimReady_ = false;

static void putBe16(uint8_t* dst, int16_t v) {
    dst[0] = static_cast<uint8_t>((static_cast<uint16_t>(v) >> 8) & 0xFF);
    dst[1] = static_cast<uint8_t>(v & 0xFF);
}

bool i2cInit(uint8_t /*sdaPin*/, uint8_t /*sclPin*/) {
    i2cSimReady_ = true;
    return true;
}

bool i2cWriteReg(uint8_t devAddr, uint8_t reg, uint8_t /*value*/) {
    if (!i2cSimReady_ || devAddr != 0x68) return false;
    // Accept wake writes to PWR_MGMT_1 (0x6B); no other register state needed.
    return reg == 0x6B;
}

bool i2cReadRegs(uint8_t devAddr, uint8_t reg, uint8_t* buf, size_t len) {
    if (!i2cSimReady_ || devAddr != 0x68 || !buf) return false;

    if (reg == 0x75 && len >= 1) {
        buf[0] = 0x68;  // WHO_AM_I
        return true;
    }

    if (reg == 0x3B && len >= 14) {
        // Deterministic slow motion: gyro ramps, accel tilted for non-zero pitch/roll.
        uint32_t t = millis() / 100;
        int16_t ax = static_cast<int16_t>(8192 + static_cast<int32_t>((t % 20) * 100));
        int16_t ay = 100;
        int16_t az = 16384;
        int16_t gx = static_cast<int16_t>(static_cast<int32_t>((t % 10) * 50));
        int16_t gy = static_cast<int16_t>(static_cast<int32_t>(((t + 3) % 10) * 30));
        int16_t gz = static_cast<int16_t>(static_cast<int32_t>(((t + 7) % 10) * 20));
        putBe16(buf + 0, ax);
        putBe16(buf + 2, ay);
        putBe16(buf + 4, az);
        putBe16(buf + 6, 0);   // temperature — unused
        putBe16(buf + 8, gx);
        putBe16(buf + 10, gy);
        putBe16(buf + 12, gz);
        return true;
    }

    return false;
}

} // namespace mm::platform
