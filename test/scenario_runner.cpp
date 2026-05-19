// Scenario runner: reads scenario JSON files, replays steps in-process.
// When HTTP API is added, the same JSON files work with a Python runner
// against a live system (like projectMM v1's deploy/scenario.py).

#include "core/Scheduler.h"
#include "light/GridLayout.h"
#include "light/RainbowEffect.h"
#include "light/NoiseEffect.h"
#include "light/MirrorModifier.h"
#include "light/ArtNetSendDriver.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <map>
#include <filesystem>
#include <vector>

// Minimal JSON value — enough for scenario files (flat objects, arrays of objects)
struct JsonVal {
    enum Type { Null, String, Number, Bool, Object, Array };
    Type type = Null;
    std::string str;
    double num = 0;
    bool boolean = false;
    std::map<std::string, JsonVal> obj;
    std::vector<JsonVal> arr;

    bool has(const char* key) const { return obj.count(key) > 0; }
    const JsonVal& operator[](const char* key) const {
        static JsonVal null;
        auto it = obj.find(key);
        return it != obj.end() ? it->second : null;
    }
    const char* c_str() const { return str.c_str(); }
    int asInt() const { return static_cast<int>(num); }
};

// Minimal JSON parser
struct JsonParser {
    const char* p;

    void skipWs() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

    JsonVal parse() {
        skipWs();
        if (*p == '"') return parseString();
        if (*p == '{') return parseObject();
        if (*p == '[') return parseArray();
        if (*p == 't' || *p == 'f') return parseBool();
        if (*p == 'n') { p += 4; return {}; }
        return parseNumber();
    }

    JsonVal parseString() {
        p++; // skip opening "
        JsonVal v; v.type = JsonVal::String;
        while (*p && *p != '"') {
            if (*p == '\\') { p++; v.str += *p++; }
            else v.str += *p++;
        }
        if (*p == '"') p++;
        return v;
    }

    JsonVal parseNumber() {
        JsonVal v; v.type = JsonVal::Number;
        const char* start = p;
        if (*p == '-') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
        v.num = std::strtod(start, nullptr);
        return v;
    }

    JsonVal parseBool() {
        JsonVal v; v.type = JsonVal::Bool;
        if (*p == 't') { v.boolean = true; p += 4; }
        else { v.boolean = false; p += 5; }
        return v;
    }

    JsonVal parseObject() {
        p++; // skip {
        JsonVal v; v.type = JsonVal::Object;
        skipWs();
        while (*p && *p != '}') {
            auto key = parseString();
            skipWs(); p++; skipWs(); // skip :
            v.obj[key.str] = parse();
            skipWs();
            if (*p == ',') { p++; skipWs(); }
        }
        if (*p == '}') p++;
        return v;
    }

    JsonVal parseArray() {
        p++; // skip [
        JsonVal v; v.type = JsonVal::Array;
        skipWs();
        while (*p && *p != ']') {
            v.arr.push_back(parse());
            skipWs();
            if (*p == ',') { p++; skipWs(); }
        }
        if (*p == ']') p++;
        return v;
    }
};

static JsonVal parseJson(const std::string& text) {
    JsonParser parser{text.c_str()};
    return parser.parse();
}

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Module registry for scenario replay
struct ScenarioContext {
    mm::Scheduler scheduler;
    std::map<std::string, mm::MoonModule*> modules;

    // Concrete module storage (stack-allocated, max one of each for now)
    mm::LayoutGroup layoutGroup;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::RainbowEffect rainbow;
    mm::NoiseEffect noise;
    mm::MirrorModifier mirror;
    mm::DriverGroup driverGroup;
    mm::ArtNetSendDriver artnet;

    mm::MoonModule* createModule(const char* type) {
        if (std::strcmp(type, "LayoutGroup") == 0) return &layoutGroup;
        if (std::strcmp(type, "GridLayout") == 0) return &grid;
        if (std::strcmp(type, "Layer") == 0) return &layer;
        if (std::strcmp(type, "RainbowEffect") == 0) return &rainbow;
        if (std::strcmp(type, "NoiseEffect") == 0) return &noise;
        if (std::strcmp(type, "MirrorModifier") == 0) return &mirror;
        if (std::strcmp(type, "DriverGroup") == 0) return &driverGroup;
        if (std::strcmp(type, "ArtNetSendDriver") == 0) return &artnet;
        return nullptr;
    }

    void wireModule(const char* type, const char* id, const JsonVal& step) {
        auto* mod = modules[id];
        if (!mod) return;

        // Wire parent/child
        if (step.has("parent_id")) {
            const char* parentId = step["parent_id"].c_str();
            auto* parent = modules[parentId];
            if (parent) {
                if (std::strcmp(type, "GridLayout") == 0) {
                    static_cast<mm::LayoutGroup*>(parent)->addLayout(
                        static_cast<mm::LayoutBase*>(mod));
                } else if (std::strcmp(type, "RainbowEffect") == 0 ||
                           std::strcmp(type, "NoiseEffect") == 0) {
                    static_cast<mm::Layer*>(parent)->addEffect(
                        static_cast<mm::EffectBase*>(mod));
                } else if (std::strcmp(type, "MirrorModifier") == 0) {
                    static_cast<mm::Layer*>(parent)->addModifier(
                        static_cast<mm::ModifierBase*>(mod));
                } else if (std::strcmp(type, "ArtNetSendDriver") == 0) {
                    static_cast<mm::DriverGroup*>(parent)->addDriver(
                        static_cast<mm::DriverBase*>(mod));
                }
            }
        }

        // Wire props
        if (step.has("props")) {
            auto& props = step["props"];
            if (std::strcmp(type, "Layer") == 0) {
                if (props.has("layoutGroup")) {
                    auto* lg = static_cast<mm::LayoutGroup*>(modules[props["layoutGroup"].str]);
                    if (lg) layer.setLayoutGroup(lg);
                }
                if (props.has("channelsPerLight")) {
                    layer.setChannelsPerLight(static_cast<uint8_t>(props["channelsPerLight"].num));
                }
            } else if (std::strcmp(type, "DriverGroup") == 0) {
                if (props.has("layer")) {
                    auto* l = static_cast<mm::Layer*>(modules[props["layer"].str]);
                    if (l) driverGroup.setLayer(l);
                }
            }
        }
    }
};

