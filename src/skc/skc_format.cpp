#include "skc_format.hpp"

#include "skc_range.hpp"

#include "format/codec.hpp"
#include "logger/logger.hpp"

#include <Geode/Geode.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>

using namespace geode::prelude;

namespace skk::skc {

// ---------------------------------------------------------------------------
// v1 reader (zstd + LEB128). Kept alive so existing .skc files keep loading;
// encode() no longer produces this layout.
// ---------------------------------------------------------------------------
namespace {

void writeVarint(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

[[nodiscard]] bool readVarint(const uint8_t*& it, const uint8_t* end, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (true) {
        if (it >= end) return false;
        uint8_t byte = *it++;
        out |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
        // A varint longer than 10 bytes overflows uint64; a malformed stream
        // would otherwise loop until it runs off the buffer.
        if (shift >= 70) return false;
    }
}

[[nodiscard]] uint64_t packAction(const Action& action, uint32_t previousFrame) {
    uint32_t delta = action.frame > previousFrame ? action.frame - previousFrame : 0;
    uint64_t packed = static_cast<uint64_t>(delta) << 4;
    packed |= (action.player2 ? 1ull : 0ull) << 3;
    packed |= (static_cast<uint64_t>(action.button) & 0x3ull) << 1;
    packed |= action.pressed ? 1ull : 0ull;
    return packed;
}

[[nodiscard]] bool unpackAction(uint64_t packed, uint32_t previousFrame, Action& out) {
    out.frame = previousFrame + static_cast<uint32_t>(packed >> 4);
    out.player2 = (packed >> 3) & 0x1ull;
    out.button = static_cast<uint8_t>((packed >> 1) & 0x3ull);
    out.pressed = (packed & 0x1ull) != 0;
    return out.button >= 1 && out.button <= 3;
}

// Guard against a truncated or hostile file asking us to allocate gigabytes.
constexpr uint32_t kMaxActionCount = 5'000'000;

// --- click stream -----------------------------------------------------------

// 18 kind symbols: kind (3) x button (3) x player2 (2).
// 18 real kind symbols (kind 0..2 x button 1..3 x player2) PLUS the END
// terminator, which is what lets the header drop its click count.
constexpr uint32_t kKindSymbolCount = kEndKind + 1u;

[[nodiscard]] uint32_t packKind(ClickKind kind, uint8_t button, bool player2) {
    return static_cast<uint32_t>(kind) * 6u +
           (static_cast<uint32_t>(button) - 1u) * 2u +
           (player2 ? 1u : 0u);
}

[[nodiscard]] Click unpackKind(uint32_t symbol) {
    Click click;
    click.kind = static_cast<ClickKind>(symbol / 6u);
    click.button = static_cast<uint8_t>(((symbol % 6u) / 2u) + 1u);
    click.player2 = (symbol % 2u) != 0;
    return click;
}

} // namespace

void Macro::sortActions() {
    std::stable_sort(actions.begin(), actions.end(),
        [](const Action& lhs, const Action& rhs) { return lhs.frame < rhs.frame; });
}

bool isValidHeader(const Header& header) {
    if (!std::isfinite(header.tps) || header.tps < 1.0f || header.tps > 1'000'000.0f)
        return false;
    // 0 means "follow tps" and is the only accepted non-positive value. Anything
    // else outside the range is a corrupt field, not a request: it would reach
    // setAnimationInterval as 1/fps and pin the display to nonsense.
    if (!std::isfinite(header.fps) ||
        (header.fps != 0.0f && (header.fps < 1.0f || header.fps > 1'000'000.0f)))
        return false;

    // This is what the magic used to do. Without a signature in the bytes, the
    // knobs themselves have to reject a file that is not ours before a single
    // click is decoded - the coder's very first symbol would otherwise come
    // from arbitrary bytes and produce an arbitrary but plausible macro.
    //
    // Most fields are bounded enumerators, where every bit pattern lands on a
    // defined value and nothing can be out of range: ctx is 6 bits and the
    // decoder treats 0 as one model; deltaCodec/holdCodec are 2 bits over four
    // codecs; the two keys are 2 bits over four; the two shifts are 2 bits
    // starting at kShiftMin, which kShiftMax leaves room for; gapFrom is one
    // bit over two. That leaves exactly two fields that index a table, and
    // those are the two worth checking.
    const uint32_t scaleIdx = (header.knobs1 >> 6) & 0x03u;
    const uint32_t budgetIdx = (header.knobs2 >> 4) & 0x03u;
    return scaleIdx < KnobTable::kScaleCount && budgetIdx < KnobTable::kBudgetCount;
}

std::vector<Click> actionsToClicks(const std::vector<Action>& actions) {
    // Group events by (button, player2); inside a channel the presses and
    // releases alternate, so a press consumes the very next event. The channel
    // order is fixed (not hash-map order) so the click stream — and therefore
    // the encoded bytes — is deterministic for a given action list.
    std::vector<Click> clicks;
    for (uint32_t channel = 0; channel < 8; ++channel) {
        const uint8_t button = static_cast<uint8_t>((channel >> 1) + 1);
        const bool player2 = (channel & 1u) != 0;

        std::vector<const Action*> events;
        for (const auto& action : actions) {
            if (action.button == button && action.player2 == player2)
                events.push_back(&action);
        }
        if (events.empty()) continue;

        // A channel is recorded in frame order already (recordInput appends in
        // order and the caller sorts), but sort defensively: the pairing below
        // only works on an ordered stream.
        std::stable_sort(events.begin(), events.end(),
            [](const Action* lhs, const Action* rhs) { return lhs->frame < rhs->frame; });

        for (size_t i = 0; i < events.size();) {
            const Action* head = events[i];
            Click click;
            click.button = head->button;
            click.player2 = head->player2;
            click.delta = head->frame;
            click.hold = 0;

            if (head->pressed) {
                if (i + 1 < events.size() && !events[i + 1]->pressed) {
                    click.kind = ClickKind::Paired;
                    click.hold = events[i + 1]->frame - head->frame;
                    i += 2;
                } else {
                    click.kind = ClickKind::PressOnly;
                    i += 1;
                }
            } else {
                // A release with no press behind it: the "buttons released at
                // frame 0" marker some recordings start with.
                click.kind = ClickKind::ReleaseOnly;
                i += 1;
            }
            clicks.push_back(click);
        }
    }

    // Sort by absolute anchor, then delta-code in that order. The anchor lives
    // in `delta` temporarily and is replaced by the difference below.
    std::stable_sort(clicks.begin(), clicks.end(),
        [](const Click& lhs, const Click& rhs) { return lhs.delta < rhs.delta; });

    uint32_t previous = 0;
    for (auto& click : clicks) {
        const uint32_t anchor = click.delta;
        click.delta = anchor > previous ? anchor - previous : 0;
        previous = anchor;
    }
    return clicks;
}

bool validateActions(const std::vector<Action>& actions, std::string& outReason) {
    // Channel key: (player2, button). Six channels (P1 x btn1..3, P2 x
    // btn1..3) cover everything a level can send.
    bool held[2][3] = {};
    bool seenPress[2][3] = {};

    for (size_t i = 0; i < actions.size(); ++i) {
        const Action& action = actions[i];
        if (i > 0 && action.frame < actions[i - 1].frame) {
            outReason = "frames go backwards: event " + std::to_string(i) +
                        " is at frame " + std::to_string(action.frame) +
                        " but the previous one was at " +
                        std::to_string(actions[i - 1].frame);
            return false;
        }
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) {
            outReason = "event " + std::to_string(i) + " has button " +
                        std::to_string(action.button) + " (must be 1..3)";
            return false;
        }
        const int slot = action.player2 ? 1 : 0;

        if (action.pressed) {
            if (held[slot][button]) {
                // Two downs without an up in between. Physically impossible for
                // one button: it means the stream spliced two attempts (a
                // practice restore dropped the release) or the pairing already
                // lost track. Either way the replay cannot reproduce it.
                outReason = "double press on button " +
                            std::to_string(action.button) +
                            (action.player2 ? " (P2)" : " (P1)") +
                            " at frame " + std::to_string(action.frame) +
                            " - no release in between";
                return false;
            }
            held[slot][button] = true;
            seenPress[slot][button] = true;
        } else if (seenPress[slot][button] && !held[slot][button]) {
            // A release is only wrong when the channel has already played a
            // FULL press/release cycle - i.e. a stray second release.
            //
            // A LEADING release is legal and in fact expected: recordings start
            // with a "buttons are up at frame 0" baseline, which actionsToClicks
            // encodes as a ReleaseOnly click (skc_format.cpp:144-146). The
            // first version of this validator rejected it and failed its own
            // self-test - and would have refused nearly every real macro at
            // save time.
            outReason = "stray release on button " +
                        std::to_string(action.button) +
                        (action.player2 ? " (P2)" : " (P1)") +
                        " at frame " + std::to_string(action.frame) +
                        " - already up, no press outstanding";
            return false;
        } else {
            // Clear the shadow. Omitting this was a real bug: the flag latched
            // true after the first press forever, so EVERY later press on that
            // channel was reported as a double press and no macro containing
            // two taps could ever be saved.
            held[slot][button] = false;
        }
    }

