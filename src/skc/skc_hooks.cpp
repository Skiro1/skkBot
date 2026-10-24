#include "skc_bot.hpp"
#include "skc_checkpoint.hpp"
#include "skc_frame.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EditorUI.hpp>

#include "logger/logger.hpp"

using namespace geode::prelude;

namespace skk::skc {

namespace {
// Drift-probe grid, in frames. Coarse enough to keep the log readable, fine
// enough to name the exact frame where a divergence starts.
constexpr uint32_t kProbeInterval = 10;
} // namespace

// ---------------------------------------------------------------------------
// PlayLayer — attempt lifecycle.
//
// resetLevel covers both a death respawn and a manual restart; GD calls it
// either way, and `m_checkpointArray->count()` distinguishes them. A full
// restart rewinds the clock and (when not playing back) clears the macro; a
// respawn from a checkpoint keeps the recorded past and only rewinds the
// cursor.
// ---------------------------------------------------------------------------
class $modify(SkcPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        auto& bot = Bot::get();

        // A session bound to a different level must not survive into this one
        // (same rule as the SKK PlayLayer::init hook's sameLevel check).
        bot.onLevelEnter(level);

        // A queued RECORD/PLAY from outside the level fires now, exactly like
        // ReplayEngine::consumePendingAction in the SKK PlayLayer::init hook.
        // init runs before the level's first physics step, so the clock starts
        // at 0 and the macro plays from the very beginning.
        const bool hadPending = bot.getPendingAction() != PendingAction::None;
        if (hadPending) {
            bot.consumePendingAction(level);
            if (bot.isRecording()) {
                bot.bindLevel(level);
            }
        }

        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        auto& bot = Bot::get();
        auto& fix = PracticeFix::get();

        // Re-assert Vanilla accuracy on EVERY reset, not just at session start.
        // A practice respawn is a resetLevel, and GD re-reads the CBS/COS
        // settings here - so a session that only forced them in startRecording /
        // startPlayback would resume recording on half-steps after the first
        // death. SKC labels every click with a count of processCommands calls,
        // so one half-step desynchronises the whole tail of the macro from that
        // point on. EclipseMenu re-applies its settings from resetLevel for the
        // same reason (Bot.cpp:341).
        if (bot.ownsTiming()) {
            Bot::forceVanillaAccuracy();
        }

        // SKK's wipe rule is `!inPractice || tick <= 1 || restorePoints.empty()`
        // (checkpoint_system.cpp:1622-1647). SKC used to key off "no checkpoint
        // array" alone, which missed the `tick <= 1` half.
        const int tick = m_gameState.m_currentProgress;
        const bool inPractice = m_isPracticeMode != 0;
        const bool hasSnapshots = fix.getSnapshotCount() > 0;
        const bool stitch = inPractice && tick > 1 && hasSnapshots &&
                            m_checkpointArray && m_checkpointArray->count() > 0;

        skkbot2::log::debug(
            "[SKC] resetLevel: inPractice={} tick={} snapshots={} -> {}",
            inPractice, tick, hasSnapshots,
            stitch ? "practice respawn (stitch)"
                   : "attempt from start (wipe)");

        if (!stitch) {
            FrameClock::get().reset();
            fix.clear();

            // An armed PLAY takes over on the next attempt without a teleport:
            // the level just restarted, so start the macro from frame 0.
            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            // Deletion policy lives in ONE place now. An idle or playing bot
            // still must not lose a macro it merely loaded - onResetEvent
            // decides that, and the reason says why.
            if (bot.isRecording()) {
                bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
            } else {
                bot.restart();
            }
        } else {
            // Respawn from a practice checkpoint: the clock rewinds to the
            // frame the checkpoint was captured at and the cursor follows.
            if (bot.isPlaybackArmed()) {
                bot.startPlayback();
            }
            const uint32_t cpFrame = fix.lastCheckpointFrame();
            skkbot2::log::info(
                "[SKC] practice respawn -> frame {} (checkpoints held: {}, "
                "recorded inputs: {}, mode: {})",
                cpFrame, fix.getSnapshotCount(), bot.getInputCount(),
                bot.isRecording() ? "RECORD" :
                (bot.isPlaying() ? "PLAY" : "idle"));
            // Vanilla restore is exact - no frame correction. See onResetEvent.
            FrameClock::get().setFrame(cpFrame);

            if (bot.isRecording()) {
                // The attempt that just died already recorded inputs past the
                // checkpoint - a future the player just undid. Cut it AND close
                // the dangling holds. Doing only half of it is what let the
                // retry interleave with a future that never happens.
                bot.onResetEvent(Bot::ResetReason::PracticeRespawn, cpFrame);
            } else {
                bot.seekTo(cpFrame);
            }
        }

        PlayLayer::resetLevel();
    }