static constexpr int WARMUP_FRAMES = 10;
static constexpr int MEASURE_FRAMES = 200;

struct Result {
    bool passed = true;
    int checks = 0;
    int failures = 0;

    void check(bool condition, const char* name) {
        checks++;
        if (condition) {
            std::printf("  PASS  %s\n", name);
        } else {
            std::printf("  FAIL  %s\n", name);
            passed = false;
            failures++;
        }
    }
};

static int runScenario(const char* path) {
    std::string text = readFile(path);
    if (text.empty()) {
        std::printf("Cannot read scenario file: %s\n", path);
        return 1;
    }

    auto scenario = parseJson(text);
    std::printf("=== Scenario: %s ===\n", scenario["name"].c_str());
    std::printf("%s\n\n", scenario["description"].c_str());

    ScenarioContext ctx;
    Result result;

    // Replay steps
    bool hasMeasure = false;
    double fpsBound = 0;
    for (auto& step : scenario["steps"].arr) {
        const char* name = step["name"].c_str();
        const char* op = step["op"].c_str();

        if (std::strcmp(op, "add_module") == 0) {
            const char* type = step["type"].c_str();
            const char* id = step["id"].c_str();

            auto* mod = ctx.createModule(type);
            if (!mod) {
                std::printf("  SKIP  %s (unknown type: %s)\n", name, type);
                continue;
            }
            mod->setName(id);
            ctx.modules[id] = mod;
            ctx.wireModule(type, id, step);

            // Only register top-level modules (no parent_id) with scheduler
            if (!step.has("parent_id")) {
                ctx.scheduler.addModule(mod);
            }

            std::printf("  +     %s (%s)\n", id, type);
        } else if (std::strcmp(op, "set_control") == 0) {
            std::printf("  SET   %s\n", name);
            // Control setting via HTTP — not yet implemented for in-process replay
        }

        if (step.has("measure") && step["measure"].boolean) {
            hasMeasure = true;
        }
        if (step.has("bounds") && step["bounds"].has("fps") &&
            step["bounds"]["fps"].has("min")) {
            fpsBound = step["bounds"]["fps"]["min"].num;
        }
    }

    // Setup all modules
    ctx.scheduler.setup();

    // Verify buffer
    auto& buf = ctx.layer.buffer();
    result.check(buf.data() != nullptr, "buffer allocated");
    result.check(buf.count() > 0, "buffer has lights");

    // Warmup
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        ctx.scheduler.tick();
    }

    // Measure
    if (hasMeasure) {
        std::printf("\nRunning %d frames...\n", MEASURE_FRAMES);
        uint32_t startUs = mm::platform::micros();

        for (int i = 0; i < MEASURE_FRAMES; i++) {
            ctx.scheduler.tick();
        }

        uint32_t elapsedUs = mm::platform::micros() - startUs;
        float elapsedMs = static_cast<float>(elapsedUs) / 1000.0f;
        float msPerFrame = elapsedMs > 0 ? elapsedMs / MEASURE_FRAMES : 0;
        float fps = msPerFrame > 0 ? 1000.0f / msPerFrame : 0;

        std::printf("\n");
        std::printf("  Total:     %.1f ms\n", static_cast<double>(elapsedMs));
        std::printf("  Per frame: %.2f ms\n", static_cast<double>(msPerFrame));
        std::printf("  FPS:       %.0f\n\n", static_cast<double>(fps));

        // Check output
        bool hasNonZero = false;
        for (size_t i = 0; i < buf.bytes(); i++) {
            if (buf.data()[i] != 0) { hasNonZero = true; break; }
        }
        result.check(hasNonZero, "buffer non-zero after render");

        // FPS bound
        if (fpsBound > 0) {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "fps >= %.0f", fpsBound);
            result.check(fps >= static_cast<float>(fpsBound), msg);
        }
    }

    ctx.scheduler.teardown();

    // Summary
    std::printf("---\n");
    if (result.passed) {
        std::printf("PASSED (%d checks)\n", result.checks);
    } else {
        std::printf("FAILED (%d/%d checks)\n", result.failures, result.checks);
    }
    return result.passed ? 0 : 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Run all scenarios in the scenarios/ directory
        int failed = 0;
        int total = 0;
        for (auto& entry : std::filesystem::directory_iterator("test/scenarios")) {
            if (entry.path().extension() == ".json") {
                total++;
                if (runScenario(entry.path().c_str()) != 0) failed++;
                std::printf("\n");
            }
        }
        std::printf("=== %d scenario(s), %d passed, %d failed ===\n",
                    total, total - failed, failed);
        return failed > 0 ? 1 : 0;
    }

    return runScenario(argv[1]);
}