    // A trailing hold is legal (the player pressed and never let go before the
    // recording ended), so held[] leftovers are not an error.
    outReason.clear();
    return true;
}

std::vector<Action> clicksToActions(const std::vector<Click>& clicks) {
    std::vector<Action> actions;
    actions.reserve(clicks.size() * 2);
    uint32_t anchor = 0;
    for (const auto& click : clicks) {
        anchor += click.delta;
        switch (click.kind) {
            case ClickKind::Paired:
                actions.push_back({anchor, click.button, true, click.player2});
                actions.push_back({anchor + click.hold, click.button, false, click.player2});
                break;
            case ClickKind::PressOnly:
                actions.push_back({anchor, click.button, true, click.player2});
                break;
            case ClickKind::ReleaseOnly:
                actions.push_back({anchor, click.button, false, click.player2});
                break;
        }
    }
    return actions;
}

// ---------------------------------------------------------------------------
// v2 codec: click stream over the range coder.
// ---------------------------------------------------------------------------

namespace {

// One thing encode() tries. The winner's fields go into the header, so the
// decoder rebuilds exactly this without being told anything else.
using Candidate = Variant;

// Builds one field's model set. `mix` switches the bucket chooser over to the
// adaptive order-0/order-1 blend; the flat codec has no buckets to mix, so the
// flag is ignored there.
std::unique_ptr<ValueModelSet> makeModel(const Candidate& cand, ValueCodec codec,
                                         uint32_t shift, bool mix) {
    if (codec != ValueCodec::Bucket) {
        return std::make_unique<DirectContextValueModel>(
            cand.contextCount, directCountOf(codec), shift, cand.countScale, cand.rowBudget);
    }
    return std::make_unique<ContextValueModel>(
        cand.contextCount, shift, cand.countScale, cand.rowBudget, mix);
}
// host does. Both helpers work on explicit byte shifts, so they are correct on a
// big-endian machine too, and they produce exactly the bytes memcpy produced on
// every little-endian build - existing .skc files keep loading unchanged.
void writeLeFloat(uint8_t* dst, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    dst[0] = static_cast<uint8_t>(bits & 0xFFu);
    dst[1] = static_cast<uint8_t>((bits >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((bits >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((bits >> 24) & 0xFFu);
}

void readLeFloat(const uint8_t* src, float& value) {
    const uint32_t bits = static_cast<uint32_t>(src[0]) |
                          (static_cast<uint32_t>(src[1]) << 8) |
                          (static_cast<uint32_t>(src[2]) << 16) |
                          (static_cast<uint32_t>(src[3]) << 24);
    std::memcpy(&value, &bits, sizeof(value));
}

void writeLeU32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void encodeClicks(std::vector<uint8_t>& out, const std::vector<Click>& clicks,
                  const Candidate& cand, CostProfile* prof = nullptr) {
    const uint32_t contextCount = cand.contextCount;
    const uint32_t deltaShift = cand.deltaShift;
    RangeEncoder enc;

    // Sticky kind: a bit per click saying whether the kind/button/player2
    // triple changed, plus a full symbol on the change.
SymbolModel kindChangedModel(2u, 4u, cand.countScale, cand.rowBudget);
    SymbolModel kindModel(kKindSymbolCount, 1u, cand.countScale, cand.rowBudget);
    // contextCount == 0 gives a single model set. Which set that is, and with
    // what shift, is the variant's business - and the variant is what is being
    // measured across candidates.
    auto deltaModel = makeModel(cand, cand.deltaCodec, cand.deltaShift, cand.mixGap);
    auto holdModel = makeModel(cand, cand.holdCodec, cand.holdShift, cand.mixHold);

    uint32_t kindCtx = 0;
    uint32_t previousKind = ~0u;   // force a symbol on the first click
    uint32_t previousDelta = 0;
    uint32_t previousHold = 0;
    uint32_t previousDelta2 = 0;
    uint32_t previousCodedValue = 0;
    // Zero means "one model", so the divisor is floored at one rather than
    // taken from contextCount directly.
    const uint32_t buckets = contextCount ? contextCount : 1u;

    // Which previous quantity picks the row. Both sides must compute this the
    // same way or the stream desynchronises on the first click, so the two
    // lambdas are deliberately identical in shape.
    //
    // The gap is measured from the previous RELEASE rather than the previous
    // press when the variant says so. press-to-press delta is really
    // hold_prev + gap coded as one number; splitting it hands the model two
    // disjoint quantities instead of their sum, and the decoder already holds
    // hold_prev when it needs it. That is the whole trick - nothing is invented,
    // it is only rearranged.
    const auto deltaContext = [=](uint32_t prevDelta, uint32_t prevHold,
                                  uint32_t prevDelta2, uint32_t kind) -> uint32_t {
        switch (cand.deltaKey) {
            case ContextKey::PrevBucket: return (prevDelta >> cand.deltaShift) % buckets;
            case ContextKey::PrevHold: return prevHold % buckets;
            case ContextKey::PrevHoldBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::PrevDelta2: return prevDelta2 % buckets;
            case ContextKey::Kind: return kind % buckets;
            case ContextKey::PrevDeltaPlusKind: return (prevDelta + kind * 7u) % buckets;
            case ContextKey::PrevHoldPlusKind: return (prevHold + kind * 7u) % buckets;
            default: return prevDelta % buckets;
        }
    };
    const auto holdContext = [=](uint32_t prevHold, uint32_t kind) -> uint32_t {
        switch (cand.holdKey) {
            case ContextKey::PrevBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::PrevHold: return prevHold % buckets;
            case ContextKey::PrevHoldBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::Kind: return kind % buckets;
            case ContextKey::PrevDeltaPlusKind: return (prevHold + kind * 7u) % buckets;
            case ContextKey::PrevHoldPlusKind: return (prevHold + kind * 7u) % buckets;
            case ContextKey::PrevDelta2: return prevHold % buckets;
            default: return prevHold % buckets;
        }
    };

    for (const auto& click : clicks) {
        const uint32_t symbol = packKind(click.kind, click.button, click.player2);
        const uint32_t changed = symbol != previousKind ? 1u : 0u;

        uint64_t mark = prof ? enc.bitsProduced() : 0;
        kindChangedModel.encode(enc, changed, kindCtx);
        kindCtx = changed & 3u;
        if (changed != 0u) {
            kindModel.encode(enc, symbol, 0u);
            previousKind = symbol;
        }
        if (prof) {
            // The bit and the symbol that follows it are one decision, so they
            // are charged together: what matters is what a click change costs,
            // not which of the two models paid.
            prof->kindChanged += enc.bitsProduced() - mark;
            mark = enc.bitsProduced();
        }

        const uint32_t kindCode = static_cast<uint32_t>(click.kind);
        // In FromRelease mode the number coded is the idle gap, not the
        // press-to-press distance: the hold is already on the tape and is not
        // part of this value, which is what stops the two quantities from
        // paying for each other twice.
        //
        // The gap can be negative - the next press on one channel can land
        // before the previous release on another - and it does, once in 537
        // clicks on GRIEF. Clamping it to zero would lose a frame every time,
        // so the sign rides in the top bit of the value instead. The value
        // model's escape already carries any number up to 65535, so this costs
        // nothing on the 99.8% of clicks that are not negative.
        int64_t gap = static_cast<int64_t>(click.delta) - static_cast<int64_t>(previousHold);
        if (cand.gapFrom == GapFrom::PressToPress) gap = click.delta;
        const uint32_t value = gap >= 0
                                   ? static_cast<uint32_t>(gap)
                                   : (kGapSignBit + static_cast<uint32_t>(-gap));
        deltaModel->encode(enc, value,
                           deltaContext(previousDelta, previousHold, previousDelta2, kindCode),
                           prof ? &prof->deltaBucket : nullptr,
                           prof ? &prof->deltaOffset : nullptr, previousCodedValue);
        if (prof) prof->delta += enc.bitsProduced() - mark;
        if (click.kind == ClickKind::Paired) {
            mark = enc.bitsProduced();
            holdModel->encode(enc, click.hold, holdContext(previousHold, kindCode),
                              prof ? &prof->holdBucket : nullptr,
                              prof ? &prof->holdOffset : nullptr, previousHold);
            previousHold = click.hold;
            if (prof) prof->hold += enc.bitsProduced() - mark;
        }
        previousDelta2 = previousDelta;
        previousCodedValue = value;
        previousDelta = click.delta;
    }

    // Stream terminator: one final "changed" bit plus the END symbol. This is
    // what lets the header drop its click count.
    const uint64_t termMark = prof ? enc.bitsProduced() : 0;
    kindChangedModel.encode(enc, 1u, kindCtx);
    kindModel.encode(enc, kEndKind, 0u);
    if (prof) prof->terminator = enc.bitsProduced() - termMark;

    out = enc.finish();
}

// Decodes until the END symbol. Returns false if the stream never terminates
// within the click ceiling (a truncated or corrupt payload).
[[nodiscard]] bool decodeClicks(const uint8_t* data, size_t size,
                                std::vector<Click>& clicks,
                                const Candidate& cand) {
    RangeDecoder dec(data, size);
    const uint32_t contextCount = cand.contextCount;

    SymbolModel kindChangedModel(2u, 4u, cand.countScale, cand.rowBudget);
    SymbolModel kindModel(kKindSymbolCount, 1u, cand.countScale, cand.rowBudget);
    auto deltaModel = makeModel(cand, cand.deltaCodec, cand.deltaShift, cand.mixGap);
    auto holdModel = makeModel(cand, cand.holdCodec, cand.holdShift, cand.mixHold);

    const uint32_t buckets = contextCount ? contextCount : 1u;
    const auto deltaContext = [=](uint32_t prevDelta, uint32_t prevHold,
                                  uint32_t prevDelta2, uint32_t kind) -> uint32_t {
        switch (cand.deltaKey) {
            case ContextKey::PrevBucket: return (prevDelta >> cand.deltaShift) % buckets;
            case ContextKey::PrevHold: return prevHold % buckets;
            case ContextKey::PrevHoldBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::PrevDelta2: return prevDelta2 % buckets;
            case ContextKey::Kind: return kind % buckets;
            case ContextKey::PrevDeltaPlusKind: return (prevDelta + kind * 7u) % buckets;
            case ContextKey::PrevHoldPlusKind: return (prevHold + kind * 7u) % buckets;
            default: return prevDelta % buckets;
        }
    };
    const auto holdContext = [=](uint32_t prevHold, uint32_t kind) -> uint32_t {
        switch (cand.holdKey) {
            case ContextKey::PrevBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::PrevHold: return prevHold % buckets;
            case ContextKey::PrevHoldBucket: return (prevHold >> cand.holdShift) % buckets;
            case ContextKey::Kind: return kind % buckets;
            case ContextKey::PrevDeltaPlusKind: return (prevHold + kind * 7u) % buckets;
            case ContextKey::PrevHoldPlusKind: return (prevHold + kind * 7u) % buckets;
            case ContextKey::PrevDelta2: return prevHold % buckets;
            default: return prevHold % buckets;
        }
    };
    uint32_t kindCtx = 0;
    uint32_t previousKind = ~0u;
    uint32_t previousDelta = 0;
    uint32_t previousHold = 0;
    uint32_t previousDelta2 = 0;
    uint32_t previousCodedValue = 0;

    while (true) {
        // Ceiling guard: a payload that never yields END is corrupt, and
        // RangeDecoder zero-pads past the buffer, so without this the loop
        // would happily manufacture clicks forever.
        if (clicks.size() >= kMaxActionCount) return false;

        const uint32_t changed = kindChangedModel.decode(dec, kindCtx);
        kindCtx = changed & 3u;

        if (changed != 0u) {
            const uint32_t symbol = kindModel.decode(dec, 0u);
            if (symbol == kEndKind) return true;   // clean terminator
            previousKind = symbol;
        }

        Click click = unpackKind(previousKind);
        const uint32_t kindCode = static_cast<uint32_t>(click.kind);
        const uint32_t raw =
            deltaModel->decode(dec, deltaContext(previousDelta, previousHold, previousDelta2, kindCode),
                             previousCodedValue);
        if (cand.gapFrom == GapFrom::PressToPress) {
            // raw already is the press-to-press distance; previousDelta has been
            // accumulated into it by the encoder's delta coding.
            click.delta = raw;
        } else {
            const int64_t gap = (raw & kGapSignBit)
                                    ? -static_cast<int64_t>(raw & (kGapSignBit - 1u))
                                    : static_cast<int64_t>(raw);
            // click.delta is the press-to-press DIFFERENCE, and the encoder
            // formed the gap by subtracting the previous hold from it. So the
            // inverse is hold_prev + gap - with no previousDelta anywhere in it.
            // Putting previousDelta in was off by exactly one accumulated gap,
            // which is why the drift grew click by click instead of appearing at
            // once.
            click.delta = static_cast<uint32_t>(static_cast<int64_t>(previousHold) + gap);
        }
        click.hold = click.kind == ClickKind::Paired
                         ? holdModel->decode(dec, holdContext(previousHold, kindCode), previousHold)
                         : 0u;
        if (click.kind == ClickKind::Paired) previousHold = click.hold;
        previousDelta2 = previousDelta;
        previousCodedValue = raw;
        previousDelta = click.delta;
        clicks.push_back(click);
    }
}

} // namespace

const std::vector<Variant>& candidateVariants() {
    using V = ValueCodec;
    using K = ContextKey;
    using G = GapFrom;
    static const std::vector<Variant> list = {
        // Measured winners from tools/skc_sweep.cpp, one per macro, all with
        // gapFrom = FromRelease. Measuring the gap from the previous release
        // instead of the previous press is worth 15-30 bytes on every file
        // tested, and it won on all seven - it is not a special case for one
        // kind of macro, it is the better parameterisation in general.
        //
        // GRIEF
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 256u},
        // RAUCHKAMMER 2
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 4u, 64u},
        // amethyst and thinking share a shape: the gap gets the flat row.
        Variant{0u, 4u, 5u, V::Direct128, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 256u},
        // kyouki
        Variant{0u, 4u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 320u},
        // as_world, the one repetitive macro: a context split keyed on the
        // previous hold's bucket, and the finest count scale in the table.
        Variant{16u, 4u, 3u, V::Direct128, V::Direct32, K::PrevHoldBucket, K::Modulo, G::FromRelease, 256u, 32u},
        // artemis
        Variant{0u, 5u, 5u, V::Bucket, V::Bucket, K::Modulo, K::Modulo, G::FromRelease, 4u, 64u},

        // Press-to-press winners from the previous round, kept as fallbacks: a
        // macro whose gaps are too large for the sign bit cannot use FromRelease
        // at all, and these are what it gets instead.
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::PressToPress, 1u, 256u},
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::PressToPress, 4u, 320u},
        Variant{0u, 4u, 5u, V::Direct128, V::Direct64, K::Modulo, K::Modulo, G::PressToPress, 1u, 320u},
        Variant{0u, 5u, 3u, V::Direct64, V::Direct32, K::Modulo, K::Modulo, G::PressToPress, 256u, 32u},
        Variant{24u, 4u, 3u, V::Bucket, V::Direct64, K::PrevHoldBucket, K::PrevBucket, G::PressToPress, 4u, 64u},
        Variant{6u, 4u, 5u, V::Bucket, V::Direct64, K::PrevHoldBucket, K::PrevBucket, G::PressToPress, 4u, 32u},
        Variant{0u, 5u, 5u, V::Bucket, V::Bucket, K::Modulo, K::Modulo, G::PressToPress, 16u, 320u},
        Variant{48u, 4u, 4u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::PressToPress, 16u, 320u},
    };
    return list;
}
// The file-type check. There is no magic: a file that is not ours has to fail
// isValidHeader() here rather than be decoded as a stream of garbage.
[[nodiscard]] bool parseHeader(const uint8_t* data, size_t size, Header& outHeader,
                               size_t& outPayloadOffset, Candidate& outCand) {
    if (size < kHeaderFixed) return false;

    outHeader.knobs0 = data[0];
    outHeader.knobs1 = data[1];
    outHeader.knobs2 = data[2];
    const bool hasTps = (outHeader.knobs2 & 0x80u) != 0u;
    if (size < kHeaderFixed + (hasTps ? kTpsBytes : 0u)) return false;
    if (hasTps) {
        readLeFloat(data + kHeaderFixed, outHeader.tps);
        readLeFloat(data + kHeaderFixed + 4, outHeader.fps);
    } else {
        outHeader.tps = kDefaultTps;
        outHeader.fps = 0.0f;
    }

    outCand.contextCount = outHeader.knobs0 & 0x3Fu;
    outCand.deltaCodec = static_cast<ValueCodec>((outHeader.knobs0 >> 6) & 0x03u);
    outCand.holdCodec = static_cast<ValueCodec>(outHeader.knobs1 & 0x03u);
    outCand.deltaKey = static_cast<ContextKey>((outHeader.knobs1 >> 2) & 0x03u);
    outCand.holdKey = static_cast<ContextKey>((outHeader.knobs1 >> 4) & 0x03u);
    const uint32_t scaleIdx = (outHeader.knobs1 >> 6) & 0x03u;
    outCand.deltaShift = static_cast<uint8_t>(ValueModel::kShiftMin +
                                              (outHeader.knobs2 & 0x03u));
    outCand.holdShift = static_cast<uint8_t>(ValueModel::kShiftMin +
                                             ((outHeader.knobs2 >> 2) & 0x03u));
    const uint32_t budgetIdx = (outHeader.knobs2 >> 4) & 0x03u;
    outCand.gapFrom = static_cast<GapFrom>((outHeader.knobs2 >> 6) & 0x01u);

    if (!isValidHeader(outHeader)) return false;

    outCand.countScale = KnobTable::kScale[scaleIdx];
    outCand.rowBudget = KnobTable::kBudget[budgetIdx];

    outPayloadOffset = kHeaderFixed + (hasTps ? kTpsBytes : 0u);
    return true;
}

// Tries every context/shift pair and keeps the smallest. Both encode() and
// profile() go through this, so the breakdown always describes the model the
// file actually got.
struct BestPick {
    Candidate cand;
    std::vector<uint8_t> payload;
};

BestPick pickBest(const std::vector<Click>& clicks, bool allowGapSplit) {
    BestPick best;
    // Seed from the first candidate. Starting with an empty payload would make
    // "smaller than the best so far" false for every trial, and the encoder
    // would emit a header with no body at all.
    bool have = false;
    for (const auto& cand : candidateVariants()) {
        // A macro with gaps too large for the sign bit cannot use FromRelease at
        // all - the candidate is skipped rather than offered and then broken.
        if (!allowGapSplit && cand.gapFrom == GapFrom::FromRelease) continue;
        std::vector<uint8_t> trial;
        encodeClicks(trial, clicks, cand);
        if (!have || trial.size() < best.payload.size()) {
            best.payload.swap(trial);
            best.cand = cand;
            have = true;
        }
    }
    return best;
}

std::vector<uint8_t> encode(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions);

