#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "platform_config.h"  // hasOta / hasPsram / … — flags this header's contract refers to

namespace mm::platform {

uint32_t millis();
uint32_t micros();

// Test-only override: when set to non-zero, millis() returns this value instead
// of reading the platform clock. Production code never calls this; tests use it
// to drive virtual time deterministically (replaces the wall-clock delayMs in
// animation tests). Pass 0 to restore real-clock behaviour — tests must reset
// in teardown so cases stay independent. ESP32 honours the override too so a
// scenario-tests run on real hardware can still freeze time if needed.
void setTestNowMs(uint32_t ms);

void* alloc(size_t bytes);
void free(void* ptr);

void yield();
void delayMs(uint32_t ms);  // blocking sleep; only use outside the hot path
void delayUs(uint32_t us);  // blocking busy-wait for sub-ms protocol gaps (e.g.
                            // the WS2812 inter-frame latch); fine for a few
                            // hundred µs, not a general-purpose sleep
size_t freeHeap();          // total free (internal + PSRAM if present)
size_t freeInternalHeap();  // internal RAM only (for stack/HTTP/WiFi reserve check)
size_t maxAllocBlock();     // largest contiguous block (any memory type — incl PSRAM)
size_t maxInternalAllocBlock(); // largest contiguous block in INTERNAL RAM only

// Test-only cap on the value maxAllocBlock() reports; 0 = no cap (real value).
// Lets a test force MappingLUT's paged-destinations fallback without an actual
// fragmented heap. Production never calls this; reset to 0 in teardown.
void setTestMaxAllocBlock(size_t bytes);
                                // (scarce; use this as the memory-pressure KPI).
                                // PSRAM blocks dominate on S3/S2 boards and make
                                // maxAllocBlock useless as a stress signal —
                                // it'll report ~8 MB even when DRAM is exhausted.
size_t totalHeap();         // total heap capacity (internal + PSRAM)
size_t totalInternalHeap(); // total internal heap capacity

// Heap to keep free for stack, HTTP, WiFi, and overhead when sizing buffers —
// a platform memory constraint, not a domain one (it guards core subsystems).
// Any allocator checks free heap against this reserve before committing.
constexpr size_t HEAP_RESERVE = 32768;

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

// STA-side RSSI in dBm (negative, e.g. -58). Returns 0 when the STA isn't
// associated or the call fails — NetworkModule only surfaces this control
// while state_ == ConnectedSta so a 0 is effectively unreachable.
int wifiStaRssi();

bool wifiApInit(const char* apName, const char* ip);
bool wifiApConnected();
void wifiApStop();

// Current WiFi transmit power, in dBm (ESP-IDF reports quarter-dBm internally
// and we round to whole). Returns 0 when WiFi isn't initialised or the call
// fails. Same value for STA and AP — WiFi has one radio at one TX power.
int wifiTxPower();

// Cap the WiFi transmit power. `quarterDbm` is in ESP-IDF's quarter-dBm units
// (valid range 8..84 → 2..21 dBm); pass 0 to skip the override and let the
// stack use its default. Used by NetworkModule to apply the LOLIN WiFi fix:
// some LOLIN-branded boards (S2/S3 minis) brown-out the on-module LDO at
// full TX power, dropping WiFi during association — capping to 8 dBm
// (32 quarter-dBm, the value `boards.json` injects for the LOLIN entries)
// keeps them stable. Returns true on success or when called with 0 (no-op).
// Call after esp_wifi_start() — earlier calls are silently ignored by ESP-IDF.
bool wifiSetTxPower(int8_t quarterDbm);

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
// Board-extension args (boardOut/boardOutLen/boardReady) are for the vendor
// SET_BOARD RPC (command 0xFE) — when set, the Improv task validates the RPC
// payload, writes the board name into boardOut, and publishes via
// boardReady's release-store. Pass nullptr/0/nullptr to opt out (desktop
// stub, future targets without BoardModule). Mirrors the ssid/password
// triple: validate + buffer-write + flag-signal, scheduler thread reads.
// SET_TX_POWER RPC (command 0xFD) — when set, the Improv task validates the
// 1-byte dBm payload (0..21), writes it to txPowerOut, and publishes via
// txPowerReady's release-store. This is the pre-association escape hatch for
// boards whose LDO browns out at full TX power (LOLIN S3/S2): their
// boards.json cap normally arrives over HTTP *after* the device is online,
// which such a board can never reach — proven on the bench 2026-06-10. Same
// validate + buffer-write + flag-signal shape as SET_BOARD.
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            char* boardOut = nullptr, size_t boardOutLen = 0,
                            std::atomic<bool>* boardReady = nullptr,
                            uint8_t* txPowerOut = nullptr,
                            std::atomic<bool>* txPowerReady = nullptr);

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
    // Receiver side (ArtNet in): listen on `port` on any interface
    // (SO_REUSEADDR) and flip the socket non-blocking — note that flips the
    // whole socket, sendTo() included. Returns false when the port is taken.
    bool bind(uint16_t port);
    // Non-blocking receive of one datagram: >0 = bytes copied into buf, -1 =
    // nothing pending. Mirrors TcpConnection::read's contract minus the
    // peer-closed 0 case (UDP has no connection to close). A datagram longer
    // than maxLen is truncated. Pass `srcIp` to also get the sender's IPv4
    // octets (ArtNet discovery replies go back to the poller's address).
    int recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4] = nullptr);
    // One-shot send to an explicit address — for replying on a bound,
    // unconnected receive socket (e.g. ArtPollReply to the poller). connect()ed
    // send sockets keep using sendTo().
    bool sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
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

