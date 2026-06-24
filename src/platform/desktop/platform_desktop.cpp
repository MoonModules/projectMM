#include "platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>     // cosf/sinf/sqrtf for the naive desktop DFT (audioFft)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <cerrno>

#ifdef _WIN32
// Winsock + Win32 socket APIs. SOCKET is an unsigned handle (INVALID_SOCKET = ~0),
// but `fd_` stays `int` in the cross-platform header — the narrowing is well-defined
// for handle values in the practical range and is the standard Win32 pattern.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>     // _fileno, _commit (POSIX fileno/fsync equivalents)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace mm::platform {

namespace {
// Tiny portability shims so each call site reads as plain code, not `#ifdef` noise.
// POSIX uses int FDs + errno + read/write/close; Winsock uses SOCKET handles +
// WSAGetLastError + recv/send/closesocket. Map to a small common surface.
#ifdef _WIN32
// SOCKET is unsigned (UINT_PTR). `sock(fd)` casts to it at API boundaries so
// /W4 doesn't warn about signed→unsigned at every call site.
inline SOCKET sock(int fd) { return static_cast<SOCKET>(fd); }
inline int close_sock(int fd) { return ::closesocket(sock(fd)); }
// WSAEWOULDBLOCK: non-blocking call had no buffer/data. WSAETIMEDOUT: blocking
// recv hit SO_RCVTIMEO without data. Both translate to POSIX EAGAIN semantics
// (the read/write path returns -1 / WouldBlock and the caller retries).
inline bool sockWouldBlock() {
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
}
inline int open_sock(int domain, int type, int protocol) {
    SOCKET s = ::socket(domain, type, protocol);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline int make_nonblocking(int fd) {
    u_long mode = 1;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
inline int make_blocking(int fd) {
    u_long mode = 0;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
#else
inline int sock(int fd) { return fd; }
inline int close_sock(int fd) { return ::close(fd); }
inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline int open_sock(int domain, int type, int protocol) {
    return ::socket(domain, type, protocol);
}
inline int make_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
inline int make_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
#endif
#ifdef _WIN32
// Winsock 2.2 must be initialized once per process before any socket call.
// A static RAII guard runs at library load (covers both the app and the test
// binaries, which have their own main() but link mm_platform). WSAStartup is
// reference-counted so this is safe alongside any future caller-side init.
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { ::WSACleanup(); }
};
static WinsockInit g_winsockInit;
#endif

}  // namespace

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

void delayUs(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

size_t freeHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t freeInternalHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// Test-only cap on the reported largest-free block; 0 = unlimited (the real
// desktop default). Lets a test force MappingLUT's paged fallback (which only
// triggers when no single contiguous block fits) without an actual fragmented
// heap. std::atomic to match setTestNowMs's cross-thread contract.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    return testMaxBlock.load(std::memory_order_relaxed); // 0 = unlimited
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
    int fd = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (::connect(sock(fd), reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(sock(fd), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        }
    }
    close_sock(fd);
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

const char* coprocessorWifi() {
    return "";   // desktop has no WiFi co-processor
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
    // path::c_str() returns wchar_t* on Windows; std::fopen needs char*. Go via
    // .string() so the call compiles on both. Costs one std::string allocation
    // per read — acceptable for /.config/*.json reads (rare, small).
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
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

    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);  // Windows equivalent of fsync
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
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
        // path::filename().c_str() returns wchar_t* on Windows; the callback
        // wants char*. Round-trip through .string() to get a portable view.
        std::string name = entry.path().filename().string();
        cb(name.c_str(), entry.is_directory(ec), user);
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

void setEthConfig(const EthPinConfig&) {}   // no eth on desktop; ethInit stubs false
void ethStop() {}                           // no eth on desktop
bool ethInit() { return false; }
bool ethLinkUp() { return false; }
bool ethConnected() { return false; }
void ethGetIPv4(uint8_t out[4]) {
    // Desktop has no real interface state, but DevicesModule needs the host's LAN
    // IP to scan from (otherwise a PC projectMM instance reports "no network" and
    // never sweeps). hostIp() resolves it via the outbound-route trick; report it
    // as the "ethernet" IP so DevicesModule's localIp() (eth-first) picks it up.
    out[0] = out[1] = out[2] = out[3] = 0;
    const char* ip = hostIp();
    if (ip && ip[0]) {
        // Parse the dotted-quad to octets with inet_pton (already used in this file)
        // — the platform layer doesn't include core/Control.h's parseDottedQuad.
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) == 1) {
            uint32_t n = a.s_addr;   // network byte order: octet 0 is the low byte
            out[0] = static_cast<uint8_t>(n & 0xff);
            out[1] = static_cast<uint8_t>((n >> 8) & 0xff);
            out[2] = static_cast<uint8_t>((n >> 16) & 0xff);
            out[3] = static_cast<uint8_t>((n >> 24) & 0xff);
        }
    }
}

bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) { return false; }
bool wifiStaConnected() { return false; }
void wifiStaGetIPv4(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
void setHostname(const char* /*name*/) {}   // no DHCP client on desktop
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
void mdnsShutdown() {}
// No mDNS on desktop — browse is a no-op (no hosts found). A PC instance discovers peers
// via the HTTP sweep instead (see DevicesModule).
bool mdnsBrowse(const char* /*service*/, const char* /*proto*/, uint32_t /*timeoutMs*/,
                MdnsHostCb /*cb*/, void* /*user*/) { return false; }

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

// Short-timeout outbound HTTP GET for device discovery (DevicesModule's scan).
// Real implementation on desktop (a PC projectMM instance must scan its LAN too):
// a plain blocking socket with a receive/send timeout — no TLS, no libcurl, LAN
// HTTP only. Parses http://host[:port]/path, returns the HTTP status code (0 on
// any failure) and fills `body` with the response body (NUL-terminated, truncated).
int httpGet(const char* url, uint32_t timeoutMs, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    if (!url) return 0;

    // Parse "http://host[:port]/path". Only plain http:// (LAN devices).
    constexpr const char* kPrefix = "http://";
    const size_t prefixLen = std::strlen(kPrefix);
    if (std::strncmp(url, kPrefix, prefixLen) != 0) return 0;
    const char* hostStart = url + prefixLen;
    const char* slash = std::strchr(hostStart, '/');
    const char* path = slash ? slash : "/";
    char host[64] = {};
    uint16_t port = 80;
    {
        const char* hostEnd = slash ? slash : (hostStart + std::strlen(hostStart));
        const char* colon = static_cast<const char*>(
            std::memchr(hostStart, ':', static_cast<size_t>(hostEnd - hostStart)));
        const char* nameEnd = colon ? colon : hostEnd;
        size_t hlen = static_cast<size_t>(nameEnd - hostStart);
        if (hlen == 0 || hlen >= sizeof(host)) return 0;
        std::memcpy(host, hostStart, hlen);
        host[hlen] = '\0';
        if (colon) {
            int p = std::atoi(colon + 1);
            if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
        }
    }

    int fd = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Discovery probes a numeric IP (the scan walks the subnet), so inet_pton is
    // enough — no DNS resolution needed for the LAN sweep.
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close_sock(fd); return 0; }

    // NON-BLOCKING connect bounded by select() — this is the crux. SO_RCVTIMEO /
    // SO_SNDTIMEO do NOT bound connect() (a POSIX gotcha): a blocking connect() to a
    // dead/firewalled IP hangs for the OS default (~75 s on macOS), which would
    // freeze the single-threaded desktop loop on the first unreachable host in the
    // sweep. So: make the socket non-blocking, start the connect, select() on
    // writability with our short timeout, and check SO_ERROR. Then restore blocking
    // mode for the recv/send (those ARE bounded by SO_*TIMEO).
    make_nonblocking(fd);
    int crc = ::connect(sock(fd), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    const bool inProgress = (crc != 0) && (::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (crc != 0) && (errno == EINPROGRESS);
#endif
    if (crc != 0 && !inProgress) { close_sock(fd); return 0; }
    if (inProgress) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock(fd), &wset);
        timeval ctv{};
        ctv.tv_sec = static_cast<long>(timeoutMs / 1000);
        ctv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        int sel = ::select(static_cast<int>(sock(fd)) + 1, nullptr, &wset, nullptr, &ctv);
        if (sel <= 0) { close_sock(fd); return 0; }   // timeout (0) or error (<0)
        int soErr = 0;
        socklen_t errLen = sizeof(soErr);
        ::getsockopt(sock(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&soErr), &errLen);
        if (soErr != 0) { close_sock(fd); return 0; }  // connect failed (refused/unreachable)
    }
    // Connected. Back to blocking, with recv/send bounded by SO_*TIMEO.
    make_blocking(fd);
#ifdef _WIN32
    DWORD tv = timeoutMs;
#else
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
#endif
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));

    char req[256];
    int reqLen = std::snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    // snprintf returns the length it WOULD have written: >= sizeof(req) means the
    // request was truncated (an over-long path/host). Reject rather than send a
    // truncated header or read past the buffer with the inflated length.
    if (reqLen <= 0 || static_cast<size_t>(reqLen) >= sizeof(req) ||
        ::send(sock(fd), req, reqLen, 0) != reqLen) {
        close_sock(fd);
        return 0;
    }

    // Read the whole (small) response into a local buffer, then split headers/body.
    char resp[2048];
    int total = 0;
    while (total < static_cast<int>(sizeof(resp)) - 1) {
        int n = ::recv(sock(fd), resp + total, static_cast<int>(sizeof(resp)) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    close_sock(fd);
    resp[total] = '\0';
    if (total == 0) return 0;

    // Status line: "HTTP/1.x <code> ...". Parse the code.
    int status = 0;
    const char* sp = std::strchr(resp, ' ');
    if (sp) status = std::atoi(sp + 1);

    // Body starts after the blank line (CRLFCRLF).
    if (body && bodyLen > 1) {
        const char* bodyStart = std::strstr(resp, "\r\n\r\n");
        if (bodyStart) {
            bodyStart += 4;
            size_t blen = std::strlen(bodyStart);
            if (blen > bodyLen - 1) blen = bodyLen - 1;
            std::memcpy(body, bodyStart, blen);
            body[blen] = '\0';
        }
    }
    return status;
}

// Improv WiFi — no USB-serial path on desktop. The module gates with
// `if constexpr (mm::platform::hasImprov)` and never calls this on desktop;
// the stub exists for compile coverage.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/, size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* /*txPowerOut*/,
                            std::atomic<bool>* /*txPowerReady*/,
                            char* /*opOut*/, size_t /*opOutLen*/,
                            std::atomic<bool>* /*opReady*/) {
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
    fd_ = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow sends to a broadcast address (e.g. 255.255.255.255 for an Art-Net /
    // E1.31 spray to every device on the LAN). Without SO_BROADCAST the OS rejects
    // such a send with EACCES; it has no effect on unicast/multicast sends.
    const int on = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    return true;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0) >= 0;
}

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    int reuse = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    return make_nonblocking(fd_) == 0;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and would-block both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    // recv() works the same on POSIX and Winsock — the socket is blocking with
    // SO_RCVTIMEO set in TcpServer::accept (Windows takes DWORD ms, POSIX takes
    // struct timeval). After the timeout, recv returns -1 with EAGAIN/EWOULDBLOCK
    // (POSIX) or WSAEWOULDBLOCK (Windows); we translate both to -1 for the caller.
    auto n = ::recv(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0; // peer closed
    if (sockWouldBlock()) return -1; // read timed out, nothing available
    return 0; // error → treat as closed
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    size_t sent = 0;
    while (sent < len) {
        auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data + sent),
                        static_cast<int>(len - sent), 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (sockWouldBlock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifndef _WIN32
        } else if (errno == EINTR) {
            continue; // interrupted by signal, retry
#endif
        } else {
            return false;
        }
    }
    return true;
}

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    // The client socket is blocking (SO_RCVTIMEO drives recv's read timeout), so a plain
    // ::send() would block when the kernel send buffer is full. Toggle non-blocking around the
    // send to keep this truly non-blocking, then restore so recv's timeout semantics hold.
    // Evaluate the would-block / EINTR status BEFORE make_blocking, since that ioctl/fcntl
    // can clobber the error state.
    make_nonblocking(fd_);
    auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    bool wouldBlock = (n < 0) && sockWouldBlock();