    // Encode several ways and keep whichever is smaller, recording the winner in
    // the header. Which way wins is macro-dependent and no threshold on the
    // click count tells them apart, so the encoder tries and measures instead
    // of guessing - but only over the eight configurations a full sweep found
    // worth keeping, because the full 45056-variant grid costs ten to eighty
    // seconds per save.
    BestPick best = pickBest(clicks, gapSplitSafe(macro));
    const Candidate& c = best.cand;

    uint32_t scaleIdx = 0, budgetIdx = 0;
    for (uint32_t i = 0; i < KnobTable::kScaleCount; ++i)
        if (KnobTable::kScale[i] == c.countScale) scaleIdx = i;
    for (uint32_t i = 0; i < KnobTable::kBudgetCount; ++i)
        if (KnobTable::kBudget[i] == c.rowBudget) budgetIdx = i;

    // The tail appears when EITHER number differs from its default. Storing tps
    // alone used to be the whole condition; making it "or a display rate worth
    // keeping" costs nothing for macros that declare neither, which is all of
    // them until someone records at a pinned rate.
    //
    // `std::signbit` is not decoration. The follow-tps marker is -0.0, which is
    // EQUAL to +0.0 under `!=`, so a plain comparison would silently drop the
    // tail for a macro recorded at fps == tps with default tps - storing
    // nothing where the user asked for "FPS on, follow tps".
    // Canonicalise the display rate before it goes on the wire. Mirror of the
    // SKK3 writer and the same rule: fps == tps is stored as the -0.0 marker
    // ("FPS on, follow the tick rate"), never as the number and never as
    // nothing at all. An importer that fills fps with the tick rate would
    // otherwise produce a file whose playback enables FPS at a pinned rate
    // instead of following tps.
    float fpsWire = macro.header.fps;
    if (!std::isfinite(fpsWire)) {
        fpsWire = 0.0f;
    } else if (fpsWire > 0.0f && std::abs(fpsWire - macro.header.tps) < 1e-3f) {
        fpsWire = std::copysign(0.0f, -1.0f);
    }
    const bool hasTps = macro.header.tps != kDefaultTps ||
                        std::signbit(fpsWire) || fpsWire != 0.0f;
    const size_t headerBytes = kHeaderFixed + (hasTps ? kTpsBytes : 0u);