    void fullReset() {
        auto& bot = Bot::get();
        skkbot2::log::debug(
            "[SKC] fullReset: clock->0, wiping {} snapshot(s), inputs={}, "
            "mode={}", PracticeFix::get().getSnapshotCount(),
            bot.getInputCount(), bot.isRecording() ? "RECORD"
                : (bot.isPlaying() ? "PLAY" : "idle"));
        FrameClock::get().reset();
        PracticeFix::get().clear();
        if (bot.isPlaybackArmed()) {
            bot.startPlayback();
        } else if (bot.isRecording()) {
            bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
        } else {
            bot.restart();
        }
        PlayLayer::fullReset();
    }

    // Platformer levels restart through this instead of resetLevel, and our own
    // GUI calls it directly for platformer playback - without a hook the frame
    // clock and the cursor would survive into the new attempt.
    void resetLevelFromStart() {
        auto& bot = Bot::get();
        FrameClock::get().reset();
        PracticeFix::get().clear();
        if (bot.isPlaybackArmed()) {
            bot.startPlayback();
        } else if (bot.isRecording()) {
            bot.onResetEvent(Bot::ResetReason::AttemptFromStart, 0);
        } else {
            bot.restart();
        }
        PlayLayer::resetLevelFromStart();
    }

    void delayedResetLevel() {
        auto& bot = Bot::get();
        // SKK deliberately does NOT touch the recorded stream here
        // (checkpoint_system.cpp:2007-2014) - delayedResetLevel is only a
        // simulation-timing reset.
        //
        // SKC used to zero the FrameClock and wipe every practice snapshot
        // here. In practice mode GD fires this MID-RUN, so a live recording
        // suddenly had clock=0 while its action stream still held events at
        // frame ~445. Every subsequent input was then rejected as
        // out-of-order, and the next checkpoint snapshot came from a clock
        // that no longer matched the level - the macro ended up a shredded
        // mix of two timelines. Neither the clock nor the snapshots may move
        // unless the same SKK wipe rule (resetLevel) says so.
        const bool wipe = !bot.isEnabled() || bot.isPlaybackArmed();
        skkbot2::log::debug(
            "[SKC] delayedResetLevel: inPractice={} tick={} snapshots={} "
            "inputs={} mode={} -> {}",
            m_isPracticeMode != 0, m_gameState.m_currentProgress,
            PracticeFix::get().getSnapshotCount(), bot.getInputCount(),
            bot.isRecording() ? "RECORD" : (bot.isPlaying() ? "PLAY" : "idle"),
            wipe ? "wipe clock+snapshots" : "KEEP clock+snapshots");
        if (wipe) {
            FrameClock::get().reset();
            PracticeFix::get().clear();
        }
        if (!bot.isEnabled() && !bot.isPlaybackArmed()) {
            bot.restart();
        }
        PlayLayer::delayedResetLevel();
    }