// ---------------------------------------------------------------------------
// RMT WS2812 LED output (classic ESP32 + S3). The driver (src/light/drivers/
// RmtLedDriver.h) does the symbol encode in domain code and may run several
// channels at once (one per pin); the platform owns only the peripheral. All
// no-ops on targets without RMT, so the driver compiles everywhere behind
// `if constexpr (platform::rmtTxChannels > 0)` (see platform_config.h) and is
// simply inert off the chips that have RMT.
// ---------------------------------------------------------------------------

// Opaque handle to one configured RMT TX channel. `impl` is set by the platform
// (a heap struct holding the channel + encoder); the driver never inspects it.
struct RmtWs2812Handle { void* impl = nullptr; };

// Allocate + configure one RMT TX channel on `gpio`. `resolutionHz` is the tick
// clock the caller expresses symbol durations in; `invert` flips output polarity
// for inverting level-shifters. Returns false on failure (and on non-ESP32).
bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert);

// The tick resolution the platform actually granted (may differ from requested).
// The driver converts its ns timings to ticks with this. 0 if not initialised.
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h);

// Start transmitting `symbolCount` pre-encoded WS2812 RMT symbols and return
// immediately — channels started back-to-back clock out concurrently. Pair with
// rmtWs2812Wait; the caller owns the inter-frame latch (delayUs) after the last
// wait. The symbol buffer must stay valid until the wait returns. Returns false
// when the channel isn't initialised (and on targets without RMT).
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols, size_t symbolCount);

// Block until the channel's in-flight transmission finishes, bounded by
// `timeoutMs` so a wedged peripheral can't hang the render tick forever — a
// timed-out frame is simply dropped and re-encoded next tick (self-heals). With
// N channels waited sequentially the worst case is N×timeoutMs; acceptable for
// the same self-healing reason.
void rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs);

void rmtWs2812Deinit(RmtWs2812Handle& h);

// RX loopback capture, on-device test only (no-op stub off ESP32). Capture up to
// `maxSymbols` pulse-duration symbols on `gpio` (jumpered from the TX pin) within
// `timeoutMs`. Returns the number captured. Used only by the loopback self-test.
size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs);

