#pragma once

#include "skc_range.hpp"

#include <cstdint>
#include <vector>
#include <filesystem>
#include <optional>
#include <string>

namespace skk::skc {

// ---------------------------------------------------------------------------
// SKC — ultra-minimal, zero-physics macro format.
//
// Design goal: the smallest possible file that still replays deterministically
// on vanilla GD physics. Stores NOTHING but input events plus the one number GD
// needs to reproduce the run (the target tick rate).
//
// --- layout (current, written by encode()) ----------------------------------
//
//   [ 0]      uint8    knobs0   ctx:6 | deltaCodec:2
//   [ 1]      uint8    knobs1   holdCodec:2 | deltaKey:2 | holdKey:2 | scale:2
//   [ 2]      uint8    knobs2   deltaShift:2 | holdShift:2 | budget:2 |
//                              gapFrom:1 | hasTps:1
//   [ 3]      uint8    ext      ctx:6 | platformer:1 | reserved:1
//                              present only when ctx == 0x3F
//   [ 3/4..]  float    tps      exact float32, then float fps (legacy 8-byte tail)
//                       OR -0.0f marker, double tps, float fps (16-byte tail)
//   [ .. ]    payload          range-coded click stream (skc_range.cpp)
//
// So three bytes normally, four for a platformer macro, eleven/twelve when
// either rate is not the default. The two floats share one presence bit on
// purpose: there is no spare bit in the knobs, and a second bit would cost a
// byte on every file just to record the answer "no" that nearly all of them
// have to give.
//
// The platformer flag rides in an escape, not in a new header byte:
//
//   - ctx == 0x3F is never a real context count. The encoder's candidates use
//     0/6/16/24/48, so no .skc written before this flag existed can contain
//     63. A platformer macro writes ctx = 0x3F and puts the real context count
//     plus the mode bit in the extra byte. A classic macro is written exactly
//     as before - byte-identical to every existing .skc - and "no extension"
//     therefore means "mode unknown", not "classic".
//   - The cost is one byte, and only on macros that need it. A classic macro
//     that is played on a platformer level is merely wrong-looking (it will
//     jump and nothing else); a platformer macro played on a classic level is
//     the case worth refusing, and that is the one that gets the flag.
//
// There is no magic. It was two bytes that every file paid and nothing needed:
// the loader is reached by extension, not by sniffing, and a file that is not
// ours still has to survive knob validation before a single click is decoded -
// every knob is either a bounded enumerator or an index into a table whose size
// the reader checks. What replaced the magic is that validation, which is the
// only thing the magic was doing in the first place.
//
// The knobs themselves, which is all the header is:
//
//   ctx        how many independent models the gap/hold streams were written
//              with; 0 means one model each. The encoder tries one setting and
//              three context counts and keeps whichever is smaller, because a
//              context split is a large win on a macro built from loop blocks,
//              whose click stream repeats itself, and a small loss on a
//              hand-played one, whose stream does not - and the click count
//              cannot tell those apart (a 500-click prefix of a looped macro
//              wins, a 537-click hand-played macro loses). Measuring beats
//              guessing.
//   deltaCodec how the gap between clicks is coded: 0 splits the magnitude
//   holdCodec  into (bucket, offset), 1/2/3 code the small values directly out
//              of one adaptive row of 32/64/128 before a tail takes over.
//   deltaKey   which previous quantity picks that row: by the previous value's
//   holdKey    remainder, or by the previous hold's bucket.
//   scale      granularity of the adaptive counters, 1/4/16/256.
//   deltaShift where the bucket boundary sits, 3..6.
//   holdShift  same, for holds.
//   budget     how much probability mass one row may hold, 32/64/256/320.
//   gapFrom    measure the gap from the previous press, or from the previous
//              release, which is the same thing minus the hold the decoder
//              already knows.
//   hasTps     whether the eight rate bytes after the knobs are there at all -
//              set when EITHER number differs from its default.
//
// What the header no longer carries, from earlier versions:
//
//   seed   - dropped. It was dead weight: nothing ever called setSeed()/getSeed(),
//            isValidHeader() never validated it, and no code read it back. SKC
//            does no RNG locking at all - it replays inputs on vanilla physics,
//            so a stored seed would buy nothing even if it were populated.
//   count  - dropped. A range-coded stream has no terminator, so the decoder
//            needs to know when to stop reading. Instead of a 4-byte count the
//            stream terminates itself: kind symbol 18 (END) is written after the
//            last click, costing ~4-5 bits instead of 32.
//
// There is one shape, and older ones are not accepted. There is no version
// byte to branch on, because a range coder's state depends on every knob: a
// header from any other shape would have to be rebuilt before a single symbol
// could be read, and keeping one decoder for one layout is cheaper than
// keeping several that each pay for it.
// ---------------------------------------------------------------------------
// --- v2 click stream --------------------------------------------------------
//
// Actions are paired into clicks per (button, player2) channel: a press
// followed by its release becomes one "paired" click storing only the delta
// to the previous click and the hold duration — one symbol pair instead of
// two independent events. Three kinds exist, plus a terminator:
//
//   kind 0  paired     (delta, hold)  press followed by its release
//   kind 1  press-only (delta)        press with no release before the macro end
//   kind 2  release-only (delta)      release with no preceding press
//                                     (e.g. the leading "button released"
//                                      marker some bots record at frame 0)
//   kind 18 END                      stream terminator, carries no click
//
// Clicks are stored globally sorted by anchor frame, so the delta is the
// distance to the previous click in the stream, not the previous click of the
// same channel. That keeps the decoder a single linear pass.
//
// Per click the stream is:
//
//   kind-changed bit    (sticky: 0 = same kind/button/player2 as the
//                         previous click, 1 = a kind symbol follows)
//   kind symbol         (only when changed)  kind*6 + (button-1)*2 + player2
//   delta value         ValueModel
//   hold value          ValueModel, only for kind 0
//
// and after the last click one final "changed=1" bit plus the END symbol.
//
// The sticky kind bit costs one bit per click on a homogeneous macro (the
// common case: one button, one player) and a full 18-symbol alphabet only on
// the rare click that changes it.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kEndKind = 18;   // kind symbol that terminates the stream

// What each part of the stream cost, in bits. Diagnostics only - nothing in the
// format depends on it. Ten candidates were picked by intuition and all ten lost;
// this is the measurement that should pick the eleventh.
struct CostProfile {
    uint64_t kindChanged = 0;
    uint64_t kindSymbol = 0;
    uint64_t delta = 0;
    uint64_t hold = 0;
    uint64_t deltaBucket = 0;
    uint64_t deltaOffset = 0;
    uint64_t holdBucket = 0;
    uint64_t holdOffset = 0;
    uint64_t terminator = 0;
    uint64_t lzHit = 0;