    // Completion ends the session in BOTH modes, same as SKK's levelComplete:
    // a finished RECORD must not stay armed past the win screen (the next
    // retry's restart-clear would silently wipe the finished timeline), and a
    // finished PLAY must hand control back.
    void levelComplete() {
        auto& bot = Bot::get();
        const bool wasPlaying = bot.isPlaying();
        const bool wasRecording = bot.isRecording();

        PlayLayer::levelComplete();

        if (wasPlaying || wasRecording) {
            bot.stop();
            if (wasRecording) {
                skkbot2::Logger::get().setSessionMode(
                    skkbot2::Logger::SessionMode::None);
            }
            skkbot2::log::info(
                "[SKC] level complete - session ended (mode={})",
                wasPlaying ? "PLAY" : "RECORD");
        }
    }

    void onQuit() {
        auto& bot = Bot::get();
        if (bot.isEnabled() || bot.isPlaybackArmed()) {
            bot.onLevelExit();
        }
        PracticeFix::get().clear();
        FrameClock::get().reset();
        PlayLayer::onQuit();
    }

    // --- practice ---------------------------------------------------------

    CheckpointObject* markCheckpoint() {
        auto& bot = Bot::get();
        // A checkpoint placed on a corpse captures a dead player; restoring to
        // it later respawns straight into the death animation. Skip it, same as
        // EclipseMenu and silicate do.
        if (bot.isRecording() && m_player1 && m_player2 &&
            (m_player1->m_isDead || m_player2->m_isDead)) {
            return nullptr;
        }
        return PlayLayer::markCheckpoint();
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();
        // Snapshot the full state while the physics is still live, so
        // loadFromCheckpoint has something complete to restore from.
        if (bot.isRecording() && checkpoint) {
            const uint32_t frame = FrameClock::get().getFrame();
            PracticeFix::get().capture(frame, this, checkpoint);
            // Position on capture. The probe grid is every 10 frames, but a
            // checkpoint lands wherever the player placed it, so without this
            // the state the stitch restores to is invisible at the exact
            // boundary - and that boundary is where the residual y drift
            // starts.
            const CCPoint pos = m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
            skkbot2::log::debug(
                "[SKC] checkpoint stored at frame {} (progress={}) pos=({:.3f},{:.3f}) "
                "rot={:.2f} - now {} snapshot(s)", frame,
                checkpoint->m_gameState.m_currentProgress / 2,
                static_cast<double>(pos.x), static_cast<double>(pos.y),
                m_player1 ? static_cast<double>(m_player1->getRotation()) : 0.0,
                PracticeFix::get().getSnapshotCount());
        }
        PlayLayer::storeCheckpoint(checkpoint);
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        auto& bot = Bot::get();

        PlayLayer::loadFromCheckpoint(checkpoint);

        // Re-pin what vanilla restore dropped. Must run AFTER the original so
        // we overwrite, not get overwritten. apply() reports the FrameClock
        // frame of the matched snapshot, which is also the correct trim bound:
        // the future the player just undid must not stay in the macro, and
        // loading an OLD checkpoint has to cut back to that checkpoint, not to
        // the most recent one.
        if (bot.isEnabled()) {
            if (auto appliedFrame = PracticeFix::get().apply(this, checkpoint)) {
                const CCPoint rpos =
                    m_player1 ? m_player1->getPosition() : CCPoint{0, 0};
                skkbot2::log::debug(
                    "[SKC] checkpoint restored to frame {} pos=({:.3f},{:.3f}) "
                    "rot={:.2f} (recording={}, playing={})", *appliedFrame,
                    static_cast<double>(rpos.x), static_cast<double>(rpos.y),
                    m_player1 ? static_cast<double>(m_player1->getRotation()) : 0.0,
                    bot.isRecording(), bot.isPlaying());
                // Stitch at the matched snapshot's frame - NOT
                // lastCheckpointFrame(): loading an OLD checkpoint must cut
                // back to THAT checkpoint, not to the most recent one.
                //
                // On a practice DEATH this also fires right after the
                // resetLevel branch did the same thing; the second pass finds
                // nothing left to cut, which is why the "dropped N" count is
                // logged by whichever ran first.
                if (bot.isRecording()) {
                    bot.onResetEvent(Bot::ResetReason::CheckpointRestore,
                                     *appliedFrame);
                    // Vanilla restore is exact, so the retry continues on the
                    // same frame grid - no correction. See onResetEvent.
                    FrameClock::get().setFrame(*appliedFrame);
                    // GD flushes buffered presses and stale releases right
                    // after the restore; keep them out of the macro stream.
                    bot.noteInternalRestore(*appliedFrame);
                    bot.seekTo(*appliedFrame);
                } else {
                    bot.seekTo(*appliedFrame);
                }
            }
        }
    }