// Self-contained RMT loopback self-test, runnable from the running firmware (the
// RmtLedDriver's loopbackTest control). Drives a known WS2812 pattern out `txGpio`
// and captures it back on `rxGpio` (the user jumpers them), proving the GPIO emits
// correct bytes on real silicon. All hardware (RMT TX/RX, the GPIO continuity
// pre-check) lives here so src/light/ stays platform-free. No-op returning a
// "not supported" result off ESP32.
struct RmtLoopbackResult {
    bool jumperDetected = false;  // plain-GPIO continuity pre-check (tx high→rx high, low→low)
    bool pass = false;            // captured bytes == sent bytes
    uint8_t sent[3] = {};         // the test pattern transmitted
    uint8_t got[3] = {};          // what was decoded back (valid only if pass-attempted)
};
RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio);

// ---------------------------------------------------------------------------
// LCD_CAM parallel WS2812 output (ESP32-S3). The driver
// (src/light/drivers/LcdLedDriver.h) pre-encodes the WHOLE frame into one
// DMA buffer (3-slot encode in LcdSlots.h, domain code); the platform owns
// only the i80 bus/peripheral AND the DMA buffer itself — the buffer must be
// DMA-capable internal RAM (platform::alloc prefers PSRAM, which the
// peripheral can't stream from at full rate), so the platform allocates it at
// init and exposes the pointer for the driver's zero-copy encode. All inert
// on targets without the i80 LCD peripheral, guarded by
// `if constexpr (platform::lcdLanes == 0)` in the driver.
// ---------------------------------------------------------------------------

// Opaque handle to one configured i80 bus + IO device + DMA frame buffer.
struct LcdWs2812Handle { void* impl = nullptr; };

// Create the 8-lane bus on `dataPins[0..laneCount)` plus the two peripheral-
// mandated lines WS2812 strands ignore: `wrGpio` (the pixel clock) and
// `dcGpio` (data/command). Allocates a zeroed DMA-capable frame buffer of
// `bufferBytes`. Returns false on any failure (bad pins, DMA memory pressure).
bool lcdWs2812Init(LcdWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes);

// The DMA frame buffer the driver encodes into (zero-copy), and its capacity
// — the driver's grow-only check. nullptr / 0 when not initialised.
uint8_t* lcdWs2812Buffer(const LcdWs2812Handle& h);
size_t lcdWs2812BufferCapacity(const LcdWs2812Handle& h);

// Start the autonomous DMA transfer of the buffer's first `bytes` and return;
// pair with lcdWs2812Wait. Once started no CPU work remains — there is no
// refill deadline for WiFi to miss (the design difference vs the ISR-refilled
// rings in the hpwit/FastLED lineage).
bool lcdWs2812Transmit(LcdWs2812Handle& h, size_t bytes);

// Block until the in-flight transfer finishes, bounded by `timeoutMs`; a
// timed-out frame is dropped and re-encoded next tick (self-heals, same
// stance as rmtWs2812Wait).
void lcdWs2812Wait(LcdWs2812Handle& h, uint32_t timeoutMs);

void lcdWs2812Deinit(LcdWs2812Handle& h);

// LCD loopback self-test: build a private FULL-WIDTH bus on the driver's
// real pins (the i80 peripheral configures all 8 data lines — a partial bus
// is rejected by the hardware layer) and transmit the caller's REAL encoded
// frame (`frame`/`frameBytes`, lane 0 = dataPins[0]) back to back, exactly
// like the render loop, while an RMT RX channel captures the whole frame off
// `rxGpio` and verifies every bit (RMT receive is transmitter-agnostic — the
// increment-1 rig reused). `dataBytes` is the slot-carrying prefix of the
// frame (before the latch pad); `rowBits` the bits per light row, so the
// expected pattern repeats per row. Testing the genuine frame matters: a
// short synthetic burst misses exactly the real-transfer failures (DMA
// descriptor boundaries, sustained-rate stalls). Same result shape as the
// RMT test; got[] holds the first mismatching row. No-op off the S3.
RmtLoopbackResult lcdWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits);

} // namespace mm::platform
