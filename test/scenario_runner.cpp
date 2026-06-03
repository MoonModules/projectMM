// Scenario runner: reads scenario JSON files, replays steps in-process.
// When HTTP API is added, the same JSON files work with a Python runner
// against a live system.

#include "core/Scheduler.h"
#include "core/ModuleFactory.h"
#include "core/Control.h"
#include "core/JsonSink.h"
#include "light/layouts/GridLayout.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layers.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/ArtNetSendDriver.h"
#include "light/drivers/PreviewDriver.h"
#include "core/PreviewFrame.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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
    mm::ModuleFactory::registerType<mm::Layers>("Layers");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MirrorModifier>("MirrorModifier");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers");
    mm::ModuleFactory::registerType<mm::ArtNetSendDriver>("ArtNetSendDriver");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver");
    done = true;
}

// PreviewDriver needs a frame to write into; main.cpp owns it. In the scenario
// runner we own a single static frame so mutate scenarios with PreviewDriver in
// their fixture work without a separate `setPreviewFrame` step.
static mm::PreviewFrame& scenarioPreviewFrame() {
    static mm::PreviewFrame frame;
    return frame;
}

// Target key for the per-step expected[<target>] lookup. The in-process runner
// builds for the host only — there's no cross-compiled scenario_runner — so the
// key is always pc-<host-os>. Matches the run_live_scenario.py convention.
static const char* hostTarget() {
#if defined(__APPLE__)
    return "pc-macos";
#elif defined(_WIN32)
    return "pc-windows";
#elif defined(__linux__)
    return "pc-linux";
#else
    return "pc-unknown";
#endif
}

