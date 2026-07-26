// ES8311 audio-codec init — the I2C control half of the microphone path on boards
// whose mic is an analog part behind an ES8311 I2S codec, rather than a direct
// digital I2S MEMS mic. The I2S *read*
// stays in platform_esp32_i2s.cpp (audioMic*); this file only brings the codec up
// over I2C so it streams its ADC (mic) onto the I2S bus the read then drains. So
// the audio domain code (AudioService) is unchanged — it calls audioCodecInit (a
// no-op on direct-mic boards) before audioMicInit, and reads samples as always.
//
// Uses Espressif's esp_codec_dev managed component (the recognized ES8311
// driver), gated to codec-bearing firmware in main/idf_component.yml. This is
// the platform layer's first I2C master bus, owned here behind the boundary.
//
// Compiles on every ESP32 chip: the codec path is under SOC_I2S_SUPPORTED and the
// esp_codec_dev availability gate; everything else gets an inert stub (audioCodecInit
// returns true — nothing to bring up — so the uniform AudioService call works).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// esp_codec_dev is only pulled on the S31 (idf_component.yml rule). Gate the codec
// implementation on its presence so the file still compiles on every other target.
#if SOC_I2S_SUPPORTED && __has_include("esp_codec_dev.h")
#define MM_HAS_ES8311 1
#endif

#if MM_HAS_ES8311

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#include <new>   // std::nothrow

namespace mm::platform {

namespace {

const char* ES_TAG = "mm_es8311";

// The codec interface + its control interface and I2C bus, kept alive between
// init and deinit (the codec keeps streaming once enabled; the I2S read drains it).
struct CodecState {
    i2c_master_bus_handle_t   i2cBus  = nullptr;
    const audio_codec_ctrl_if_t* ctrl = nullptr;
    const audio_codec_if_t*   codec   = nullptr;
};

CodecState* g_codec = nullptr;

// Tear down a partially- or fully-built CodecState in reverse order.
void deinitState(CodecState* st) {
    if (!st) return;
    if (st->codec) audio_codec_delete_codec_if(st->codec);
    if (st->ctrl) audio_codec_delete_ctrl_if(st->ctrl);
    if (st->i2cBus) i2c_del_master_bus(st->i2cBus);
    delete st;
}

}  // namespace

bool audioCodecInit(CodecType type, const AudioCodecPins& pins, uint32_t sampleRate) {
    if (type == CodecType::None) return true;     // direct-mic board: nothing to do
    if (type != CodecType::Es8311) return false;  // unknown codec for this build

    audioCodecDeinit();   // idempotent: a re-init (pin/rate change) rebuilds cleanly
    auto* st = new (std::nothrow) CodecState();
    if (!st) return false;

    // The platform's I2C master bus — owned by the codec (no other platform user yet).
    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = I2C_NUM_0;
    busCfg.sda_io_num = static_cast<gpio_num_t>(pins.i2cSda);
    busCfg.scl_io_num = static_cast<gpio_num_t>(pins.i2cScl);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&busCfg, &st->i2cBus) != ESP_OK) {
        ESP_LOGE(ES_TAG, "i2c bus init failed (sda %u scl %u)", pins.i2cSda, pins.i2cScl);
        delete st;
        return false;
    }

    // ES8311 over I2C — the codec's control interface (the I2S *data* interface is
    // ours: audioMicInit owns the RX channel, so we don't hand esp_codec_dev the I2S
    // handles or use esp_codec_dev_read; the codec just needs its registers set so it
    // streams the ADC onto the bus). The mic path is record-only.
    audio_codec_i2c_cfg_t i2cCtrlCfg = {};
    i2cCtrlCfg.port = I2C_NUM_0;
    // esp_codec_dev keeps the legacy 8-bit wire address and shifts it to seven
    // bits inside its modern i2c_master adapter. The platform config stores the
    // address in the standard i2cdetect shape (0x18), so convert at this seam.
    i2cCtrlCfg.addr = pins.i2cAddr << 1;
    i2cCtrlCfg.bus_handle = st->i2cBus;
    st->ctrl = audio_codec_new_i2c_ctrl(&i2cCtrlCfg);
    if (!st->ctrl) { ESP_LOGE(ES_TAG, "codec i2c ctrl failed"); deinitState(st); return false; }

    es8311_codec_cfg_t es8311Cfg = {};
    es8311Cfg.ctrl_if = st->ctrl;
    es8311Cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC;   // record / mic only
    es8311Cfg.digital_mic = false;                        // onboard analog microphone
    es8311Cfg.use_mclk = true;                            // MCLK uses the firmware target's codec pin
    es8311Cfg.mclk_div = 256;                             // MCLK = 256 * sample_rate (the standard
                                                          // I2S ratio; the codec's coeff table is
                                                          // keyed on it — 0 fails "configure rate").
    es8311Cfg.pa_pin = -1;                                // mic path needs no power amp
    st->codec = es8311_codec_new(&es8311Cfg);
    if (!st->codec) { ESP_LOGE(ES_TAG, "es8311_codec_new failed"); deinitState(st); return false; }

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sampleRate;
    fs.channel = 1;
    // The platform RX channel uses a 32-bit standard-I2S slot. Match the codec's
    // word length so its 24-bit ADC result stays aligned in the int32_t samples
    // AudioService analyzes; a 16-bit codec word in that slot loses precision
    // and presents the sample at a different significance.
    fs.bits_per_sample = 32;
    // ProjectMM owns the I2S channel in platform_esp32_i2s.cpp, so configure the
    // codec interface directly. esp_codec_dev_new is the higher-level adapter
    // for callers that also give the component a data_if; it rejects a null
    // data_if and is therefore the wrong layer for this split-ownership design.
    if (!st->codec->set_fs || st->codec->set_fs(st->codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(ES_TAG, "es8311 sample format failed");
        deinitState(st);
        return false;
    }
    if (!st->codec->enable || st->codec->enable(st->codec, true) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(ES_TAG, "es8311 enable failed");
        deinitState(st);
        return false;
    }
    if (st->codec->set_mic_gain) st->codec->set_mic_gain(st->codec, 30.0f);

    g_codec = st;
    return true;
}

void audioCodecDeinit() {
    if (g_codec) { deinitState(g_codec); g_codec = nullptr; }
}

}  // namespace mm::platform

#else  // !MM_HAS_ES8311 — no codec on this target: inert stub.

#include <new>

namespace mm::platform {
bool audioCodecInit(CodecType type, const AudioCodecPins&, uint32_t) {
    // No codec: there's nothing to configure, so a None request succeeds and any
    // codec request fails (a board asking for a codec this build can't drive).
    return type == CodecType::None;
}
void audioCodecDeinit() {}
}  // namespace mm::platform

#endif  // MM_HAS_ES8311