    // --- death ------------------------------------------------------------

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& bot = Bot::get();

        // SKK uses this hook for its persistence death-chain, which SKC does not
        // have (the format stores no physics). SKC only needs to explain the
        // restart: without a chain the macro simply replays from frame 0 on the
        // next attempt, so a silent death looks like "the bot forgot to click".
        const bool playerOne = player == m_player1;

        if (bot.isRecording() && playerOne) {
            // resetLevel() will wipe the timeline a moment later; log it at the
            // death itself so the frame the run failed on is never lost.
            bot.noteAttemptReset(FrameClock::get().getFrame());
        } else if (bot.isPlaying() && playerOne && bot.getInputIndex() > 0) {
            skkbot2::log::info(
                "[SKC] playback: player died on frame {} after consuming "
                "{}/{} inputs - SKC has no death chain, the macro will replay "
                "from frame 0",
                FrameClock::get().getFrame(), bot.getInputIndex(),
                bot.getInputCount());
        }

        PlayLayer::destroyPlayer(player, object);
    }

    // --- pause ------------------------------------------------------------

    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);

        auto& bot = Bot::get();
        if (!bot.isRecording())
            return;

        // RobTop calls releaseAllButtons on pause, which never goes through
        // handleButton — so the recording would keep a hold that GD itself
        // released. Record the releases explicitly, one frame ahead, exactly
        // like EclipseMenu does. Without this a paused hold replays stuck.
        const bool swap = GameManager::get()->getGameVariable("0010");
        const uint32_t frame = FrameClock::get().getFrame() + 1;
        for (int button = 1; button <= 3; ++button) {
            bot.recordInput(frame, static_cast<uint8_t>(button), swap, false);
        }
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer — the clock, the recording tap and the playback inject.
// ---------------------------------------------------------------------------
class $modify(SkcBaseGameLayer, GJBaseGameLayer) {
    struct Fields {
        // True while vanilla processQueuedButtons is draining OUR commands, so
        // the handleButton gate lets the macro's own clicks through instead of
        // swallowing them as if they were human input.
        bool macroDispatch = false;
    };

