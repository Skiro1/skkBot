#include "skkBot.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "physics/step_grid_hooks.hpp"

#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

#ifdef GEODE_IS_INTEL_MAC
#include <mach-o/loader.h>
#endif

using namespace geode::prelude;

namespace {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_INTEL_MAC)
    using ExpectedTicksType = uint32_t;

    static ExpectedTicksType g_expectedTicks = 0;
    static geode::Patch* g_expectedTicksPatch = nullptr;
    static bool g_expectedTicksPatchAttempted = false;

    static std::optional<size_t> findHexPattern(uint8_t* base, size_t size, std::string_view textPattern) {
        std::vector<uint8_t> bytes;
        std::vector<bool> mask;

        for (size_t pos = 0; pos < textPattern.size();) {
            while (pos < textPattern.size() && textPattern[pos] == ' ') ++pos;
            if (pos >= textPattern.size()) break;

            if (textPattern[pos] == '?') {
                bytes.push_back(0);
                mask.push_back(false);
                while (pos < textPattern.size() && textPattern[pos] == '?') ++pos;
            } else {
                auto hi = textPattern[pos++];
                auto lo = textPattern[pos++];
                auto hexVal = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                    return 0;
                };
                bytes.push_back(static_cast<uint8_t>((hexVal(hi) << 4) | hexVal(lo)));
                mask.push_back(true);
            }
        }

        size_t patLen = bytes.size();
        if (!base || size < patLen) return std::nullopt;

        for (size_t i = 0; i + patLen <= size; ++i) {
            bool hit = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (mask[j] && base[i + j] != bytes[j]) {
                    hit = false;
                    break;
                }
            }
            if (hit) return i;
        }

        return std::nullopt;
    }

#ifdef GEODE_IS_WINDOWS
    static size_t getModuleSize(uint8_t* base) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
            return 0;
        }

        return static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    }

#pragma pack(push, 1)
    struct PatchPayload {
        uint8_t movRaxOpcode[2];
        uint64_t pointer;
        uint8_t movR11dOpcode[3];
        uint8_t jmpOpcode;
        int32_t jmpOffset;
        uint8_t nopPad[4];
    };
#pragma pack(pop)

    static std::vector<uint8_t> buildExpectedTicksPatch(uintptr_t patchAddress) {
        PatchPayload payload;
        payload.movRaxOpcode[0] = 0x48;
        payload.movRaxOpcode[1] = 0xB8;
        payload.pointer = reinterpret_cast<uint64_t>(&g_expectedTicks);
        payload.movR11dOpcode[0] = 0x44;
        payload.movR11dOpcode[1] = 0x8B;
        payload.movR11dOpcode[2] = 0x18;
        payload.jmpOpcode = 0xE9;
        payload.jmpOffset = static_cast<int32_t>((patchAddress + 0x43) - (patchAddress + 18));
        payload.nopPad[0] = 0x90;
        payload.nopPad[1] = 0x90;
        payload.nopPad[2] = 0x90;
        payload.nopPad[3] = 0x90;

        std::vector<uint8_t> result(sizeof(payload));
        std::memcpy(result.data(), &payload, sizeof(payload));
        return result;
    }

    static bool ensureExpectedTicksPatch() {
        if (g_expectedTicksPatchAttempted) {
            return g_expectedTicksPatch != nullptr;
        }

        g_expectedTicksPatchAttempted = true;

        auto* base = reinterpret_cast<uint8_t*>(geode::base::get());
        size_t size = getModuleSize(base);
        if (!base || size == 0) {
            skkbot2::log::error("TPS bypass could not read the game module");
            return false;
        }

        static constexpr std::string_view kExpectedTicksSig =
            "FF 90 ?? ?? ?? ?? F3 0F 10 ?? ?? ?? ?? ?? F3 44 0F 10 ?? ?? ?? ?? 00 F3 41 0F 5D";

        auto match = findHexPattern(base, size, kExpectedTicksSig);
        if (!match) {
            skkbot2::log::error("TPS bypass could not find the expected-ticks pattern");
            return false;
        }

        uintptr_t patchAddress = reinterpret_cast<uintptr_t>(base) + *match + 6;
        auto patchBytes = buildExpectedTicksPatch(patchAddress);
        auto result = Mod::get()->patch(reinterpret_cast<void*>(patchAddress), patchBytes);
        if (!result) {
            skkbot2::log::error("TPS bypass could not install the expected-ticks patch: {}", result.unwrapErr());
            return false;
        }

        g_expectedTicksPatch = result.unwrap();
        skkbot2::log::debug("Installed the TPS bypass patch at module offset 0x{:X}", patchAddress - reinterpret_cast<uintptr_t>(base));
        return true;
    }
