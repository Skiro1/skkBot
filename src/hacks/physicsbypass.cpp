#include "skkBot.hpp"
#include "core/autosave_recorder.hpp"
#include "core/frame_stepper.hpp"
#include "core/hotpath.hpp"
#include "core/playback_fps.hpp"
#include "core/step_trace.hpp"
#include "core/step_profiler.hpp"
#include "version_offsets.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "hacks/win_refresh_clock.hpp"
#include "physics/step_grid_hooks.hpp"
#include "skc/skc_batch_probe.hpp"
#include "skc/skc_bot.hpp"
#include "skc/skc_frame.hpp"

#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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

namespace skkbot2 {
void entryFreezeWatch(GJBaseGameLayer* layer);
void entryFreezeReset();
std::string fmodChannelDump();
}

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
        uint8_t movCountOpcode[2];
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
        payload.movCountOpcode[0] = 0x8B;
        payload.movCountOpcode[1] = 0x38;
        payload.jmpOpcode = 0xE9;
        payload.jmpOffset = static_cast<int32_t>((patchAddress + 0x42) - (patchAddress + 17));
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

        auto match = skk::offsets::resolve(skk::offsets::kTpsExpectedTicks, base, size);
        if (!match) {
            skkbot2::log::error("TPS bypass could not find the expected-ticks pattern");
            return false;
        }

        uintptr_t patchAddress = *match + 6;
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
        const bool skcSession = skk::skc::Bot::get().isEnabled();

        if (skk::gameplay::isEditorPlaytest()) {
            return engine.tickStepping
                || engine.singleTickStep
                || skcSession
                || skkbot2::render::Renderer::get().isRecording()
                || !nearlyEqual(engine.runtimeTickRate(), ReplayEngine::kBaseTickRate, 0.01)
                || !nearlyEqual(engine.gameSpeed, 1.0, 0.0001)
                || skkbot2::fps::enabled()
                || Autoclicker::get()->enabled;
        }
        return engine.tickStepping
            || engine.singleTickStep
            || skcSession
            || skkbot2::render::Renderer::get().isRecording()
            || engine.engineMode == MODE_CAPTURE
            || engine.engineMode == MODE_EXECUTE
            || !nearlyEqual(engine.runtimeTickRate(), ReplayEngine::kBaseTickRate, 0.01)
            || !nearlyEqual(engine.gameSpeed, 1.0, 0.0001)
            || skkbot2::fps::enabled()
            || Autoclicker::get()->enabled;
    }

    struct TickStepPlanner {
        double accumulated = 0.0;

        void absorb(double dt) {
            accumulated += dt;
        }

        int planSteps(double timestep) const {
            if (timestep <= 0.0) return 0;
            return static_cast<int>(std::floor(accumulated / timestep));
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

    constexpr long long kLockRateWindow = 240;
    constexpr double kLockCadenceTolerance = 0.05;

    static double g_extraDelta = 0.0;

    static bool skcDeterministicGrid() {
        auto& bot = skk::skc::Bot::get();
        return bot.isRecording() || bot.isPlaying() || bot.isPlaybackArmed();
    }

    static void logDeterministicGridToast() {
        static bool logged = false;
        if (logged) return;
        logged = true;
        skkbot2::log::info(
            "[SKC] deterministic step grid engaged (BUG-93); the timing "
            "mode follows the GUI row: Lock Delta = learned display cadence "
            "constant, Real Time = measured wall frame time - steps stay "
            "1/tps in both");
    }

    struct SimulationTimingController {

        double fixedDelta(ReplayEngine const& engine) const {
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

        void advanceSingleStep(CCScheduler* scheduler, ReplayEngine& engine) {
            resetSimulation(engine);

            if (FrameStepper::get().consumeAdvanceStep()) {
                engine.singleTickStep = true;
            }

            if (!engine.singleTickStep) {
                return;
            }

            engine.singleTickStep = false;

            auto* pl = PlayLayer::get();
            if (pl) {
                engine.bridgeUserHoldsToPlayer(pl);
                stretchTrailingTapsForFrameStep(pl);
            }

            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                scheduler->CCScheduler::update(fixedDelta(engine));
            }

            if (pl) {
                requeueDeferredFrameStepReleases(pl);
            }
        }

        void advanceNormalScheduler(CCScheduler* scheduler, ReplayEngine& engine, float rawDt) {
            skkbot2::debug::profilerNoteMainThread();
            engine.recentlyInitialized = false;
            engine.tickAccumulator = 0.0f;
            float wallGapNow = -1.0f;
            engine.sampleRawFrames += 1;
            if (rawDt > 0.0f) {
                engine.sampleRawDtSum += static_cast<double>(rawDt);
                if (engine.sampleRawDtMin <= 0.0f || rawDt < engine.sampleRawDtMin) {
                    engine.sampleRawDtMin = rawDt;
                }
                if (rawDt > engine.sampleRawDtMax) {
                    engine.sampleRawDtMax = rawDt;
                }
            }
            {
                double wallNowSk = skkbot2::hot::now();
                wallGapNow = -1.0f;
                if (engine.lastDriveWall <= 0.0) {
                    engine.lastDriveWall = wallNowSk;
                } else {
                    const float gf = static_cast<float>(wallNowSk - engine.lastDriveWall);
                    engine.sampleWallGapSum += static_cast<double>(gf);
                    if (engine.sampleWallGapMin < 0.0f || gf < engine.sampleWallGapMin) {
                        engine.sampleWallGapMin = gf;
                    }
                    if (gf > engine.sampleWallGapMax) {
                        engine.sampleWallGapMax = gf;
                    }
                    if (rawDt > 0.0f && static_cast<double>(rawDt) >
                            static_cast<double>(gf) * 1.25 + 0.002) {
                        engine.samplePhantomCount += 1;
                        engine.samplePhantomDtSum += static_cast<double>(rawDt);
                    }
                    engine.lastDriveWall = wallNowSk;
                    wallGapNow = gf;
                }
            }
            const double step = fixedDelta(engine);
            if (!(step > 0.0) || !(rawDt >= 0.0f)) {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                scheduler->CCScheduler::update(speedDelta(engine, rawDt));
                return;
            }

            const auto& gui = skkbot2::gui::get();
            const bool skcForce = skcDeterministicGrid();
            if (skcForce) {
                logDeterministicGridToast();
            }
            const bool skcRealTime =
                skcForce && gui.realTime && !gui.lockDelta;
            const bool lockDeltaActive =
                (skcForce && !skcRealTime) ||
                (!skcForce && gui.lockDelta && !gui.realTime);
            const bool realTimeActive = !skcForce
                ? (gui.realTime && !gui.lockDelta)
                : skcRealTime;
            const bool rendering = skkbot2::render::Renderer::get().isRecording();

            float frame_dt = rawDt;
            const double rawDtForAudit = static_cast<double>(rawDt);
            double dwmDtForAudit = static_cast<double>(frame_dt);
#if defined(GEODE_IS_WINDOWS)
            if (!rendering && !skcForce) {
                frame_dt = static_cast<float>(skkbot2::timing::refresh_frame_time(frame_dt));
                dwmDtForAudit = static_cast<double>(frame_dt);
            }
#endif
            engine.sampleDwmSum += dwmDtForAudit;
            engine.sampleDrives += 1;
            if (std::abs(dwmDtForAudit - rawDtForAudit) > 1e-6) {
                engine.sampleDwmRewrites += 1;
            }
            if (!rendering && !realTimeActive && frame_dt > 0.25f) {
                frame_dt = 0.25f;
            }
            if (!rendering && realTimeActive && !skcForce &&
                wallGapNow > 0.0f) {
                const double stallCapSeconds =
                    60.0 * static_cast<double>(step);
                frame_dt = static_cast<float>(std::min(
                    static_cast<double>(wallGapNow), stallCapSeconds));
            }

            float frameTime;
            double promisedInterval = 0.0;
            bool skcConstSubst = false;
            {
                constexpr double kNoFrame = 0.0;
                auto pin = rawDt > 0.0f ? static_cast<double>(rawDt) : kNoFrame;
                const double interval = static_cast<double>(
                    cocos2d::CCDirector::sharedDirector()->getAnimationInterval());
                constexpr long long kCadenceWindow = 240;
                constexpr double kCadenceTol = 0.05;
                if (interval > 0.0 && pin > 0.0 &&
                    std::abs(pin - interval) / interval > kCadenceTol) {
                    engine.lockOffCadenceInWindow += 1;
                }
                engine.lockWindowFrames += 1;
                if (engine.lockWindowFrames >= kCadenceWindow) {
                    engine.lockOffCadenceFraction =
                        static_cast<double>(engine.lockOffCadenceInWindow) /
                        static_cast<double>(kCadenceWindow);
                    engine.lockWindowFrames = 0;
                    engine.lockOffCadenceInWindow = 0;
                }
                const double learnSpeed =
                    engine.gameSpeed > 0.01 ? engine.gameSpeed : 1.0;
                engine.lockLearnsWindow.push_back(
                    static_cast<float>(pin / learnSpeed));
                constexpr size_t kLearnWindow = 240;
                static std::vector<float> learnSorted;
                if (!rendering && engine.lockLearnsWindow.size() >= kLearnWindow && pin > 0.0) {
                    learnSorted = engine.lockLearnsWindow;
                    std::sort(learnSorted.begin(), learnSorted.end());
                    const size_t q1 = (learnSorted.size() - 1) / 4;
                    const size_t q3 = learnSorted.size() - 1 - q1;
                    const float p25 = learnSorted[q1];
                    const float p75 = learnSorted[q3];
                    float share = 0.0f;
                    if (p25 > 0.0f) {
                        size_t hits = 0;
                        for (float dt : engine.lockLearnsWindow) {
                            if (std::abs(dt - p25) <= 0.02f * p25) ++hits;
                        }
                        share = static_cast<float>(hits) /
                                static_cast<float>(engine.lockLearnsWindow.size());
                    }
                    const bool clean = p25 > 0.0f && share >= 0.8f;
                    const int snapped =
                        std::max(1, static_cast<int>(std::lround(
                            static_cast<float>(1.0) / p25)));
                    if (!clean) {
                        engine.lockLearnsWindow.clear();
                    }
                    else if (engine.lockAnchorFps == 0) {
                        engine.lockAnchorFps = snapped;
                        engine.lockLearnedInterval =
                            1.0f / static_cast<float>(snapped);
                        skkbot2::log::info(
                            "[SKC] cadence learned: {:.3f}ms ({} fps, {:.0f}% "
                            "of the window inside the cluster) - the "
                            "deterministic grid substitutes this, not the "
                            "nominal interval",
                            engine.lockLearnedInterval * 1000.0f, snapped,
                            share * 100.0f);
                        engine.lockLearnsWindow.clear();
                    } else if (std::abs(snapped - engine.lockAnchorFps) <=
                               std::max(1, engine.lockAnchorFps / 100)) {
                        engine.lockAnchorPendingFps = 0;
                        engine.lockAnchorPendingHits = 0;
                        engine.lockLearnsWindow.clear();
                    } else if (snapped == engine.lockAnchorPendingFps) {
                        engine.lockAnchorPendingHits += 1;
                        engine.lockLearnsWindow.clear();
                        if (engine.lockAnchorPendingHits >= 2) {
                            skkbot2::log::info(
                                "[SKC] cadence re-anchor: {} fps -> {} fps "
                                "(confirmed over two windows); the step grid "
                                "re-anchors on the new rate",
                                engine.lockAnchorFps, snapped);
                            engine.lockAnchorFps = snapped;
                            engine.lockLearnedInterval =
                                1.0f / static_cast<float>(snapped);
                            engine.lockAnchorPendingFps = 0;
                            engine.lockAnchorPendingHits = 0;
                        }
                    } else {
                        engine.lockAnchorPendingFps = snapped;
                        engine.lockAnchorPendingHits = 1;
                        engine.lockLearnsWindow.clear();
                    }
                }
                if (rendering) {
                    promisedInterval = 0.0;
                } else if (realTimeActive) {
                    promisedInterval = 0.0;
                } else if (skkbot2::fps::macroHolds() &&
                           skkbot2::fps::enabled() &&
                           skkbot2::fps::target() > 0.0) {
                    static bool storedRateLockLogged = false;
                    if (!storedRateLockLogged) {
                        storedRateLockLogged = true;
                        skkbot2::log::info(
                            "[TIMING] lock-delta stored-rate lock: feeding "
                            "{:.4f}ms ({} fps) from the macro's rate, not a "
                            "re-learned cadence (BUG-107)",
                            1000.0 / skkbot2::fps::target(),
                            skkbot2::fps::target());
                    }
                    promisedInterval = 1.0 / skkbot2::fps::target();
                } else if (engine.lockLearnedInterval > 0.0f) {
                    promisedInterval =
                        static_cast<double>(engine.lockLearnedInterval);
                } else {
                    promisedInterval = interval;
                }
            }
            if (promisedInterval > 0.0) {
                frameTime = static_cast<float>(promisedInterval);
                skcConstSubst = true;
            } else {
                frameTime = frame_dt;
            }
            if (!rendering && wallGapNow > 0.0f && frameTime > wallGapNow) {
                frameTime = wallGapNow;
            }
            engine.sampleClampSum += static_cast<double>(frameTime);
            engine.sampleFedSum += static_cast<double>(frameTime);
            engine.lockDroppedCarry += static_cast<double>(rawDt) -
                static_cast<double>(frameTime);
            engine.lockMaxDtSkew = std::max(
                engine.lockMaxDtSkew,
                std::abs(static_cast<double>(rawDt) -
                         static_cast<double>(frameTime)));

            skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
            scheduler->CCScheduler::update(rendering
                                               ? frameTime
                                               : speedDelta(engine, frameTime));
        }

        void resetVanillaState(ReplayEngine& engine) {
            skkbot2::fps::release();
            setExpectedTicks(0);
            engine.tickAccumulator = 0.0f;
            engine.recentlyInitialized = false;
            g_extraDelta = 0.0;
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

    static void batProbe(GJBaseGameLayer* layer, int64_t steps, double totalDelta,
                         double levelBefore, ReplayEngine* engine) {
        if (!layer || !(steps > 0)) {
            return;
        }
        if (!skk::skc::Bot::get().isEnabled() &&
            engine->engineMode != MODE_CAPTURE) {
            return;
        }
        static uint64_t probePlans = 0;
        static uint64_t probeWin = 0;
        static int64_t probeSteps = 0;
        static double probeLvlExpected = 0.0;
        static double probeLvlInUpdate = 0.0;
        static uint32_t probeFc = 0;
        static double probeLvl = 0.0;
        ++probePlans;
        ++probeWin;
        probeSteps += steps;
        probeLvlExpected += totalDelta;
        probeLvlInUpdate += layer->m_gameState.m_levelTime - levelBefore;
        if (probeWin % 1000 == 0) {
            const uint32_t fc = skk::skc::FrameClock::get().getFrame();
            const double lvl = layer->m_gameState.m_levelTime;
            const uint32_t dFc = fc - probeFc;
            const double dLvl = lvl - probeLvl;
            if (fc < probeFc || dLvl < -1e-6) {
                probeSteps = 0;
                probeLvlExpected = 0.0;
                probeLvlInUpdate = 0.0;
                probeFc = fc;
                probeLvl = lvl;
                probeWin = 0;
                engine->batFrozenFlagPlans = 0;
                engine->batFrozenFlagSteps = 0;
                engine->batFreezeCtrPlans = 0;
                engine->batFreezeCtrSteps = 0;
                skk::skc::FrameClock::get().probeReset();
                return;
            }
            const uint64_t halves = skk::skc::FrameClock::get().probeHalf();
            const uint64_t fulls = skk::skc::FrameClock::get().probeFull();
            const double sumDt = skk::skc::FrameClock::get().probeDtSum();
            skkbot2::log::info(
                "[BAT] plans={} steps={} dFC={} halves={} fulls={} sumDt={:.6f}"
                " dLvl={:.6f} inUpdate={:.6f} expected={:.6f}"
                " frozenFlag={}/{} freezeCtr={}/{}{}{}",
                probeWin, probeSteps, dFc, halves, fulls, sumDt,
                dLvl, probeLvlInUpdate, probeLvlExpected,
                engine->batFrozenFlagPlans, engine->batFrozenFlagSteps,
                engine->batFreezeCtrPlans, engine->batFreezeCtrSteps,
                dFc == static_cast<uint32_t>(probeSteps)
                    ? ""
                    : " << FRAME CLOCK MISMATCH",
                std::abs(sumDt - probeLvlExpected) < 1e-4
                    ? ""
                    : " << DT MISMATCH");
            probeSteps = 0;
            probeLvlExpected = 0.0;
            probeLvlInUpdate = 0.0;
            probeFc = fc;
            probeLvl = lvl;
            probeWin = 0;
            engine->batFrozenFlagPlans = 0;
            engine->batFrozenFlagSteps = 0;
            engine->batFreezeCtrPlans = 0;
            engine->batFreezeCtrSteps = 0;
            skk::skc::FrameClock::get().probeReset();
        }
    }

    static void applyCustomTickPlan(GJBaseGameLayer* layer, double* extraDelta, float dt, ReplayEngine* engine) {
        skkbot2::entryFreezeWatch(layer);
        auto& controller = timingController();

        const bool locked =
            !skkbot2::render::Renderer::get().isRecording() &&
            (skkbot2::fps::macroHolds() || skkbot2::fps::enabled());
        const double frameDt = static_cast<double>(dt);
        *extraDelta += frameDt;
        engine->recentlyInitialized = false;

        {
            const auto layerBase = reinterpret_cast<uintptr_t>(layer);
            const uint8_t frozenFlag =
                *reinterpret_cast<const uint8_t*>(layerBase + skk::offsets::kFieldPauseFlagOffset);
            const int32_t freezeCtr =
                *reinterpret_cast<const int32_t*>(layerBase + skk::offsets::kFieldResumeGraceCtrOffset);
            const bool frozen = frozenFlag == 0 || freezeCtr >= 2 ||
                layer->m_resumeTimer > 0.0f;
            if (frozen) {
                ++engine->startFrozenFrames;
                if (!engine->startFrozenReported) {
                    engine->startFrozenReported = true;
                    engine->startFrozenWall = skkbot2::hot::now();
                    skkbot2::log::info(
                        "[START-WIN] physics frozen from the first plan frame: "
                        "gate={} (frozenFlag={} freezeCtr={} resumeTimer={}) - "
                        "song/triggers run, physics does not; the macro cannot "
                        "dispatch until this lifts (BUG-70 family)",
                        frozenFlag == 0 ? "pause-flag"
                            : (freezeCtr >= 2 ? "resume-grace" : "resume-fade"),
                        static_cast<int>(frozenFlag), freezeCtr,
                        static_cast<int>(layer->m_resumeTimer));
                }
                const double warp = std::clamp(
                    static_cast<double>(layer->m_gameState.m_timeWarp), 0.0, 1.0);
                const double step = engine->fixedPhysicsDelta(warp);
                const int64_t frozenSteps =
                    step > 0.0 ? static_cast<int64_t>(frameDt / step) : 0;
                if (frozenFlag == 0) {
                    ++engine->batFrozenFlagPlans;
                    engine->batFrozenFlagSteps += frozenSteps;
                }
                if (freezeCtr >= 2) {
                    ++engine->batFreezeCtrPlans;
                    engine->batFreezeCtrSteps += frozenSteps;
                }
                if (layer->m_resumeTimer > 0.0f) {
                }
                engine->sampleFreezeDropped += *extraDelta;
                *extraDelta = 0.0;
                engine->tickAccumulator = 0.0f;
                ++engine->sessionPlanCount;
                engine->sessionLastLevelTime = layer->m_gameState.m_levelTime;
                if (!engine->sessionLevelTimeInit) {
                    engine->sessionLevelTimeInit = true;
                    engine->sessionFirstLevelTime = layer->m_gameState.m_levelTime;
                    engine->sessionSampleLastLevelTime = layer->m_gameState.m_levelTime;
                }
                engine->sessionSampleDtSum += frameDt;
                if (engine->sessionSampleWallStart <= 0.0) {
                    engine->sessionSampleWallStart = skkbot2::hot::now();
                }
                setExpectedTicks(0);
                layer->GJBaseGameLayer::update(dt);
                return;
            }
            if (engine->startFrozenReported && !engine->startLiveReported) {
                engine->startLiveReported = true;
                skkbot2::log::info(
                    "[START-WIN] physics live after {} frozen frame(s), {:.3f}s "
                    "wall from first frozen frame (gate plans: pause={} grace={} "
                    "fade={}); those frames owed nothing - this is the visible "
                    "start delay",
                    engine->startFrozenFrames, skkbot2::hot::now() - engine->startFrozenWall,
                    engine->batFrozenFlagPlans, engine->batFreezeCtrPlans,
                    engine->sessionResumeZeroPlans);
            }
        }

        double timeWarp = std::clamp(static_cast<double>(layer->m_gameState.m_timeWarp), 0.0, 1.0);
        double timestep = engine->fixedPhysicsDelta(timeWarp);

        TickStepPlanner planner;
        planner.accumulated = *extraDelta;
        int64_t wantedSteps = static_cast<int64_t>(planner.planSteps(timestep));
        engine->sampleWantSum += static_cast<double>(wantedSteps) * timestep;
        static double emaStepCost = 0.0;
        constexpr int64_t kMinStepsPerFrame = 8;
        const auto& gui = skkbot2::gui::get();
        const bool skcForce = skcDeterministicGrid();
        const bool skcRealTime =
            skcForce && gui.realTime && !gui.lockDelta;
        const bool lockDeltaActive =
            (skcForce && !skcRealTime) ||
            (!skcForce && gui.lockDelta && !gui.realTime);
        const bool realTimeActive = skcRealTime ||
            (!skcForce && gui.realTime && !gui.lockDelta);

        const bool lockLocked = skcForce ||
            !skkbot2::render::Renderer::get().isRecording() &&
            (skkbot2::fps::macroHolds() || skkbot2::fps::enabled() || lockDeltaActive);
        const double lockInterval = static_cast<double>(
            cocos2d::CCDirector::sharedDirector()->getAnimationInterval());
        double skcCadence = static_cast<double>(engine->lockLearnedInterval);
        if (skcForce && skcCadence <= 0.0) {
            skcCadence = static_cast<double>(frameDt);
        }
        const double lockPromise = engine->lockLearnedInterval > 0.0f
            ? static_cast<double>(engine->lockLearnedInterval)
            : lockInterval;
        double promisedFrame = static_cast<double>(frameDt);
        if (lockLocked && lockInterval > 0.0) {
            if (skcForce) {
                promisedFrame = skcRealTime ? static_cast<double>(frameDt)
                                            : skcCadence;
            } else if (lockDeltaActive && !skcForce) {
                promisedFrame = lockPromise;
            } else if (realTimeActive) {
                promisedFrame = static_cast<double>(frameDt);
            } else {
                const bool lockHolds = engine->lockOffCadenceFraction < 0.25;
                promisedFrame = lockHolds ? lockInterval : static_cast<double>(frameDt);
            }
        }
        if (skkbot2::render::Renderer::get().isRecording()) {
            promisedFrame = static_cast<double>(frameDt);
        }
        const double steadyState = timestep > 0.0 ? promisedFrame / timestep : 1.0;

        int64_t stepCap = std::numeric_limits<int64_t>::max();
        if (!lockDeltaActive && !realTimeActive) {
            double interval = cocos2d::CCDirector::sharedDirector()->getAnimationInterval();
            double speed = std::max(static_cast<double>(engine->gameSpeed), 1.0);
            double per_frame = std::ceil(engine->runtimeTickRate() * std::max(interval, 1.0 / 1000.0) * speed);
            int64_t unlockedCap = static_cast<int64_t>(std::clamp(per_frame * 10.0, 1.0, 10000000.0));

            int64_t affordableCap = std::numeric_limits<int64_t>::max();
            if (emaStepCost > 0.0 && std::isfinite(emaStepCost)) {
                double budget = std::clamp(interval * 1.5, 0.004, 0.05);
                double affordable = std::floor(budget / emaStepCost);
                if (std::isfinite(affordable) && affordable > 0.0) {
                    affordableCap = std::max<int64_t>(static_cast<int64_t>(affordable), 8);
                }
            }
            stepCap = std::max<int64_t>(std::min<int64_t>(unlockedCap, affordableCap), 8);
        }

        int64_t steps = lockDeltaActive ? wantedSteps : std::min<int64_t>(wantedSteps, stepCap);

        const double carryBefore = *extraDelta;
        double totalDelta = static_cast<double>(steps) * timestep;
        *extraDelta -= totalDelta;

        if (steps < wantedSteps) {
            const double backlog = static_cast<double>(wantedSteps - steps) * timestep;
            ++engine->lockBacklogDrops;
            engine->lockBacklogDropped += backlog;
            (void)backlog;
        }
        if (steps >= wantedSteps) {
            if (engine->emaFrameTime <= 0.0) {
                engine->emaFrameTime = frameDt;
            } else {
                engine->emaFrameTime +=
                    0.1 * (static_cast<double>(frameDt) - engine->emaFrameTime);
            }
        }

        if (locked) {
            ++engine->lockPlanCount;
            engine->lockWantedSum += wantedSteps;
            engine->lockStepSum += steps;
            if (wantedSteps > engine->lockMaxWanted) {
                engine->lockMaxWanted = wantedSteps;
            }
            if (steps == 0) {
                ++engine->lockZeroFrames;
            }
            if (!engine->lockLoggedOn) {
                engine->lockLoggedOn = true;
                engine->lockFirstPlanFrame = static_cast<int>(
                    layer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;
                skkbot2::log::info(
                    "lock-delta: on at frame {}, batch dt={:.6f} (frame time is "
                    "substituted in advanceNormalScheduler), timestep={:.7f} -> "
                    "{} steps/plan",
                    engine->lockFirstPlanFrame, dt, timestep, wantedSteps);
            }
        }
        (void)carryBefore;

        engine->tickAccumulator = static_cast<float>(std::max(0.0, *extraDelta));

        engine->frameEntryTick = static_cast<int>(
            layer->m_gameState.m_levelTime * engine->runtimeTickRate()) + 1;

        ++engine->sessionPlanCount;
        engine->sessionStepSum += steps;
        engine->sessionLastLevelTime = layer->m_gameState.m_levelTime;
        if (!engine->sessionLevelTimeInit) {
            engine->sessionLevelTimeInit = true;
            engine->sessionFirstLevelTime = layer->m_gameState.m_levelTime;
            engine->sessionSampleLastLevelTime = layer->m_gameState.m_levelTime;
        }

        skkbot2::autosave::tickRecording();

#ifdef GEODE_IS_WINDOWS
        {
            static bool modeLogged = false;
            if (!modeLogged && steps > 0) {
                modeLogged = true;
                skkbot2::log::info(
                    "step-grid: mode=patch (echo-style batch, midhooks not "
                    "armed), steps={} nominalStep={:.12f} physicsStep={:.12f} "
                    "warp={:.6f}",
                    steps, engine->fixedSimulationDelta(), timestep, timeWarp);
            }
        }
        {
            engine->sessionSampleStepsWindow += steps;
            engine->sessionSampleDtSum += frameDt;
            const double wallNow = skkbot2::hot::now();
            if (engine->sessionSampleWallStart <= 0.0) {
                engine->sessionSampleWallStart = wallNow;
            } else if (wallNow - engine->sessionSampleWallStart >= 2.0) {
                const double wall = wallNow - engine->sessionSampleWallStart;
                const double levelNow = layer->m_gameState.m_levelTime;
                if (skk::skc::Bot::get().isRecording() &&
                    engine->sampleSpdLast > 0.0 &&
                    std::abs(engine->gameSpeed - engine->sampleSpdLast) > 0.001) {
                    skkbot2::log::info(
                        "[SKC] RECORD: speedhack changed inside the recording "
                        "({:.2f}x -> {:.2f}x); the play side replays speeds "
                        "from its own live value per frame - dynamic speed "
                        "tracking (BUG-97) decides whether the two can ever "
                        "match.",
                        engine->sampleSpdLast, engine->gameSpeed);
                }
                engine->sampleSpdLast = engine->gameSpeed;
                if (engine->lockPlanCount > 0) {
                    skkbot2::log::info(
                        "lock-delta sample: plans={} served={} wanted={} "
                        "(avg {:.3f}/frame, worst {}) zero-frames={} "
                        "backlog refused {}x ({:.2f}s) walks={} "
                        "dt-skew sum={:.4f}s worst={:.4f}s carry-now={:.7f}",
                        engine->lockPlanCount, engine->lockStepSum,
                        engine->lockWantedSum,
                        static_cast<double>(engine->lockWantedSum) /
                            static_cast<double>(engine->lockPlanCount),
                        engine->lockMaxWanted, engine->lockZeroFrames,
                        engine->lockBacklogDrops, engine->lockBacklogDropped,
                        engine->sessionSchedWalks,
                        engine->lockDroppedCarry, engine->lockMaxDtSkew,
                        *extraDelta);
                }
                skkbot2::log::info(
                    "step-grid sample: wall={:.2f}s dtSum={:.4f}s "
                    "dtRate={:.1f}% plans={} steps={} ticks/plan={:.2f} "
                    "carry={:.7f} warp={:.3f} spd={:.2f} | levelTime +{:.4f}s "
                    "(planned {:.4f}s) lag={:+.1f}ms | resume={} walks={} "
                    "sess[plans={},steps={},resumeZero={}] "
                    "rawDt[min={:.5f} avg={:.5f} max={:.5f} n={}] "
                    "wallGap[min={:.5f} avg={:.5f} max={:.5f} sum={:.3f}s] "
                    "dtRate2={:.1f}% "
                    "feedLayers[raw={:.3f} dwm={:.3f} clamp={:.3f} "
                    "fed={:.3f} drives={} rewrites={}] "
                    "phantom[n={} dtSum={:.3f}s] "
                    "feed[sum={:.3f}s n={}] want={:.3f}s freezeDrop={:.3f}s "
                    "vanillaExits={} skcFrac={:.2f}",
                    wall, engine->sessionSampleDtSum,
                    wall > 0.0 ? engine->sessionSampleDtSum / wall * 100.0 : 0.0,
                    static_cast<long long>(engine->sessionPlanCount),
                    engine->sessionSampleStepsWindow,
                    wall > 0.0 && engine->sessionPlanCount > 0
                        ? static_cast<double>(engine->sessionSampleStepsWindow) /
                              static_cast<double>(engine->sessionPlanCount)
                        : 0.0,
                    *extraDelta, layer->m_gameState.m_timeWarp,
                    engine->gameSpeed,
                    levelNow - engine->sessionSampleLastLevelTime,
                    static_cast<double>(engine->sessionSampleStepsWindow) /
                        std::max(1.0, engine->runtimeTickRate()),
                    (wall - engine->sessionSampleDtSum) * 1000.0,
                    static_cast<int>(layer->m_resumeTimer),
                    engine->sessionSchedWalks,
                    engine->sessionPlanCount, engine->sessionStepSum,
                    engine->sessionResumeZeroPlans,
                    engine->sampleRawDtMin,
                    engine->sampleRawFrames > 0
                        ? engine->sampleRawDtSum /
                              static_cast<double>(engine->sampleRawFrames)
                        : 0.0,
                    engine->sampleRawDtMax,
                    engine->sampleRawFrames,
                    engine->sampleWallGapMin,
                    static_cast<double>(engine->sampleRawFrames) > 0
                        ? engine->sampleWallGapSum /
                              static_cast<double>(engine->sampleRawFrames)
                        : 0.0,
                    engine->sampleWallGapMax,
                    engine->sampleWallGapSum,
                    engine->sampleWallGapSum > 0.0
                        ? engine->sessionSampleDtSum /
                              engine->sampleWallGapSum * 100.0
                        : 0.0,
                    engine->sampleRawDtSum,
                    engine->sampleDwmSum,
                    engine->sampleClampSum,
                    engine->sampleFedSum,
                    engine->sampleDrives,
                    engine->sampleDwmRewrites,
                    engine->samplePhantomCount,
                    engine->samplePhantomDtSum,
                    engine->sampleDebtSum,
                    engine->sampleDebtCount,
                    engine->sampleWantSum,
                    engine->sampleFreezeDropped,
                    engine->sampleVanillaExits,
                    engine->lockOffCadenceFraction);
                engine->sessionSampleWallStart = wallNow;
                engine->sessionSampleLastLevelTime = levelNow;
                engine->sessionSampleStepsWindow = 0;
                engine->sessionSampleDtSum = 0.0;
                engine->sampleRawDtMin = 0.0f;
                engine->sampleRawDtMax = 0.0f;
                engine->sampleRawDtSum = 0.0;
                engine->sampleRawFrames = 0;
                engine->sampleWallGapSum = 0.0;
                engine->sampleWallGapMin = -1.0f;
                engine->sampleWallGapMax = 0.0f;
                engine->sampleDwmSum = 0.0;
                engine->sampleClampSum = 0.0;
                engine->sampleFedSum = 0.0;
                engine->sampleDrives = 0;
                engine->sampleDwmRewrites = 0;
                engine->samplePhantomCount = 0;
                engine->samplePhantomDtSum = 0.0;
                engine->sampleDebtSum = 0.0;
                engine->sampleDebtCount = 0;
                engine->sampleWantSum = 0.0;
                engine->sampleFreezeDropped = 0.0;
                engine->lastDriveWall = skkbot2::hot::now();
                engine->sampleVanillaExits = 0;
            }
        }

#endif

        const double planLevelBefore = layer->m_gameState.m_levelTime;
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
        const bool deferArt = steps > 1;
        if (deferArt) {
            skk::setPlayerArtDeferred(true);
        }
        auto stepStart = std::chrono::steady_clock::now();
        layer->GJBaseGameLayer::update(static_cast<float>(totalDelta));
        if (deferArt) {
            skk::setPlayerArtDeferred(false);
            if (layer->m_player1) {
                layer->m_player1->setPosition(layer->m_player1->getPosition());
            }
            if (layer->m_player2) {
                layer->m_player2->setPosition(layer->m_player2->getPosition());
            }
        }
        engine->customTickPlanActive = false;
        if (steps > 0) {
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - stepStart).count();
            double perStep = elapsed / static_cast<double>(steps);
            if (emaStepCost <= 0.0) emaStepCost = perStep;
            else emaStepCost = emaStepCost * 0.7 + perStep * 0.3;
        }
        setExpectedTicks(0);
#endif
        batProbe(layer, steps, totalDelta, planLevelBefore, engine);
    }}

void resetSimulationTimingState() {
    auto& controller = timingController();
    setExpectedTicks(0);
    g_extraDelta = 0.0;
    skkbot2::timing::reset_refresh_clock();

    if (auto* engine = ReplayEngine::get()) {
        controller.resetSimulation(*engine);
        engine->clearQueuedSubstepState();
    }
}

void disableExpectedTicksPatch() {
    setExpectedTicksPatchEnabled(false);
}

void dropCarry() {
    g_extraDelta = 0.0;
    skkbot2::timing::reset_refresh_clock();
    if (auto* play = PlayLayer::get()) {
        play->m_extraDelta = 0.0f;
    }
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
          skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::SchedUpdate);
          skkbot2::hot::bumpVisualFrame();
          skkbot2::step_trace::refreshCache();
        FrameStepper::get().updateAutoPause();

        auto* engine = ReplayEngine::get();
        skkbot2::fps::sync(engine->runtimeTickRate(), engine->gameSpeed);

        {
            static int entryLogged = 0;
            if (entryLogged < 60) {
                entryLogged++;
                const bool inLevel = PlayLayer::get() != nullptr;
                const bool editor = skk::gameplay::isEditorPlaytest();
                const bool custom = shouldUseCustomTiming(*engine);
                skkbot2::log::info(
                    "[SCHED] entry #{}: dt={:.5f} level={} editor={} spd={:.3f} "
                    "tps={:.2f} stepping={} custom={}",
                    entryLogged, static_cast<double>(dt), inLevel, editor,
                    engine->gameSpeed, engine->runtimeTickRate(),
                    engine->tickStepping, custom);
            }
        }

        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            ++engine->sessionVanillaExits;
            ++engine->sampleVanillaExits;
            controller.resetVanillaState(*engine);
            setExpectedTicksPatchEnabled(false);
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        ++engine->sessionSchedWalks;

        setExpectedTicksPatchEnabled(true);

        if (engine->tickStepping) {
            skkbot2::fps::release();
            auto* pl = PlayLayer::get();
            bool canStep = (pl && !pl->m_isPaused && !pl->m_hasCompletedLevel)
                        || (!pl && skk::gameplay::isEditorPlaytest());
            if (!canStep) {
                controller.resetVanillaState(*engine);
                {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
            }

            if (FrameStepper::get().updateScheduler(this, dt)) {
                return;
            }
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        controller.advanceNormalScheduler(this, *engine, dt);
    }
};

namespace skkbot2 {
std::string fmodChannelDump() {
    auto* audio = FMODAudioEngine::get();
    if (!audio) return "(no fmod engine)";
    std::string dump;
    for (auto& [id, chan] : audio->m_channelIDToChannel) {
        if (!chan) continue;
        bool playing = false;
        bool paused = false;
        unsigned int pos = 0;
        int state = 0;
        if (chan->isPlaying(&playing) == FMOD_OK && playing) {
            state = (chan->getPaused(&paused) == FMOD_OK && paused) ? 2 : 1;
        }
        (void)chan->getPosition(&pos, FMOD_TIMEUNIT_MS);
        dump += fmt::format("[{}:s{}@{}]", id, state, pos);
    }
    if (dump.empty()) { return "(none)"; }
    return dump;
}

namespace entry_state {
    const void* g_entryLayer = nullptr;
    double g_entryWall = 0.0;
    int g_prevPause = -1;
    bool g_done = false;
    double g_lastChanDump = -1.0;
}

void entryFreezeWatch(GJBaseGameLayer* layer) {
    if (PlayLayer::get() !=
            reinterpret_cast<const PlayLayer*>(layer)) {
        return;
    }
    if (entry_state::g_entryLayer != layer) {
        entry_state::g_entryLayer = layer;
        entry_state::g_entryWall = hot::now();
        entry_state::g_prevPause = -1;
        entry_state::g_done = false;
        entry_state::g_lastChanDump = -1.0;
        log::info("[ENTRY-FREEZE] watching level entry");
    }
    if (entry_state::g_done) { return; }
    const auto base = reinterpret_cast<uintptr_t>(layer);
    const int pause =
        static_cast<int>(*reinterpret_cast<const uint8_t*>(base + skk::offsets::kFieldPauseFlagOffset));
    const int grace = static_cast<int>(
        *reinterpret_cast<const int32_t*>(base + skk::offsets::kFieldResumeGraceCtrOffset));
    const double since = hot::now() - entry_state::g_entryWall;
    if (since >= 8.0) {
        entry_state::g_done = true;
        return;
    }
    if (pause != entry_state::g_prevPause) {
        entry_state::g_prevPause = pause;
        log::info(
            "[ENTRY-FREEZE] pauseFlag={} grace={} t={:.3f}s",
            pause, grace, since);
    }
    if (since - entry_state::g_lastChanDump > 0.25) {
        entry_state::g_lastChanDump = since;
        log::info(
            "[ENTRY-FREEZE] t={:.3f}s chans={}", since, fmodChannelDump());
    }
    if (pause != 0 && grace < 2 && layer->m_resumeTimer <= 0.0f) {
        entry_state::g_done = true;
        log::info(
            "[ENTRY-FREEZE] physics LIVE at {:.3f}s from entry (pause={} "
            "grace={} resume={}) chans={}",
            since, pause, grace,
            static_cast<int>(layer->m_resumeTimer), fmodChannelDump());
    }
}

void entryFreezeReset() {
    entry_state::g_entryLayer = nullptr;
    entry_state::g_entryWall = 0.0;
    entry_state::g_prevPause = -1;
    entry_state::g_done = false;
    entry_state::g_lastChanDump = -1.0;
}

bool isEntryFrozen() {
    return entry_state::g_entryLayer != nullptr && !entry_state::g_done;
}
}

class $modify(PhysicsControlLayer, GJBaseGameLayer) {
    void update(float dt) {
        skkbot2::entryFreezeWatch(this);
        skkbot2::skcprobe::beginFrame(static_cast<double>(dt));
        auto* engine = ReplayEngine::get();
        if (engine && engine->customTickPlanActive) {
            return GJBaseGameLayer::update(dt);
        }

        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return GJBaseGameLayer::update(dt);
        }

        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            controller.resetVanillaState(*engine);
            setExpectedTicksPatchEnabled(false);
            GJBaseGameLayer::update(dt);
            return;
        }

        setExpectedTicksPatchEnabled(true);

        if (engine->tickStepping) {
            skkbot2::fps::release();
            auto* pl = PlayLayer::get();
            bool canStep = (pl && !pl->m_isPaused && !pl->m_hasCompletedLevel)
                        || (!pl && skk::gameplay::isEditorPlaytest());
            if (!canStep) {
                controller.resetVanillaState(*engine);
                setExpectedTicksPatchEnabled(false);
                GJBaseGameLayer::update(dt);
                return;
            }

            g_extraDelta = 0.0;
            const double timestep = engine->fixedPhysicsDelta(
                std::clamp<double>(pl->m_gameState.m_timeWarp, 0.0, 1.0));
            setExpectedTicks(1);
            engine->customTickPlanActive = true;
            skk::StepGridEnforcement enforcement(1, timestep);
            GJBaseGameLayer::update(static_cast<float>(timestep));
            engine->customTickPlanActive = false;
            return;
        } else {
            applyCustomTickPlan(this, &g_extraDelta, dt, engine);
        }
    }

    double getModifiedDelta(float dt) {
        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        auto* engine = ReplayEngine::get();
        if (!shouldUseCustomTiming(*engine)) {
            setExpectedTicksPatchEnabled(false);
            return GJBaseGameLayer::getModifiedDelta(dt);
        }

        setExpectedTicksPatchEnabled(true);

        float modDelta = GJBaseGameLayer::getModifiedDelta(dt);

        if (engine->customTickPlanActive) {
            if (m_resumeTimer > 0) {
                const uint8_t pauseFlag =
                    *reinterpret_cast<const uint8_t*>(
                        reinterpret_cast<uintptr_t>(this) + skk::offsets::kFieldPauseFlagOffset);
                if (pauseFlag != 1) {
                    return 0.0;
                }
                --m_resumeTimer;
                ++engine->sessionResumeZeroPlans;
                return 0.0;
            }
            const double perStep = engine->fixedPhysicsDelta(
                std::clamp<double>(m_gameState.m_timeWarp, 0.0, 1.0));
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_INTEL_MAC)
            return perStep * static_cast<double>(g_expectedTicks);
#else
            return perStep;
#endif
        }

        return modDelta;
    }
};