    std::vector<uint8_t> out;
    out.reserve(headerBytes + best.payload.size());
    out.resize(headerBytes);

    Header header;
    header.knobs0 = static_cast<uint8_t>((c.contextCount & 0x3Fu) |
                                         (static_cast<uint8_t>(c.deltaCodec) << 6));
    header.knobs1 = static_cast<uint8_t>(static_cast<uint8_t>(c.holdCodec) |
                                         (static_cast<uint8_t>(c.deltaKey) << 2) |
                                         (static_cast<uint8_t>(c.holdKey) << 4) |
                                         (static_cast<uint8_t>(scaleIdx) << 6));
    header.knobs2 = static_cast<uint8_t>((c.deltaShift - ValueModel::kShiftMin) |
                                         ((c.holdShift - ValueModel::kShiftMin) << 2) |
                                         (static_cast<uint8_t>(budgetIdx) << 4) |
                                         (static_cast<uint8_t>(c.gapFrom) << 6) |
                                         (hasTps ? 0x80u : 0u));
    header.tps = macro.header.tps;
    header.fps = fpsWire;

    // Byte by byte. A raw memcpy would put the bytes in the host's order, which
    // is fine on x86/ARM and wrong on a big-endian host. Emitting explicitly
    // makes the header's byte order part of the format rather than a property
    // of the machine that wrote the file.
    out[0] = header.knobs0;
    out[1] = header.knobs1;
    out[2] = header.knobs2;
    if (hasTps) {
        writeLeFloat(out.data() + kHeaderFixed, header.tps);
        writeLeFloat(out.data() + kHeaderFixed + 4, header.fps);
    }