    [[nodiscard]] uint64_t total() const {
        return kindChanged + kindSymbol + delta + hold + terminator + lzHit;
    }
};

#pragma pack(push, 1)
// The whole header is three knob bytes plus, when the tick rate is not the
// default, a float. Nothing identifies the format here: the loader is reached
// by extension, and a file that is not ours cannot get past knob validation.

// How a value is coded: the (bucket, offset) split, or one adaptive row over
// the actual values with a ValueModel tail. The Direct widths are how many of
// the small values get their own row before the tail takes over.
enum class ValueCodec : uint8_t { Bucket = 0, Direct32 = 1, Direct64 = 2, Direct128 = 3 };
inline constexpr uint32_t kValueCodecCount = 4u;

// Which previous quantity selects the model row. PrevHoldBucket exists because
// delta = hold_prev + gap, so the previous hold is the part of the previous gap
// that is already known - the sweep found it worth 26 bytes on GRIEF.
//
// The second four are for a family the first sweep never touched: conditioning
// on more than one thing. Kind is cheap to test because kind is already decoded
// before the gap, and PrevDelta2 asks whether a second order of gap helps once
// the first order turned out to be mostly dead weight.
enum class ContextKey : uint8_t {
    Modulo = 0,
    PrevBucket = 1,
    PrevHold = 2,
    PrevHoldBucket = 3,
    PrevDelta2 = 4,
    Kind = 5,
    PrevDeltaPlusKind = 6,
    PrevHoldPlusKind = 7,
};
inline constexpr uint32_t kContextKeyCount = 8u;

// What the gap is measured from.
//
// PressToPress is the long-standing behaviour: delta is the distance from the
// previous press anchor. But each click also carries its hold, so delta is really
// hold_prev + gap - a sum of two independent quantities, coded as one. Measuring
// the gap from the previous RELEASE instead makes the two disjoint, and the
// decoder already knows hold_prev at that point, so nothing is lost. Whether
// that helps is an empirical question, which is why it is a switch.
enum class GapFrom : uint8_t { PressToPress = 0, FromRelease = 1 };
inline constexpr uint32_t kGapFromCount = 2u;

// A negative gap (the next press on one channel landing before the previous
// release on another) is carried in the top bit of the coded value. The value
// model's escape already passes any 16-bit number through untouched, so this is
// free on the common path - and a negative gap really does occur, once in 537
// clicks on GRIEF, so it cannot be clamped away.
inline constexpr uint32_t kGapSignBit = 0x8000u;

// FromRelease is only offered to the encoder when every gap fits that bit - see
// gapSplitSafe() below.

inline constexpr uint8_t kContextPlain = 0;

// ctx == 0x3F marks the optional extension byte after the knobs. The encoder
// never chooses 63 as a context count (candidate counts are 0..48), so this is
// free: no existing file contains it. The extension byte is
//     realCtx:6 | platformer:1 | reserved:1
inline constexpr uint8_t kCtxExtension = 0x3Fu;
inline constexpr uint8_t kExtPlatformer = 0x40u;

inline const char* VariantCodecName(ValueCodec codec) {
    return codec == ValueCodec::Bucket ? "bucket"
           : codec == ValueCodec::Direct32 ? "dir32"
           : codec == ValueCodec::Direct64 ? "dir64"
                                           : "dir128";
}

inline const char* VariantKeyName(ContextKey key) {
    return key == ContextKey::Modulo ? "mod"
           : key == ContextKey::PrevBucket ? "prevbucket"
           : key == ContextKey::PrevHold ? "prevhold"
                                          : "prevholdb";
}

inline uint32_t directCountOf(ValueCodec codec) {
    return codec == ValueCodec::Direct32 ? 32u
           : codec == ValueCodec::Direct64 ? 64u
           : codec == ValueCodec::Direct128 ? 128u
                                            : 32u;
}

// One complete set of encoding choices. The winner's fields go into the header
// so the decoder rebuilds exactly this; the sweep tool passes it directly
// instead, which is why this is public.
struct Variant {
    uint32_t contextCount = kContextPlain;
    uint8_t deltaShift = ValueModel::kDefaultShift;
    uint8_t holdShift = ValueModel::kDefaultShift;
    ValueCodec deltaCodec = ValueCodec::Bucket;
    ValueCodec holdCodec = ValueCodec::Bucket;
    ContextKey deltaKey = ContextKey::Modulo;
    ContextKey holdKey = ContextKey::Modulo;
    GapFrom gapFrom = GapFrom::PressToPress;
    uint32_t countScale = kCountScale;
    uint32_t rowBudget = kRowBudget;
    // Mix the bucket choice with an order-1 transition table instead of using
    // a plain order-0 chooser. Last in the struct so the measured candidate
    // lists above do not have to grow a column.
    bool mixGap = false;
    bool mixHold = false;
    bool lzMode = false;
};

// Every setting the sweep found actually winning, in one list. encode() tries
// exactly these and keeps the smallest.
//
// The full grid is 45056 variants per macro and takes ten seconds on a
// hand-played macro and eighty on a 20k-click one - fine for the sweep tool,
// absurd to do while the user waits for a save. So the sweep ran once and the
// seven winners it found were kept. They are not interchangeable: GRIEF wants a
// context split keyed on the previous hold, as_world wants no context at all
// but a very fine count scale, and two hand-played macros want exactly the old
// settings. Trying eight candidates costs eight passes and keeps all of it.
//
// The knobs have to fit the header. Nine of them, 24 bits, three bytes, plus
// the magic and the tps - eleven bytes, one more than before.
struct KnobTable {
    static constexpr uint32_t kScaleCount = 4;
    static constexpr uint32_t kScale[4] = {1u, 4u, 16u, 256u};
    // Only the four budgets a measured winner ever chose, which is what leaves
    // the header a spare bit for the tps flag.
    static constexpr uint32_t kBudgetCount = 4;
    static constexpr uint32_t kBudget[4] = {32u, 64u, 256u, 320u};
};

// On-disk size: 3 bytes always, plus 1 when the platformer extension is
// present, plus 8 more when the tps is not the default. Six of the seven real
// macros run at exactly 240.0, which a float stores exactly, so omitting it
// there costs nothing in fidelity and saves eight bytes on every ordinary
// macro.
inline constexpr uint32_t kHeaderFixed = 3;
// The extension byte that follows the knobs when ctx == kCtxExtension.
inline constexpr uint32_t kExtBytes = 1;
// New optional settings byte. It is present only if CBS or COS is enabled, or
// the click stream uses half-tick units; legacy files never set the marker.
inline constexpr uint32_t kModeBytes = 1;
inline constexpr uint8_t kExtHasModes = 0x80u;
inline constexpr uint8_t kModeCBS = 0x01u;
inline constexpr uint8_t kModeCOS = 0x02u;
inline constexpr uint8_t kModeSubticks = 0x04u;
inline constexpr uint8_t kModePlatformerKnown = 0x08u;
inline constexpr uint8_t kModeLz = 0x10u;
inline constexpr uint8_t kModeLockDelta = 0x20u;
inline constexpr uint8_t kModeRealTime = 0x40u;

// The rate tail. Two floats, both always together: tps, then the display rate
// the macro was made under (0 = follow TPS). They share one presence bit
// because there is no spare bit in the knobs to give the second one, and a
// second bit would cost a whole extra byte on every macro just to say "no" -
// which is the answer for essentially all of them.
inline constexpr uint32_t kTpsBytes = 8;
inline constexpr uint32_t kTpsDoubleBytes = 16;
inline constexpr float kTpsDoubleMarker = -0.0f;
inline constexpr double kDefaultTps = 240.0;

struct Header {
    uint8_t knobs0 = 0;   // ctx:6 | deltaCodec:2
    uint8_t knobs1 = 0;   // holdCodec:2 | deltaKey:2 | holdKey:2 | scale:2
    uint8_t knobs2 = 0;   // deltaShift:2 | holdShift:2 | budget:2 | gapFrom:1 | hasTps:1
    // In-memory only; the wire format stores them only when the flag says so.
    double tps = kDefaultTps;
    // Display rate as a WIRE value, not a number: +0.0 means the macro carries
    // no display rate at all (FPS was off while recording), -0.0 means "FPS on,
    // follow TPS", and any n > 0 is a pinned rate. Decode it with
    // skkbot2::fps::MacroDisplayRate::fromWire - never with a comparison, since
    // -0.0 == +0.0. Not stored when neither rate differs from its default.
    float fps = 0.0f;
    // Present only when the file carried the extension byte. `platformerMode`
    // is meaningless unless `platformerKnown` is true: an old file (and a
    // classic macro, which is not extended) cannot say which mode it was
    // recorded in, and refusing those on a guess would be worse than letting
    // them play.
    bool platformerKnown = false;
    bool platformerMode = false;
    bool cbsMode = false;
    bool cosMode = false;
    bool subtickEncoded = false;
    bool lzMode = false;
    bool lockDeltaMode = true;
    bool realTimeMode = false;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 23, "in-memory Header is packed");

// The candidates encode() searches. Each line is a measured winner from
// tools/skc_sweep.cpp, or the long-standing default.
[[nodiscard]] const std::vector<Variant>& candidateVariants();

struct Action {
    uint32_t frame = 0;
    uint8_t button = 1;     // 1 = Jump, 2 = Left, 3 = Right
    bool pressed = false;   // true = push, false = release
    bool player2 = false;   // false = P1, true = P2
    // True when this transition was observed in GD's CBS half-tick callback.
    // Ignored unless the macro declares CBS.
    bool halfTick = false;