#ifndef _WIN32
    bool interrupted = (n < 0) && (errno == EINTR);
#endif
    make_blocking(fd_);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (wouldBlock) return 0;               // buffer full — try later
#ifndef _WIN32
    if (interrupted) return 0;              // interrupted — try later
#endif
    return -1;                              // real socket error
}


void TcpConnection::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock(fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    if (::listen(sock(fd_), 8) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    make_nonblocking(fd_);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
#ifdef _WIN32
    SOCKET client = ::accept(sock(fd_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return TcpConnection();
    int clientFd = static_cast<int>(client);
    // Match POSIX: socket stays blocking, SO_RCVTIMEO gives recv a 2-second
    // timeout. Windows SO_RCVTIMEO takes a DWORD millisecond count (not a
    // timeval). writeSome() toggles non-blocking around its send call to
    // emulate POSIX's MSG_DONTWAIT.
    DWORD timeoutMs = 2000;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();
    // Set read timeout (2 seconds) instead of non-blocking
    struct timeval tv = {2, 0};
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// RMT WS2812 — no-op stubs. Desktop has no RMT peripheral; the driver guards
// every call with `if constexpr (platform::rmtTxChannels == 0)` (0 here), so
// these exist only to satisfy the linker and are never reached at runtime.
// ---------------------------------------------------------------------------
bool rmtWs2812Init(RmtWs2812Handle& /*h*/, uint8_t /*gpio*/, uint32_t /*resolutionHz*/,
                   bool /*invert*/) {
    return false;
}
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& /*h*/) { return 0; }
bool rmtWs2812Transmit(RmtWs2812Handle& /*h*/, const uint32_t* /*symbols*/,
                       size_t /*symbolCount*/) {
    return false;
}
void rmtWs2812Wait(RmtWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) {}
void rmtWs2812Deinit(RmtWs2812Handle& /*h*/) {}
size_t rmtWs2812RxCapture(uint8_t /*gpio*/, uint32_t /*resolutionHz*/,
                          uint32_t* /*outSymbols*/, size_t /*maxSymbols*/,
                          uint32_t /*timeoutMs*/) {
    return 0;
}
RmtLoopbackResult rmtWs2812Loopback(uint8_t /*txGpio*/, uint8_t /*rxGpio*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t /*txGpio*/, uint8_t /*rxGpio*/,
                                         uint16_t /*lights*/, uint8_t /*channels*/) {
    return {};   // not supported off ESP32
}

// ---------------------------------------------------------------------------
// LCD_CAM WS2812 — no-op stubs. Desktop has no i80 peripheral; the LCD LED
// driver guards every call with `if constexpr (platform::lcdLanes == 0)`
// (0 here), so these exist only to satisfy the linker.
// ---------------------------------------------------------------------------
bool lcdWs2812Init(LcdWs2812Handle& /*h*/, const uint16_t* /*dataPins*/,
                   uint8_t /*laneCount*/, uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                   size_t /*bufferBytes*/) {
    return false;
}
uint8_t* lcdWs2812Buffer(const LcdWs2812Handle& /*h*/) { return nullptr; }
size_t lcdWs2812BufferCapacity(const LcdWs2812Handle& /*h*/) { return 0; }
bool lcdWs2812Transmit(LcdWs2812Handle& /*h*/, size_t /*bytes*/) { return false; }
void lcdWs2812Wait(LcdWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) {}
void lcdWs2812Deinit(LcdWs2812Handle& /*h*/) {}
RmtLoopbackResult lcdWs2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                    uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                                    uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                    size_t /*frameBytes*/, size_t /*dataBytes*/,
                                    uint8_t /*rowBits*/) {
    return {};   // not supported off the S3
}

// Parlio WS2812 — no-op stubs. Desktop has no Parlio peripheral; the driver
// idles (parlioLanes == 0). Sizing/slicing is host-pinned by the driver tests.
bool parlioWs2812Init(ParlioWs2812Handle& /*h*/, const uint16_t* /*dataPins*/,
                      uint8_t /*laneCount*/, uint32_t /*pclkHz*/, size_t /*bufferBytes*/) {
    return false;
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& /*h*/) { return nullptr; }
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& /*h*/) { return 0; }
bool parlioWs2812Transmit(ParlioWs2812Handle& /*h*/, size_t /*bytes*/) { return false; }
void parlioWs2812Wait(ParlioWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) {}
void parlioWs2812Deinit(ParlioWs2812Handle& /*h*/) {}
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                       uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                       size_t /*frameBytes*/, size_t /*dataBytes*/,
                                       uint8_t /*rowBits*/) {
    return {};   // not supported off the P4
}