    out.insert(out.end(), best.payload.begin(), best.payload.end());
    return out;
}

CostProfile profile(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions);
    const BestPick best = pickBest(clicks, gapSplitSafe(macro));

    // Re-encode the winner with the counters on. The bytes are identical, so
    // the breakdown is the one the saved file has.
    CostProfile prof;
    std::vector<uint8_t> ignored;
    encodeClicks(ignored, clicks, best.cand, &prof);
    return prof;
}

[[nodiscard]] std::optional<Macro> decode(const uint8_t* data, size_t size) {
    if (!data) return std::nullopt;

Header header;
    size_t payloadOffset = 0;
    Candidate cand;
    if (!parseHeader(data, size, header, payloadOffset, cand))
        return std::nullopt;

    Macro macro;
    macro.header.tps = header.tps;
    macro.header.fps = header.fps;

    const size_t payloadSize = size - payloadOffset;
    if (payloadSize == 0) {
        // A header with no body used to be accepted here as "an empty macro.
        // Legal: CLEAR then SAVE". It is not legal, and it is not empty: the
        // encoder always writes the END terminator (encodeClicks, "one final
        // changed bit plus the END symbol"), so finish() always emits at least
        // one byte even for a macro with no clicks. A bodyless file is therefore
        // a write that was cut off - and loading it as an empty macro is exactly
        // how a half-written save turns into a silent CLEAR of the level's
        // inputs.
        return std::nullopt;
    }

    std::vector<Click> clicks;
    if (!decodeClicks(data + payloadOffset, payloadSize, clicks, cand))
        return std::nullopt;

    macro.actions = clicksToActions(clicks);
    macro.sortActions();

    // With no magic in the bytes, the header check can only prove the knobs are
    // in range - and 'AST9' decodes into a perfectly legal knob set, so a
    // renamed .ast walks straight through parseHeader(). The stream is what has
    // to reject it: a macro that survives validateActions() has its clicks
    // paired, ordered and in range, which arbitrary bytes do not produce.
    // Checking the whole file beats checking its first two bytes.
    std::string reason;
    if (!validateActions(macro.actions, reason)) return std::nullopt;
    return macro;
}

