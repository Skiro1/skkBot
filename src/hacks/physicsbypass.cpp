#include "skkBot.hpp"
#include "core/autosave_recorder.hpp"
#include "core/frame_stepper.hpp"
#include "core/hotpath.hpp"
#include "core/playback_fps.hpp"
#include "core/step_trace.hpp"
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

// BUG-80: shared level-entry freeze watcher, called from BOTH plan paths -
// the GJBaseGameLayer::update hook AND SimulationTimingController::
// applyCustomTickPlan. Vanilla entries freeze through update only; a bot
// session queued outside the level drives its plan frames through the custom
// tick planner, so without the second call site its entry window goes
// unwatched (proved 11:24: [ENTRY-FREEZE] missing on record.log while
// [START-WIN] counted 180 frozen plans). Defined after the anonymous
// namespace; prototype here because applyCustomTickPlan lives above it.
namespace skkbot2 {
void entryFreezeWatch(GJBaseGameLayer* layer);
void entryFreezeReset();
// FMOD channel dump "[id:sX@posMs]..." used by the watcher AND by the frozen
// gate's engagement line (applyCustomTickPlan runs early in this file).
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

    // Planner accumulator. Used to live in the modify's per-layer Fields; it
    // is file-global now so resetVanillaState (which has no m_fields) zeroes
    // it on every path back to vanilla, and resetSimulationTimingState zeroes
    // it on level resets. There is exactly one gameplay layer at a time.
    static double g_extraDelta = 0.0;

    // BUG-93: SKC sessions run a deterministic step grid. GD derives the per-
    // frame step count from the wall dt (count = round(dt*240/timeWarp)); the
    // wall-clock rework (BUG-66/70) made BOTH timing modes feed QPC jitter, so
    // the step-count sequence per frame is session-local noise. Physics per
    // STEP stayed bit-equal (positions proved it), but m_currentStep carries
    // the accumulated count and GD schedules object windows by ABSOLUTE step
    // number: a +-2 step offset between the record and the playback world
    // moves collision/contact decisions by one tick - the "stuck where it
    // should not" that every practice seam showed (The Yandere 2 f=12509:
    // rec airborne slope-attach, play standing on the same slope with the
    // same position). The record ran under speedhack 0.40x on top, which
    // diverged the per-frame carry pattern even further.
    //
    // So while a SKC session is live the frame feed is quantized on the fixed
    // grid (steps never change length), while the per-drive FEED time follows
    // the user's timing mode (ECHO PARITY, BUG-96 stage 5): Lock Delta
    // substitutes the learned display cadence (Echo's original lock-delta
    // shape), Real Time feeds the measured wall frame time. The macro header
    // records the record-side mode and loading it restores the GUI toggles,
    // so record and play sessions carry the same mode automatically.
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
        // The step carry lives ONLY in g_extraDelta now: the scheduler hook
        // performs ONE walk per drawn frame (see advanceNormalScheduler), the
        // layer hook accumulates the walk dt into g_extraDelta and plans whole
        // steps from it. A second carry in this struct would track the same
        // stream in a second place and the two could only disagree.

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
            float wallGapNow = -1.0f;
            // BUG-94 per-sample raw-dt accounting: this hook receives frame dt
            // from whatever drives CCScene (display, render capture, stepper) -
            // record every non-zero arrival's magnitude in the sample window.
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
            // BUG-96: same drive, the honest wall time. A dt far above the
            // wall gap is a phantom (the drive claims time that did not pass).
            {
                double wallNowSk = skkbot2::hot::now();
                // BUG-96: same drive, the honest wall time. A dt far above the
                // wall gap is a phantom (the drive claims time that did not
                // pass). The first drive after session start has no previous
                // wall to compare against - seed the clock, do not measure.
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
                    // Phantom audit: dt far above the gap this drive claims.
                    if (rawDt > 0.0f && static_cast<double>(rawDt) >
                            static_cast<double>(gf) * 1.25 + 0.002) {
                        engine.samplePhantomCount += 1;
                        engine.samplePhantomDtSum += static_cast<double>(rawDt);
                    }
                    engine.lastDriveWall = wallNowSk;
                    wallGapNow = gf;
                }
            }
            // Fixed-step grid (determinism): record and playback sessions must
            // feed the game the IDENTICAL substep sequence regardless of the
            // display fps. A 240fps record ran 1/240 substeps while a faster
            // playback ran fractional substeps - same tick labels, different
            // physics integration, so portals and orb windows missed and
            // per-frame pins turned into visible teleports. Accumulate raw
            // frame time (gameSpeed-scaled, as before) and emit whole 1/tps
            // batches; the double-precision remainder rides the carry instead
            // of reaching the game as fractional substeps. Mirrors renderer.
            const double step = fixedDelta(engine);
            if (!(step > 0.0) || !(rawDt >= 0.0f)) {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                scheduler->CCScheduler::update(speedDelta(engine, rawDt));
                return;
            }
            // The display-FPS pin was synced once at the top of the scheduler
            // hook. The per-step budget (fps::stepCap) that used to live here
            // belonged to the per-step loop, which is gone (see below): the
            // batch planner derives its own stall cap (lockStepCap in
            // applyCustomTickPlan) from the frame time and the timestep.

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
            // Echo updater.cpp (lines 332-348):
            //   float speed = render_active() ? 1.f : speedhack;
            //   float frame_dt = dt;
            //   if (!rendering) frame_dt = (float)refresh_frame_time(frame_dt);
            //   if (!rendering && !real_time_mode && frame_dt > 0.25f) frame_dt = 0.25f;
            //   if (!rendering && lock_delta) frame_dt = (float)CCDirector::sharedDirector()->getAnimationInterval();
            //   frame_dt *= speed;
            const auto& gui = skkbot2::gui::get();
            // BUG-93: a live SKC session still runs the deterministic grid -
            // the mode no longer changes THAT, only the feed.
            const bool skcForce = skcDeterministicGrid();
            if (skcForce) {
                logDeterministicGridToast();
            }
            // ECHO PARITY (BUG-96 stage 5, user request): the deterministic
            // grid used to hard-override the GUI timing row into Lock Delta.
            // Now a live SKC session honours the user's choice, exactly like
            // Echo's two modes:
            //   * LOCK DELTA  - the frame time is replaced with the learned
            //     display cadence (Echo: getAnimationInterval). The session
            //     pace follows the constant, so slow frames SLOW the level -
            //     that is the mode's contract (stage 6: the tempo-debt feed
            //     violated it and was removed, commit 27f1e5f).
            //   * REAL TIME   - the level eats the measured frame time,
            //     wall-bounded. Pace is always 1.0x (times the speedhack).
            // In both modes physics still runs on the SAME 1/tps step grid
            // (steps never change length) - only the feed rate differs, so
            // whole-step integration is untouched. The SKC macro header
            // records which mode it was recorded with, and loading the macro
            // restores the GUI toggles (skc_bot.cpp), so record and play
            // sessions carry the same mode automatically.
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
            // Pacing-deficit instrument (stage 3): the feed's stages are
            // measured separately so the loss can be NAMED, not guessed.
            const double rawDtForAudit = static_cast<double>(rawDt);
            double dwmDtForAudit = static_cast<double>(frame_dt);