#else
    static size_t getModuleSize(uint8_t* base) {
        auto* header = reinterpret_cast<mach_header_64*>(base);
        if (!header || header->magic != MH_MAGIC_64) {
            return 0;
        }

        auto* cursor = base + sizeof(mach_header_64);
        for (uint32_t i = 0; i < header->ncmds; ++i) {
            auto* command = reinterpret_cast<load_command*>(cursor);
            if (command->cmd == LC_SEGMENT_64) {
                auto* segment = reinterpret_cast<segment_command_64*>(cursor);
                if (std::strcmp(segment->segname, "__TEXT") == 0) {
                    return static_cast<size_t>(segment->vmsize);
                }
            }
            cursor += command->cmdsize;
        }

        return 0;
    }

#pragma pack(push, 1)
    struct PatchPayload {
        uint8_t movRaxOpcode[2];
        uint64_t pointer;
        uint8_t xorpsOpcode[3];
        uint8_t cvtsi2ssOpcode[4];
    };
#pragma pack(pop)

    static std::vector<uint8_t> buildExpectedTicksPatch() {
        PatchPayload payload;
        payload.movRaxOpcode[0] = 0x48;
        payload.movRaxOpcode[1] = 0xB8;
        payload.pointer = reinterpret_cast<uint64_t>(&g_expectedTicks);
        payload.xorpsOpcode[0] = 0x0F;
        payload.xorpsOpcode[1] = 0x57;
        payload.xorpsOpcode[2] = 0xC0;
        payload.cvtsi2ssOpcode[0] = 0xF3;
        payload.cvtsi2ssOpcode[1] = 0x0F;
        payload.cvtsi2ssOpcode[2] = 0x2A;
        payload.cvtsi2ssOpcode[3] = 0x00;

        std::vector<uint8_t> result(sizeof(payload));
        std::memcpy(result.data(), &payload, sizeof(payload));
        return result;
    }

    static bool ensureExpectedTicksPatch() {
        if (g_expectedTicksPatchAttempted) {
            return g_expectedTicksPatch != nullptr;
        }

        g_expectedTicksPatchAttempted = true;

        auto* base = reinterpret_cast<uint8_t*>(geode::base::get());
        size_t size = getModuleSize(base);
        if (!base || size == 0) {
            skkbot2::log::error("TPS bypass could not read the game module");
            return false;
        }

        static constexpr std::string_view kExpectedTicksSig =
            "F3 0F 58 C8 0F 57 C0 66 0F 3A 0A C1 0B F3 0F 5F C2 0F 29 45 ?? F3 44 0F 2C F8";

        auto match = findHexPattern(base, size, kExpectedTicksSig);
        if (!match) {
            skkbot2::log::error("TPS bypass could not find the expected-ticks pattern");
            return false;
        }

        uintptr_t patchAddress = reinterpret_cast<uintptr_t>(base) + *match;
        auto patchBytes = buildExpectedTicksPatch();
        auto result = Mod::get()->patch(reinterpret_cast<void*>(patchAddress), patchBytes);
        if (!result) {
            skkbot2::log::error("TPS bypass could not install the expected-ticks patch: {}", result.unwrapErr());
            return false;
        }

        g_expectedTicksPatch = result.unwrap();
        skkbot2::log::debug("Installed the TPS bypass patch at module offset 0x{:X}", patchAddress - reinterpret_cast<uintptr_t>(base));
        return true;
    }
#endif

    static bool setExpectedTicksPatchEnabled(bool enabled) {
        if (enabled && !ensureExpectedTicksPatch()) {
            return false;
        }

        if (!g_expectedTicksPatch) {
            return !enabled;
        }

        if (g_expectedTicksPatch->isEnabled() == enabled) {
            return true;
        }

        auto result = enabled
            ? g_expectedTicksPatch->enable()
            : g_expectedTicksPatch->disable();
        if (!result) {
            skkbot2::log::error("TPS bypass could not {} the expected-ticks patch: {}",
                enabled ? "enable" : "disable", result.unwrapErr());
            return false;
        }
        skkbot2::log::info("expected-ticks patch {}", enabled ? "ENABLED" : "disabled");

        return true;
    }

    static void setExpectedTicks(int64_t steps) {
        g_expectedTicks = static_cast<ExpectedTicksType>(std::clamp<int64_t>(
            steps,
            0,
            static_cast<int64_t>(std::numeric_limits<ExpectedTicksType>::max())
        ));
    }