std::optional<Header> readHeader(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;

    // Read the whole file and go through the same header parser as decode(),
    // rather than reading sizeof(Header) bytes into a struct and hoping.
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

Header header;
    size_t payloadOffset = 0;
    Candidate cand;
    if (!parseHeader(data.data(), data.size(), header, payloadOffset, cand))
        return std::nullopt;
    return header;
}

std::optional<Macro> loadFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    return decode(data.data(), data.size());
}

bool saveToFile(const std::filesystem::path& path, const Macro& macro) {
    const auto bytes = encode(macro);
    if (bytes.empty())
        return false;

    // Atomic-ish write: build in a temp file, then move, so a crash mid-save
    // can never truncate a macro the user just recorded.
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// selfTest — round-trip a representative macro (holds, taps, P2, long gaps,
// orphan releases) and require a byte-identical re-encode. Runs once at mod
// load, same as the SKK3 per-frame codec test.
// ---------------------------------------------------------------------------
bool selfTest() {
    Macro macro;
    macro.header.tps = 360.0f;
    // Non-default on purpose: the fixture has to exercise the whole rate tail,
    // not just the tps half of it. With fps at 0 the tail is still written
    // (tps differs) but the second float is zero, and a bug that never touched
    // it would pass every time.
    macro.header.fps = 144.0f;
    // Press and release MUST share a channel (same button, same player) so the
    // click pairing produces a Paired click. The earlier fixture deliberately
    // alternated the player2 flag, which made every event an orphan release and
    // therefore could not catch a press/release swap.
    macro.actions = {
        { 0,     1, false, false },   // leading "released" marker
        { 1,     1, true,  false },   // P1 jump press
        { 2,     1, false, false },   // P1 jump release one frame later
        { 120,   1, true,  false },   // half a second of silence, then a tap
        { 121,   1, false, false },
        { 240,   3, true,  false },   // platformer right hold
        { 290,   3, false, false },
        { 300,   2, true,  true  },   // P2 left press (orphan, no release)
        { 4000,  1, true,  false },   // long gap (~11 s @ 360)
        { 4001,  1, false, false },
    };
    macro.sortActions();

    // The fixture above must actually exercise the press/release pairing: a
    // macro that degenerates into only orphan releases (which is what a
    // swapped pressed/player2 produced) round-trips fine but replays nothing.
    size_t presses = 0;
    for (const auto& action : macro.actions)
        if (action.pressed) ++presses;
    if (presses == 0) {
        skkbot2::log::error("[SKC] self-test: fixture has no press events");
        return false;
    }

    const auto clicks = actionsToClicks(macro.actions);
    size_t paired = 0;
    for (const auto& click : clicks)
        if (click.kind == ClickKind::Paired) ++paired;
    if (paired == 0) {
        skkbot2::log::error("[SKC] self-test: no Paired clicks produced");
        return false;
    }

    const auto bytes = encode(macro);
    if (bytes.size() <= sizeof(Header)) {
        skkbot2::log::error("[SKC] self-test: encode produced no payload");
        return false;
    }

    auto decoded = decode(bytes.data(), bytes.size());
    if (!decoded) {
        skkbot2::log::error("[SKC] self-test: decode failed");
        return false;
    }

    if (decoded->header.tps != macro.header.tps ||
        decoded->header.fps != macro.header.fps) {
        skkbot2::log::error("[SKC] self-test: header mismatch (tps {:.1f}/{:.1f}, "
                            "fps {:.1f}/{:.1f})",
                            macro.header.tps, decoded->header.tps,
                            macro.header.fps, decoded->header.fps);
        return false;
    }

    if (decoded->actions != macro.actions) {
        skkbot2::log::error("[SKC] self-test: action mismatch ({} vs {})",
                   decoded->actions.size(), macro.actions.size());
        for (size_t i = 0; i < std::min(decoded->actions.size(), macro.actions.size()); ++i) {
            const auto& a = macro.actions[i];
            const auto& b = decoded->actions[i];
            if (a != b) {
                skkbot2::log::error("[SKC] self-test:   first diff #{}: ({},{},{},{}) vs ({},{},{},{})",
                           i, a.frame, a.button, a.pressed, a.player2,
                           b.frame, b.button, b.pressed, b.player2);
                break;
            }
        }
        return false;
    }

    // Re-encoding must be stable: identical input, identical bytes.
    const auto reencoded = encode(*decoded);
    if (reencoded != bytes) {
        skkbot2::log::error("[SKC] self-test: re-encode is not byte-stable");
        return false;
    }

    // The other direction: a macro that declares neither rate must not grow a
    // tail. Every existing .skc on disk is in that state, and eight bytes per
    // file is the whole difference between "the format got 4% bigger" and "it
    // didn't". This is the check that keeps the feature free for the macros
    // that never use it.
    {
        Macro plain = macro;
        plain.header.tps = kDefaultTps;
        plain.header.fps = 0.0f;
        const auto plainBytes = encode(plain);
        // Same payload, header only 3 bytes instead of 11. Comparing against
        // `bytes.size() - kTpsBytes` is the whole claim: nothing about the
        // stream moved, only the tail went away.
        if (plainBytes.size() != bytes.size() - kTpsBytes) {
            skkbot2::log::error("[SKC] self-test: default-rate macro is {} B, "
                                "expected {}", plainBytes.size(),
                                bytes.size() - kTpsBytes);
            return false;
        }
        auto plainBack = decode(plainBytes.data(), plainBytes.size());
        if (!plainBack || plainBack->header.tps != kDefaultTps ||
            plainBack->header.fps != 0.0f) {
            skkbot2::log::error("[SKC] self-test: default-rate macro did not "
                                "round-trip");
            return false;
        }
    }

    // The THIRD state: FPS was on and equal to the tick rate. It has to survive
    // as -0.0, and it must cost its own tail at the default tps - otherwise the
    // playback side cannot tell "follow tps" from "FPS was off", which is the
    // whole distinction this state exists for. -0.0 == +0.0, so both ends have
    // to be tested with signbit.
    {
        Macro follow = macro;
        follow.header.tps = kDefaultTps;
        follow.header.fps = -0.0f;
        const auto followBytes = encode(follow);
        // The tail must be there: a default-tps macro with no display rate is
        // the 3-byte header, and this one is 8 bytes longer.
        Macro followPlain = follow;
        followPlain.header.fps = 0.0f;
        const auto followPlainBytes = encode(followPlain);
        if (followBytes.size() != followPlainBytes.size() + kTpsBytes) {
            skkbot2::log::error("[SKC] self-test: follow-tps macro lost its tail "
                                "({} B vs {} B)",
                                followBytes.size(), followPlainBytes.size());
            return false;
        }
        auto followBack = decode(followBytes.data(), followBytes.size());
        if (!followBack || followBack->header.fps != 0.0f ||
            !std::signbit(followBack->header.fps)) {
            skkbot2::log::error("[SKC] self-test: follow-tps marker did not "
                                "round-trip");
            return false;
        }
        if (encode(*followBack) != followBytes) {
            skkbot2::log::error("[SKC] self-test: follow-tps macro is not "
                                "byte-stable");
            return false;
        }
    }

    // Canonicalisation: an importer that fills fps with the tick rate must end
    // up as the marker, not as a pinned number. Playback of the second form
    // turns FPS on at a fixed rate instead of following tps - the same display
    // in the common case, a different instruction in the contract.
    {
        Macro equal = macro;  // tps 360
        equal.header.fps = 360.0f;
        const auto equalBytes = encode(equal);
        auto equalBack = decode(equalBytes.data(), equalBytes.size());
        if (!equalBack || equalBack->header.fps != 0.0f ||
            !std::signbit(equalBack->header.fps)) {
            skkbot2::log::error("[SKC] self-test: fps == tps was not normalised "
                                "to the follow-tps marker");
            return false;
        }
    }

    // gapSplitSafe() must track previousHold exactly the way the ENCODER does:
    // only a Paired click advances it, because only a Paired click carries a
    // hold. The check used to advance it on every click, so a PressOnly click
    // (hold 0) reset it - and a gap measured from a release still in the past
    // then passed a check the encoder could not honour. The fixture below is
    // that shape, and the wrong answer is not merely "a different scheme": the
    // FromRelease candidate would code kGapSignBit + 39990 = 72758, which no
    // longer fits the 15 magnitude bits, so a click would come back on another
    // frame the moment that candidate won the size comparison. Verified to
    // fail: with the old tracking restored the check accepts this macro and
    // this self-test reports failure.
    {
        Macro trap;
        trap.header.tps = kDefaultTps;
        trap.header.fps = 0.0f;
        trap.actions = {
            { 0,     1, true,  false },  // A press
            { 40000, 1, false, false },  // A release: Paired, hold 40000
            { 40100, 2, true,  false },  // B: PressOnly, hold 0
            { 40110, 3, true,  false },  // C press: gap from A's release is
                                         // 10 - 40000 = -39990, past the sign bit
            { 40120, 3, false, false },  // C release
        };
        trap.sortActions();
        if (gapSplitSafe(trap)) {
            skkbot2::log::error("[SKC] self-test: gapSplitSafe accepted a macro "
                                "whose gap past the last release does not fit the "
                                "sign bit");
            return false;
        }
        const auto trapBytes = encode(trap);
        auto trapBack = decode(trapBytes.data(), trapBytes.size());
        if (!trapBack || trapBack->actions != trap.actions) {
            skkbot2::log::error("[SKC] self-test: refused-scheme macro did not "
                                "round-trip through the fallback");
            return false;
        }
        // And the refusal has to have reached the file: this macro can only be
        // coded press-to-press.
        if (((trapBack->header.knobs2 >> 6) & 0x01u) != 0u) {
            skkbot2::log::error("[SKC] self-test: refused-scheme macro was still "
                                "coded from the previous release");
            return false;
        }
    }

    // The wide escape: the value model codes 0..255 in a 256-symbol row and
    // everything above it through a uniform byte alphabet, where values at or
    // above 0x10000 set a flag and spend four bytes instead of two. A macro
    // whose largest number is 40100 never touches that branch, and long silences
    // are ordinary in real macros (one real file reaches frame 93032).
    {
        Macro wide;
        wide.header.tps = kDefaultTps;
        wide.header.fps = 0.0f;
        wide.actions = {
            { 0,     1, true,  false },
            { 5,     1, false, false },
            { 70000, 2, true,  false },  // anchor delta 70000 >= 0x10000
            { 70005, 2, false, false },
        };
        wide.sortActions();
        const auto wideBytes = encode(wide);
        auto wideBack = decode(wideBytes.data(), wideBytes.size());
        if (!wideBack || wideBack->actions != wide.actions) {
            skkbot2::log::error("[SKC] self-test: wide-escape macro did not "
                                "round-trip");
            return false;
        }
        if (encode(*wideBack) != wideBytes) {
            skkbot2::log::error("[SKC] self-test: wide-escape macro is not "
                                "byte-stable");
            return false;
        }
    }

    // A good sanity number: 9 events must cost well under 40 bytes.
    const double bytesPerEvent =
        static_cast<double>(bytes.size() - sizeof(Header)) / macro.actions.size();

    // The validator must ACCEPT the fixture above (it is well formed)...
    std::string reason;
    if (!validateActions(macro.actions, reason)) {
        skkbot2::log::error("[SKC] self-test: validator rejected a GOOD stream: {}",
                            reason);
        return false;
    }

    // ...and REJECT the exact corruption a practice respawn produces. This is
    // the shape every unusable macro so far had: a press whose release was
    // lost when the frame clock jumped back, leaving two downs in a row.
    const std::vector<Action> corrupt = {
        { 100, 1, true,  false },   // press
        { 150, 1, false, false },   // release
        { 200, 1, true,  false },   // press
        { 374, 1, true,  false },   // press again with no release in between
        { 412, 1, false, false },
    };
    if (validateActions(corrupt, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED a double press");
        return false;
    }

    // Frames must not go backwards (the delta coder assumes they don't).
    const std::vector<Action> backwards = {
        { 400, 1, true,  false },
        { 300, 1, false, false },
    };
    if (validateActions(backwards, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED backwards frames");
        return false;
    }

    // A trailing hold is legal - the player pressed and never let go.
    const std::vector<Action> trailingHold = {
        { 100, 1, true, false },
        { 150, 1, false, false },
        { 200, 1, true, false },
    };
    if (!validateActions(trailingHold, reason)) {
        skkbot2::log::error("[SKC] self-test: validator rejected a legal trailing hold: {}",
                            reason);
        return false;
    }

    // A LEADING release is the standard "buttons are up at frame 0" baseline
    // and must stay legal - it is exactly what the fixture above starts with,
    // and actionsToClicks encodes it as ReleaseOnly. Assert it separately so a
    // future tightening of the validator cannot silently reject every real
    // macro at save time again.
    if (!validateActions({ { 0, 1, false, false } }, reason)) {
        skkbot2::log::error("[SKC] self-test: validator rejected the leading release baseline: {}",
                            reason);
        return false;
    }

    // ...but a stray SECOND release on a channel that already cycled is not.
    if (validateActions({ { 0, 1, false, false },
                          { 10, 1, true,  false },
                          { 20, 1, false, false },
                          { 30, 1, false, false } }, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED a stray release");
        return false;
    }

    skkbot2::log::info("[SKC] self-test: ok ({} events, {} bytes payload, {:.2} B/event, "
                       "validator accepts good/rejects double-press+backwards)",
               macro.actions.size(), bytes.size() - sizeof(Header), bytesPerEvent);

    return true;
}

std::vector<uint8_t> encodeWith(const Macro& macro, const Variant& variant) {
    const auto clicks = actionsToClicks(macro.actions);
    std::vector<uint8_t> out;
    encodeClicks(out, clicks, variant);
    return out;
}

bool decodeWith(const uint8_t* data, size_t size, const Variant& variant,
                std::vector<Action>& outActions) {
    std::vector<Click> clicks;
    if (!decodeClicks(data, size, clicks, variant)) return false;
    Macro macro;
    macro.actions = clicksToActions(clicks);
    // Must be the same order encode() would produce. Adding tie-breakers here
    // reorders actions that share a frame and makes a perfectly good round-trip
    // look lossy - which is exactly what it did to two of the seven macros
    // before this was fixed.
    macro.sortActions();

    // What the magic used to do, one level down. With no signature in the bytes
    // the header check only proves the knobs are in range, and range is all it
    // can prove: 'AST9' happens to give three bytes that decode into a legal
    // knob set. So the stream itself has to be what rejects the file. A macro
    // that survives this is one whose clicks pair up, run forward and stay in
    // range - arbitrary bytes essentially never do, which makes this a better
    // discriminator than a two-byte constant was: it checks the whole file
    // rather than its first two bytes.
    std::string reason;
    if (!validateActions(macro.actions, reason)) return false;

    outActions = std::move(macro.actions);
    return true;
}

bool gapSplitSafe(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions);
    int64_t previousHold = 0;
    for (const auto& click : clicks) {
        const int64_t gap = static_cast<int64_t>(click.delta) - previousHold;
        // Two bounds, and both were wrong in different directions.
        //
        // The low one admitted exactly -kGapSignBit, which encodes as
        // kGapSignBit + kGapSignBit = 65536 and comes back masked to 15 bits -
        // the click lands at the wrong frame. The decoder reconstructs the sign
        // from the top bit and the magnitude from the low 15, so the magnitude
        // must be strictly below the sign bit.
        //
        // The tracking one used to advance previousHold on EVERY click, while
        // the encoder (and the decoder) only advance it after a Paired click:
        // a PressOnly click carries hold 0 and must leave the previous release
        // where it was. With a large hold behind it the check therefore passed
        // gaps the encoder could not represent - the exact shape of "gapSplitSafe
        // says yes, the encoder silently differs".
        if (gap >= static_cast<int64_t>(kGapSignBit) ||
            gap <= -static_cast<int64_t>(kGapSignBit))
            return false;
        if (click.kind == ClickKind::Paired) previousHold = click.hold;
    }
    return true;
}
} // namespace skk::skc
