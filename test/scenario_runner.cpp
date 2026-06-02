// Scenario runner: reads scenario JSON files, replays steps in-process.
// When HTTP API is added, the same JSON files work with a Python runner
// against a live system.

#include "core/Scheduler.h"
#include "core/ModuleFactory.h"
#include "light/layouts/GridLayout.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/ArtNetSendDriver.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <fstream>

static void printModuleMemory(mm::MoonModule* mod, int indent) {
    if (!mod) return;
    for (int i = 0; i < indent; i++) std::printf("  ");
    std::printf("%s: sizeof=%zu heap=%zu\n",
                mod->name() ? mod->name() : "?",
                mod->classSize(), mod->dynamicBytes());
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        printModuleMemory(mod->child(i), indent + 1);
    }
}
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

// Register the module types this runner can replay. Heap-allocated by the
// factory (new T()) so Scheduler::teardown()'s deleteTree can validly delete
// them — same ownership model as production main.cpp. Idempotent: safe to call
// before every scenario.
static void registerScenarioTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts");
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers");
    mm::ModuleFactory::registerType<mm::ArtNetSendDriver>("ArtNetSendDriver");
    done = true;
}

// Module registry for scenario replay
struct ScenarioContext {
    mm::Scheduler scheduler;
    std::map<std::string, mm::MoonModule*> modules;

    // Modules are heap-allocated by the factory; Scheduler::teardown owns and
    // deletes them.
    mm::MoonModule* createModule(const char* type) {
        return mm::ModuleFactory::create(type);
    }