#else
    static bool ensureExpectedTicksPatch() {
        return true;
    }

    static bool setExpectedTicksPatchEnabled(bool) {
        return true;
    }

    static void setExpectedTicks(int64_t) {}
#endif

    static bool nearlyEqual(double lhs, double rhs, double epsilon) {
        return std::abs(lhs - rhs) <= epsilon;
    }

    static bool shouldUseCustomTiming(ReplayEngine const& engine) {
        // Editor playtest: GD funnels processCommands at ~180/s with dt=1/180,
        // and the step clock is compensated for exactly that cadence (BUG-10).
        // Do NOT drag a plain editor PLAY onto the custom 240 grid - it would
        // break the compensation. Only take over timing when the user actually
        // changed TPS/Speed (or render/autoclicker/frame-step needs it).
        if (skk::gameplay::isEditorPlaytest()) {
            return engine.tickStepping
                || engine.singleTickStep
                || skkbot2::render::Renderer::get().isRecording()
                || !nearlyEqual(engine.runtimeTickRate(), ReplayEngine::kBaseTickRate, 0.01)
                || !nearlyEqual(engine.gameSpeed, 1.0, 0.0001)
                || Autoclicker::get()->enabled;
        }
        return engine.tickStepping
            || engine.singleTickStep
            || skkbot2::render::Renderer::get().isRecording()
            || engine.engineMode == MODE_CAPTURE
            || engine.engineMode == MODE_EXECUTE
            || !nearlyEqual(engine.runtimeTickRate(), ReplayEngine::kBaseTickRate, 0.01)
            || !nearlyEqual(engine.gameSpeed, 1.0, 0.0001)
            || Autoclicker::get()->enabled;
    }

    struct TickStepPlanner {
        double accumulated = 0.0;

        void absorb(double dt) {
            accumulated += dt;
        }

        int planSteps(double timestep) const {
            if (timestep <= 0.0) return 0;
            return static_cast<int>(std::llround(accumulated / timestep));
        }

        double residual() const {
            return accumulated;
        }
    };

    struct DeferredFrameStepRelease {
        PlayerButton button;
        bool player2;
    };

    static std::vector<DeferredFrameStepRelease> g_frameStepDeferredReleases;

    static constexpr double kFrameStepPairTimestampTolerance = 1e-6;

    static bool isStretchableTapButton(PlayerButton button) {
        return button == PlayerButton::Jump;
    }

    static void stretchTrailingTapsForFrameStep(GJBaseGameLayer* layer) {
        if (!layer) {
            return;
        }

        auto& queue = layer->m_queuedButtons;
        while (queue.size() >= 2) {
            auto const& release = queue[queue.size() - 1];
            auto const& press = queue[queue.size() - 2];

            if (release.m_isPush || !press.m_isPush) break;
            if (release.m_button != press.m_button) break;
            if (release.m_isPlayer2 != press.m_isPlayer2) break;
            if (!isStretchableTapButton(release.m_button)) break;
            if (std::abs(release.m_timestamp - press.m_timestamp) > kFrameStepPairTimestampTolerance) break;

            g_frameStepDeferredReleases.push_back({ release.m_button, release.m_isPlayer2 });
            queue.pop_back();
        }
    }

    static void requeueDeferredFrameStepReleases(GJBaseGameLayer* layer) {
        if (!layer || g_frameStepDeferredReleases.empty()) {
            return;
        }

        double timestamp = layer->m_timestamp;
        for (auto const& deferred : g_frameStepDeferredReleases) {
            PlayerButtonCommand command = {};
            command.m_button = deferred.button;
            command.m_isPush = false;
            command.m_isPlayer2 = deferred.player2;
            command.m_timestamp = timestamp;
            layer->m_queuedButtons.push_back(command);
        }

        g_frameStepDeferredReleases.clear();
    }

    struct SimulationTimingController {
        float schedulerCarry = 0.0f;

        float fixedDelta(ReplayEngine const& engine) const {
            return engine.fixedSimulationDelta();
        }

        float speedDelta(ReplayEngine const& engine, float rawDt) const {
            return rawDt * static_cast<float>(engine.gameSpeed);
        }

        void resetSimulation(ReplayEngine& engine) {
            engine.tickAccumulator = 0.0f;
            engine.renderingDisabled = false;
            engine.recentlyInitialized = true;
        }

        void resetSchedulingCarry() {
            schedulerCarry = 0.0f;
        }

        void advanceSingleStep(CCScheduler* scheduler, ReplayEngine& engine) {
            resetSimulation(engine);
            resetSchedulingCarry();

            if (!engine.singleTickStep) {
                return;
            }

            engine.singleTickStep = false;

            auto* pl = PlayLayer::get();
            if (pl) {
                engine.bridgeUserHoldsToPlayer(pl);
                stretchTrailingTapsForFrameStep(pl);
            }

            scheduler->CCScheduler::update(fixedDelta(engine));

            if (pl) {
                requeueDeferredFrameStepReleases(pl);
            }
        }

        void advanceNormalScheduler(CCScheduler* scheduler, ReplayEngine& engine, float rawDt) {
            engine.recentlyInitialized = false;
            engine.tickAccumulator = 0.0f;
            // Fixed-step grid (determinism): record and playback sessions must
            // feed the game the IDENTICAL substep sequence regardless of the
            // display fps. A 240fps record ran 1/240 substeps while a faster
            // playback ran fractional substeps - same tick labels, different
            // physics integration, so portals and orb windows missed and
            // per-frame pins turned into visible teleports. Accumulate raw
            // frame time (gameSpeed-scaled, as before) and emit whole 1/240
            // batches; the remainder rides the carry instead of reaching the
            // game as fractional substeps. Mirrors the renderer path.
            float step = fixedDelta(engine);
            if (!(step > 0.0f) || !(rawDt >= 0.0f)) {
                scheduler->CCScheduler::update(speedDelta(engine, rawDt));
                return;
            }
            schedulerCarry += speedDelta(engine, rawDt);
            constexpr int kMaxBatchesPerFrame = 8;
            int batches = 0;
            while (schedulerCarry + step * 0.001f >= step && batches < kMaxBatchesPerFrame) {
                schedulerCarry -= step;
                scheduler->CCScheduler::update(step);
                ++batches;
            }
            const float maxCarry = step * static_cast<float>(kMaxBatchesPerFrame);
            if (schedulerCarry > maxCarry) schedulerCarry = maxCarry;
            if (schedulerCarry < 0.0f) schedulerCarry = 0.0f;
        }

        void resetVanillaState(ReplayEngine& engine, double* extraDelta = nullptr) {
            resetSchedulingCarry();
            setExpectedTicks(0);
            engine.tickAccumulator = 0.0f;
            engine.recentlyInitialized = false;
            if (extraDelta) {
                *extraDelta = 0.0;
            }
        }

        void consumeSingleTick(GJBaseGameLayer* layer, ReplayEngine& engine) {
            resetSimulation(engine);
            setExpectedTicks(1);

            engine.customTickPlanActive = true;
            layer->GJBaseGameLayer::update(fixedDelta(engine));
            engine.customTickPlanActive = false;
        }
    };

    static SimulationTimingController& timingController() {
        static SimulationTimingController controller;
        return controller;
    }

    static void applyCustomTickPlan(GJBaseGameLayer* layer, double* extraDelta, float dt, ReplayEngine* engine) {
        auto& controller = timingController();

        *extraDelta += dt;
        engine->recentlyInitialized = false;

        double timeWarp = std::clamp(static_cast<double>(layer->m_gameState.m_timeWarp), 0.0, 1.0);
        double timestep = timeWarp * static_cast<double>(controller.fixedDelta(*engine));

        TickStepPlanner planner;
        planner.accumulated = *extraDelta;
        int64_t wantedSteps = static_cast<int64_t>(planner.planSteps(timestep));
        constexpr int64_t kMaxStepsPerFrame = 8;
        int64_t steps = std::min<int64_t>(wantedSteps, kMaxStepsPerFrame);
        double totalDelta = static_cast<double>(steps) * timestep;
        *extraDelta -= totalDelta;

        engine->tickAccumulator = static_cast<float>(std::max(0.0, *extraDelta));

        engine->frameEntryTick = static_cast<int>(
            layer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;

        // Session step-grid audit (reset per session; logged in haltExecution).
        ++engine->sessionPlanCount;
        engine->sessionStepSum += steps;
        engine->sessionLastLevelTime = layer->m_gameState.m_levelTime;
        if (!engine->sessionLevelTimeInit) {
            engine->sessionLevelTimeInit = true;
            engine->sessionFirstLevelTime = layer->m_gameState.m_levelTime;
            engine->sessionSampleLastLevelTime = layer->m_gameState.m_levelTime;
        }

#ifdef GEODE_IS_WINDOWS
        // One-shot: which step-grid mechanism this session actually uses.
        {
            static bool modeLogged = false;
            if (!modeLogged && steps > 0) {
                modeLogged = true;
                skkbot2::log::info("step-grid: mode=hooks (silicate-style engaged), hooks={} steps={} timestep={:.7f}",
                    skk::stepGridHooksAvailable() ? "engaged" : "unavailable",
                    steps, timestep);
            }
        }
        {
            engine->sessionSampleStepsWindow += steps;
            if (++engine->phaseSampleCounter % 2400 == 0) {
                const double levelNow = layer->m_gameState.m_levelTime;
                skkbot2::log::info("step-grid sample: dt={:.6f} steps={} carry={:.7f} warp={:.3f} | "
                                   "levelTime +{:.4f}s/2400plans (planned {:.4f}s) | "
                                   "resume={} sess[plans={},steps={},resumeZero={}]",
                    dt, steps, *extraDelta, layer->m_gameState.m_timeWarp,
                    levelNow - engine->sessionSampleLastLevelTime,
                    static_cast<double>(engine->sessionSampleStepsWindow) / 240.0,
                    static_cast<int>(layer->m_resumeTimer),
                    engine->sessionPlanCount, engine->sessionStepSum, engine->sessionResumeZeroPlans);
                engine->sessionSampleLastLevelTime = levelNow;
                engine->sessionSampleStepsWindow = 0;
            }
        }

        // Step-grid enforcement (silicate midhooks) - ENGAGED. Now that
        // getModifiedDelta is frozen to a single per-substep dt
        // (warp * physicsDt), the rdx-pin model holds: the step loop runs
        // exactly `steps` iterations of `timestep` (physStepCount midhook)
        // and reports `steps * timestep` as consumed (restorePhysDt midhook),
        // so the engine's internal tick count can never drift one substep
        // off the tick planner's grid. The scheduler keeps its proven
        // pre-quantized 1/tps batching (this exactly matches one physics
        // tick per batch at warp=1); getModifiedDelta never re-rounds.
        constexpr bool kEnableStepGridHooks = true;
        if (kEnableStepGridHooks && steps > 0 && skk::stepGridHooksAvailable()) {
            setExpectedTicks(steps);
            engine->customTickPlanActive = true;
            skk::StepGridEnforcement enforcement(static_cast<int>(steps), timestep);
            layer->GJBaseGameLayer::update(static_cast<float>(totalDelta));
            engine->customTickPlanActive = false;
            return;
        }
#endif

        // Proven path (matches pre-investigation behavior that recorded
        // fine): ONE batched update(totalDelta) with expectedTicks=steps on
        // Windows - always at least one update() call per frame, so game
        // systems never stall; the planner's quantization keeps totalDelta
        // on the 1/240 grid. Step-grid samples in the log tell us when the
        // engine's internal count diverges from 'steps' (the +-1 phase we
        // hunt). ARM keeps its explicit per-step loop (patch semantics).
#if defined(GEODE_IS_ARM_MAC)
        engine->customTickPlanActive = true;
        for (int64_t i = 0; i < steps; ++i) {
            setExpectedTicks(1);
            layer->GJBaseGameLayer::update(static_cast<float>(timestep));
        }
        engine->customTickPlanActive = false;
#else
        setExpectedTicks(std::max<int64_t>(steps, 0));
        engine->customTickPlanActive = true;
        layer->GJBaseGameLayer::update(static_cast<float>(totalDelta));
        engine->customTickPlanActive = false;
#endif
    }}