    bool operator==(const Action& other) const = default;
};

enum class ClickKind : uint8_t {
    Paired = 0,      // press + release: (delta, hold)
    PressOnly = 1,   // press with no following release
    ReleaseOnly = 2, // release with no preceding press
};

struct Click {
    ClickKind kind = ClickKind::Paired;
    uint8_t button = 1;
    bool player2 = false;
    uint32_t delta = 0;     // frames from the previous click's anchor
    uint32_t hold = 0;      // release - press (kind == Paired only)
    bool operator==(const Click& other) const = default;
};

struct Macro {
    Header header;
    // Recorded in platformer mode. `platformerKnown` is false for a file that
    // predates the flag and for formats that do not declare a mode, which is
    // what keeps playback validation from refusing a macro we cannot classify.
    // Native recording and every importer that knows the answer set both.
    bool platformerMode = false;
    bool platformerKnown = false;
    bool cbsMode = false;
    bool cosMode = false;
    bool subtickEncoded = false;
    bool lockDeltaMode = true;
    bool realTimeMode = false;
    std::vector<Action> actions;

    [[nodiscard]] double durationSeconds() const {
        if (actions.empty() || header.tps <= 0.0f) return 0.0;
        return static_cast<double>(actions.back().frame) / header.tps;
    }

    // Actions must be sorted by frame for the encoder (delta-coded) and for the
    // playback cursor. stable_sort keeps same-frame events in record order.
    void sortActions();
};

// --- action <-> click conversion -------------------------------------------

// FromRelease is only offered to the encoder when every gap fits the sign bit. A
// macro whose gaps exceed 32768 frames - over two minutes of holding at 240 tps -
// simply does not get the candidate, because the sign bit would collide with a
// legitimate large gap. The check keeps the scheme exact rather than merely
// probably exact.
[[nodiscard]] bool gapSplitSafe(const Macro& macro);

// Pairs presses with their following release per (button, player2) channel and
// emits clicks sorted by anchor frame. Lossless for any action stream.
[[nodiscard]] std::vector<Click> actionsToClicks(const std::vector<Action>& actions,
                                                 bool subticks = false);

// Inverse of actionsToClicks. Reconstructs absolute frames and sorts.
[[nodiscard]] std::vector<Action> clicksToActions(const std::vector<Click>& clicks,
                                                  bool subticks = false);

// --- validation --------------------------------------------------------------
//
// Every bug that produced an unusable macro so far showed up HERE first: a press
// with no matching release, two downs in a row on one channel, frames going
// backwards. All of them survived encoding and only surfaced as "the bot dies
// at frame ~230", which is an expensive way to learn you have a corrupt file.
//
// Checks the action stream (not the encoded bytes) against the invariants the
// click pairing depends on:
//   - frames strictly non-decreasing (the delta coder assumes it)
//   - per (button, player2) channel: no press while already pressed, and a
//     press is followed by a release except for the final trailing hold
//   - a trailing hold is allowed (the player pressed and never let go)
// `outReason` receives a human-readable reason on failure.
[[nodiscard]] bool validateActions(const std::vector<Action>& actions,
                                   std::string& outReason);

// What normalizeActionStream() changed on the way in.
struct NormalizeReport {
    std::size_t droppedRelease = 0; // release with no press outstanding
    std::size_t rebuiltTaps = 0;    // duplicate press -> same-frame up + down
    // First few rebuilt frames, purely for the import log.
    std::vector<uint32_t> rebuiltAt;
};

// Normalises a foreign importer's raw edge stream into what validateActions()
// demands, in place. Two repairs:
//
//   - a release with nothing held is dropped (other bots emit a leading
//     "buttons are up" release, and repeated trailing releases);
//   - a duplicate press is NOT dropped. A second press on a channel that is
//     still held is a TAP the source format flattened onto one frame - the
//     release physically happened but was never stored. Rebuild it as a
//     zero-length hold on that same frame (release, then press), which is the
//     shape the AST importer produces and the shape GD's queue has to see.
//
// Dropping that press instead is a replayed-input bug, not a cosmetic one:
// measured on `Windy Road.slc` the single remaining press makes GD apply the
// jump one physics step earlier (and through a different code path) than the
// same timeline replayed as up+down, so the run diverges right at the first
// click. Frames are preserved exactly - nothing is shifted.
void normalizeActionStream(std::vector<Action>& actions, NormalizeReport& report);

// --- codec ------------------------------------------------------------------

// Writes the v4 format: the 3-byte knob header (plus the optional platformer
// extension and rate tail) followed by the self-terminating range-coded click
// stream.
[[nodiscard]] std::vector<uint8_t> encode(const Macro& macro);

// Same encoding path as encode(), but also reports what each part of the stream
// cost in bits. Returns the same bytes - the profile is a by-product of the
// passes encode() already makes, not an extra one.
[[nodiscard]] CostProfile profile(const Macro& macro);

// Encodes with an explicit variant instead of searching for one, and decodes it
// back with the same variant. This is the sweep tool's entry point: it lets a
// grid of thousands of settings be measured against the real production codec
// and - the part that matters - checked for losslessness by decoding every one
// of them back and comparing actions. No header is involved, so the header's
// fixed cost is excluded; what is measured is the payload each setting buys.
[[nodiscard]] std::vector<uint8_t> encodeWith(const Macro& macro, const Variant& variant);
[[nodiscard]] bool decodeWith(const uint8_t* data, size_t size, const Variant& variant,
                              std::vector<Action>& outActions);

// Reads the v3 format. The click stream terminates itself, so no count is
// needed; a corrupt stream is caught by the click-count ceiling in decodeClicks.
[[nodiscard]] std::optional<Macro> decode(const uint8_t* data, size_t size);

// GUI preview: reads the header without touching the payload.
[[nodiscard]] std::optional<Header> readHeader(const std::filesystem::path& path);

[[nodiscard]] std::optional<Macro> loadFromFile(const std::filesystem::path& path);
[[nodiscard]] bool saveToFile(const std::filesystem::path& path, const Macro& macro);

// Round-trip check used at startup, mirroring the SKK3 self-test.
[[nodiscard]] bool selfTest();

// --- header sanity ----------------------------------------------------------

// Rejects obviously broken headers (negative/zero tps, absurd counts) before
// we ever hand the macro to the bot.
[[nodiscard]] bool isValidHeader(const Header& header);

} // namespace skk::skc