#if defined(GEODE_IS_WINDOWS)
            // The SKC grid must not be smoothed through DWM ticks either:
            // refresh_frame_time rounds to the display cadence and can flip a
            // frame into the neighbouring tick - the substitution below is the
            // ONLY smoothing a deterministic session may take.
            if (!rendering && !skcForce) {
                frame_dt = static_cast<float>(skkbot2::timing::refresh_frame_time(frame_dt));
                dwmDtForAudit = static_cast<double>(frame_dt);
            }
#endif
            // Pacing-deficit instrument: the DWM stage's contribution.
            engine.sampleDwmSum += dwmDtForAudit;
            engine.sampleDrives += 1;
            if (std::abs(dwmDtForAudit - rawDtForAudit) > 1e-6) {
                engine.sampleDwmRewrites += 1;
            }
            if (!rendering && !realTimeActive && frame_dt > 0.25f) {
                frame_dt = 0.25f;
            }
            // REAL TIME (SKK and SKC alike), PACING FIX: the feed must carry
            // the REAL time that passed, or the level runs slow.
            //
            // The DWM-tick smoothing (refresh_frame_time) rounds the frame
            // time to whole COMPOSITION ticks. That rounding is a DISPLAY
            // nicety (it keeps the on-screen motion even), but the level
            // clock must not inherit it: the measured session lost 4 steps
            // out of 480 (1.9866s fed vs 2.001s of wall - 0.8%), because the
            // rounded dt summed to LESS than the real elapsed time and the
            // wall clamp (frameTime > wallGap -> frameTime = wallGap) never
            // gives time BACK - it only refuses time that never passed.
            //
            // The RT contract is "the level eats the MEASURED frame time"
            // (Echo updater.cpp:333-346) - so in RT the honest wall gap
            // between the previous drive and this one IS the frame time.
            // The DWM-smoothed value stays as the DISPLAY's notion of the
            // frame (it is what the game renders with), but the LEVEL's
            // clock is fed the real gap, so the pace is exactly 1.0x wall.
            //
            // Lock Delta is untouched: its contract is the opposite (the
            // level deliberately runs at the substituted constant), and the
            // deterministic SKC grid is untouched (it never smoothed).
            //
            // SCOPE: the SKC path keeps its own feed. SKC's record/play
            // equality is bit-wise (BUG-93), and its grid already never
            // smooths - changing its feed would break the contract that
            // whole SKC pipeline rests on. The pacing defect was measured on
            // the SKK path (DWM smoothing active there), so the fix lands
            // there.
            if (!rendering && realTimeActive && !skcForce &&
                wallGapNow > 0.0f) {
                // The 0.25s cap is Echo's own (updater.cpp:342) and it keeps
                // a stall (alt-tab, breakpoint, level load) from
                // fast-forwarding the level by the whole stall in one frame.
                // The lost remainder is real elapsed time nobody simulated -
                // the level simply starts from where the frame ended.
                frame_dt = std::min(wallGapNow, 0.25f);
            }

            // SPEEDHACK COMPOSITION (Echo formula, updater.cpp:653): the
            // mode decides the frame time, the speedhack multiplies it ONCE
            // at the dispatch below (speedDelta = frameTime * gameSpeed). So
            // SPEED is master over both timing modes:
            //   * Lock Delta + speed 0.4x -> the level advances at 0.4x wall
            //     (measured: the 02:55 record, dtRate exactly 40%), and the
            //     lighter per-frame load can make the display deliver MORE
            //     frames - then the level pace acknowledges nothing and just
            //     rides the multiplier; if the display chokes instead, the
            //     level slows further - the Lock Delta contract, faithfully
            //     scaled by the speedhack.
            //   * Real Time + speed s -> the level advances at exactly s x
            //     wall: measured frame time times s. Whatever the fps does
            //     (fast or drowned), pace stays s; fps "pays" for it - the
            //     Real Time contract, scaled by the speedhack.
            // The wall clamp below compares the PRE-speed frame time with
            // the wall gap, so it never fights the speedhack: it only
            // refuses a frame that claims more naked time than passed
            // (phantom drives, BUG-96); feeding speed x wall remains legal
            // at any speed.
            // BUG-93 (SKC): a live SKC session quantizes the carry on a
            // fixed grid, and m_currentStep (GD schedules object windows by
            // ABSOLUTE step number) must repeat bit-equal from the
            // recording session to the playback session.
            //
            // BUG-93a (and the fix-of-the-fix BUG-95): the substituted
            // constant must be the display's ACTUAL cadence. The BUG-93a
            // first take substituted the nominal getAnimationInterval gated
            // by a rawDt-vs-interval verdict, and the 00:12 session probed
            // exactly why that fails: getAnimationInterval promises 1/240
            // while the V-Sync'd display honestly delivers 1/180 - the
            // verdict was off-cadence forever, the grid stayed
            // undeterministic, and the earlier un-gated run accelerated
            // 3.37x. So now we LEARN the cadence: a window of measured
            // frame dts, its median snapped to an integer fps, and that
            // snapped constant is the substitution. The snapping makes the
            // constant reproducible: record and play sessions on the same
            // monitor snap to the same integer rate -> the grid repeats
            // bit-wise. A mid-session cadence change (cap change, window
            // drag between monitors) re-learns within one window and logs
            // the re-anchor.
            float frameTime;
            double promisedInterval = 0.0;
            bool skcConstSubst = false;
            {
                constexpr double kNoFrame = 0.0;
                auto pin = rawDt > 0.0f ? static_cast<double>(rawDt) : kNoFrame;
                // Keep the rolling off-cadence metrics (BUG-93a fields, they
                // mirror into applyCustomTickPlan and the sample log).
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
                // BUG-95 learn. Raw parked dts are outliers in every real
                // session (verse hitches, window drags), so a sorted-order
                // statistic is the robust read. BUG-95a: the median dragged
                // the anchor into the slow cluster whenever hitches filled
                // half the window and the level audibly wobbled; the 25th
                // percentile sits in the fast frame cluster through a stall.
                // Snapping to an integer fps makes the constant reproducible:
                // record and play sessions on the same display snap to the
                // same rate; without the snap a 0.1% difference between
                // sessions could flip a rare step-boundary frame.
                engine.lockLearnsWindow.push_back(static_cast<float>(pin));
                constexpr size_t kLearnWindow = 240;
                static std::vector<float> learnSorted;
                // BUG-96 stage 8: do NOT learn during a render - the render
                // drives arrive at the fast-forward pace (hundreds of fake
                // frames per wall second), and learning them would re-anchor
                // the deterministic grid on a fantasy cadence that the next
                // live session would then substitute.
                if (!rendering && engine.lockLearnsWindow.size() >= kLearnWindow && pin > 0.0) {
                    learnSorted = engine.lockLearnsWindow;
                    std::sort(learnSorted.begin(), learnSorted.end());
                    const size_t q1 = (learnSorted.size() - 1) / 4;
                    const size_t q3 = learnSorted.size() - 1 - q1;
                    const float p25 = learnSorted[q1];
                    const float p75 = learnSorted[q3];
                    // BUG-95b: only windows where the distribution itself is
                    // ONE tight frame length may teach the anchor. The 00:53
                    // session anchored on 5.435ms (184 fps) by learning a
                    // warm-up/mixed window while the honest display cadence
                    // was 5.556ms (180 fps) - a 2% rate error baked into
                    // every later frame. A clean window is one where at
                    // least 80% of frames sit within +-2% of the p25
                    // candidate: pure display cadence, nothing else.
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
                        // Quiet discard: refill and try the next window.
                        engine.lockLearnsWindow.clear();
                    }
                    // BUG-95a hysteresis: the anchor only moves when a
                    // different candidate is CONFIRMED - a >10% gap from the
                    // current anchor repeated across two consecutive windows.
                    // One hitchy window no longer re-anchors the grid, and
                    // the 180<->158 flap (a ~12% gap, i.e. inside tolerance)
                    // is refused outright.
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
                        // BUG-95b: tolerance narrowed to 1%. The wider 10%
                        // tolerance let the warm-up-learned 184 fps anchor
                        // become permanent; with the clean-window gate above,
                        // only honest clusters reach this point, so a 1%
                        // divergence between two confirmed windows is a real
                        // re-anchor, not noise.
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
                    // BUG-96 stage 8: RENDER DRIVES feed VERBATIM. The render
                    // clock (Renderer::driveOneFrame) asks for exactly N/tps
                    // per video frame - the raw dt the stepOne lambda passes
                    // IS the sim time the video timeline needs. No Lock
                    // Delta constant (it would replace the asked dt), no
                    // Real-Time clamp (a fast-forward drive "claims" more
                    // naked time than its wall gap by design), and the
                    // speedhack multiplier is applied HERE ASK NOTHING: the
                    // render clock already bakes mSpeed into the tick count,
                    // so the feed must not be scaled again (a double
                    // multiply).
                    promisedInterval = 0.0;
                } else if (realTimeActive) {
                    // ECHO PARITY, Real Time (SKC and SKK alike): the
                    // measured frame time feeds the level verbatim
                    // (wall-clamped below) - no interval substitution, no
                    // tempo debt, pace is 1.0x by construction. The 1/tps
                    // step grid still quantizes the carry into whole steps,
                    // so whole-step integration is untouched.
                    promisedInterval = 0.0;
                } else if (skkbot2::fps::macroHolds() &&
                           skkbot2::fps::enabled() &&
                           skkbot2::fps::target() > 0.0) {
                    // STORED-RATE LOCK (BUG-107, 04.11.2026): a macro that
                    // carries a pinned display rate feeds Lock Delta with
                    // exactly 1/storedRate - the SAME constant the record
                    // session was pinned to - instead of a per-session
                    // re-learning of the cadence. The live sessions proved
                    // why: at a 180 pin the record world held 180 (learned
                    // 5.556ms, 99% clean) while playback under level load
                    // delivered 156-164 fps, so the learner snapped a
                    // DIFFERENT constant and the batch patterns diverged
                    // (flips +-1..3). The stored rate removes the lottery:
                    // the feed sequence is bit-identical by construction for
                    // any stored rate, integer ratio or not; wall-clock pace
                    // may slow on slow frames, which is the mode's contract.
                    // Learning stays for macros without a rate (Absent) and
                    // for follow TPS (target < 0), where there is nothing
                    // stored to lock onto.
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
                    // ECHO PARITY, Lock Delta (SKC and SKK alike, BUG-96
                    // stage 5): the record and playback headers both store
                    // the macro's timing mode, so substituting the LEARNED
                    // display cadence (not the nominal interval, BUG-95)
                    // makes the two sessions on the same display snap to
                    // the same constant - a deterministic step sequence,
                    // fed slower on slow frames (the mode's contract).
                    promisedInterval =
                        static_cast<double>(engine.lockLearnedInterval);
                } else {
                    promisedInterval = interval;
                }
            }
            if (promisedInterval > 0.0) {
                frameTime = static_cast<float>(promisedInterval);
                // BUG-96 stage 5: the tempo-debt feed below only applies
                // when a CONSTANT was actually substituted (Lock Delta).
                // Real Time already feeds the measured wall time - a debt
                // there would double-feed.
                skcConstSubst = true;
            } else {
                frameTime = frame_dt;
            }
            // BUG-96 wall clamp holds in every regime: a drive cannot owe
            // more sim time than REALLY passed since the previous drive.
            // The 00:58/01:09 sessions measured the honest stream itself
            // feeding ~2.4x wall - faster-than-wall SIMULATIONS are
            // structurally impossible while the steady regime (constant <
            // gap) is completely untouched.
            // The render feed is the asked dt verbatim - the wall clamp only
            // governs honest display streams (BUG-96); a fast-forward render
            // drive legitimately carries more sim time than its wall gap.
            if (!rendering && wallGapNow > 0.0f && frameTime > wallGapNow) {
                frameTime = wallGapNow;
            }
            // Pacing-deficit instrument: the clamp stage's contribution and
            // what the planner actually received (frameTime is what the
            // scheduler walk carries; the planner sees it inside the layer
            // hook as the walk's dt).
            engine.sampleClampSum += static_cast<double>(frameTime);
            engine.sampleFedSum += static_cast<double>(frameTime);
            engine.lockDroppedCarry += static_cast<double>(rawDt) -
                static_cast<double>(frameTime);
            engine.lockMaxDtSkew = std::max(
                engine.lockMaxDtSkew,
                std::abs(static_cast<double>(rawDt) -
                         static_cast<double>(frameTime)));

            // ONE scheduler walk per drawn frame - the Echo shape (updater.cpp
            // run_steps 150-165 + EchoScheduler hooks.cpp:2267-2273), ported
            // to our structure where the LAYER hook owns the planning:
            //
            //   * The walk dispatches every scheduled selector once with the
            //     (gameSpeed-scaled) frame time - the vanilla dispatch shape.
            //     The proven per-step drive walked the whole scheduler table
            //     once per PHYSICS step (3414 times a second at tps 3414),
            //     including GJBaseGameLayer::update's full preamble (QPC
            //     bookkeeping, the flag-path virtual call at
            //     sub_140237850:205-206) - that preamble is per-UPDATE work in
            //     vanilla (once per frame, count=4 steps inside), and running
            //     it per step was a large share of the 1-2.3 ms/step cost.
            //   * Inside that walk the layer hook lands in
            //     applyCustomTickPlan, which quantizes the frame time into
            //     whole 1/tps steps from its own carry (g_extraDelta), patches
            //     GD's step count to `steps` (expected-ticks patch) and calls
            //     the base update ONCE with steps * timestep.
            //   * getModifiedDelta (bottom of this file) returns
            //     fixedPhysicsDelta(warp) * steps, so GD's own division
            //     (sub_140237850:248: v16 = v7 / v14, in double) recovers the
            //     exact float step no matter how large the batch is -
            //     record == play bit-for-bit by construction.
            //
            // Why the two earlier batch attempts failed, so this is not
            // repeated (docs/Р‘Р°РіРё.md BUG-67 / BUG-68):
            //   * BUG-67: expected-ticks patch AND step-grid midhooks armed
            //     together = steps^2 physics iterations per plan. The patch
            //     sets the OUTER loop count at 0x237A5B, the midhook seeds the
            //     INNER loop at 0x237DCE - they are alternatives, not
            //     complements. This path arms NEITHER midhook (no
            //     StepGridEnforcement); the inner loop keeps vanilla's
            //     half-tick seeding, which is Echo behaviour.
            //   * BUG-68: the plan was driven from the scheduler hook with the
            //     layer dispatch deferred, while getModifiedDelta still
            //     returned a single substep - GD integrated batchDt / count
            //     with the wrong numerator (dt collapsed, clock raced). Here
            //     the plan is driven from the layer hook itself (the proven
            //     entry point) and getModifiedDelta returns the whole batch.
            skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
            // ECHO CONTRACT, stage 6 (user's clarification): LOCK DELTA
            // trades LEVEL SPEED for steady steps - every frame feeds the
            // constant, slow frames slow the LEVEL, and nothing ever "catches
            // back up". The stage-3 tempo-debt feed (returning the lost wall
            // time into the planner carry, commit 51f882b) VIOLATED that
            // contract - it turned Lock Delta into a stuttering quasi-Real-
            // Time, which is exactly the "both modes speed-wibble" the user
            // rejected. Removed. REAL TIME is the mirror contract: pace of
            // the level is always 1.0x wall (the measured feed), whatever
            // the frame rate does - it lives purely on the frameTime =
            // frame_dt branch. The wall clamp alone stays in every mode: it
            // only refuses a stream that claims MORE time than really
            // passed (phantom drives, BUG-96), it never lends time back.
            // The speedhack multiplies live feeds once (Echo formula). The
            // render drive is NOT multiplied: the render clock already put
            // mSpeed into the asked tick count (Renderer::driveOneFrame) and
            // speedDelta would scale the level twice.
            scheduler->CCScheduler::update(rendering
                                               ? frameTime
                                               : speedDelta(engine, frameTime));
        }

        void resetVanillaState(ReplayEngine& engine) {
            // Every path back to vanilla scheduling goes through here, so this
            // is where a pinned display rate is handed back. Leaking a 30 fps
            // pin into ordinary play would be far worse than never pinning, and
            // there are four callers that all reach vanilla the same way.
            skkbot2::fps::release();
            setExpectedTicks(0);
            engine.tickAccumulator = 0.0f;
            engine.recentlyInitialized = false;
            // The planner accumulator is file-global now (the direct drive
            // plans from the scheduler hook), so every vanilla handback
            // zeroes it - not just the callers that used to pass their own
            // Fields pointer down.
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

    // Plan-vs-clocks consistency probe (28.10, written for the batch attempts,
    // kept because it is the instrument that caught BUG-68). Three clocks must
    // agree over any window of plans:
    //   steps      - what the planner served,
    //   FrameClock - processCommands fires once per physics step, so the clock
    //                must advance by exactly `steps` per window,
    //   levelTime  - GD's own clock, must advance by steps * timestep
    //                (= totalDelta) per window.
    // In the per-step drive a healthy window reads steps=1200 dFC=1200 and
    // dlvl == expected; any mismatch names the clock that moved. Resume-fade
    // frames (dt=0 substeps) legitimately advance levelTime by less than
    // expected - the resumeZero counter in the step-grid sample separates
    // that case.
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
        // inUpdate: how much levelTime THIS plan's update() itself added.
        // dlvl - inUpdate over a window is then the deficit that happened
        // BETWEEN plans (resets, stray writes) rather than inside update.
        probeLvlInUpdate += layer->m_gameState.m_levelTime - levelBefore;
        // 100, not 600: a session at tps 3414 makes 457 plans in ~8 s of play
        // (the 22.10 run), so a 600-plan window NEVER printed. 100 plans is
        // ~0.5 s of wall at 167 fps and ~1.7 s at 58 fps - several windows
        // per short session, enough to see the deficit move between them.
        if (probeWin % 1000 == 0) {
            const uint32_t fc = skk::skc::FrameClock::get().getFrame();
            const double lvl = layer->m_gameState.m_levelTime;
            const uint32_t dFc = fc - probeFc;
            const double dLvl = lvl - probeLvl;
            // A level restart inside the window makes both the frame clock
            // and levelTime jump backwards; re-arm instead of printing a
            // meaningless negative window.
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
        // BUG-80: a bot session queued outside the level reaches its plan
        // frames through THIS path, not the GJBaseGameLayer::update hook -
        // so run the shared [ENTRY-FREEZE] watcher here too, else the entry
        // window (and the song channel's paused->playing flip) goes unlogged
        // exactly where the "music plays before the run" bug lives.
        skkbot2::entryFreezeWatch(layer);
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
        // The substitution itself happens one call up, in
        // advanceNormalScheduler, where the MEASURED FRAME TIME exists. This
        // function now runs once per drawn frame (ONE scheduler walk), so
        // `dt` here is the walk's frame time - but the substitution still
        // belongs above: the historic bug below is why. When this function
        // ran once per 1/tps scheduler batch, comparing dt against the
        // display interval measured the batch grid, not the display, and
        // substituting 1/fps for it multiplied the clock by the number of
        // batches a frame contains. The long comment that used to justify
        // the substitution here described the right behaviour in the wrong
        // place - see advanceNormalScheduler for what replaced it.
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

        // FROZEN GATE (BUG-70, 4th run measured): the batch can be eaten
        // BEFORE the physics loop by two gates, and then the plan's steps
        // vanish without a single processCommands call (zero physics, zero
        // levelTime, no FrameClock):
        //   12888 (0x3258) == 0 - pause-family flag, freezes every such frame;
        //   12964 (0x32A4) >= 2 - post-resume grace; >= 2 because this very
        //     update's own getModifiedDelta call decrements it first, so a
        //     plan-time 1 survives and freezes on the following frame.
        // Read raw: neither offset has a Geode binding.
        //
        // The old planner promised AND subtracted the plan's steps from the
        // carry before update() froze the whole batch - so every frozen frame
        // became permanent debt, the debt grew the next plan into a
        // mega-batch, and the level ran slow-motion forever after. A frozen
        // frame is a PAUSE, not a debt: nobody owes the wall time it took.
        // Drop the carry to zero, promise nothing, and hand the frame to
        // vanilla update(dt) - the freeze path advances the scene, triggers
        // and trigger-time, but not physics or levelTime. When the pause
        // lifts, the first plan starts from a zero carry: no backlog, no
        // mega-batches, no slow-motion spiral. Measured on the 4th run:
        // the whole deficit was the ~1.36 s of frozen frames at level start;
        // every window after the first two was already exact.
        //
        // m_resumeTimer joins the gate for the same loss class: resume-fade
        // frames used to return 0.0 from getModifiedDelta AFTER the planner
        // had subtracted the batch - the same steps-lost-without-a-loop
        // shape, just one frame later. The decrement moves here from the
        // customTickPlanActive branch below (it only ran when a plan was
        // active; a gated frame never reaches it).
        {
            const auto layerBase = reinterpret_cast<uintptr_t>(layer);
            const uint8_t frozenFlag =
                *reinterpret_cast<const uint8_t*>(layerBase + 12888);
            const int32_t freezeCtr =
                *reinterpret_cast<const int32_t*>(layerBase + 12964);
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
                    // BUG-80 NOTE: the level song is prepared audibly BEFORE
                    // this freeze (vanilla loadMusic + setMusicTimeMS at
                    // entry). Any muting experiments here (song-gate v1/v2)
                    // were removed by user request - the music stays 100%
                    // vanilla; the [SONG]/[ENTRY-FREEZE] traces remain for
                    // diagnostics only.
                }
                const double warp = std::clamp(
                    static_cast<double>(layer->m_gameState.m_timeWarp), 0.0, 1.0);
                const double step = engine->fixedPhysicsDelta(warp);
                // What the plan WOULD have promised - audit only, the gate
                // itself promises nothing.
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
                    // BUG-80 FIX (12:02 evidence): DO NOT decay GD's resume
                    // counters here. The 12:02 afterGD bracket pinned the
                    // song's early s2->s1 release INSIDE the vanilla update
                    // call of plan #2 - exactly the frame this decrement
                    // pushed m_resumeTimer 1.0 -> 0.0. Vanilla holds BOTH
                    // resume counters frozen for the whole entry window
                    // (grace=2 from t=0 to the byte lift, 11:32) and releases
                    // the song only when the pause flag lifts - our per-plan
                    // decay (165 fps cadence) collapsed it in 2 plans and GD
                    // treated the entry as a completed resume mid-freeze.
                    // GD owns and decays these counters itself at the lift;
                    // nothing to do here but keep freezing on the pause flag.
                }
                // The pause is not a debt: nobody owes this wall time.
                engine->sampleFreezeDropped += *extraDelta;
                *extraDelta = 0.0;
                engine->tickAccumulator = 0.0f;
                // Minimal session accounting so the step-grid sample and the
                // epilogue see the plan happened. dtSum gets the real dt (it
                // measures the dt stream fed, frozen or not); the levelTime
                // line in the sample shows what the freeze did not convert.
                // EMA cadence is NOT touched: a frozen frame's dt is pause
                // time, and feeding it back re-creates the BUG-66 budget
                // loop from the pause side.
                ++engine->sessionPlanCount;
                engine->sessionLastLevelTime = layer->m_gameState.m_levelTime;
                if (!engine->sessionLevelTimeInit) {
                    engine->sessionLevelTimeInit = true;
                    engine->sessionFirstLevelTime = layer->m_gameState.m_levelTime;
                    engine->sessionSampleLastLevelTime = layer->m_gameState.m_levelTime;
                }
                engine->sessionSampleDtSum += frameDt;
                // wallStart belongs to dtSum: if the session OPENS on frozen
                // plans, the sample's wall window must start here too, or the
                // first sample compares dtSum (which already carries the
                // frozen dt) against a wall clock that started later -
                // dtRate inflated past 100% as a pure accounting artifact
                // (run 5, sample 1: dtSum 3.31 vs wall 2.20 -> dtRate 150%).
                if (engine->sessionSampleWallStart <= 0.0) {
                    engine->sessionSampleWallStart = skkbot2::hot::now();
                }
                // Vanilla semantics: update(dt) with the count patched to 0
                // runs the freeze path itself - zero physics regardless of
                // which gate fired, scene and triggers alive.
                setExpectedTicks(0);
                layer->GJBaseGameLayer::update(dt);
                return;
            }
            // First UNFROZEN plan of the session: if the gate ate frames at
            // attempt start, print the wall cost of that window - the number
            // to compare against the "song started early" report.
            if (engine->startFrozenReported && !engine->startLiveReported) {
                engine->startLiveReported = true;
                // Which gate ate the window: batFrozenFlag = pause-family
                // (0x3258==0), batFreezeCtr = post-resume grace (0x32A4>=2),
                // sessionResumeZero = resume fade (m_resumeTimer).
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
        // Use the same once-rounded float quantum GD integrates. The scheduler
        // carry stays double, but step counting must use the actual physics
        // period rather than the ideal 1/TPS period.
        double timestep = engine->fixedPhysicsDelta(timeWarp);

        TickStepPlanner planner;
        planner.accumulated = *extraDelta;
        int64_t wantedSteps = static_cast<int64_t>(planner.planSteps(timestep));
        // BUG-96 stage-3 audit: what the planner was OFFERED, vs what the
        // tempo-debt feed claims it put in (printed side by side in the
        // step-grid sample as `feed` vs `want`).
        engine->sampleWantSum += static_cast<double>(wantedSteps) * timestep;
        // Measure per-step cost for adaptive cap (Echo-style affordable_steps).
        static double emaStepCost = 0.0;
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
        // The cadence the cap is derived from - the PROMISED frame time, never
        // the dt currently in hand. A stall frame's dt carries the whole
        // accumulated backlog, and a cap derived from it grows with the debt
        // it is supposed to refuse (BUG-70, measured: worst frame wanted 3799
        // steps = 1.11 s of sim in ONE batch, cap-hits 0 - the cap rose to meet
        // the debt, the frame ate seconds of CPU, and the display collapsed to
        // single digits: the "fps drops to 0" the run showed).
        //
        // lockLocked/lockInterval/lockHolds mirror advanceNormalScheduler's
        // decision word for word - that hook runs once per drawn frame, this
        // runs once per layer update inside the walk, and the promise both
        // compute must be the same one.
        // When the pin is being honoured the promise is the interval; when it
        // is not, the promise is the smoothed MEASURED cadence (emaFrameTime).
        const auto& gui = skkbot2::gui::get();
        // BUG-93: the forced SKC grid counts as Lock Delta here too, so the
        // promise the plan is built on is the interval, not the measured dt
        // (mirrors the substitution in advanceNormalScheduler).
        const bool skcForce = skcDeterministicGrid();
        // ECHO PARITY (BUG-96 stage 5): a live SKC session honours the same
        // GUI mode advanceNormalScheduler honours - Lock Delta promises the
        // substituted constant, Real Time promises the measured frame. The
        // promise and the actual feed must stay the same number or the
        // steady-state cap drifts from reality.
        const bool skcRealTime =
            skcForce && gui.realTime && !gui.lockDelta;
        const bool lockDeltaActive =
            (skcForce && !skcRealTime) ||
            (!skcForce && gui.lockDelta && !gui.realTime);
        const bool realTimeActive = skcRealTime;

        const bool lockLocked = skcForce ||
            !skkbot2::render::Renderer::get().isRecording() &&
            (skkbot2::fps::macroHolds() || skkbot2::fps::enabled() || lockDeltaActive);
        const double lockInterval = static_cast<double>(
            cocos2d::CCDirector::sharedDirector()->getAnimationInterval());
        // BUG-95 gate mirror: the forced SKC grid promises the LEARNED display
        // cadence (the same median-snapped constant advanceNormalScheduler
        // substitutes), not the nominal animation interval - the probes showed
        // the two diverge (1/180 delivered vs 1/240 promised) and a promise
        // built on the nominal value planned a rate the frames never carry.
        // Until the first learn window fills, the promise is the measured
        // frame (warm-up, no substitution anywhere).
        double skcCadence = static_cast<double>(engine->lockLearnedInterval);
        if (skcForce && skcCadence <= 0.0) {
            skcCadence = static_cast<double>(frameDt);
        }
        // ECHO PARITY (stage 5), the promise mirrors the actual feed: Lock
        // Delta (SKC and SKK) is fed the LEARNED cadence when one exists -
        // an old nominal-interval promise (pre-learn) diverged from the
        // delivered reality (1/240 promised vs 1/180 fed, BUG-95).
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
        // BUG-96 stage 8: a render drive promises the RAW ASKED dt, exactly
        // the verbatim feed advanceNormalScheduler delivers (no mode
        // substitution, no speed multiply - the render clock owns both).
        if (skkbot2::render::Renderer::get().isRecording()) {
            promisedFrame = static_cast<double>(frameDt);
        }
        const double steadyState = timestep > 0.0 ? promisedFrame / timestep : 1.0;

        // Step capping logic:
        // Echo updater.cpp (unlocked_step_cap & count_unlocked_steps):
        //   - In Echo's lock_delta: NO capping by affordability.
        //     steps = wantedSteps. Simulation advances strictly by frame interval.
        //   - In Echo's Real Time: cap = min(unlocked_step_cap(), affordable)
        //     and leftover_time capped at REAL_TIME_BACKLOG (0.10s).
        //
        // Ours (since the wall-clock rework, 31.10.2026) diverges deliberately:
        // BOTH modes feed the same smoothed wall time and stay uncapped in the
        // timing dimension - Lock Delta never caps, Real Time never caps, carry
        // is always kept. Echo's backlog reset was removed (BUG-66/70: a cap on
        // the backlog raced itself). ONLY the default mode (neither toggle on)
        // caps, and the cap exists to refuse a STALL backlog, not the steady
        // state: see the historical note above.
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
            // No debt truncation: the leftover carry is kept so the level catches
            // up (real time preserved). Only the per-frame step cap (above) bounds
            // how much we serve in one frame.
            (void)backlog;
        }
        // Cadence EMA from NON-stall frames only. A stall frame's dt is our
        // own serving time; feeding it back into the cadence is exactly the
        // self-reinforcing budget loop BUG-66 documented.
        if (steps >= wantedSteps) {
            if (engine->emaFrameTime <= 0.0) {
                engine->emaFrameTime = frameDt;
            } else {
                engine->emaFrameTime +=
                    0.1 * (static_cast<double>(frameDt) - engine->emaFrameTime);
            }
        }
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

        // Autosave while recording: throttled rescue snapshots of the growing
        // SKK/SKC macro (Settings -> MACRO BOT -> Autosave). The tick itself
        // is two atomic loads + one chrono read when nothing is due, and the
        // snapshot/serialize/write all happen off this thread.
        skkbot2::autosave::tickRecording();