void resetSimulationTimingState() {
    auto& controller = timingController();
    controller.resetSchedulingCarry();
    setExpectedTicks(0);

    if (auto* engine = ReplayEngine::get()) {
        controller.resetSimulation(*engine);
        engine->clearQueuedSubstepState();
    }
}

void disableExpectedTicksPatch() {
    setExpectedTicksPatchEnabled(false);
}

class $modify(TickControlPlayLayer, PlayLayer) {
    struct Fields {
        double pulseFixAccum = 0.0;
    };

    void update(float dt) {
        ReplayEngine::get()->processHotkeys();
        PlayLayer::update(dt);
    }

    void updateVisibility(float dt) {
        auto* engine = ReplayEngine::get();
        if (engine->pulseFix && skkbot2::render::Renderer::get().isRecording()
            && engine->runtimeTickRate() > ReplayEngine::kBaseTickRate) {
            double baseStep = 1.0 / ReplayEngine::kBaseTickRate;
            double& accum = m_fields->pulseFixAccum;
            accum += dt;
            int guard = 0;
            while (accum + 1e-9 >= baseStep && guard++ < 16) {
                PlayLayer::updateVisibility(static_cast<float>(baseStep));
                accum -= baseStep;
            }
            if (accum >= baseStep) {
                accum = std::fmod(accum, baseStep);
            }
            return;
        }
        m_fields->pulseFixAccum = 0.0;
        PlayLayer::updateVisibility(dt);
    }
};

