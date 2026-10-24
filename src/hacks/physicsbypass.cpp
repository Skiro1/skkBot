#include "skkBot.hpp"
#include "core/frame_stepper.hpp"
#include "core/hotpath.hpp"
#include "core/playback_fps.hpp"
#include "gui/skkbot_gui.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/physicsbypass.hpp"
#include "physics/step_grid_hooks.hpp"
#include "skc/skc_bot.hpp"

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
        // SKC runs its own record/playback session on the same step grid. It
        // sets engine.tickRate from its macro, so the runtimeTickRate probe
        // below covers it; the explicit check keeps an SKC session at exactly
        // 240 tps (a legal value) from silently falling back to vanilla.
        const bool skcSession = skk::skc::Bot::get().isEnabled();

        // Editor playtest: GD funnels processCommands at ~180/s with dt=1/180,
        // and the step clock is compensated for exactly that cadence (BUG-10).
        // Do NOT drag a plain editor PLAY onto the custom 240 grid - it would
        // break the compensation. Only take over timing when the user actually
        // changed TPS/Speed (or render/autoclicker/frame-step needs it).
        if (skk::gameplay::isEditorPlaytest()) {
            return engine.tickStepping
                || engine.singleTickStep
                || skcSession
                || skkbot2::render::Renderer::get().isRecording()
                || !nearlyEqual(engine.runtimeTickRate(), ReplayEngine::kBaseTickRate, 0.01)
                || !nearlyEqual(engine.gameSpeed, 1.0, 0.0001)
                // Same reason as the non-editor branch below: a pinned display
                // rate only takes effect if we take over scheduling, and in the
                // editor the other terms can all be false at once.
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
            // A pinned display rate is a scheduling input too, and leaving it
            // out of this list made the FPS row silently do nothing for anyone
            // who set it without a macro loaded: 240 tps / speed 1 / no session
            // is vanilla by every other term here, so the scheduler ran
            // untouched and fps::sync() was never reached. The setting then
            // looked broken rather than inactive.
            || skkbot2::fps::enabled()
            || Autoclicker::get()->enabled;
    }

    struct TickStepPlanner {
        double accumulated = 0.0;

        void absorb(double dt) {
            accumulated += dt;
        }

        // floor, not llround.
        //
        // Silicate uses floor (updater.cpp:82: int steps = std::floor(dt /
        // wantedDt)), and so must we, because floor is what makes the carry
        // well-behaved: the remainder dt - steps * wantedDt is then in
        // [0, wantedDt) by construction.
        //
        // llround is equally correct on average - the sums match - but it emits a
        // step before the time has actually accumulated, so the carry goes
        // NEGATIVE half the time. That matters here specifically because the same
        // quantity is kept twice: the accumulator, and engine->tickAccumulator,
        // assigned with std::max(0.0, *extraDelta). With a negative carry those
        // two disagree - the engine is handed 0 while the planner is carrying a
        // debt - and the clamp hides it rather than showing it.
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

    // How many drawn frames the "is the display keeping up" verdict is taken
    // over. It is a count of FRAMES, so the window is ~1 s at 240 fps and ~4 s
    // at 60 - short enough to react to a machine that cannot hold the rate, long
    // enough that one late frame (exactly what lock delta exists to smooth away)
    // does not flip the verdict on its own.
    constexpr long long kLockRateWindow = 240;
    // How far the measured frame time may sit from the interval and still be
    // considered the same clock.
    constexpr double kLockCadenceTolerance = 0.05;

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

            // Every path into GD's own scheduler is timed separately from our
            // hook body. The hook runs once per drive step and can call the base
            // up to kMaxBatchesPerFrame times, so the outer timer mostly
            // measures the game's physics and the bot looks expensive when it is
            // not. Subtracting SchedVanilla from SchedUpdate is what makes the
            // difference visible.
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                scheduler->CCScheduler::update(fixedDelta(engine));
            }

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
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                scheduler->CCScheduler::update(speedDelta(engine, rawDt));
                return;
            }
            // Display-FPS control and the step budget derived from it. The
            // budget used to be a constant 8 batches, which is 2x what 240 tps
            // needs at 60 fps and therefore looked fine until the display
            // dropped: at 20 fps the frame owes 12 steps, the loop stopped at
            // 8, and the leftover carry was clamped away - so the macro simply
            // played slower, with nothing logged anywhere. fps::stepCap()
            // derives the budget from the frame rate that is actually in
            // effect, so a slow display costs headroom instead of correctness.
            skkbot2::fps::sync(engine.runtimeTickRate(), engine.gameSpeed);
            const int stepCap = skkbot2::fps::stepCap(engine.runtimeTickRate(), engine.gameSpeed);

            // LOCK DELTA, and this is the only place it can honestly live.
            //
            // The mode is Echo's (updater.cpp:341-344): replace the MEASURED
            // frame time with the CONFIGURED interval, so the level advances by
            // exactly 1/fps per drawn frame however long that frame took. The
            // measured frame time exists here - rawDt, once per drawn frame -
            // and nowhere else. It used to be applied two calls down in
            // applyCustomTickPlan, which runs once per 1/tps SCHEDULER BATCH, so
            // the "measured dt" it compared against the interval was always
            // 1/tps and never a frame time:
            //
            //   * at 240 tps pinned to 60 fps it read |1/240 - 1/60| / (1/60) =
            //     75%, decided the display was off cadence, and printed that
            //     verdict with `arriving at about 0 fps` because the argument
            //     list was shifted one slot;
            //   * for the first kLockRateWindow plans the verdict window had not
            //     filled yet, so it concluded the opposite - and advanced the
            //     clock by 1/60 for every 1/240 of a batch, four times real time,
            //     for the first second of every session. The macro raced at the
            //     start and then settled: a defect that looks like a warm-up.
            //
            // Substituting here costs nothing when the display really does keep
            // the interval (rawDt is already that number) and is the whole point
            // when frames jitter around it.
            const bool lockLocked =
                !skkbot2::render::Renderer::get().isRecording() &&
                (skkbot2::fps::macroHolds() || skkbot2::fps::enabled());
            const double lockInterval = static_cast<double>(
                cocos2d::CCDirector::sharedDirector()->getAnimationInterval());
            if (lockLocked && lockInterval > 0.0) {
                // 5% is scheduler rounding on the levels that really are
                // display-paced; in an editor playtest the gap is a factor of
                // two or three, which no plausible rounding explains. Kept
                // two-sided for the reason BUG-10 records: a one-sided test
                // calls an editor frame at dt=1/180 PERFECTLY ON CADENCE against
                // an interval of 1/60.
                const double cadenceError =
                    std::abs(static_cast<double>(rawDt) - lockInterval) / lockInterval;
                const bool onCadence = cadenceError <= kLockCadenceTolerance;
                engine.lockOffCadenceInWindow += onCadence ? 0 : 1;
                engine.lockWindowFrames += 1;
                if (engine.lockWindowFrames >= kLockRateWindow) {
                    engine.lockOffCadenceFraction =
                        static_cast<double>(engine.lockOffCadenceInWindow) /
                        static_cast<double>(engine.lockWindowFrames);
                    engine.lockOffCadenceInWindow = 0;
                    engine.lockWindowFrames = 0;
                }
                // Delivered rate from FRAME times. This is the number the user
                // asked for as a fact and the log never carried: requested fps
                // was printed, delivered fps was not.
                engine.lockDeliveredTotal += static_cast<double>(rawDt);
                engine.lockDeliveredFrames += 1;

                const bool holds = engine.lockOffCadenceFraction < 0.25;
                if (!holds && !engine.lockWarnedSlow) {
                    engine.lockWarnedSlow = true;
                    const double deliveredFps = engine.lockDeliveredTotal > 0.0
                        ? static_cast<double>(engine.lockDeliveredFrames) /
                              engine.lockDeliveredTotal
                        : 0.0;
                    skkbot2::log::warn(
                        "[TIMING] display is not holding the pinned rate - asked "
                        "{:.0f} fps (interval {:.6f}), frames are arriving at "
                        "about {:.1f} fps (measured dt {:.6f} against interval "
                        "{:.6f}). The nominal interval is not being used, "
                        "because advancing the clock by a frame time the machine "
                        "never delivers is what makes the level run slow. Level "
                        "speed stays correct either way.",
                        1.0 / lockInterval, lockInterval, deliveredFps,
                        static_cast<double>(rawDt), lockInterval);
                }
                if (!engine.lockEverOn && holds) {
                    // The frame number is stamped by the planner below, which is
                    // the first place that has the layer's level time. This flag
                    // only says "the substitution has engaged at least once".
                    engine.lockEverOn = true;
                }
            }

            const bool lockHolds =
                engine.lockOffCadenceFraction < 0.25 && lockInterval > 0.0;
            const float frameTime =
                (lockLocked && lockHolds) ? static_cast<float>(lockInterval) : rawDt;
            if (lockLocked) {
                // What the substitution actually changed, measured against the
                // frame time it replaced. Positive means the clock advanced by
                // less than the frame really took.
                engine.lockDroppedCarry +=
                    static_cast<double>(rawDt) - static_cast<double>(frameTime);
                engine.lockMaxDtSkew = std::max(
                    engine.lockMaxDtSkew,
                    std::abs(static_cast<double>(rawDt) - static_cast<double>(frameTime)));
            }

            schedulerCarry += speedDelta(engine, frameTime);
            int batches = 0;
            while (schedulerCarry + step * 0.001f >= step && batches < stepCap) {
                schedulerCarry -= step;
                // Timed as GD's own work, not ours. Our hook runs once per drive
                // step and calls this up to the budget times, so the
                // outer hook timer alone reports mostly the game's physics and
                // makes the bot look expensive when it is not. Measured on
                // 2026-10-18: sched.update(hook) = 287.9ms per wall second -
                // subtracting this slot is what turns that number into "what the
                // mod actually costs".
                {
                    skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                    scheduler->CCScheduler::update(step);
                }
                ++batches;
            }
            // Stopped by the budget rather than by the carry: this frame could
            // not deliver everything the clock owed. That is the exact
            // condition under which a macro falls behind, and before this
            // counter it was indistinguishable from "the bot is a bit slow".
            if (batches >= stepCap && schedulerCarry + step * 0.001f >= step) {
                // Debt in STEPS, measured here and now - not a rate-derived
                // guess. The first version printed "needed ~N steps" computed
                // from the live animation interval, which is the rate AFTER the
                // long frame had passed, and it produced a message that
                // contradicted itself: "needed 5, budget 20, cannot keep up".
                // 20 is four times 5. It fired once, 10ms after the step grid
                // engaged, on a loading frame where catch-up had worked exactly
                // as intended. The numbers have to come from the same instant.
                const double owedSteps = schedulerCarry / step;
                skkbot2::fps::noteCapHit();
                // Still only a warning after a sustained run of them: a single
                // long frame is normal and the catch-up handled it.
                const unsigned long long hits = skkbot2::fps::capHits();
                if (hits == 1 || hits == 10 || (hits % 600) == 0) {
                    skkbot2::log::warn(
                        "[TIMING] step budget hit {} times - {:.0f} steps were "
                        "owed and only {} could run this frame (budget {}, was "
                        "about {:.0f} fps). The surplus {:.0f} is dropped: the "
                        "display cannot keep up at this frame rate. Set FPS to a "
                        "value the machine actually holds, or to 0 to follow TPS.",
                        hits, owedSteps, batches, stepCap,
                        1.0 / std::max(1e-6f, rawDt), owedSteps - batches);
                }
            }
            const float maxCarry = step * static_cast<float>(stepCap);
            if (schedulerCarry > maxCarry) schedulerCarry = maxCarry;
            if (schedulerCarry < 0.0f) schedulerCarry = 0.0f;
        }

        void resetVanillaState(ReplayEngine& engine, double* extraDelta = nullptr) {
            resetSchedulingCarry();
            // Every path back to vanilla scheduling goes through here, so this
            // is where a pinned display rate is handed back. Leaking a 30 fps
            // pin into ordinary play would be far worse than never pinning, and
            // there are four callers that all reach vanilla the same way.
            skkbot2::fps::release();
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

        // LOCK DELTA - always on, and it is NOT what the removed button did.
        //
        // Echo, updater.cpp:341-344:
        //     if (!rendering && lock_delta) {
        //         frame_dt = CCDirector::sharedDirector()->getAnimationInterval();
        //     }
        //
        // It replaces the MEASURED frame time with the CONFIGURED interval and
        // lets the ordinary planner derive the step count from that. The level
        // then advances by exactly 1/fps per drawn frame however long the frame
        // actually took - deterministic instead of hostage to the wall clock.
        //
        // The button clamped steps to 1 and threw the carry away instead. That
        // coincides with Echo only when fps == tps and BREAKS real time when they
        // differ: at fps 60 / tps 240 the macro ran at a quarter speed. The user
        // asking for lock delta to "work well with tps and fps" is what exposed
        // the shape. There is no flag and no row now because this is a property
        // of correct playback, not an option.
        //
        // ...but only while the display is ACTUALLY DELIVERING that interval.
        //
        // This is the bug the user's report found, and the log line that found it
        // is worth keeping in this comment:
        //
        //   measured dt=0.004167 replaced by interval=0.002778, timestep=0.0041667
        //
        // 1/240 measured against 1/360 promised. The slider said 360, the machine
        // delivered 240, and the accumulator was advanced by 1/360 for every one
        // of those 240 frames a second - so the level got 2/3 of the time it
        // should have and ran at 160 steps instead of 240. A third slow, in the
        // menu as well as in the macro, because fps::enabled() puts this path in
        // the loop for the whole game.
        //
        // Substituting the NOMINAL interval is only sound when the display keeps
        // it. When it does not, the honest frame time is the measured one, and the
        // only substitute that preserves real time in every case is a smoothed
        // MEASURED mean - steps/sec must equal tps, and that fixes frameDt at
        // 1/delivered_fps, not 1/requested_fps.
        //
        // So: the interval is used when the frames keep it, and the measurement
        // decides. The test is a rolling window rather than a single frame, since
        // one late frame is exactly what lock delta exists to smooth away, and
        // flipping per frame would re-introduce the jitter it removes.
        // The substitution itself now happens one call up, in
        // advanceNormalScheduler, where the MEASURED FRAME TIME exists. This
        // function runs once per 1/tps scheduler batch, so `dt` here is always
        // 1/tps: comparing it against the display interval measured the batch
        // grid, not the display, and substituting 1/fps for it multiplied the
        // clock by the number of batches a frame contains. The long comment that
        // used to justify the substitution here described the right behaviour in
        // the wrong place - see advanceNormalScheduler for what replaced it.
        //
        // What stays here is the per-PLAN audit: how many steps the planner was
        // asked for and how many it served, which only exists at this level.
        // What is left of `locked` is only "should these counters run at all".
        const bool locked =
            !skkbot2::render::Renderer::get().isRecording() &&
            (skkbot2::fps::macroHolds() || skkbot2::fps::enabled());
        const double frameDt = static_cast<double>(dt);
        *extraDelta += frameDt;
        engine->recentlyInitialized = false;

        double timeWarp = std::clamp(static_cast<double>(layer->m_gameState.m_timeWarp), 0.0, 1.0);
        double timestep = timeWarp * static_cast<double>(controller.fixedDelta(*engine));

        TickStepPlanner planner;
        planner.accumulated = *extraDelta;
        int64_t wantedSteps = static_cast<int64_t>(planner.planSteps(timestep));
        // The step cap, derived instead of fixed.
        //
        // Silicate: stepLimit = (int)m_tps / (int)fps, times an UPR setting
        //   (updater.cpp:95-101). Echo: ceil(sample_rate() * interval * speed) * 10
        //   (Logic::unlocked_step_cap). Both derive it from the ratio, because the
        //   number of steps a frame legitimately needs IS that ratio.
        //
        // Ours was a flat 8, which is a time machine pointed the wrong way: at
        // fps 15 / tps 240 a frame needs 16 steps, the clamp serves 8, and the
        // macro runs at half speed with nothing on screen saying why. Even at
        // fps 30 / tps 240 the steady state sits exactly ON the cap, so the first
        // frame of jitter clamps and the debt starts silently growing.
        //
        // The cap exists to refuse a STALL backlog, not to refuse the steady
        // state. So it is the steady state plus headroom - and never below the
        // old 8, because lowering it in the ranges that already worked would be
        // a change with no evidence behind it. Behaviour differs only where it
        // was broken.
        constexpr int64_t kMinStepsPerFrame = 8;
        constexpr int64_t kStallHeadroom = 4;
        const double steadyState = timestep > 0.0 ? frameDt / timestep : 1.0;
        // Named lockStepCap, not stepCap: this file already has a stepCap() from
        // fps::stepCap() meaning the number of SCHEDULER BATCHES, and two
        // unrelated limits under one name is how the wrong one gets edited.
        const int64_t lockStepCap = std::max<int64_t>(
            kMinStepsPerFrame,
            static_cast<int64_t>(std::ceil(steadyState)) + kStallHeadroom);
        int64_t steps = std::min<int64_t>(wantedSteps, lockStepCap);
        if (steps < wantedSteps) {
            ++engine->lockCapHits;
        }
        const double carryBefore = *extraDelta;
        double totalDelta = static_cast<double>(steps) * timestep;
        *extraDelta -= totalDelta;
        // The carry is KEPT, and that is the whole difference from the removed
        // row. With fps below tps the level has to advance several frames per
        // drawn frame to stay in real time, and this remainder is how that stays
        // true instead of drifting. Only a backlog past one step's worth is
        // refused, by kMaxStepsPerFrame above - that is the stall guard, nothing
        // else.

        // Lock delta audit, and only for frames that are actually locked. Now
        // that the carry survives, what is worth keeping is
        // whether the substitution is actually happening and what it produces: the
        // gap between the measured frame time and the interval replacing it, and
        // the step pattern the planner then decides. A deterministic mode whose
        // inputs stop moving has an output that cannot move either - this is the
        // reading that shows it rather than the assertion that it is so.
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
                // The frame number has to be computed HERE, not read from
                // frameEntryTick: that field is assigned at the end of this
                // function, so on the first plan it still holds the -1 it was
                // initialised with and the line that says where lock delta
                // engaged printed "on at frame -1".
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
                // The lock line is part of the same sample on purpose: comparing
                // "steps=2400 over 2400 plans" against "lock served 800 of 1800"
                // in two different places would make the reader do the subtraction
                // that the sample exists to avoid.
                if (engine->lockPlanCount > 0) {
                    skkbot2::log::info(
                        "lock-delta sample: plans={} served={} wanted={} "
                        "(avg {:.3f}/frame, worst {}) zero-frames={} cap-hits={} "
                        "delivered={:.1f}fps off-cadence={:.1f}% "
                        "dt-skew sum={:.4f}s worst={:.4f}s carry-now={:.7f}",
                        engine->lockPlanCount, engine->lockStepSum,
                        engine->lockWantedSum,
                        static_cast<double>(engine->lockWantedSum) /
                            static_cast<double>(engine->lockPlanCount),
                        engine->lockMaxWanted, engine->lockZeroFrames,
                        engine->lockCapHits,
                        engine->lockDeliveredTotal > 0.0
                            ? static_cast<double>(engine->lockDeliveredFrames) /
                                  engine->lockDeliveredTotal
                            : 0.0,
                        engine->lockOffCadenceFraction * 100.0,
                        engine->lockDroppedCarry, engine->lockMaxDtSkew,
                        *extraDelta);
                }
                skkbot2::log::info("step-grid sample: dt={:.6f} steps={} carry={:.7f} warp={:.3f} | "
                                   "levelTime +{:.4f}s/2400plans (planned {:.4f}s) | "
                                   "resume={} sess[plans={},steps={},resumeZero={}]",
                    dt, steps, *extraDelta, layer->m_gameState.m_timeWarp,
                    levelNow - engine->sessionSampleLastLevelTime,
                    // Steps are 1/tps seconds each, so the planned time is
                    // steps / tps. This was a literal 240.0 next to a real
                    // elapsed time in the same sentence: at 480 tps the
                    // "planned" figure was twice the truth, at 60 tps a quarter
                    // of it, and the comparison this sample exists for was
                    // unreadable at every rate but the one the constant named.
                    static_cast<double>(engine->sessionSampleStepsWindow) /
                        std::max(1.0, engine->runtimeTickRate()),
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
          // Accounting for the per-tick hook chain. Timed (not just counted)
          // because CCScheduler::update fires once per our drive step, so two
          // clock reads here are noise next to what it measures.
          skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::SchedUpdate);
          // FrameStepper auto-pause-on-death is serviced HERE, at the very top of
        // the scheduler tick and before ANY target has been updated. It used to
        // run from the processCommands hook, which is INSIDE the physics loop:
        // unfreezing there calls setFrameStepEnabled(false) -> clears the queued
        // substep/macro queues and releases player buttons while GD is walking
        // exactly those queues, which is how the engine ended up jumping through
        // a null vtable inside GJBaseGameLayer::update.
        FrameStepper::get().updateAutoPause();

        // Engage for a PlayLayer attempt OR an editor playtest (TPS/Speed
        // control). Plain editor play with default timing falls through to
        // vanilla inside shouldUseCustomTiming (BUG-10 compensation stays
        // intact); build mode keeps the vanilla scheduler entirely.
        // The display pin does NOT need a level to exist.
        //
        // The pin is two things: the animation interval and GD's vsync flag.
        // Neither belongs to a level - they are how the game presents frames at
        // all - and the vsync toggle lives in GD's video options screen, which is
        // exactly where a user goes to look at it. Releasing the pin outside a
        // level therefore meant that turning FPS on in the menu and then opening
        // the settings to confirm it showed the game in its old state, with
        // nothing in the log: the release ran before sync() was ever reached.
        //
        // What DOES need a level is the step grid - there is nothing to step.
        // So the pin is taken up here, and the level check below stays put.
        auto* engine = ReplayEngine::get();
        skkbot2::fps::sync(engine->runtimeTickRate(), engine->gameSpeed);

        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        // Render capture: physics is stepped by the CCDirector::drawScene hook
        // (Renderer::drive -> m_pScheduler->update(1/fps)), but it still flows
        // through the step-grid machinery below so the simulation runs on the
        // macro's tps grid (1/tps substeps), exactly like the PLAY session the
        // macro was recorded on. The scheduler carry turns one 1/fps drive
        // call into floor((1/fps)/(1/tps)) batches of 1/tps.
        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            controller.resetVanillaState(*engine);
            setExpectedTicksPatchEnabled(false);
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        setExpectedTicksPatchEnabled(true);

        if (engine->tickStepping) {
            // The stepper drives CCScheduler::update by hand, one tick at a
            // time, so a pinned display rate buys it nothing and costs a busy-
            // wait on a rate nobody is asking for. Drop it for the duration.
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
            // The stepper drives CCScheduler::update by hand, one tick at a
            // time, so a pinned display rate buys it nothing and costs a busy-
            // wait on a rate nobody is asking for. Drop it for the duration.
            skkbot2::fps::release();
            auto* pl = PlayLayer::get();
            bool canStep = (pl && !pl->m_isPaused && !pl->m_hasCompletedLevel)
                        || (!pl && skk::gameplay::isEditorPlaytest());
            if (!canStep) {
                controller.resetVanillaState(*engine, &m_fields->extraDelta);
                setExpectedTicksPatchEnabled(false);
                GJBaseGameLayer::update(dt);
                return;
            }

            m_fields->extraDelta = 0.0;
            const double timestep = controller.fixedDelta(*engine);
            setExpectedTicks(1);
            engine->customTickPlanActive = true;
            skk::StepGridEnforcement enforcement(1, timestep);
            GJBaseGameLayer::update(static_cast<float>(timestep));
            engine->customTickPlanActive = false;
            return;
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