#ifdef GEODE_IS_WINDOWS
        // One-shot: which step-grid mechanism this session actually uses.
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
            // TIME-based, not plan-count-based (2.0 s of wall). The old gate
            // (every 2400 plans) assumed ~1 ms per plan; with the batch drive
            // a plan is one DRAWN FRAME, and a machine that cannot hold the
            // pinned rate delivers 100 of them a second - 2400 plans was
            // 24 s, longer than the BUG-69 session lasted, so no sample ever
            // printed. The window sums the delivered dt stream as well: if
            // dtSum per wall second is 1.0, the game was fed real time; if it
            // is the pinned-interval ratio (e.g. 0.8), the dt stream itself
            // lost the missing time and that is where the slow motion lives.
            engine->sessionSampleStepsWindow += steps;
            engine->sessionSampleDtSum += frameDt;
            const double wallNow = skkbot2::hot::now();
            if (engine->sessionSampleWallStart <= 0.0) {
                engine->sessionSampleWallStart = wallNow;
            } else if (wallNow - engine->sessionSampleWallStart >= 2.0) {
                const double wall = wallNow - engine->sessionSampleWallStart;
                const double levelNow = layer->m_gameState.m_levelTime;
                // BUG-97 instrument: in an SKC record session a DYNAMIC
                // speed change is legal - the user primes hard sections at
                // 0.4x and flips to 1.0x freely (that is how reference bots
                // behave). Until the SKC speed track lands, every change is
                // logged once so a divergent playback can be correlated
                // with the record's speed switches by frame.
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
                    // BUG-96 pacing deficit: the 2s WALL window straddles the
                    // feed stream's own boundaries, so wall itself can lose
                    // up to one frame of time no honest stream can cover -
                    // dtRate2 divides the fd dt by the SUM of the gaps the
                    // same frames delivered (same plans, same window edges),
                    // removing that sampler artifact entirely.
                    "wallGap[min={:.5f} avg={:.5f} max={:.5f} sum={:.3f}s] "
                    "dtRate2={:.1f}% "
                    // Pacing-deficit stage 3: the feed's layers, side by side.
                    // rawSum = what GD reported, dwmSum = after the display-tick
                    // smoothing, clampSum = after the wall clamp, fedSum = what
                    // the planner received. A gap between two of them names the
                    // stage that ate the time.
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
                    // BUG-96 verdict aid: a 0.x dt-rate that is not a level
                    // slowdown is usually the SPEEDHACK multiplier feeding
                    // less per drive; printing it next to warp ends the
                    // guesswork (the 02:55 record read 40% because the
                    // speedhack was at 0.40x, not because Lock Delta broke).
                    engine->gameSpeed,
                    levelNow - engine->sessionSampleLastLevelTime,
                    // Steps are 1/tps seconds each, so the planned time is
                    // steps / tps. This was a literal 240.0 next to a real
                    // elapsed time in the same sentence: at 480 tps the
                    // "planned" figure was twice the truth, at 60 tps a quarter
                    // of it, and the comparison this sample exists for was
                    // unreadable at every rate but the one the constant named.
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

        // Step-grid enforcement (silicate midhooks) is NOT armed here.
        //
        // BUG-67 root cause, proven against the decompile: the expected-ticks
        // patch (count store, 0x237A5B) and the physStepCount midhook (inner
        // loop seed, 0x237DCE) BOTH force the step count - the patch sets the
        // outer loop's [rbp+0xB0], the midhook seeds the inner loop's
        // [rbp+0xA0] - and applying both at once ran steps^2 physics
        // iterations per plan with full timestep each. They are alternatives.
        // Echo uses the patch alone (install_physics_step_patch,
        // hooks.cpp:2551) and leaves the inner loop seeded by vanilla, which
        // also restores half-tick splitting of steps whose window contains a
        // queued button (sub_140237850:303-334). getModifiedDelta (bottom of
        // this file) returns the batch, so GD's own division recovers the
        // exact float step per iteration. The only remaining consumer of the
        // midhooks is the frame stepper, where steps == 1 and both mechanisms
        // agree.
#endif

        // THE batch path (Echo-exact, see the advanceNormalScheduler comment
        // for the full scheme): ONE update(totalDelta) per plan with
        // expectedTicks=steps and getModifiedDelta returning the batch. ARM
        // has no expected-ticks patch and keeps its explicit per-step loop
        // (patch semantics are unavailable there).
        // Capture levelTime BEFORE the update call: batProbe runs right after
        // it, so layer time now minus this capture is what update itself
        // wrote - separating an in-update deficit from one between plans.
        const double planLevelBefore = layer->m_gameState.m_levelTime;
        // The freeze-flag capture that used to live here moved UP into the
        // frozen gate at the top of this function: it promises nothing and
        // returns before the planner, so by the time the batch path runs the
        // gates are already known-clear (frozenFlag != 0 && freezeCtr < 2 &&
        // m_resumeTimer <= 0) and re-reading them here could only ever print
        // zeros.
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
        // Zero the patched count once the plan is served. Between plans the
        // patch stays ENABLED (custom timing is on), and a stale count of 57
        // applied to any update() call we did not plan (another mod driving
        // the layer by hand) would run 57 full physics steps; count 0 runs
        // none, which is the honest answer for a call we did not plan.
        setExpectedTicks(0);
#endif
        batProbe(layer, steps, totalDelta, planLevelBefore, engine);
    }}