class $modify(SpeedControlScheduler, CCScheduler) {
    void update(float dt) {
        // Engage for a PlayLayer attempt OR an editor playtest (TPS/Speed
        // control). Plain editor play with default timing falls through to
        // vanilla inside shouldUseCustomTiming (BUG-10 compensation stays
        // intact); build mode keeps the vanilla scheduler entirely.
        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return CCScheduler::update(dt);
        }

        // Render capture: physics is stepped by the CCDirector::drawScene hook
        // (Renderer::drive -> m_pScheduler->update(1/fps)), but it still flows
        // through the step-grid machinery below so the simulation runs on the
        // macro's tps grid (1/tps substeps), exactly like the PLAY session the
        // macro was recorded on. The scheduler carry turns one 1/fps drive
        // call into floor((1/fps)/(1/tps)) batches of 1/tps.
        auto* engine = ReplayEngine::get();
        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            controller.resetVanillaState(*engine);
            setExpectedTicksPatchEnabled(false);
            return CCScheduler::update(dt);
        }

        setExpectedTicksPatchEnabled(true);

        if (engine->tickStepping) {
            controller.advanceSingleStep(this, *engine);
            return;
        }

        controller.advanceNormalScheduler(this, *engine, dt);
    }
};

class $modify(PhysicsControlLayer, GJBaseGameLayer) {
    struct Fields {
        double extraDelta = 0.0;
    };

