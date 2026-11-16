#pragma once

// ECH2 reader - imports macros written by the Echo bot (decks.echo).
//
// This reader was recovered from the decompiled binary
// (GDBOTS\echo\decks.echo\decks.echo.dll) and checked field-by-field against the
// bot's own JSON export (converted\*.echo.json). Echo's source is public now
// (GDBOTS\echo-beta-main\src\Logic\conversions\Echo.cpp, v2.2.1), so the
// constants below can be checked against it directly - they are kept as
// measured, because that is what the real files were verified with.
//
//   "ECH2"                     4 bytes
//   uint8                      version, 3 or 4 (4 only adds an fps-change table)
//   uint8                      flags - bit 2 seed present, bit 0 debug present
//   [rates]     gamma          only when version == 4
//   float32                    fps, Elias-gamma coded as an integer of its bits
//   float32                    endX, coded the same way
//   float64                    recordingSeconds, coded as a 64-bit integer
//   gamma                      attempts
//   gamma                      input count
//   input stream               range-coded, see below
//
// THE BODY IS ENTROPY CODED, not stored. There is no checksum anywhere - the
// writer only flushes and trims trailing zero bytes (sub_1800F55D0:39-47) - so a
// wrong model does not announce itself, it just returns plausible frames. Every
// constant below was pinned against real files rather than inferred.
//
// Each input is a packed 64-bit word, (16 * frameDelta) | (down + 2 *
// (button - 1 + 3 * player2)), and the words are reconstructed by an adaptive
// binary model in this order:
//
//   1. three context-tree bits giving the button and the player (sub_1800F36E0
//      :263-280), decoded through an SVM against contexts at 1920 / 7680 / 8160;
//   2. one context bit for press-or-release (:304);
//   3. a six-bit prefix tree, then length - 1 value bits (sub_1800F7910), the
//      first four of them against the per-delta contexts at 48464 / 101200 /
//      2210640 and the rest against the shared tails at 48592 / 101328 / 2210768;
//   4. optionally a dictionary back-reference bit (:161-242).
//
// "SVM" here is a small learned linear predictor: three counters index a 4096
// entry ramp, their values are dotted with four weights, and the result is mapped
// through that ramp to a probability in [1, 4095]. The counters move toward 0 or
// 0xffffffff by a fraction of a move-counter each time the slot is seen, which
// means the whole 4.7 MB model has to be materialised - there is no shortcut.
//
// TWO QUIRKS OF THE ORIGINAL ARE FAITHFULLY REPRODUCED, because the encoder has
// the same two and would desynchronise otherwise. The SSE block at
// sub_1800F74A0:179-185 decompiles into what looks like a cyclic pairing between
// the weights and the base values; it is not, and brute-forcing all six
// permutations settles it - the identity pairing decodes 238/238 and the
// "cyclic" reading decodes two records before collapsing. And the move-counter
// cap is written two ways, `< 0x1e` on the first context and `<= 0x1d` on the
// other two, which are the same bound; reading the second as `<` caps at 29
// instead of 30 and shifts every probability by one part in fifty million, which
// is invisible for sixty-odd records and then flips a bit.
//
// THE SHIFT IS ZERO, MEASURED - a shape-based prediction of -1 was wrong.
//
// Echo hooks GJBaseGameLayer::processCommands and the body (sub_1800B3E80) runs
// the original first, then bumps its own frame counter at rec+328, then applies
// the macro's inputs:
//
//     *(BYTE *)(rec + 341) = 1;
//     sub_1803E8E00(...);                      // the real processCommands
//     *(BYTE *)(rec + 341) = 0;
//     rec[328] += 1;
//     sub_180120140(rec, rec[328]);            // the inputs
//
// sub_1803E8E00 resolves base + 2333792, the same address the hook was installed
// on, so it is unambiguously the vanilla function. That reads as
// drain-then-queue - inputs landing after the physics they affect - which is
// GDH's RE4 shape, the one that needs minus one. RE4 and Echo were then
// predicted to agree.
//
// They do not. RAUCHKAMMER 2 runs at zero and dies at minus one, the exact
// opposite of RE4. So the post-physics ordering above is not what decides this,
// and the extrapolation from two data points was the mistake. Echo also hooks
// processQueuedButtons (sub_1800B1630), which was never examined, and the block
// that dispatches is gated on several conditions - v20 being non-zero, m_
// currentProgress at +12964 being non-positive, a flag at +12874 being clear -
// so "inputs after processCommands" never reduced to one bit of evidence about
// when the click lands.
//
// Two points calibrated this heuristic - TTRL at zero, RE4 at minus one - and
// three do not. The tracker now records Echo as measured, not inferred.
//
// The file's frames are separately already zero-based: the recorder counts from
// one (sub_180123150:280) and every exporter subtracts one before writing
// (sub_1800DB130:216-232).
//
// Unlike RE4 and TTR3 there is no physics in this format at all, so the import
// carries no warning.

#include "skc/skc_format.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace skk::echo {

inline constexpr size_t kMagicSize = 6;
inline constexpr uint8_t kMinVersion = 3;
inline constexpr uint8_t kMaxVersion = 4;

inline constexpr uint8_t kFlagSamples = 1;
inline constexpr uint8_t kFlagWholeFps = 2;
inline constexpr uint8_t kFlagSeed = 4;
inline constexpr uint8_t kFlagUnrounded = 8;
inline constexpr uint8_t kFlagSilicateVariance = 16;
inline constexpr uint8_t kFlagLdmKnown = 32;
inline constexpr uint8_t kFlagLdm = 64;

inline constexpr size_t kMaxActions = 1u * 1024u * 1024u;
// Echo.cpp: MAX_RATE_CHANGES = 1 << 16.
inline constexpr uint64_t kMaxRateChanges = 1ull << 16;

struct Rate {
    uint32_t frame = 0;
    float fps = 0.0f;
};

struct Action {
    uint32_t frame = 0;
    uint8_t button = 1;  // 1 = Jump, 2 = Left, 3 = Right
    bool pressed = false;
    bool player2 = false;
};

struct Replay {
    uint8_t version = 0;
    uint8_t flags = 0;
    float tps = 240.0f;
    float endX = 0.0f;
    double recordingSeconds = 0.0;
    uint64_t attempts = 0;
    bool hasSeed = false;
    uint64_t seed = 0;
    std::vector<Rate> rates;
    std::vector<Action> actions;
    // Always empty. Kept so the loader can treat every format the same way.
    std::string caution;
};

[[nodiscard]] bool looksLikeEcho(std::span<const uint8_t> data);

[[nodiscard]] bool parse(std::span<const uint8_t> data, Replay& out,
                         std::string& outReason);

[[nodiscard]] bool toSkcMacro(const Replay& replay, skc::Macro& out,
                              std::string& outReason);

[[nodiscard]] bool importFile(const std::filesystem::path& path, skc::Macro& out,
                              Replay* outInfo, std::string& outReason);

}  // namespace skk::echo