void resetSimulationTimingState() {
    auto& controller = timingController();
    setExpectedTicks(0);
    // The planner accumulator is file-global; a level reset must not carry
    // a half-plan of debt into the next attempt.
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
          // Accounting for the per-tick hook chain. Timed (not just counted)
          // because CCScheduler::update fires once per our drive step, so two
          // clock reads here are noise next to what it measures.
          skkbot2::hot::Timed skkbot2Timing(skkbot2::hot::SchedUpdate);
          // One tick of the visual-frame identity used by the playback
          // visual throttles (activateStreak / orbs circles / spawn effects):
          // first caller inside this frame runs, the rest early-return.
          skkbot2::hot::bumpVisualFrame();
          // The step-trace flag cache: the per-SUBSTEP hooks (processCommands
          // path) read a plain bool instead of paying a geode getSavedValue
          // JSON lookup up to 3414 times a second. Refreshed once per drawn
          // frame here - the one hook that runs in every mode.
          skkbot2::step_trace::refreshCache();
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
            ++engine->sessionVanillaExits;
            ++engine->sampleVanillaExits;
            controller.resetVanillaState(*engine);
            setExpectedTicksPatchEnabled(false);
            {
                skkbot2::hot::Timed skkbot2GdBase(skkbot2::hot::SchedVanilla);
                return CCScheduler::update(dt);
            }
        }

        // One walk = one entry into the custom-timing path of this hook. The
        // first instrumented run showed 1310 of these against 401 plans and
        // nothing in the log split them apart; this is that split.
        ++engine->sessionSchedWalks;

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