    // processCommands is called exactly once per physics step, which makes it
    // the cheapest reliable tick source. The SKK step clock uses the same
    // signal; using it here keeps both bots on the same grid.
    //
    // It also drives the clock the playback cursor is compared against:
    // vanilla calls processQueuedButtons from INSIDE this function (GD 2.2081
    // RVA 0x239C60 -> 0x239D51), so dispatching there happens at exactly the
    // same point inside the step where a human press is applied.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        FrameClock::get().tick();
    }

    // Playback goes through the game's OWN input queue, exactly like
    // silicate does (silicate-main/src/hooks/GJBaseGameLayer.cpp:535-540:
    // poll -> queueButton -> THEN the base processQueuedButtons drains it).
    //
    // This is the single biggest accuracy difference between SKC and the
    // reference bots. A human press travels
    // UILayer -> m_queuedButtons -> processQueuedButtons -> handleButton.
    // Calling handleButton directly - which is what SKC used to do - skips the
    // queue, so anything that INSPECTS m_queuedButtons instead of the hold map
    // (dash-ring activation, orbs reacting to queued input) behaves differently
    // during playback than it did during recording, and the run drifts.
    //
    // silicate is input-only too and pins no physics: this canonical path IS
    // its accuracy mechanism.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto& bot = Bot::get();
        const bool playing = bot.isPlaying();

        if (playing) {
            // Human input must not interleave with the macro. Silicate does the
            // same (GJBaseGameLayer.cpp:498-500) before pushing its own. Skipped
            // when the user turned "Ignore Inputs" off - that toggle exists to
            // let them interleave deliberately.
            if (bot.getIgnoreInputs()) {
                m_queuedButtons.clear();
            }

            const uint32_t frame = FrameClock::get().getFrame();
            uint32_t dispatched = 0;
            while (auto opt = bot.poll(frame)) {
                PlayerButtonCommand command = {};
                command.m_button = static_cast<PlayerButton>(opt->button);
                command.m_isPush = opt->pressed;
                command.m_isPlayer2 = opt->player2;
                command.m_step = 0;
                command.m_timestamp = 0.0;
                m_queuedButtons.push_back(command);
                ++dispatched;
            }
            if (dispatched) {
                skkbot2::log::debug("[SKC] playback frame {}: queued {} click(s)",
                                    frame, dispatched);
            }
        }

        // Drift probe. An input-only replay is exact only if the physics ends
        // up in the same state every run; this prints the live position on a
        // fixed frame grid so attempts can be diffed. It runs during RECORDING
        // too - comparing a recording's probe line against the playback of the
        // same frame is the only direct test of "does playback match what was
        // recorded", and playback-vs-playback only ever proves determinism.
        //
        // 10 frames, not 120. At the coarser grid the residual y difference
        // (1-4 px with dx exactly 0, so timing was already perfect) first shows
        // up somewhere between two probes and the causing frame is
        // unrecoverable. Velocity is included for the same reason: y alone does
        // not say whether a divergence is still growing or has settled.
        if ((playing || bot.isRecording()) && m_player1 &&
            FrameClock::get().getFrame() % kProbeInterval == 0) {
            const auto pos = m_player1->getPosition();
            skkbot2::log::info(
                "[SKC] probe{} f={} x={:.3f} y={:.3f} rot={:.2f} spd={:.4f} vx={:.4f}",
                playing ? "P" : "R", FrameClock::get().getFrame(),
                static_cast<double>(pos.x), static_cast<double>(pos.y),
                static_cast<double>(m_player1->getRotation()),
                static_cast<double>(m_player1->m_playerSpeed),
                static_cast<double>(m_player1->m_xVelocityRelated));
        }

        // Always drain vanilla's queue, playing or not - this is the game's own
        // input pump and skipping it would kill all input during a recording.
        m_fields->macroDispatch = playing;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        m_fields->macroDispatch = false;

        if (!playing)
            return;

        // All inputs dispatched and the player is still alive: hand control
        // back to vanilla GD (SKK's playbackExhaustionNotified rule), instead
        // of freezing the player with a held button.
        if (bot.isPlaybackExhausted() && m_player1 && !m_player1->m_isDead) {
            bot.onPlaybackExhausted();
        } else if (bot.isPlaybackExhausted()) {
            skkbot2::log::info("[SKC] playback: macro fully dispatched at frame {} "
                               "(waited for the player to respawn)",
                               FrameClock::get().getFrame());
        }
    }

    void handleButton(bool down, int button, bool player2) {
        auto& bot = Bot::get();
        // The macro's clicks reach us through m_queuedButtons, so while vanilla
        // drains them this flag is up and they must pass. Human input cannot
        // appear here during playback: processQueuedButtons clears the queue
        // before pushing the macro's commands.
        const bool fromMacro = m_fields->macroDispatch;

        if (!fromMacro && bot.isPlaying() && bot.getIgnoreInputs())
            return;

        // GD's handleButton third parameter is NOT "is player 2" - it is the
        // raw flag and it is INVERTED relative to its name. SKK's
        // deferredSlotForPlayer makes that explicit (replay_engine.cpp:200-202):
        //     return twoPlayerMode ? static_cast<int>(!player2) : 0;
        // and EclipseMenu does the same thing (Bot.cpp:532, `!player1`).
        //
        // SKC read the flag straight, so on a two-player level it filed the
        // second player's clicks under P1: measured 586/586 clicks recorded
        // with p2=false and zero with p2=true. Worse, the pairing groups by
        // (button, player2), so both players' presses landed in one channel and
        // every tap became an orphan.
        //
        // Slot 1 only when the level actually HAS a second player; a solo run
        // must stay all-P1 regardless of what the flag says.
        // Routing, and ONLY m_twoPlayerMode decides it - never m_isDualMode.
//
// Measured on Artemis 13, a level with dual and no second player: 70 clicks
// arrived as rawPlayer2=false with twoPlayerMode=false and isDualMode=true,
// and every one of them was filed as P2. Those clicks jumped during recording
// and do nothing on playback, so the macro lost them and died.
//
// This is exactly the shape of a bug we had already fixed once and then
// reintroduced from the wrong helper. SKK has TWO functions that look like the
// same question:
//     deferredSlotForPlayer  replay_engine.cpp:200 - routing, m_twoPlayerMode
//     keepSecondPlayerSlot   replay_engine.cpp:204 - something else, and this
//                                                      one DOES include dual
// SKC took the inversion from the first and the condition from the second, so
// dual switched the second channel on without a second player existing. On a
// real two-player level m_isDualMode adds nothing, which is why this went
// unnoticed until a dual level recorded a solo run.
const bool keepSecond = m_levelSettings && m_levelSettings->m_twoPlayerMode;
        const bool recordPlayer2 = keepSecond && !player2;

        GJBaseGameLayer::handleButton(down, button, player2);

        if (!fromMacro && bot.isRecording()) {
            // Every click filed to the second channel, with every input to the
            // decision. A dual-mode level turns keepSecond on without having a
            // second player, so this is where a solo run quietly grows a P2
            // stream - and a P2 press is not the same physics as a P1 press, so
            // the macro stops reproducing the moment one shows up.
            if (recordPlayer2) {
                skkbot2::log::warn(
                    "[SKC] record P2 press: frame={} down={} btn={} rawPlayer2={} "
                    "twoPlayerMode={} isDualMode={} keepSecond={}",
                    FrameClock::get().getFrame(), down, button, player2,
                    m_levelSettings ? m_levelSettings->m_twoPlayerMode : false,
                    m_gameState.m_isDualMode, keepSecond);
            }
            bot.recordInput(
                FrameClock::get().getFrame(),
                static_cast<uint8_t>(button),
                recordPlayer2,
                down
            );
        }
    }
};

// ---------------------------------------------------------------------------
// Editor playtest. RobTop does not reset m_currentProgress when entering a
// playtest, which would leave the clock running from the build mode; reset it
// so a recorded macro starts at frame 0 like in PlayLayer.
// ---------------------------------------------------------------------------
class $modify(SkcEditorUI, EditorUI) {
    void onPlaytest(CCObject* sender) {
        auto& bot = Bot::get();
        // An armed PLAY (queued from the editor's build mode) takes over when
        // the playtest physics begin, mirroring SKK's editor arming.
        if (auto* editor = LevelEditorLayer::get()) {
            if (editor->m_playbackMode == PlaybackMode::Not) {
                if (bot.isPlaybackArmed()) {
                    bot.startPlayback();
                }
                bot.restart();
                FrameClock::get().reset();
                editor->m_gameState.m_currentProgress = 0;
            }
        }
        EditorUI::onPlaytest(sender);
    }
};

// --- session bookkeeping ----------------------------------------------------

} // namespace skk::skc