    void update(float dt) {
        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return GJBaseGameLayer::update(dt);
        }

        // Render capture: physics runs via the drawScene hook's own stepping,
        // but through the shared step-grid path below (see
        // SpeedControlScheduler::update) so substeps integrate on the macro's
        // 1/tps grid instead of the vanilla 240-Hz one.
        auto* engine = ReplayEngine::get();
        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            controller.resetVanillaState(*engine, &m_fields->extraDelta);
            setExpectedTicksPatchEnabled(false);
            GJBaseGameLayer::update(dt);
            return;
        }

        setExpectedTicksPatchEnabled(true);

        if (engine->tickStepping) {
            m_fields->extraDelta = 0.0;
            controller.consumeSingleTick(this, *engine);
            engine->requestFrameStepMusicSync(PlayLayer::get());
            engine->syncFrameStepAudio(FMODAudioEngine::sharedEngine());
        } else {
            applyCustomTickPlan(this, &m_fields->extraDelta, dt, engine);
        }
    }

    double getModifiedDelta(float dt) {
        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        // Render capture keeps the step-grid freeze (customTickPlanActive) so
        // substeps stay on the 1/tps grid, matching the recorded macro.
        auto* engine = ReplayEngine::get();
        if (!shouldUseCustomTiming(*engine)) {
            setExpectedTicksPatchEnabled(false);
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        setExpectedTicksPatchEnabled(true);

        // Vanilla MUST be called first (GucciBot v7.1 lesson - frozen player
        // otherwise). Return value is discarded; we freeze to the one-substep
        // delta that pairs with the step-grid midhooks (silicate pattern):
        // within our update scope the engine integrates exactly N substeps of
        // this timestep, never re-quantizing or touching extraDelta (that is
        // applyCustomTickPlan's job alone).
        float modDelta = GJBaseGameLayer::getModifiedDelta(dt);

        if (engine->customTickPlanActive) {
            // Resume-fade frames advance no physics (vanilla semantics, kept
            // from the old getCustomDelta path).
            if (m_resumeTimer > 0) {
                --m_resumeTimer;
                ++engine->sessionResumeZeroPlans;
                return 0.0;
            }
            auto& controller = timingController();
            double warp = std::clamp<double>(m_gameState.m_timeWarp, 0.0, 1.0);
            return warp * controller.fixedDelta(*engine);
        }

        return modDelta;
    }
};
