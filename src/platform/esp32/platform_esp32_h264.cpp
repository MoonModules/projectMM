/// @defgroup platform_esp32_h264 Live streaming on the P4
/// The platform half of the encoder seam.
///
/// Where a desktop hands the whole job to an external encoder, this chip does all three parts itself.
/// The hardware encoder, our own muxer, and a segment ring served straight out of memory.
///
/// @moreinfo
///
/// ## No filesystem in the path
///
/// At one segment a second, writing them to flash would wear it out for no gain, a live segment being stale within seconds.
///
/// ## Why a worker task
///
/// The write is called from the render tick and must never block on an encode.
/// It copies the frame into a slot ring and returns, and the worker does the conversion, the encode and the muxing.
/// A full ring drops the newest frame, exactly as the desktop path does when its encoder falls behind.
///
/// ## An orphaned worker must not become a second producer
///
/// Stopping a worker detaches one that overruns its join deadline rather than freeing it, so an orphan can still be parked when the next start runs.
/// A running flag alone cannot gate that, since the new start sets it true again.
/// The orphan would then resume as a second producer on the one encoder handle and scratch buffer.
/// So each worker captures the generation it was spawned for and exits as soon as it is no longer current.
///
/// For the same reason the buffers are freed only once the worker has actually returned: a detached one is mid-encode holding raw pointers to them and to the encoder handle.
/// Freeing there would be a use-after-free plus a call into a deleted session, so leaking a few megabytes until the next start is the better trade.
///
/// ## The playlist advertises from the oldest plus a margin
///
/// Never the oldest itself, since that slot is the next one rotation overwrites and a player fetching it races the encoder and gets nothing.
/// The margin is what a player has left to fetch what it was promised, and the rest of the ring is its buffering budget.
/// On the bench, listing the true oldest failed immediately and listing only the newest few failed within about five seconds, and both spin forever.

#include "platform/platform.h"
#include "sdkconfig.h"

#if defined(CONFIG_MM_HLS)

// The Kconfig symbol cannot enforce this itself: `depends on IDF_TARGET_ESP32P4` would hide
// MM_HLS from the component solver, which reads it to gate the esp_h264 dependency, and every
// non-P4 build then fails at cmake. Catch it here instead, where the message names the cause
// rather than surfacing as a link error against a missing hardware encoder.
#include "soc/soc_caps.h"
#if !defined(SOC_H264_ENCODER_SUPPORTED) || !SOC_H264_ENCODER_SUPPORTED
#error "CONFIG_MM_HLS is set on a chip with no hardware H.264 encoder (P4 only)."
#endif

#include "light/util/MpegTs.h"