// Apply a set_control step in-process: find the module by id, find the control by
// name, write the typed value, then mirror what HttpServerModule::handleSetControl
// does — call onUpdate(), and if controlChangeTriggersBuildState() returns true
// trigger Scheduler::buildState() so the pipeline reconciles. Returns true if the
// write applied; false on any lookup miss or unsupported type (caller may want to
static bool applySetControl(mm::Scheduler& scheduler,
                            mm::MoonModule* target,
                            const char* controlName,
                            const JsonVal& value) {
    if (!target || !controlName) return false;
    auto& controls = target->controls();
    for (uint8_t i = 0; i < controls.count(); i++) {
        const auto& c = controls[i];
        if (!c.name || std::strcmp(c.name, controlName) != 0) continue;
        // Bridge JsonVal → raw JSON text → mm::applyControlValue, so this
        // file no longer hand-rolls the per-ControlType dispatch that
        // Control.cpp owns. Build a tiny wrapper object `{"v":VALUE}` via
        // JsonSink (heap-grow mode); writeNumber/writeBool/writeJsonString
        // produce JSON-correct text per JsonVal::type. Re-serialize-then-
        // parse cost is irrelevant in test code (≤100 set_control ops per
        // scenario). Strict policy: out-of-range Uint8/Int16/Select fails
        // the set_control so scenario-authoring bugs surface instead of
        // silently clamping into a boundary value. This is stricter than
        // the pre-refactor behaviour for Uint8/Int16 (which silently
        // clamped) but matches it for Select/IPv4 (which already failed);
        // no existing scenario relied on the silent-clamp shape.
        mm::JsonSink wrapper;
        wrapper.append("{\"v\":");
        switch (value.type) {
            case JsonVal::Number: wrapper.writeNumber(value.num); break;
            case JsonVal::Bool:   wrapper.writeBool(value.boolean); break;
            case JsonVal::String: wrapper.writeJsonString(value.str.c_str()); break;
            default:              wrapper.append("null"); break;
        }
        wrapper.append("}");
        mm::ApplyResult r = mm::applyControlValue(c, wrapper.data(), "v",
                                                  mm::ApplyPolicy::Strict);
        if (r != mm::ApplyResult::Ok) return false;
        if (c.type == mm::ControlType::Select) target->rebuildControls();
        target->onUpdate(controlName);
        if (target->controlChangeTriggersBuildState(controlName)) {
            scheduler.buildState();
        }
        return true;
    }
    return false;
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

        // Wire props (only when the step has any).
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

        // PreviewDriver always needs its scenario-static PreviewFrame target
        // wired — independent of any step "props" the scenario provides. The
        // driver reads no other init from props (no layouts/parent like Layer
        // or Drivers do), but its loop() early-outs without a frame_, so
        // setPreviewFrame must run unconditionally for honest tick measurement.
        // Production wires this via HttpServerModule on the device.
        if (std::strcmp(type, "PreviewDriver") == 0) {
            static_cast<mm::PreviewDriver*>(mod)->setPreviewFrame(&scenarioPreviewFrame());
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
    std::printf("%s\n", scenario["description"].c_str());
    std::printf("Target: %s\n\n", hostTarget());

    // Mode field (construct/mutate) determines what shape the scenario expects
    // the world to be in. See docs/testing.md § Scenario modes.
    //   construct → scenario builds the pipeline from an empty scheduler; runs
    //               in-process only (live device's main.cpp owns the top-level
    //               shape; constructing fresh requires an empty scheduler that
    //               only the in-process runner can provide).
    //   mutate    → scenario assumes a wired pipeline. In-process replays the
    //               embedded `fixture` array first, then the steps. Live runs
    //               steps directly against whatever's wired.
    // Default: construct (back-compat with the existing scenarios that pre-date
    // this field; they all build pipelines explicitly).
    // Bespoke convention: the construct/mutate split + fixture + reset trinity
    // is projectMM-specific (no off-the-shelf BDD/scenario framework borrowed
    // wholesale). It exists because the same JSON has to serve both an
    // in-process runner (which owns the scheduler) and a live runner (which
    // doesn't — main.cpp does). xUnit fixtures are the closest analog for
    // `fixture`; SQL BEGIN/ROLLBACK is the closest for `reset`.
    std::string mode = scenario.has("mode") ? scenario["mode"].str : std::string("construct");

    if (mode == "mutate") {
        // In-process replays the fixture (an array of add_module steps in the
        // same shape as `steps`) before running the scenario's actual steps.
        // A mutate scenario without a fixture can still run live — the device
        // is its own fixture — but cannot run in-process.
        if (!scenario.has("fixture") || scenario["fixture"].arr.empty()) {
            std::printf("  SKIP (mutate scenario with no fixture — runs live only)\n");
            return 0;
        }
    } else if (mode != "construct") {
        std::printf("  FAIL — unknown mode: %s (expected construct or mutate)\n", mode.c_str());
        return 1;
    }

    // Legacy tier flag: live_only still honoured for any scenario that uses it.
    // Newer scenarios should prefer mode=mutate (with/without fixture) instead.
    if (scenario.has("live_only") && scenario["live_only"].boolean) {
        std::printf("  SKIP (live_only)\n");
        return 0;
    }

    ScenarioContext ctx;
    Result result;

    // Lazy-setup model: process steps in order. First measure step (or the end
    // of the scenario, whichever comes first) flips the scheduler into
    // setup+running mode. After that, mid-scenario add_module / set_control
    // steps mutate the running pipeline — same shape as the live runner driving
    // changes over REST. Per-step heap snapshots roll forward so each
    // `measure: true` step reports its delta against the previous one.
    bool schedulerStarted = false;
    size_t heapBefore = mm::platform::freeHeap();
    size_t heapAfter = heapBefore;       // updated on setup + after every measure
    auto ensureStarted = [&]() {
        if (schedulerStarted) return;
        ctx.scheduler.setup();
        schedulerStarted = true;
        heapAfter = mm::platform::freeHeap();
        if (heapBefore > 0) {
            long delta = static_cast<long>(heapBefore) - static_cast<long>(heapAfter);
            std::printf("\n  Heap: %u → %u (pipeline: %ld bytes)\n",
                        static_cast<unsigned>(heapBefore),
                        static_cast<unsigned>(heapAfter), delta);
        }
        std::printf("  Memory:\n");
        for (uint8_t m = 0; m < ctx.scheduler.moduleCount(); m++) {
            auto* mod = ctx.scheduler.module(m);
            if (mod) printModuleMemory(mod, 2);
        }
    };

    // Three sections in order: fixture (add_module shape, in-process only —
    // builds the wired pipeline), reset (set_control to a known state — runs
    // both tiers, makes the scenario start from the same place regardless of
    // previous runs), steps (the actual scenario). Each section gets its own
    // banner so the output is easy to scan.
    std::vector<const JsonVal*> allSteps;
    size_t fixtureSize = 0, resetSize = 0;
    if (scenario.has("fixture")) {
        for (auto& s : scenario["fixture"].arr) allSteps.push_back(&s);
        fixtureSize = scenario["fixture"].arr.size();
    }
    if (scenario.has("reset")) {
        for (auto& s : scenario["reset"].arr) allSteps.push_back(&s);
        resetSize = scenario["reset"].arr.size();
    }
    for (auto& s : scenario["steps"].arr) allSteps.push_back(&s);
    enum class Section { Fixture, Reset, Steps };
    Section section = fixtureSize > 0 ? Section::Fixture
                    : resetSize > 0   ? Section::Reset
                    :                   Section::Steps;
    if (section == Section::Fixture) {
        std::printf("  --- fixture (%u steps) ---\n", static_cast<unsigned>(fixtureSize));
    } else if (section == Section::Reset) {
        std::printf("  --- reset (%u steps) ---\n", static_cast<unsigned>(resetSize));
    }

    for (size_t stepIdx = 0; stepIdx < allSteps.size(); stepIdx++) {
        const JsonVal& step = *allSteps[stepIdx];
        // Section boundary banners + lazy scheduler start.
        if (section == Section::Fixture && stepIdx == fixtureSize) {
            // Fixture done: start the scheduler so set_control works (controls
            // are populated in onBuildControls during setup()).
            ensureStarted();
            section = resetSize > 0 ? Section::Reset : Section::Steps;
            std::printf(section == Section::Reset
                        ? "  --- reset (%u steps) ---\n"
                        : "  --- steps ---\n",
                        static_cast<unsigned>(resetSize));
        }
        if (section == Section::Reset && stepIdx == fixtureSize + resetSize) {
            section = Section::Steps;
            std::printf("  --- steps ---\n");
        }
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

            // Mid-scenario adds: setup the new module immediately and rebuild
            // pipeline state. Mirrors what HttpServerModule does on /api/modules.
            if (schedulerStarted) {
                mod->onBuildControls();
                mod->setup();
                ctx.scheduler.buildState();
            }
            std::printf("  +     %s (%s)\n", id, type);
        } else if (std::strcmp(op, "set_control") == 0) {
            if (!step.has("id") || !step.has("key")) {
                std::printf("  SET   %s — missing id/key, skipped\n", name);
                continue;
            }
            const char* targetId = step["id"].c_str();
            const char* key = step["key"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target) {
                std::printf("  SET   %s — module %s not found, skipped\n", name, targetId);
                continue;
            }
            if (!applySetControl(ctx.scheduler, target, key, step["value"])) {
                std::printf("  SET   %s — control %s.%s not applied\n", name, targetId, key);
            } else {
                std::printf("  SET   %s (%s.%s)\n", name, targetId, key);
            }
        } else if (std::strcmp(op, "measure") == 0) {
            // Pure measurement step — no side effects. op:"measure" is the
            // implicit-measure shape so scenarios can interleave snapshots
            // without faking a control write. The measurement block below
            // honours both `measure: true` AND op:"measure".
            std::printf("  ...   %s\n", name);
        }

        // Per-step measurement: warmup + measure + bounded assertions.
        // Triggered by either `"measure": true` on the step (the explicit
        // flag, used alongside set_control to measure after a mutation) or
        // op:"measure" (the implicit-measure shape — a snapshot step with
        // no other side effects).
        const bool isMeasure = (step.has("measure") && step["measure"].boolean)
                            || std::strcmp(op, "measure") == 0;
        if (isMeasure) {
            ensureStarted();
            double fpsBound = 0;
            double fpsLedProduct = 0;
            if (step.has("bounds") && step["bounds"].has("fps")) {
                if (step["bounds"]["fps"].has("min"))
                    fpsBound = step["bounds"]["fps"]["min"].num;
                else if (step["bounds"]["fps"].has("min_pct")) {
                    // min_pct is relative to a live baseline (the WiFi-vs-Eth
                    // scenarios use it) and only the live runner has a baseline
                    // to compare against. In-process can't enforce it — log a
                    // clear skip so users see *why* the bound wasn't applied
                    // instead of silently treating it as "FPS > 0".
                    double pct = step["bounds"]["fps"]["min_pct"].num;
                    std::printf("  WARN  %s: bounds.fps.min_pct=%g requires a live "
                                "baseline; in-process runner cannot enforce — skipped\n",
                                name, pct);
                    fpsBound = 0;
                }
                if (step["bounds"]["fps"].has("min_fps_led_product"))
                    fpsLedProduct = step["bounds"]["fps"]["min_fps_led_product"].num;
            }
            long maxHeapDelta = 0;
            bool hasHeapBound = false;
            if (step.has("bounds") && step["bounds"].has("heap") &&
                step["bounds"]["heap"].has("max_delta_bytes")) {
                maxHeapDelta = static_cast<long>(step["bounds"]["heap"]["max_delta_bytes"].num);
                hasHeapBound = true;
            }

            for (int i = 0; i < WARMUP_FRAMES; i++) ctx.scheduler.tick();
            size_t heapBeforeMeasure = mm::platform::freeHeap();
            uint32_t startUs = mm::platform::micros();
            for (int i = 0; i < MEASURE_FRAMES; i++) ctx.scheduler.tick();
            uint32_t elapsedUs = mm::platform::micros() - startUs;
            uint32_t tickTimeUs = MEASURE_FRAMES > 0 ? elapsedUs / MEASURE_FRAMES : 0;
            uint32_t fps = tickTimeUs > 0 ? 1000000 / tickTimeUs : 0;
            size_t heapAfterMeasure = mm::platform::freeHeap();
            // Largest contiguous block — diagnoses heap fragmentation, which on
            // no-PSRAM ESP32 silently degrades the Layer LUT (the buffer needs
            // 60-90 KB contiguous at 128×128 with mirror; fragmentation drops
            // mirror without changing free_heap). 0 on desktop = unlimited.
            size_t maxBlock = mm::platform::maxAllocBlock();

            // Buffer state at this measurement (may be empty in early build-up steps).
            auto* layer = static_cast<mm::Layer*>(
                ctx.modules.count("Layer") ? ctx.modules["Layer"] : nullptr);
            unsigned lights = layer ? static_cast<unsigned>(layer->buffer().count()) : 0;

            // `heap=` is the absolute free-heap after the measurement window
            // — that's what observed.<target>.free_heap consumes (the rolling
            // promise is on actual free heap, not on a delta). On desktop
            // freeHeap() returns 0 ("unlimited") and the value is rendered
            // as 0, which the runner treats as "no heap assertion".
            //
            // `(step: ±N)` is the signed step delta from the pre-step heap to
            // the post-measurement heap — useful for diagnosing which step
            // consumed memory, but not what the contract asserts on. Kept
            // for human-readable diagnostics.
            long stepDelta = heapBefore > 0
                ? static_cast<long>(heapAfter) - static_cast<long>(heapAfterMeasure)
                : 0;
            std::printf("  MEASURE %s: tick=%uus FPS=%u lights=%u heap=%u (step: %+ld) block=%u\n",
                        name,
                        static_cast<unsigned>(tickTimeUs), static_cast<unsigned>(fps),
                        lights, static_cast<unsigned>(heapAfterMeasure), stepDelta,
                        static_cast<unsigned>(maxBlock));
            (void)heapBeforeMeasure;  // tracked through stepDelta above

            // FPS bound (when set)
            if (fpsBound > 0) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "%s fps >= %.0f", name, fpsBound);
                result.check(fps >= static_cast<float>(fpsBound), msg);
            }
            // FPS×lights throughput floor — compared against the measured tick
            // *time* (native unit), not derived FPS.
            if (fpsLedProduct > 0 && lights > 0) {
                double maxTickUs = lights * 1000000.0 / fpsLedProduct;
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s tick <= %.0fus (%u lights, throughput floor)",
                              name, maxTickUs, lights);
                result.check(static_cast<double>(tickTimeUs) <= maxTickUs, msg);
            }
            // Heap-delta bound — fail if this step grew the heap by more than
            // max_delta_bytes vs the previous measurement (catches leaks / unintended allocs).
            if (hasHeapBound && heapBefore > 0) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s heap step delta %+ld <= %ld bytes",
                              name, stepDelta, maxHeapDelta);
                result.check(stepDelta <= maxHeapDelta, msg);
            }

            // Per-step performance contract. Each measure step can carry a
            // `contract[<target>]` block with `tick_us`, `free_heap`, optional
            // `tick_tolerance_pct` / `heap_tolerance_pct` / `tolerance_us`, and
            // `set_by` + `reason` describing when/why the contract was set.
            // Contracts are hand-blessed promises; --update-contract --reason
            // renegotiates them. The whole block is optional; the live runner
            // shares this shape — same scenarios serve both tiers.
            if (step.has("contract") && step["contract"].has(hostTarget())) {
                const auto& exp = step["contract"][hostTarget()];
                // Per-target defaults reflect run-to-run variance, not "I don't care":
                //   pc-*    — multi-process OS jitter, 20% pct + 200us absolute floor.
                //             The floor dominates below ~1ms tick (the realistic case).
                //   esp32-* — bounded RTOS but lwIP/EMAC jitter, 10% pct + 5us floor.
                // KEEP IN SYNC: the live runner re-declares the same defaults at
                // scripts/scenario/run_live_scenario.py contract-block handler —
                // tuning one without the other silently desyncs the two tiers.
                const bool isPc = std::strncmp(hostTarget(), "pc-", 3) == 0;
                double tickTolPct = exp.has("tick_tolerance_pct") ? exp["tick_tolerance_pct"].num
                                                                   : (isPc ? 20.0 : 10.0);
                double heapTolPct = exp.has("heap_tolerance_pct") ? exp["heap_tolerance_pct"].num
                                                                   : (isPc ? 20.0 : 10.0);
                // Absolute floor: at very small ticks (sub-millisecond on PC),
                // OS scheduling jitter dwarfs any percentage tolerance. The PC
                // floor of 200us absorbs typical desktop noise; the ESP32 floor
                // of 5us is realistic for the bounded RTOS clock.
                double tolUs = exp.has("tolerance_us") ? exp["tolerance_us"].num
                                                       : (isPc ? 200.0 : 5.0);
                if (exp.has("tick_us") && exp["tick_us"].num > 0) {
                    // tick is a *ceiling* — faster than contract is good news,
                    // same shape as heap being a floor. Tolerance absorbs upward
                    // jitter only; speedups never fail.
                    double expTick = exp["tick_us"].num;
                    double overshoot = static_cast<double>(tickTimeUs) - expTick;
                    double allowed = expTick * tickTolPct / 100.0;
                    if (allowed < tolUs) allowed = tolUs;
                    char msg[200];
                    if (overshoot <= 0) {
                        std::snprintf(msg, sizeof(msg),
                                      "%s tick %uus <= contract %.0fus (margin %.0fus)",
                                      name, static_cast<unsigned>(tickTimeUs), expTick, -overshoot);
                        result.check(true, msg);
                    } else {
                        std::snprintf(msg, sizeof(msg),
                                      "%s tick %uus vs contract %.0fus (over by %.0fus <= %.0fus)",
                                      name, static_cast<unsigned>(tickTimeUs), expTick, overshoot, allowed);
                        result.check(overshoot <= allowed, msg);
                    }
                }
                // free_heap is meaningful on ESP32 (reports unlimited / 0 on desktop).
                // Contract is a *floor*: the device must deliver at least this much
                // free heap; more is fine; less by more than tolerance is a regression.
                if (exp.has("free_heap") && exp["free_heap"].num > 0 &&
                    heapAfterMeasure > 0) {
                    double expHeap = exp["free_heap"].num;
                    double dropPct = (heapAfterMeasure < expHeap)
                        ? (expHeap - heapAfterMeasure) * 100.0 / expHeap
                        : 0.0;
                    char msg[180];
                    std::snprintf(msg, sizeof(msg),
                                  "%s free_heap %u vs contract %.0f (drop %.1f%% <= %.0f%%)",
                                  name, static_cast<unsigned>(heapAfterMeasure), expHeap, dropPct, heapTolPct);
                    result.check(dropPct <= heapTolPct, msg);
                }
                // max_alloc_block is also a *floor* — the LUT and driver buffers
                // need a single contiguous chunk that's much larger than total
                // free heap when fragmentation kicks in. A scenario can opt into
                // this assertion when its workload depends on a specific minimum
                // (mirror LUT silently degrades when the block won't fit; see
                // src/light/layers/Layer.h Layer::rebuildLUT). Optional field;
                // skipped on desktop where the value is always 0 (unlimited).
                if (exp.has("max_alloc_block") && exp["max_alloc_block"].num > 0 &&
                    maxBlock > 0) {
                    double expBlock = exp["max_alloc_block"].num;
                    double dropPct = (maxBlock < expBlock)
                        ? (expBlock - maxBlock) * 100.0 / expBlock
                        : 0.0;
                    char msg[200];
                    std::snprintf(msg, sizeof(msg),
                                  "%s max_alloc_block %u vs contract %.0f (drop %.1f%% <= %.0f%%)",
                                  name, static_cast<unsigned>(maxBlock), expBlock, dropPct, heapTolPct);
                    result.check(dropPct <= heapTolPct, msg);
                }
            }

            heapAfter = heapAfterMeasure;  // roll forward for the next step's delta
        }
    }

    // After all steps, do the legacy end-of-scenario buffer check IF a Layer
    // module is present and the scheduler was started. Build-up scenarios that
    // explicitly assert in their measure steps don't need this redundancy, but
    // existing scenarios depend on it.
    ensureStarted();
    auto* layer = static_cast<mm::Layer*>(
        ctx.modules.count("Layer") ? ctx.modules["Layer"] : nullptr);
    auto* drivers = static_cast<mm::Drivers*>(
        ctx.modules.count("Drivers") ? ctx.modules["Drivers"] : nullptr);
    if (layer) {
        auto& buf = layer->buffer();
        result.check(buf.data() != nullptr, "buffer allocated");
        result.check(buf.count() > 0, "buffer has lights");
        std::printf("  Buffer: %u lights, %u bytes  LUT: %s  dynamicBytes: Layer=%u Drivers=%u\n",
                    static_cast<unsigned>(buf.count()),
                    static_cast<unsigned>(buf.bytes()),
                    layer->lut().hasLUT() ? "has LUT" : "identity",
                    static_cast<unsigned>(layer->dynamicBytes()),
                    static_cast<unsigned>(drivers ? drivers->dynamicBytes() : 0));
        bool hasNonZero = false;
        if (buf.data()) {
            for (size_t i = 0; i < buf.bytes(); i++) {
                if (buf.data()[i] != 0) { hasNonZero = true; break; }
            }
        }
        // Only assert non-zero output if the scenario actually rendered (lights > 0).
        if (buf.count() > 0) {
            result.check(hasNonZero, "buffer non-zero after render");
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