namespace skkbot2 {
// FMOD channel dump for the [ENTRY-FREEZE] watcher: "[id:sX@posMs]" per live
// channel handle; s0=stopped s1=playing s2=paused. Mirrors the [SONG] dump in
// render_hooks.cpp but callable without a music API call at that instant.
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

// The level-entry freeze watcher itself, shared by both update paths. State
// is file-scope so entryFreezeReset() can disarm it from level exit and
// session arming.
//
// PTR-REUSE TRAP (fixed after 11:32): GD's PlayLayer died and its replacement
// landed on the SAME heap address the previous (vanilla) entry already
// watched - that watch had ended with done=true, so the bot entry took the
// "same layer" branch and the s_done gate swallowed every log line. The 11:24
// and 11:32 bot entries both lost [ENTRY-FREEZE] this way (not a separate
// plan path after all). entryFreezeReset() unarms the state on level exit and
// on session arming - both precede any new allocation that can reuse the
// address - so a queued-session entry rearms from scratch.
namespace entry_state {
    const void* g_entryLayer = nullptr;
    double g_entryWall = 0.0;
    int g_prevPause = -1;
    bool g_done = false;
    double g_lastChanDump = -1.0;
}  // namespace entry_state

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
        static_cast<int>(*reinterpret_cast<const uint8_t*>(base + 12888));
    const int grace = static_cast<int>(
        *reinterpret_cast<const int32_t*>(base + 12964));
    const double since = hot::now() - entry_state::g_entryWall;
    if (since >= 8.0) {
        // Give up: entry watcher is only for the start window.
        entry_state::g_done = true;
        return;
    }
    if (pause != entry_state::g_prevPause) {
        entry_state::g_prevPause = pause;
        log::info(
            "[ENTRY-FREEZE] pauseFlag={} grace={} t={:.3f}s",
            pause, grace, since);
    }
    // Channel dump every ~0.25 s until live: catches the song channel
    // flipping paused (s2) -> playing (s1) even when no music API call
    // happens at that instant - the unpause sits OUTSIDE the traced hooks on
    // bot entries (11:24: s2@0 at 43.322, s1@50615 by 45.302).
    if (since - entry_state::g_lastChanDump > 0.25) {
        entry_state::g_lastChanDump = since;
        log::info(
            "[ENTRY-FREEZE] t={:.3f}s chans={}", since, fmodChannelDump());
    }
    // Live semantics = the frozen-gate's: pause byte set, grace over and
    // resume fade done.
    if (pause != 0 && grace < 2 && layer->m_resumeTimer <= 0.0f) {
        entry_state::g_done = true;
        log::info(
            "[ENTRY-FREEZE] physics LIVE at {:.3f}s from entry (pause={} "
            "grace={} resume={}) chans={}",
            since, pause, grace,
            static_cast<int>(layer->m_resumeTimer), fmodChannelDump());
    }
}