#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace mm::platform {
namespace {

// Frame slots between the render tick and the encode task. Three is the desktop's number and the
// same reasoning: enough to absorb a burst, few enough that a backlog is dropped rather than
// queued into latency.
constexpr size_t kSlots = 3;

// Segments kept in the ring, and so also the playlist's depth: a segment survives this many
// seconds after it closes, which is the whole budget a player has to parse the playlist, fetch
// and buffer before what it asked for is recycled. Browsers want several seconds of that, so the
// ring is the lifetime, not a cache.
constexpr size_t kSegments = 12;
// Segments held back from the playlist: the slots rotation is about to reuse. Without this margin
// a player is handed a segment that is overwritten while it fetches it.
constexpr uint32_t kReserved = 3;

// One second at a generous bitrate, with headroom for the keyframe that opens every segment.
constexpr size_t kSegmentBytes = 512 * 1024;

struct Slot {
    uint8_t* data = nullptr;
    size_t   len  = 0;
};

struct Segment {
    uint8_t* data   = nullptr;
    size_t   len    = 0;
    uint32_t seq    = 0;    // its number in the playlist; 0 = never filled
    uint16_t frames = 0;    // frames muxed into it, so the playlist can state its REAL duration
};

// Everything the worker and the producers share. Guarded by the FreeRTOS mutex below, except the
// atomics, which are read without it.
Slot     slots_[kSlots];
size_t   head_ = 0, count_ = 0;
Segment  segments_[kSegments];
size_t   segWrite_ = 0;          // segment currently being filled
uint32_t nextSeq_  = 1;
// The segment a socket is currently reading, if any. hlsSegment hands out a pointer that the
// caller reads AFTER the lock drops, so the encoder must not recycle that slot underneath it;
// serving is far shorter than the eight seconds the ring takes to lap, but "usually in time" is
// not a lifetime guarantee. kNoSeg = nothing being served.
constexpr uint32_t kNoSeg = 0;
std::atomic<uint32_t> serving_{kNoSeg};

WorkerTask       task_;
std::atomic<bool> running_{false};
std::atomic<bool> dead_{false};   // the encoder failed: writes are refused until a restart
// Set by the worker as its LAST act. stopPinnedTask detaches rather than joins if the worker
// overruns its deadline (platform_esp32_worker.cpp), so its return does not prove the worker is
// gone; freeing the buffers on that path would pull them out from under a live encode.
std::atomic<bool> workerExited_{false};

// Which worker generation is the live one: @xref{an-orphaned-worker-must-not-become-a-second-producer|why a running flag alone cannot gate it}.
std::atomic<uint32_t> generation_{0};

// Set when a segment was closed early (a frame that did not fit), so the fresh one is still
// waiting for its first keyframe. Without it the next P-frame would open the segment and a
// player seeking there would have no reference frame to decode against.
bool needKeyframe_ = false;

esp_h264_enc_handle_t enc_ = nullptr;
uint8_t*  yuv_    = nullptr;      // one converted frame, the encoder's input
uint8_t*  nal_    = nullptr;      // one encoded frame, the encoder's output
uint16_t  width_  = 0, height_ = 0;
uint8_t   fps_    = 30;
uint32_t  frameNo_ = 0;
// One per stream, never per frame or per segment: see mm::ts::Continuity.
mm::ts::Continuity cc_;

SemaphoreHandle_t mutex_ = nullptr;

struct Lock {
    Lock()  { if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
};

/// Convert to the encoder's own layout: chroma-prefixed alternating lines, with the integer coefficients the default color matrix expects.
void rgbToEncoderFormat(const uint8_t* rgb, uint8_t* out, uint16_t w, uint16_t h) {
    const size_t lineBytes = static_cast<size_t>(w) * 3 / 2;
    for (uint16_t y = 0; y < h; y++) {
        uint8_t* dst = out + static_cast<size_t>(y) * lineBytes;
        const uint8_t* src = rgb + static_cast<size_t>(y) * w * 3;
        const bool evenRow = (y & 1) == 0;   // rows 0, 2, 4...: these carry U, the others V
        for (uint16_t x = 0; x < w; x += 2) {
            const uint8_t* p0 = src + static_cast<size_t>(x) * 3;
            const uint8_t* p1 = (x + 1 < w) ? p0 + 3 : p0;
            const int r0 = p0[0], g0 = p0[1], b0 = p0[2];
            const int r1 = p1[0], g1 = p1[1], b1 = p1[2];

            const int y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8;
            const int y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8;
            // Chroma is subsampled 2x2; averaging the pair costs nothing and avoids the crawl a
            // nearest-sample pick gives on hard edges.
            const int rA = (r0 + r1) >> 1, gA = (g0 + g1) >> 1, bA = (b0 + b1) >> 1;
            const int c = evenRow ? (((-43 * rA - 84 * gA + 128 * bA) >> 8) + 128)    // U
                                  : (((128 * rA - 107 * gA - 21 * bA) >> 8) + 128);   // V

            *dst++ = static_cast<uint8_t>(c < 0 ? 0 : (c > 255 ? 255 : c));
            *dst++ = static_cast<uint8_t>(y0 < 0 ? 0 : (y0 > 255 ? 255 : y0));
            *dst++ = static_cast<uint8_t>(y1 < 0 ? 0 : (y1 > 255 ? 255 : y1));
        }
    }
}

/// Close the current segment and open the next, overwriting the oldest and skipping one still being served, called with the lock held.
void rotateSegment() {
    segments_[segWrite_].seq = nextSeq_++;
    const uint32_t busy = serving_.load();
    for (size_t tried = 0; tried < kSegments; tried++) {
        segWrite_ = (segWrite_ + 1) % kSegments;
        // Only a slot actually being served is off limits. An empty slot carries seq 0, which is
        // also kNoSeg, so comparing without the busy check skips every free slot and the ring
        // never advances (bench: 12 rotations, all eight slots still seq 0).
        if (busy == kNoSeg || segments_[segWrite_].seq != busy) break;
    }
    segments_[segWrite_].len    = 0;
    segments_[segWrite_].seq    = 0;
    segments_[segWrite_].frames = 0;
}

void encodeOne(const uint8_t* rgb, size_t rgbLen) {
    if (!enc_ || !yuv_ || !nal_) return;
    const size_t need = static_cast<size_t>(width_) * height_ * 3;
    if (rgbLen < need) return;

    rgbToEncoderFormat(rgb, yuv_, width_, height_);

    esp_h264_enc_in_frame_t in{};
    in.raw_data.buffer = yuv_;
    in.raw_data.len    = static_cast<uint32_t>(need / 2);   // 1.5 bytes per pixel
    in.pts             = frameNo_ * (1000u / (fps_ ? fps_ : 30));

    esp_h264_enc_out_frame_t out{};
    out.raw_data.buffer = nal_;
    out.raw_data.len    = static_cast<uint32_t>(kSegmentBytes / 4);

    const esp_h264_err_t perr = esp_h264_enc_process(enc_, &in, &out);
    if (perr != ESP_H264_ERR_OK || out.length == 0) {
        dead_ = true;
        return;
    }

    const bool keyframe = out.frame_type == ESP_H264_FRAME_TYPE_IDR ||
                          out.frame_type == ESP_H264_FRAME_TYPE_I;
    const uint32_t pts90 = static_cast<uint32_t>(
        static_cast<uint64_t>(frameNo_) * mm::ts::kClockHz / (fps_ ? fps_ : 30));
    frameNo_++;

    Lock lk;
    Segment& seg = segments_[segWrite_];
    // A segment must START on a keyframe (a player seeking to it has nothing to reference
    // otherwise), so a keyframe closes the previous one. GOP == fps, so this lands once a second.
    if (keyframe && seg.len > 0) {
        rotateSegment();
    }
    // A segment opened by an overflow rotate holds nothing until a keyframe arrives: dropping
    // these few P-frames costs a fraction of a second, where admitting them costs the segment.
    if (needKeyframe_ && !keyframe) return;
    needKeyframe_ = false;
    Segment& dst = segments_[segWrite_];
    if (!dst.data) return;

    // The counters advance per packet as the writer emits. A discarded frame must not keep that
    // advance: the packets were never sent, so a player would read the gap as lost packets, the
    // exact corruption Continuity exists to prevent.
    const mm::ts::Continuity ccBefore = cc_;
    mm::ts::Writer w(dst.data + dst.len, kSegmentBytes - dst.len, cc_);
    if (dst.len == 0) w.writeTables();
    w.writeAccessUnit(nal_, out.length, pts90, keyframe);
    if (w.overflowed()) {
        cc_ = ccBefore;
        // The frame did not fit: close the segment here rather than emit a torn one, and hold
        // the fresh one empty until a keyframe can open it (needKeyframe_).
        if (dst.len > 0) rotateSegment();
        needKeyframe_ = true;
        return;
    }
    dst.len += w.size();
    dst.frames++;
}

void workerFn(void* arg) {
    const uint32_t myGen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    taskWdtSubscribe();
    while (running_ && generation_.load() == myGen) {
        taskWdtReset();
        const uint8_t* frame = nullptr;
        size_t len = 0;
        {
            Lock lk;
            if (count_ > 0) { frame = slots_[head_].data; len = slots_[head_].len; }
        }
        if (!frame) { waitNotify(task_, 100); continue; }
        encodeOne(frame, len);
        {
            Lock lk;
            head_ = (head_ + 1) % kSlots;
            count_--;
        }
    }
    taskWdtUnsubscribe();
    workerExited_ = true;   // the buffers are now nobody's: encoderStop may free them
}

void freeAll() {
    if (enc_) { esp_h264_enc_close(enc_); esp_h264_enc_del(enc_); enc_ = nullptr; }
    for (auto& s : slots_)    { heap_caps_free(s.data); s.data = nullptr; s.len = 0; }
    for (auto& s : segments_) { heap_caps_free(s.data); s.data = nullptr; s.len = 0; s.seq = 0; s.frames = 0; }
    heap_caps_free(yuv_); yuv_ = nullptr;
    heap_caps_free(nal_); nal_ = nullptr;
}

void* psram(size_t bytes) {
    return heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace

bool encoderStart(const EncoderConfig& cfg) {
    // If the previous stop had to detach a wedged worker, encoderStop left its buffers alive on
    // purpose (see there) and the pointers below are overwritten rather than freed: a bounded
    // one-time leak, deliberately preferred to freeing memory a live task is still writing.
    encoderStop();
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;

    // The hardware encoder's own limits (esp_h264_types.h): below 80 or above 1920x2032 it will
    // refuse, so decline here with a status rather than fail obscurely mid-stream.
    if (cfg.width < 80 || cfg.height < 80 || cfg.width > 1920 || cfg.height > 2032) return false;
    // 4:2:0 chroma needs even dimensions.
    if ((cfg.width & 1) || (cfg.height & 1)) return false;

    width_  = cfg.width;
    height_ = cfg.height;
    fps_    = cfg.fps ? cfg.fps : 30;
    frameNo_ = 0;

    esp_h264_enc_cfg_hw_t hw{};
    hw.pic_type   = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    hw.gop        = fps_;            // one keyframe per second: the segment boundary
    hw.fps        = fps_;
    hw.res.width  = width_;
    hw.res.height = height_;
    hw.rc.bitrate = static_cast<uint32_t>(cfg.bitrateKbit) * 1000u;
    // The QP window the rate controller may use. A near-fixed window (the 25/26 this started
    // with) overrides the bitrate entirely: quality is pinned, so the encoder spends whatever
    // that costs and ignores rc.bitrate. Opening the window lets the configured bitrate actually
    // govern, which is what the driver's control promises.
    hw.rc.qp_min  = 10;
    hw.rc.qp_max  = 40;

    if (esp_h264_enc_hw_new(&hw, &enc_) != ESP_H264_ERR_OK || !enc_) { enc_ = nullptr; return false; }
    if (esp_h264_enc_open(enc_) != ESP_H264_ERR_OK) {
        esp_h264_enc_del(enc_);
        enc_ = nullptr;
        return false;
    }

    const size_t rgbBytes = static_cast<size_t>(width_) * height_ * 3;
    yuv_ = static_cast<uint8_t*>(psram(rgbBytes / 2));
    nal_ = static_cast<uint8_t*>(psram(kSegmentBytes / 4));
    for (auto& s : slots_) s.data = static_cast<uint8_t*>(psram(rgbBytes));
    for (auto& s : segments_) { s.data = static_cast<uint8_t*>(psram(kSegmentBytes)); s.len = 0; s.seq = 0; s.frames = 0; }
    if (!yuv_ || !nal_) { freeAll(); return false; }
    for (const auto& s : slots_)    if (!s.data) { freeAll(); return false; }
    for (const auto& s : segments_) if (!s.data) { freeAll(); return false; }

    head_ = count_ = 0;
    segWrite_ = 0;
    nextSeq_  = 1;
    cc_       = mm::ts::Continuity{};
    needKeyframe_ = false;
    dead_     = false;
    running_  = true;
    // A new generation retires any orphan the previous stop had to detach.
    const uint32_t myGen = generation_.fetch_add(1) + 1;
    // Clear HERE, not in the worker: the worker's first instruction runs only once the scheduler
    // reaches it, and a stop landing in that window would read the previous stop's `true` and
    // free the buffers the worker is about to encode from.
    workerExited_ = false;
    // The second core, since the first runs the network stack and starving it stalls the very server that serves these segments.
    // The stack is twice what this started with: the encoder call chain plus our muxer overflowed the smaller one.
    // It jumped into the maths library with a corrupted pointer, panicking in a loop.
    // The vendor's own example runs its encode from a comparable stack, and the muxer's frame loop sits on top of that.
    if (!spawnPinnedTask(task_, "mmH264", workerFn,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(myGen)),
                         16 * 1024, 5, 1)) {
        running_ = false;
        freeAll();
        return false;
    }
    return true;
}

int encoderWrite(const uint8_t* data, size_t len) {
    if (!data || len == 0) return -1;   // invalid input, distinct from the queue-full drop (0)
    if (!running_ || dead_) return -1;
    Lock lk;
    if (count_ >= kSlots) return 0;             // encoder behind: drop-newest, stay live
    Slot& s = slots_[(head_ + count_) % kSlots];
    if (!s.data) return -1;
    const size_t cap = static_cast<size_t>(width_) * height_ * 3;
    const size_t n = len < cap ? len : cap;
    std::memcpy(s.data, data, n);
    s.len = n;
    count_++;
    notifyTask(task_);
    return static_cast<int>(n);
}

bool encoderRunning() { return running_ && !dead_; }

void encoderStop() {
    if (running_) {
        running_ = false;
        stopPinnedTask(task_);
    }
    Lock lk;
    // Free only once the worker has actually returned: @xref{an-orphaned-worker-must-not-become-a-second-producer|why a detached one still holds these pointers}.
    if (workerExited_) {
        freeAll();
        head_ = count_ = 0;
        segWrite_ = 0;
    }
}

bool hlsSegment(const char* name, const uint8_t** data, size_t* len) {
    if (!name || !data || !len) return false;
    Lock lk;

    // The playlist is generated on demand from the ring: whatever segments are currently complete,
    // newest last. Static buffer because the caller writes it straight to the socket.
    if (std::strcmp(name, "stream.m3u8") == 0) {
        static char playlist[640];   // header + two lines per segment, kSegments of them
        uint32_t oldest = 0;
        for (const auto& s : segments_)
            if (s.seq && (oldest == 0 || s.seq < oldest)) oldest = s.seq;
        if (!oldest) return false;                       // nothing complete yet

        // Advertise from the oldest plus a margin, never the oldest itself: @xref{the-playlist-advertises-from-the-oldest-plus-a-margin|what each end failed at on the bench}.
        uint32_t first = oldest + kReserved;
        if (first >= nextSeq_) first = oldest;   // ring not yet full: nothing to reserve
        int n = std::snprintf(playlist, sizeof(playlist),
                              "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                              "#EXT-X-MEDIA-SEQUENCE:%u\n", static_cast<unsigned>(first));
        for (uint32_t q = first; q < nextSeq_ && n > 0 && n < static_cast<int>(sizeof(playlist)); q++) {
            const Segment* seg = nullptr;
            for (const auto& s : segments_) if (s.seq == q) seg = &s;
            if (!seg) continue;
            // The segment's REAL duration, from the frames actually in it. Claiming a flat 1.0 s
            // while delivering fewer makes the player run ahead of the stream until it stalls to
            // re-buffer -- the periodic hiccup, visible as segments arriving every ~0.7 s.
            const uint32_t milli = fps_ ? (static_cast<uint32_t>(seg->frames) * 1000u) / fps_ : 1000u;
            // snprintf returns the length it WOULD have written, so an unchecked accumulate can
            // push n past the buffer and report more bytes than exist. Not reachable at this
            // sizing, but the clamp costs nothing and the failure would be served garbage.
            if (n < 0 || n >= static_cast<int>(sizeof(playlist))) break;
            n += std::snprintf(playlist + n, sizeof(playlist) - n,
                               "#EXTINF:%u.%03u,\nseg%u.ts\n",
                               static_cast<unsigned>(milli / 1000u),
                               static_cast<unsigned>(milli % 1000u), static_cast<unsigned>(q));
        }
        // Clamp: snprintf reports the length it WOULD have written, so an accumulated n can
        // exceed the buffer and hand the caller bytes past its end.
        if (n < 0) return false;
        if (n > static_cast<int>(sizeof(playlist)) - 1) n = static_cast<int>(sizeof(playlist)) - 1;
        *data = reinterpret_cast<const uint8_t*>(playlist);
        *len  = static_cast<size_t>(n);
        return *len > 0;
    }

    unsigned q = 0;
    if (std::sscanf(name, "seg%u.ts", &q) != 1) return false;
    for (const auto& s : segments_) {
        if (s.seq == q && s.len > 0) {
            // Reserved until hlsSegmentRelease: the caller reads this pointer after the lock
            // drops, and the encoder must not recycle the slot underneath it.
            serving_ = q;
            *data = s.data;
            *len = s.len;
            return true;
        }
    }
    return false;
}

void hlsSegmentRelease() { serving_ = kNoSeg; }

}  // namespace mm::platform

#else   // !CONFIG_MM_HLS

// The HTTP server calls the RAM-segment seam on every /hls/ request whatever the platform, so a
// build without the encoder still has to answer it: no segments in RAM, fall through to the
// filesystem path (where there is nothing either, and the request 404s as it should).
namespace mm::platform {
bool hlsSegment(const char*, const uint8_t**, size_t*) { return false; }
void hlsSegmentRelease() {}
// The whole encoder seam, not just the segment half: platform.h declares these for every target,
// so a build that reaches them without CONFIG_MM_HLS must link rather than fail. Starting fails,
// which is what the driver reports; the rest are inert.
bool encoderStart(const EncoderConfig&) { return false; }
int  encoderWrite(const uint8_t*, size_t) { return -1; }
bool encoderRunning() { return false; }
void encoderStop() {}
}  // namespace mm::platform

#endif  // CONFIG_MM_HLS