// I2S microphone — no capture on desktop (hasI2sMic == false, AudioModule inert),
// so init fails and read returns nothing.
bool audioMicInit(AudioMicHandle& /*h*/, uint16_t /*wsPin*/, uint16_t /*sdPin*/,
                  uint16_t /*sckPin*/, uint32_t /*sampleRate*/) {
    return false;
}
size_t audioMicRead(AudioMicHandle& /*h*/, int32_t* /*out*/, size_t /*maxSamples*/) {
    return 0;
}
void audioMicDeinit(AudioMicHandle& /*h*/) {}

// FFT kernel — a real but naive O(n^2) DFT. NOT the production kernel (the ESP32
// uses esp-dsp's fast radix-2), but a correct reference so the host tests run the
// genuine magnitude->band path on synthesized signals. n must be a power of two;
// fills outMag[0..n/2) with the bin magnitudes.
void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0) return;
    const float twoPiOverN = -2.0f * 3.14159265358979323846f / static_cast<float>(n);
    for (size_t k = 0; k < n / 2; k++) {
        float re = 0.0f, im = 0.0f;
        for (size_t t = 0; t < n; t++) {
            const float a = twoPiOverN * static_cast<float>(k) * static_cast<float>(t);
            re += windowed[t] * std::cos(a);
            im += windowed[t] * std::sin(a);
        }
        outMag[k] = std::sqrt(re * re + im * im);
    }
}

} // namespace mm::platform