// Disarm the watcher state so the next entry always rearms from scratch; a
// killed PlayLayer's replacement can land on the same heap address and the
// watcher must NOT treat it as the watched entry (see PTR-REUSE TRAP).
void entryFreezeReset() {
    entry_state::g_entryLayer = nullptr;
    entry_state::g_entryWall = 0.0;
    entry_state::g_prevPause = -1;
    entry_state::g_done = false;
    entry_state::g_lastChanDump = -1.0;
}
}  // namespace skkbot2

class $modify(PhysicsControlLayer, GJBaseGameLayer) {
    // BUG-80 [ENTRY-FREEZE] watcher: logic lives in skkbot2::entryFreezeWatch
    // (below) so the bot-entry path (SimulationTimingController::
    // applyCustomTickPlan) traces the same window.
    void update(float dt) {
        skkbot2::entryFreezeWatch(this);
        // BUG-102 v3: батч-контекст подшагов - сброс на границе кадра дисплея,
        // до любого пути планировщика (см. src/skc/skc_batch_probe.hpp).
        skkbot2::skcprobe::beginFrame(static_cast<double>(dt));
        auto* engine = ReplayEngine::get();
        if (engine && engine->customTickPlanActive) {
            return GJBaseGameLayer::update(dt);
        }

        if (!PlayLayer::get() && !skk::gameplay::isEditorPlaytest()) {
            return GJBaseGameLayer::update(dt);
        }

        // Render capture: physics runs via the drawScene hook's own stepping,
        // but through the shared step-grid path below (see
        // SpeedControlScheduler::update) so substeps integrate on the macro's
        // 1/tps grid instead of the vanilla 240-Hz one.
        auto& controller = timingController();

        if (!shouldUseCustomTiming(*engine)) {
            controller.resetVanillaState(*engine);
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
                // BUG-80 FIX (same as the frozen gate): while the pause flag
                // (0x3258) is held 0 by GD's ENTRY window, this is NOT a
                // resume - it is the level-entry freeze. Vanilla never decays
                // the resume counters in that window (grace=2 from entry to
                // the byte lift) and only releases the song at the lift. Our
                // per-plan decay here collapsed m_resumeTimer in 2 plans and
                // GD released the music ~0.97 s before physics went live.
                // Freeze the frame (no physics) but leave GD's counters
                // alone; GD collapses and releases everything itself when
                // the pause flag lifts.
                const uint8_t pauseFlag =
                    *reinterpret_cast<const uint8_t*>(
                        reinterpret_cast<uintptr_t>(this) + 12888);
                if (pauseFlag != 1) {
                    return 0.0;
                }
                --m_resumeTimer;
                ++engine->sessionResumeZeroPlans;
                return 0.0;
            }
            // Echo getModifiedDelta (hooks.cpp:1026-1044): the batch handed to
            // GD is ONE float step times the planned count. update() divides
            // it back out in double (sub_140237850:248, v16 = v7 / v14), so
            // every physics step integrates exactly fixedPhysicsDelta(warp) -
            // the same quantum the proven per-step drive fed - regardless of
            // the batch size. (double)(float)step * count is exact in double
            // (a 24-bit mantissa times a <=2^24 count fits in 53 bits), so
            // the division recovers the float step with ZERO rounding, and
            // record == play bit-for-bit.
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