    void wireModule(const char* type, const char* id, const JsonVal& step) {
        auto* mod = modules[id];
        if (!mod) return;

        // Wire parent/child
        if (step.has("parent_id")) {
            const char* parentId = step["parent_id"].c_str();
            auto* parent = modules[parentId];
            if (parent) {
                parent->addChild(mod);
            }
        }

        // Wire props
        if (step.has("props")) {
            auto& props = step["props"];
            if (std::strcmp(type, "Layer") == 0) {
                auto* layer = static_cast<mm::Layer*>(mod);
                if (props.has("layouts")) {
                    auto* layoutsModule = static_cast<mm::Layouts*>(modules[props["layouts"].str]);
                    if (layoutsModule) layer->setLayouts(layoutsModule);
                }
                if (props.has("channelsPerLight")) {
                    layer->setChannelsPerLight(static_cast<uint8_t>(props["channelsPerLight"].num));
                }
            } else if (std::strcmp(type, "Drivers") == 0) {
                if (props.has("layer")) {
                    auto* layerModule = static_cast<mm::Layer*>(modules[props["layer"].str]);
                    if (layerModule) static_cast<mm::Drivers*>(mod)->setLayer(layerModule);
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
    registerScenarioTypes();

    std::string text = readFile(path);
    if (text.empty()) {
        std::printf("Cannot read scenario file: %s\n", path);
        return 1;
    }

    auto scenario = parseJson(text);
    std::printf("=== Scenario: %s ===\n", scenario["name"].c_str());
    std::printf("%s\n\n", scenario["description"].c_str());

    // Skip live-only scenarios (no add_module steps — requires running device)
    bool hasAddModule = false;
    for (auto& step : scenario["steps"].arr) {
        if (std::strcmp(step["op"].c_str(), "add_module") == 0) { hasAddModule = true; break; }
    }
    if (!hasAddModule) {
        std::printf("  SKIP (live-only scenario, no add_module steps)\n");
        return 0;
    }

    ScenarioContext ctx;
    Result result;

    // Replay steps
    bool hasMeasure = false;
    double fpsBound = 0;
    double fpsLedProduct = 0;
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
        if (step.has("bounds") && step["bounds"].has("fps")) {
            if (step["bounds"]["fps"].has("min"))
                fpsBound = step["bounds"]["fps"]["min"].num;
            else if (step["bounds"]["fps"].has("min_pct"))
                fpsBound = 1; // min_pct is for live runner; in-process just checks FPS > 0
            // min_fps_led_product: an FPS×lights throughput floor that scales to
            // any grid. Compared against the measured tick *time* (the native
            // unit — FPS is only ever derived by lossy division): the per-grid
            // budget is maxTickUs = lights × 1e6 / product, resolved at measure
            // time once the buffer size is known.
            if (step["bounds"]["fps"].has("min_fps_led_product"))
                fpsLedProduct = step["bounds"]["fps"]["min_fps_led_product"].num;
        }
    }

    // Memory before setup
    size_t heapBefore = mm::platform::freeHeap();

    // Setup all modules
    ctx.scheduler.setup();

    // Memory after setup
    size_t heapAfter = mm::platform::freeHeap();
    if (heapBefore > 0) {
        long delta = static_cast<long>(heapBefore) - static_cast<long>(heapAfter);
        std::printf("\n  Heap: %u → %u (pipeline: %ld bytes)\n",
                    static_cast<unsigned>(heapBefore),
                    static_cast<unsigned>(heapAfter), delta);
    }

    // Memory report per module
    std::printf("  Memory:\n");
    for (uint8_t m = 0; m < ctx.scheduler.moduleCount(); m++) {
        auto* mod = ctx.scheduler.module(m);
        if (!mod) continue;
        printModuleMemory(mod, 2);
    }

    // Verify buffer — the render buffer lives on the Layer module.
    auto* layer = static_cast<mm::Layer*>(ctx.modules.count("Layer")
                                              ? ctx.modules["Layer"] : nullptr);
    auto* drivers = static_cast<mm::Drivers*>(ctx.modules.count("Drivers")
                                              ? ctx.modules["Drivers"] : nullptr);
    if (!layer) {
        std::printf("  (no Layer module — skipping buffer checks)\n");
        ctx.scheduler.teardown();
        std::printf("---\nPASSED (%d checks)\n", result.checks);
        return result.passed ? 0 : 1;
    }
    auto& buf = layer->buffer();
    result.check(buf.data() != nullptr, "buffer allocated");
    result.check(buf.count() > 0, "buffer has lights");
    std::printf("  Buffer: %u lights, %u bytes\n",
                static_cast<unsigned>(buf.count()),
                static_cast<unsigned>(buf.bytes()));
    std::printf("  LUT: %s  dynamicBytes: Layer=%u Drivers=%u\n",
                layer->lut().hasLUT() ? "has LUT" : "identity",
                static_cast<unsigned>(layer->dynamicBytes()),
                static_cast<unsigned>(drivers ? drivers->dynamicBytes() : 0));

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
        uint32_t tickTimeUs = MEASURE_FRAMES > 0 ? elapsedUs / MEASURE_FRAMES : 0;
        uint32_t fps = tickTimeUs > 0 ? 1000000 / tickTimeUs : 0;

        std::printf("\n");
        std::printf("  tick: %uus (FPS: %u)\n\n",
                    static_cast<unsigned>(tickTimeUs), static_cast<unsigned>(fps));

        // Heap after render (check for leaks)
        size_t heapAfterRender = mm::platform::freeHeap();
        if (heapAfter > 0) {
            int64_t delta = static_cast<int64_t>(heapAfter) - static_cast<int64_t>(heapAfterRender);
            std::printf("  Heap after render: %u (delta: %+lld bytes)\n",
                        static_cast<unsigned>(heapAfterRender),
                        static_cast<long long>(delta));
        }

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

        // FPS×lights throughput floor — compared against the measured tick
        // *time* (native unit), not derived FPS. Per-grid budget scales with the
        // light count: maxTickUs = lights × 1e6 / product.
        if (fpsLedProduct > 0 && buf.count() > 0) {
            double maxTickUs = buf.count() * 1000000.0 / fpsLedProduct;
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "tick <= %.0fus (%u lights, throughput floor)",
                          maxTickUs, static_cast<unsigned>(buf.count()));
            result.check(static_cast<double>(tickTimeUs) <= maxTickUs, msg);
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
        // Run all scenarios in the scenarios/ directory tree.
        // Recursive so the core/ + light/ split picks up every JSON without
        // each subfolder needing its own discovery loop.
        int failed = 0;
        int total = 0;
        for (auto& entry : std::filesystem::recursive_directory_iterator("test/scenarios")) {
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
