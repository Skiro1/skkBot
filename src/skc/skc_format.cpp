#include "skc_format.hpp"

#include "skc_range.hpp"

#include "format/codec.hpp"
#include "logger/logger.hpp"

#include "utils.hpp"

#include <Geode/Geode.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

using namespace geode::prelude;

namespace skk::skc {

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

constexpr uint32_t kMaxActionCount = 5'000'000;


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

}

void Macro::sortActions() {
    std::stable_sort(actions.begin(), actions.end(),
        [](const Action& lhs, const Action& rhs) {
            return lhs.frame != rhs.frame ? lhs.frame < rhs.frame
                                          : lhs.halfTick < rhs.halfTick;
        });
}

bool isValidHeader(const Header& header) {
    if (!std::isfinite(header.tps) || header.tps < 1.0f || header.tps > 1'000'000.0f)
        return false;
    if (!std::isfinite(header.fps) ||
        (header.fps != 0.0f && (header.fps < 1.0f || header.fps > 1'000'000.0f)))
        return false;

    const uint32_t scaleIdx = (header.knobs1 >> 6) & 0x03u;
    const uint32_t budgetIdx = (header.knobs2 >> 4) & 0x03u;
    return scaleIdx < KnobTable::kScaleCount && budgetIdx < KnobTable::kBudgetCount;
}

std::vector<Click> actionsToClicks(const std::vector<Action>& actions, bool subticks) {
    std::vector<Click> clicks;
    struct OrderedClick { Click click; size_t sourceIndex; };
    std::vector<OrderedClick> orderedClicks;
    const auto timestamp = [subticks](const Action& action) {
        return static_cast<uint64_t>(action.frame) * (subticks ? 2u : 1u) +
            (subticks && action.halfTick ? 1u : 0u);
    };
    std::vector<const Action*> timeline;
    timeline.reserve(actions.size());
    for (const auto& action : actions) timeline.push_back(&action);
    std::stable_sort(timeline.begin(), timeline.end(), [&](auto* a, auto* b) {
        return timestamp(*a) < timestamp(*b);
    });
    std::vector<uint64_t> sharedTimes;
    for (size_t i = 0; i < timeline.size();) {
        size_t end = i + 1;
        bool shared = false;
        while (end < timeline.size() && timestamp(*timeline[end]) == timestamp(*timeline[i])) {
            shared |= timeline[end]->button != timeline[i]->button ||
                timeline[end]->player2 != timeline[i]->player2;
            ++end;
        }
        if (shared) sharedTimes.push_back(timestamp(*timeline[i]));
        i = end;
    }
    for (uint32_t channel = 0; channel < 8; ++channel) {
        const uint8_t button = static_cast<uint8_t>((channel >> 1) + 1);
        const bool player2 = (channel & 1u) != 0;

        std::vector<const Action*> events;
        for (const auto& action : actions) {
            if (action.button == button && action.player2 == player2)
                events.push_back(&action);
        }
        if (events.empty()) continue;

        std::stable_sort(events.begin(), events.end(), [subticks](const Action* lhs,
                                                                  const Action* rhs) {
            const uint64_t lt = static_cast<uint64_t>(lhs->frame) * (subticks ? 2u : 1u) +
                                (subticks && lhs->halfTick ? 1u : 0u);
            const uint64_t rt = static_cast<uint64_t>(rhs->frame) * (subticks ? 2u : 1u) +
                                (subticks && rhs->halfTick ? 1u : 0u);
            return lt < rt;
        });

        for (size_t i = 0; i < events.size();) {
            const Action* head = events[i];
            Click click;
            click.button = head->button;
            click.player2 = head->player2;
            const uint64_t anchor = static_cast<uint64_t>(head->frame) *
                                    (subticks ? 2u : 1u) +
                                    (subticks && head->halfTick ? 1u : 0u);
            click.delta = static_cast<uint32_t>(anchor);
            click.hold = 0;

            if (head->pressed) {
                if (i + 1 < events.size() && !events[i + 1]->pressed &&
                    !std::binary_search(sharedTimes.begin(), sharedTimes.end(),
                                        timestamp(*events[i + 1]))) {
                    click.kind = ClickKind::Paired;
                    const Action* release = events[i + 1];
                    const uint64_t releaseAt = static_cast<uint64_t>(release->frame) *
                                               (subticks ? 2u : 1u) +
                                               (subticks && release->halfTick ? 1u : 0u);
                    click.hold = static_cast<uint32_t>(releaseAt - anchor);
                    i += 2;
                } else {
                    click.kind = ClickKind::PressOnly;
                    i += 1;
                }
            } else {
                click.kind = ClickKind::ReleaseOnly;
                i += 1;
            }
            orderedClicks.push_back({click, static_cast<size_t>(head - actions.data())});
        }
    }

    std::sort(orderedClicks.begin(), orderedClicks.end(),
        [](const OrderedClick& lhs, const OrderedClick& rhs) {
            return lhs.click.delta != rhs.click.delta ? lhs.click.delta < rhs.click.delta
                                                      : lhs.sourceIndex < rhs.sourceIndex;
        });
    clicks.reserve(orderedClicks.size());
    for (const auto& entry : orderedClicks) clicks.push_back(entry.click);

    uint32_t previous = 0;
    for (auto& click : clicks) {
        const uint32_t anchor = click.delta;
        click.delta = anchor > previous ? anchor - previous : 0;
        previous = anchor;
    }
    return clicks;
}

void normalizeActionStream(std::vector<Action>& actions, NormalizeReport& report) {
    bool held[2][3] = {};
    std::vector<Action> out;
    out.reserve(actions.size() + 8);

    for (const Action& action : actions) {
        const int button = static_cast<int>(action.button) - 1;
        if (button < 0 || button > 2) {
            out.push_back(action);
            continue;
        }
        const int slot = action.player2 ? 1 : 0;

        if (!action.pressed) {
            if (!held[slot][button]) {
                ++report.droppedRelease;
                continue;
            }
        } else if (held[slot][button]) {
            out.push_back(Action{action.frame, action.button, false, action.player2,
                                 action.halfTick});
            ++report.rebuiltTaps;
            if (report.rebuiltAt.size() < 8) report.rebuiltAt.push_back(action.frame);
        }

        held[slot][button] = action.pressed;
        out.push_back(action);
    }

    actions.swap(out);
}

bool validateActions(const std::vector<Action>& actions, std::string& outReason,
                     bool allowRepeatedPress) {
    bool held[2][3] = {};
    bool seenPress[2][3] = {};

    for (size_t i = 0; i < actions.size(); ++i) {
        const Action& action = actions[i];
        if (i > 0 && (action.frame < actions[i - 1].frame ||
            (action.frame == actions[i - 1].frame &&
             action.halfTick < actions[i - 1].halfTick))) {
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
            if (held[slot][button] && !allowRepeatedPress) {
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
            outReason = "stray release on button " +
                        std::to_string(action.button) +
                        (action.player2 ? " (P2)" : " (P1)") +
                        " at frame " + std::to_string(action.frame) +
                        " - already up, no press outstanding";
            return false;
        } else {
            held[slot][button] = false;
        }
    }

    outReason.clear();
    return true;
}

std::vector<Action> clicksToActions(const std::vector<Click>& clicks, bool subticks) {
    std::vector<Action> actions;
    actions.reserve(clicks.size() * 2);
    uint32_t anchor = 0;
    for (const auto& click : clicks) {
        anchor += click.delta;
        switch (click.kind) {
            case ClickKind::Paired:
                if (subticks) {
                    actions.push_back({anchor / 2u, click.button, true, click.player2,
                                       (anchor & 1u) != 0});
                    const uint32_t releaseAt = anchor + click.hold;
                    actions.push_back({releaseAt / 2u, click.button, false, click.player2,
                                       (releaseAt & 1u) != 0});
                } else {
                    actions.push_back({anchor, click.button, true, click.player2});
                    actions.push_back({anchor + click.hold, click.button, false, click.player2});
                }
                break;
            case ClickKind::PressOnly:
                actions.push_back({subticks ? anchor / 2u : anchor, click.button, true,
                                   click.player2, subticks && (anchor & 1u) != 0});
                break;
            case ClickKind::ReleaseOnly:
                actions.push_back({subticks ? anchor / 2u : anchor, click.button, false,
                                   click.player2, subticks && (anchor & 1u) != 0});
                break;
        }
    }
    return actions;
}


namespace {

using Candidate = Variant;

std::unique_ptr<ValueModelSet> makeModel(const Candidate& cand, ValueCodec codec,
                                         uint32_t shift, bool mix) {
    if (codec != ValueCodec::Bucket) {
        return std::make_unique<DirectContextValueModel>(
            cand.contextCount, directCountOf(codec), shift, cand.countScale, cand.rowBudget);
    }
    return std::make_unique<ContextValueModel>(
        cand.contextCount, shift, cand.countScale, cand.rowBudget, mix);
}
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

void writeLeDouble(uint8_t* dst, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned i = 0; i < 8; ++i)
        dst[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFFu);
}

void readLeDouble(const uint8_t* src, double& value) {
    uint64_t bits = 0;
    for (unsigned i = 0; i < 8; ++i)
        bits |= static_cast<uint64_t>(src[i]) << (i * 8);
    std::memcpy(&value, &bits, sizeof(value));
}

void writeLeU32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

constexpr size_t kLzHashBits = 18;
constexpr size_t kLzHashSlots = 1u << kLzHashBits;
constexpr size_t kLzMinMatch = 4;
constexpr size_t kLzMaxCheck = 64;
constexpr uint64_t kLzHashStart = 2685821657736338717ull;
constexpr uint64_t kLzHashMix = 11400714819323198485ull;
constexpr uint64_t kLzHashAdd = 7146057691288625177ull;
constexpr uint32_t kLzBuckets = 16;

inline uint32_t lzLengthBucket(size_t len) {
    if (len < 4) return static_cast<uint32_t>(len);
    int width = std::bit_width(static_cast<uint64_t>(len));
    return static_cast<uint32_t>(std::min<int>(4 + (width - 3) * 2 + (static_cast<int>((len >> (width - 2)) & 1)), kLzBuckets - 1));
}

inline uint32_t lzMatchSlot(const std::vector<uint64_t>& history) {
    uint64_t hash = kLzMinMatch * kLzHashStart;
    for (size_t k = history.size() - kLzMinMatch; k < history.size(); ++k)
        hash = (hash ^ history[k]) * kLzHashMix + kLzHashAdd;
    return static_cast<uint32_t>(hash >> (64 - kLzHashBits));
}

inline uint64_t packClickKey(const Click& click) {
    return (static_cast<uint64_t>(click.delta) << 32) |
           (static_cast<uint64_t>(click.hold) << 16) |
           (static_cast<uint64_t>(click.kind) << 4) |
           (static_cast<uint64_t>(click.button) << 1) |
           (click.player2 ? 1ull : 0ull);
}

void encodeClicks(std::vector<uint8_t>& out, const std::vector<Click>& clicks,
                  const Candidate& cand, CostProfile* prof = nullptr) {
    const uint32_t contextCount = cand.contextCount;
    const uint32_t deltaShift = cand.deltaShift;
    RangeEncoder enc;

    SymbolModel kindChangedModel(2u, 4u, cand.countScale, cand.rowBudget);
    SymbolModel kindModel(kKindSymbolCount, 1u, cand.countScale, cand.rowBudget);
    SymbolModel lzHitModel(2u, kLzBuckets, cand.countScale, cand.rowBudget);
    auto deltaModel = makeModel(cand, cand.deltaCodec, cand.deltaShift, cand.mixGap);
    auto holdModel = makeModel(cand, cand.holdCodec, cand.holdShift, cand.mixHold);

    uint32_t kindCtx = 0;
    uint32_t previousKind = ~0u;
    uint32_t previousDelta = 0;
    uint32_t previousHold = 0;
    uint32_t previousDelta2 = 0;
    uint32_t previousCodedValue = 0;
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

    std::vector<uint32_t> lzTable;
    std::vector<uint64_t> lzHistory;
    uint64_t lzHash = 3ull * 2685821657736338717ull;
    size_t lzMatched = 0;
    size_t lzAhead = 0;
    if (cand.lzMode) {
        lzTable.assign(kLzHashSlots, 0xFFFFFFFFu);
        lzHistory.reserve(clicks.size());
    }

    for (const auto& click : clicks) {
        const uint64_t key = cand.lzMode ? packClickKey(click) : 0ull;

        if (cand.lzMode && lzMatched > 0 && lzAhead < lzHistory.size()) {
            const uint32_t bucket = lzLengthBucket(lzMatched);
            if (lzHistory[lzAhead] == key) {
                uint64_t mark = prof ? enc.bitsProduced() : 0;
                lzHitModel.encode(enc, 1u, bucket);
                if (prof) prof->lzHit += enc.bitsProduced() - mark;
                lzMatched++;
                lzAhead++;

                lzHistory.push_back(key);
                if (lzHistory.size() >= kLzMinMatch) {
                    const uint32_t h = lzMatchSlot(lzHistory);
                    lzTable[h] = static_cast<uint32_t>(lzHistory.size());
                }

                previousDelta2 = previousDelta;
                previousDelta = click.delta;
                if (click.kind == ClickKind::Paired) previousHold = click.hold;
                previousKind = packKind(click.kind, click.button, click.player2);
                int64_t gap = static_cast<int64_t>(click.delta) - static_cast<int64_t>(previousHold);
                if (cand.gapFrom == GapFrom::PressToPress) gap = click.delta;
                previousCodedValue = gap >= 0
                                         ? static_cast<uint32_t>(gap)
                                         : (kGapSignBit + static_cast<uint32_t>(-gap));
                continue;
            } else {
                uint64_t mark = prof ? enc.bitsProduced() : 0;
                lzHitModel.encode(enc, 0u, bucket);
                if (prof) prof->lzHit += enc.bitsProduced() - mark;
                lzMatched = 0;
            }
        }

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
            prof->kindChanged += enc.bitsProduced() - mark;
            mark = enc.bitsProduced();
        }

        const uint32_t kindCode = static_cast<uint32_t>(click.kind);
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

        if (cand.lzMode) {
            lzHistory.push_back(key);
            if (lzHistory.size() >= kLzMinMatch) {
                const uint32_t h = lzMatchSlot(lzHistory);
                const uint32_t at = lzTable[h];
                if (lzMatched == 0 && at != 0xFFFFFFFFu && at < lzHistory.size()) {
                    size_t len = 0;
                    while (len < at && len < kLzMaxCheck) {
                        size_t here = lzHistory.size() - 1 - len;
                        size_t there = at - 1 - len;
                        if (lzHistory[there] != lzHistory[here]) break;
                        ++len;
                    }
                    if (len >= kLzMinMatch) {
                        lzMatched = len;
                        lzAhead = at;
                    }
                }
                lzTable[h] = static_cast<uint32_t>(lzHistory.size());
            }
        }
    }

    if (cand.lzMode && lzMatched > 0) {
        uint64_t mark = prof ? enc.bitsProduced() : 0;
        lzHitModel.encode(enc, 0u, lzLengthBucket(lzMatched));
        if (prof) prof->lzHit += enc.bitsProduced() - mark;
    }

    const uint64_t termMark = prof ? enc.bitsProduced() : 0;
    kindChangedModel.encode(enc, 1u, kindCtx);
    kindModel.encode(enc, kEndKind, 0u);
    if (prof) prof->terminator = enc.bitsProduced() - termMark;

    out = enc.finish();
}

[[nodiscard]] bool decodeClicks(const uint8_t* data, size_t size,
                                std::vector<Click>& clicks,
                                const Candidate& cand,
                                size_t* consumedOut = nullptr) {
    RangeDecoder dec(data, size);
    const uint32_t contextCount = cand.contextCount;

    SymbolModel kindChangedModel(2u, 4u, cand.countScale, cand.rowBudget);
    SymbolModel kindModel(kKindSymbolCount, 1u, cand.countScale, cand.rowBudget);
    SymbolModel lzHitModel(2u, kLzBuckets, cand.countScale, cand.rowBudget);
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

    std::vector<uint32_t> lzTable;
    std::vector<uint64_t> lzHistory;
    uint64_t lzHash = 3ull * 2685821657736338717ull;
    size_t lzMatched = 0;
    size_t lzAhead = 0;
    if (cand.lzMode) {
        lzTable.assign(kLzHashSlots, 0xFFFFFFFFu);
    }

    while (true) {
        if (clicks.size() >= kMaxActionCount) return false;

        if (cand.lzMode && lzMatched > 0 && lzAhead < clicks.size()) {
            const uint32_t bucket = lzLengthBucket(lzMatched);
            const uint32_t hit = lzHitModel.decode(dec, bucket);
            if (hit == 1u) {
                Click click = clicks[lzAhead];
                const uint64_t key = packClickKey(click);
                lzMatched++;
                lzAhead++;

                lzHistory.push_back(key);
                if (lzHistory.size() >= kLzMinMatch) {
                    const uint32_t h = lzMatchSlot(lzHistory);
                    lzTable[h] = static_cast<uint32_t>(lzHistory.size());
                }

                previousDelta2 = previousDelta;
                previousDelta = click.delta;
                if (click.kind == ClickKind::Paired) previousHold = click.hold;
                previousKind = packKind(click.kind, click.button, click.player2);
                int64_t gap = static_cast<int64_t>(click.delta) - static_cast<int64_t>(previousHold);
                if (cand.gapFrom == GapFrom::PressToPress) gap = click.delta;
                previousCodedValue = gap >= 0
                                         ? static_cast<uint32_t>(gap)
                                         : (kGapSignBit + static_cast<uint32_t>(-gap));
                clicks.push_back(click);
                continue;
            } else {
                lzMatched = 0;
            }
        }

        const uint32_t changed = kindChangedModel.decode(dec, kindCtx);
        kindCtx = changed & 3u;

        if (changed != 0u) {
            const uint32_t symbol = kindModel.decode(dec, 0u);
            if (symbol == kEndKind) {
                if (consumedOut) *consumedOut = dec.consumedBytes();
                return true;
            }
            previousKind = symbol;
        }

        Click click = unpackKind(previousKind);
        const uint32_t kindCode = static_cast<uint32_t>(click.kind);
        const uint32_t raw =
            deltaModel->decode(dec, deltaContext(previousDelta, previousHold, previousDelta2, kindCode),
                             previousCodedValue);
        if (cand.gapFrom == GapFrom::PressToPress) {
            click.delta = raw;
        } else {
            const int64_t gap = (raw & kGapSignBit)
                                    ? -static_cast<int64_t>(raw & (kGapSignBit - 1u))
                                    : static_cast<int64_t>(raw);
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

        if (cand.lzMode) {
            const uint64_t key = packClickKey(click);
            lzHistory.push_back(key);
            if (lzHistory.size() >= kLzMinMatch) {
                const uint32_t h = lzMatchSlot(lzHistory);
                const uint32_t at = lzTable[h];
                if (lzMatched == 0 && at != 0xFFFFFFFFu && at < lzHistory.size()) {
                    size_t len = 0;
                    while (len < at && len < kLzMaxCheck) {
                        size_t here = lzHistory.size() - 1 - len;
                        size_t there = at - 1 - len;
                        if (lzHistory[there] != lzHistory[here]) break;
                        ++len;
                    }
                    if (len >= kLzMinMatch) {
                        lzMatched = len;
                        lzAhead = at;
                    }
                }
                lzTable[h] = static_cast<uint32_t>(lzHistory.size());
            }
        }
    }
}

}

const std::vector<Variant>& candidateVariants() {
    using V = ValueCodec;
    using K = ContextKey;
    using G = GapFrom;
    static const std::vector<Variant> list = {
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 256u},
        Variant{0u, 5u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 4u, 64u},
        Variant{0u, 4u, 5u, V::Direct128, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 256u},
        Variant{0u, 4u, 5u, V::Bucket, V::Direct32, K::Modulo, K::Modulo, G::FromRelease, 1u, 320u},
        Variant{16u, 4u, 3u, V::Direct128, V::Direct32, K::PrevHoldBucket, K::Modulo, G::FromRelease, 256u, 32u},
        Variant{0u, 5u, 5u, V::Bucket, V::Bucket, K::Modulo, K::Modulo, G::FromRelease, 4u, 64u},

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
[[nodiscard]] bool parseHeader(const uint8_t* data, size_t size, Header& outHeader,
                               size_t& outPayloadOffset, Candidate& outCand) {
    if (size < kHeaderFixed) return false;

    outHeader.knobs0 = data[0];
    outHeader.knobs1 = data[1];
    outHeader.knobs2 = data[2];
    const bool hasTps = (outHeader.knobs2 & 0x80u) != 0u;

    uint32_t contextCount = outHeader.knobs0 & 0x3Fu;
    size_t rateOffset = kHeaderFixed;
    outHeader.platformerKnown = false;
    outHeader.platformerMode = false;
    outHeader.cbsMode = false;
    outHeader.cosMode = false;
    outHeader.subtickEncoded = false;
    if (contextCount == kCtxExtension) {
        if (size < kHeaderFixed + kExtBytes) return false;
        const uint8_t ext = data[kHeaderFixed];
        contextCount = ext & 0x3Fu;
        if (contextCount == kCtxExtension) return false;
        outHeader.platformerKnown = true;
        outHeader.platformerMode = (ext & kExtPlatformer) != 0;
        rateOffset = kHeaderFixed + kExtBytes;
        if (ext & kExtHasModes) {
            if (size < rateOffset + kModeBytes) return false;
            const uint8_t modes = data[rateOffset];
            if (modes & ~(kModeCBS | kModeCOS | kModeSubticks |
                          kModePlatformerKnown | kModeLz |
                          kModeLockDelta | kModeRealTime | kModePhysics21)) return false;
            outHeader.cbsMode = (modes & kModeCBS) != 0;
            outHeader.cosMode = (modes & kModeCOS) != 0;
            outHeader.subtickEncoded = (modes & kModeSubticks) != 0;
            outHeader.platformerKnown = (modes & kModePlatformerKnown) != 0;
            outHeader.lzMode = (modes & kModeLz) != 0;
            outHeader.lockDeltaMode = (modes & kModeLockDelta) != 0;
            outHeader.realTimeMode = (modes & kModeRealTime) != 0;
            outHeader.physics21 = (modes & kModePhysics21) != 0;
            if (!outHeader.lockDeltaMode && !outHeader.realTimeMode) {
                outHeader.lockDeltaMode = true;
            }
            if (outHeader.subtickEncoded && !outHeader.cbsMode) return false;
            rateOffset += kModeBytes;
        }
    }

    bool hasDoubleTps = false;
    if (hasTps) {
        if (size < rateOffset + kTpsBytes) return false;
        float tpsWire = 0.0f;
        readLeFloat(data + rateOffset, tpsWire);
        hasDoubleTps = std::signbit(tpsWire) && tpsWire == 0.0f;
        if (hasDoubleTps && size < rateOffset + kTpsDoubleBytes) return false;
    }
    if (hasTps) {
        if (hasDoubleTps) {
            readLeDouble(data + rateOffset + 4, outHeader.tps);
            readLeFloat(data + rateOffset + 12, outHeader.fps);
        } else {
            float tpsWire = 0.0f;
            readLeFloat(data + rateOffset, tpsWire);
            outHeader.tps = static_cast<double>(tpsWire);
            readLeFloat(data + rateOffset + 4, outHeader.fps);
        }
    } else {
        outHeader.tps = kDefaultTps;
        outHeader.fps = 0.0f;
    }

    outCand.contextCount = contextCount;
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
    outCand.lzMode = outHeader.lzMode;

    if (!isValidHeader(outHeader)) return false;

    outCand.countScale = KnobTable::kScale[scaleIdx];
    outCand.rowBudget = KnobTable::kBudget[budgetIdx];

    outPayloadOffset = rateOffset + (hasTps
        ? (hasDoubleTps ? kTpsDoubleBytes : kTpsBytes) : 0u);
    return true;
}

struct BestPick {
    Candidate cand;
    std::vector<uint8_t> payload;
};

BestPick pickBest(const std::vector<Click>& clicks, bool allowGapSplit) {
    BestPick best;
    bool have = false;
    for (const auto& candBase : candidateVariants()) {
        if (!allowGapSplit && candBase.gapFrom == GapFrom::FromRelease) continue;
        for (int lzPass = 0; lzPass < 2; ++lzPass) {
            Candidate cand = candBase;
            cand.lzMode = (lzPass == 1);
            std::vector<uint8_t> trial;
            encodeClicks(trial, clicks, cand);
            if (!have || trial.size() < best.payload.size()) {
                best.payload.swap(trial);
                best.cand = cand;
                have = true;
            }
        }
    }
    return best;
}

std::vector<uint8_t> encode(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions, false);

    BestPick best = pickBest(clicks, gapSplitSafe(macro));
    const Candidate& c = best.cand;

    uint32_t scaleIdx = 0, budgetIdx = 0;
    for (uint32_t i = 0; i < KnobTable::kScaleCount; ++i)
        if (KnobTable::kScale[i] == c.countScale) scaleIdx = i;
    for (uint32_t i = 0; i < KnobTable::kBudgetCount; ++i)
        if (KnobTable::kBudget[i] == c.rowBudget) budgetIdx = i;

    float fpsWire = macro.header.fps;
    if (!std::isfinite(fpsWire)) {
        fpsWire = 0.0f;
    } else if (fpsWire > 0.0f && std::abs(fpsWire - macro.header.tps) < 1e-3f) {
        fpsWire = std::copysign(0.0f, -1.0f);
    }
    const float tpsAsFloat = static_cast<float>(macro.header.tps);
    const bool useDoubleTps = static_cast<double>(tpsAsFloat) != macro.header.tps;
    const bool hasTps = macro.header.tps != kDefaultTps ||
                        std::signbit(fpsWire) || fpsWire != 0.0f;
    const bool hasTimingMode = macro.realTimeMode;
    const bool hasModes = c.lzMode || hasTimingMode || macro.physics21;
    const bool hasPlatformerFlag = macro.platformerKnown && macro.platformerMode;
    const bool hasExtension = hasPlatformerFlag || hasModes;
    const size_t tpsTailBytes = useDoubleTps ? kTpsDoubleBytes : kTpsBytes;
    const size_t headerBytes = kHeaderFixed + (hasExtension ? kExtBytes : 0u) +
                               (hasModes ? kModeBytes : 0u) +
                               (hasTps ? (useDoubleTps ? tpsTailBytes : kTpsBytes) : 0u);

    std::vector<uint8_t> out;
    out.reserve(headerBytes + best.payload.size());
    out.resize(headerBytes);

    Header header;
    if (hasExtension) {
        header.knobs0 = static_cast<uint8_t>(kCtxExtension |
                                             (static_cast<uint8_t>(c.deltaCodec) << 6));
    } else {
        header.knobs0 = static_cast<uint8_t>((c.contextCount & 0x3Fu) |
                                             (static_cast<uint8_t>(c.deltaCodec) << 6));
    }
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
    header.platformerKnown = macro.platformerKnown;
    header.platformerMode = macro.platformerMode;
    header.cbsMode = false;
    header.cosMode = false;
    header.subtickEncoded = false;
    header.lzMode = c.lzMode;
    header.lockDeltaMode = macro.lockDeltaMode;
    header.realTimeMode = macro.realTimeMode;
    header.physics21 = macro.physics21;

    out[0] = header.knobs0;
    out[1] = header.knobs1;
    out[2] = header.knobs2;
    size_t rateOffset = kHeaderFixed;
    if (hasExtension) {
        out[kHeaderFixed] = static_cast<uint8_t>((c.contextCount & 0x3Fu) |
            (hasPlatformerFlag ? kExtPlatformer : 0u) |
            (hasModes ? kExtHasModes : 0u));
        rateOffset = kHeaderFixed + kExtBytes;
        if (hasModes) {
            out[rateOffset] = static_cast<uint8_t>(
                (macro.platformerKnown ? kModePlatformerKnown : 0u) |
                (c.lzMode ? kModeLz : 0u) |
                (macro.lockDeltaMode ? kModeLockDelta : 0u) |
                (macro.realTimeMode ? kModeRealTime : 0u) |
                (macro.physics21 ? kModePhysics21 : 0u));
            rateOffset += kModeBytes;
        }
    }
    if (hasTps) {
        if (useDoubleTps) {
            writeLeFloat(out.data() + rateOffset, kTpsDoubleMarker);
            writeLeDouble(out.data() + rateOffset + 4, header.tps);
            writeLeFloat(out.data() + rateOffset + 12, header.fps);
        } else {
            writeLeFloat(out.data() + rateOffset, static_cast<float>(header.tps));
            writeLeFloat(out.data() + rateOffset + 4, header.fps);
        }
    }

    out.insert(out.end(), best.payload.begin(), best.payload.end());
    const bool seedInFile =
        macro.wireSeedStamp != 0 ? macro.wireSeedStamp > 0
                                 : geode::Mod::get()->getSavedValue<bool>(
                                       "gui_record_seed", true);
    if (seedInFile) {
        appendRngJournal(out, macro.rngJournal);
    }
    return out;
}

CostProfile profile(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions, false);
    const BestPick best = pickBest(clicks, gapSplitSafe(macro));

    CostProfile prof;
    std::vector<uint8_t> ignored;
    encodeClicks(ignored, clicks, best.cand, &prof);
    return prof;
}

namespace {

std::optional<Macro> decodeNative(const uint8_t* data, size_t size,
                                  size_t* droppedRelease) {
    if (!data) return std::nullopt;

    Header header;
    size_t payloadOffset = 0;
    Candidate cand;
    if (!parseHeader(data, size, header, payloadOffset, cand))
        return std::nullopt;

    Macro macro;
    macro.header = header;
    macro.platformerKnown = header.platformerKnown;
    macro.platformerMode = header.platformerMode;
    macro.cbsMode = header.cbsMode;
    macro.cosMode = header.cosMode;
    macro.subtickEncoded = header.subtickEncoded;
    macro.lockDeltaMode = header.lockDeltaMode;
    macro.realTimeMode = header.realTimeMode;
    macro.physics21 = header.physics21;
    macro.header.physics21 = header.physics21;

    const size_t payloadSize = size - payloadOffset;
    if (payloadSize == 0) {
        return std::nullopt;
    }

    std::vector<Click> clicks;
    size_t streamBytes = 0;
    if (!decodeClicks(data + payloadOffset, payloadSize, clicks, cand,
                      &streamBytes))
        return std::nullopt;
    if (streamBytes < payloadSize) {
        const size_t boundary = payloadOffset + streamBytes;
        const size_t back = std::min<size_t>(streamBytes, 8u);
        (void)skk::skc::locateAndParseRngJournal(data + boundary - back,
                                                 size - boundary + back,
                                                 macro.rngJournal);
    }

    macro.actions = clicksToActions(clicks, header.subtickEncoded);
    macro.sortActions();

    if (macro.cbsMode || macro.cosMode || macro.subtickEncoded) {
        for (auto& action : macro.actions) {
            action.halfTick = false;
        }
        macro.cbsMode = false;
        macro.cosMode = false;
        macro.subtickEncoded = false;
        macro.header.cbsMode = false;
        macro.header.cosMode = false;
        macro.header.subtickEncoded = false;
        skkbot2::log::info(
            "[SKC] file was recorded with CBS/COS - SKC is vanilla-only now, "
            "the sub-step marks were dropped and the session runs vanilla");
    }

    if (droppedRelease) {
        *droppedRelease = 0;
        bool held[2][3] = {};
        bool seenPress[2][3] = {};
        std::erase_if(macro.actions, [&](const Action& action) {
            if (action.button < 1 || action.button > 3) return false;
            const int slot = action.player2 ? 1 : 0;
            const int button = action.button - 1;
            if (!action.pressed && seenPress[slot][button] &&
                !held[slot][button]) {
                ++*droppedRelease;
                return true;
            }
            held[slot][button] = action.pressed;
            seenPress[slot][button] |= action.pressed;
            return false;
        });
    }

    std::string reason;
    if (!validateActions(macro.actions, reason, true)) return std::nullopt;
    return macro;
}

}

[[nodiscard]] std::optional<Macro> decode(const uint8_t* data, size_t size) {
    return decodeNative(data, size, nullptr);
}

std::optional<Header> readHeader(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;

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

    if (auto macro = decode(data.data(), data.size()))
        return macro;

    size_t droppedRelease = 0;
    auto macro = decodeNative(data.data(), data.size(), &droppedRelease);
    if (!macro) return std::nullopt;

    skkbot2::log::warn(
        "[SKC] SALVAGED {} - dropped {} stray release(s); "
        "native presses preserved, no releases inserted; re-verify the replay.",
        skk::pathToUtf8(path.filename()), droppedRelease);
    return macro;
}

bool saveToFile(const std::filesystem::path& path, const Macro& macro) {
    const auto bytes = encode(macro);
    if (bytes.empty())
        return false;

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

bool selfTest() {
    {
        Macro ordered;
        ordered.wireSeedStamp = -1;
        ordered.actions = {{1, 1, true, false}, {2, 2, true, false},
                           {10, 2, false, false}, {10, 1, false, false},
                           {11, 1, true, true}, {11, 1, true, false},
                           {12, 2, true, false}, {12, 1, false, false}};
        const auto bytes = encode(ordered);
        const auto back = decode(bytes.data(), bytes.size());
        if (!back || back->actions != ordered.actions) {
            skkbot2::log::error("[SKC] self-test BUG-139: simultaneous input order changed");
            return false;
        }
    }
    for (const uint32_t secondFrame : {13663u, 13664u}) {
        Macro native;
        native.wireSeedStamp = -1;
        native.actions = {{13663, 1, true, false},
                          {secondFrame, 1, true, false},
                          {13665, 1, false, false}};
        const auto bytes = encode(native);
        auto back = decode(bytes.data(), bytes.size());
        if (!back || back->actions != native.actions || encode(*back) != bytes) {
            skkbot2::log::error("[SKC] self-test BUG-138: native repeated press changed");
            return false;
        }
        auto foreign = native.actions;
        NormalizeReport report;
        normalizeActionStream(foreign, report);
        if (report.rebuiltTaps != 1 || foreign.size() != 4) {
            skkbot2::log::error("[SKC] self-test BUG-138: foreign tap repair changed");
            return false;
        }
    }

    Macro macro;
    macro.header.tps = 360.0f;
    macro.header.fps = 144.0f;
    macro.actions = {
        { 0,     1, false, false },
        { 1,     1, true,  false },
        { 2,     1, false, false },
        { 120,   1, true,  false },
        { 121,   1, false, false },
        { 240,   3, true,  false },
        { 290,   3, false, false },
        { 300,   2, true,  true  },
        { 4000,  1, true,  false },
        { 4001,  1, false, false },
    };
    macro.sortActions();

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
    if (bytes.size() <= kHeaderFixed) {
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

    const auto reencoded = encode(*decoded);
    if (reencoded != bytes) {
        skkbot2::log::error("[SKC] self-test: re-encode is not byte-stable");
        return false;
    }

    {
        if (decoded->cbsMode || decoded->cosMode || decoded->subtickEncoded ||
            encode(*decoded) != bytes) {
            skkbot2::log::error("[SKC] self-test: legacy Vanilla macro changed wire shape");
            return false;
        }
        Macro cbs = macro;
        cbs.cbsMode = true;
        cbs.cosMode = true;
        cbs.actions[1].halfTick = true;
        cbs.actions[4].halfTick = true;
        const auto cbsBytes = encode(cbs);
        if (cbsBytes != bytes) {
            skkbot2::log::error("[SKC] self-test: CBS flags reached the wire - "
                                "encode must stay vanilla-only");
            return false;
        }
        auto cbsBack = decode(cbsBytes.data(), cbsBytes.size());
        if (!cbsBack || cbsBack->cbsMode || cbsBack->cosMode ||
            cbsBack->subtickEncoded ||
            std::any_of(cbsBack->actions.begin(), cbsBack->actions.end(),
                        [](const Action& action) { return action.halfTick; }) ||
            cbsBack->actions != decoded->actions) {
            skkbot2::log::error("[SKC] self-test: stale CBS marks normalized "
                                "away incorrectly");
            return false;
        }
    }

    {
        Macro rt = macro;
        rt.lockDeltaMode = false;
        rt.realTimeMode = true;
        const auto rtBytes = encode(rt);
        auto rtBack = decode(rtBytes.data(), rtBytes.size());
        if (!rtBack || !rtBack->realTimeMode || rtBack->lockDeltaMode ||
            rtBack->actions != rt.actions ||
            rtBytes.size() != bytes.size() + kExtBytes + kModeBytes) {
            skkbot2::log::error("[SKC] self-test: Real Time metadata check failed");
            return false;
        }
        if (encode(*rtBack) != rtBytes) {
            skkbot2::log::error("[SKC] self-test: Real Time re-encode is not byte-stable");
            return false;
        }
        if (!decoded->lockDeltaMode || decoded->realTimeMode) {
            skkbot2::log::error("[SKC] self-test: default timing mode is not Lock Delta");
            return false;
        }
    }

    {
        Macro plain = macro;
        plain.header.tps = kDefaultTps;
        plain.header.fps = 0.0f;
        const auto plainBytes = encode(plain);
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

    {
        Macro follow = macro;
        follow.header.tps = kDefaultTps;
        follow.header.fps = -0.0f;
        const auto followBytes = encode(follow);
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

    {
        if (decoded->platformerKnown) {
            skkbot2::log::error("[SKC] self-test: unextended macro decoded as a "
                                "known mode");
            return false;
        }

        Macro platformer = macro;
        platformer.platformerMode = true;
        platformer.platformerKnown = true;
        const auto platformerBytes = encode(platformer);
        if (platformerBytes.size() != bytes.size() + kExtBytes) {
            skkbot2::log::error("[SKC] self-test: platformer extension costs {} B, "
                                "expected {}",
                                platformerBytes.size() - bytes.size(), kExtBytes);
            return false;
        }

        auto platformerBack =
            decode(platformerBytes.data(), platformerBytes.size());
        if (!platformerBack || !platformerBack->platformerKnown ||
            !platformerBack->platformerMode) {
            skkbot2::log::error("[SKC] self-test: platformer flag did not "
                                "round-trip");
            return false;
        }
        if (encode(*platformerBack) != platformerBytes) {
            skkbot2::log::error("[SKC] self-test: platformer macro is not "
                                "byte-stable");
            return false;
        }

        Macro platformerPlain = platformer;
        platformerPlain.header.tps = kDefaultTps;
        platformerPlain.header.fps = 0.0f;
        const auto platformerPlainBytes = encode(platformerPlain);
        auto platformerPlainBack =
            decode(platformerPlainBytes.data(), platformerPlainBytes.size());
        if (!platformerPlainBack || !platformerPlainBack->platformerKnown ||
            !platformerPlainBack->platformerMode) {
            skkbot2::log::error("[SKC] self-test: platformer flag with default "
                                "rates did not round-trip");
            return false;
        }
    }

    {
        Macro equal = macro;
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

    {
        for (const auto [tps, expectedTail] : {
                 std::pair<double, size_t>{240.5, kTpsBytes},
                 {360.25, kTpsBytes},
                 {144.144, kTpsDoubleBytes}}) {
            Macro fractional = macro;
            fractional.header.tps = tps;
            fractional.header.fps = 60.0f;
            const auto fractionalBytes = encode(fractional);
            auto fractionalBack = decode(fractionalBytes.data(), fractionalBytes.size());
            size_t tailStart = kHeaderFixed;
            if ((fractionalBytes[0] & 0x3Fu) == kCtxExtension) tailStart += kExtBytes;
            float tpsMarker = 1.0f;
            if (fractionalBytes.size() >= tailStart + 4)
                readLeFloat(fractionalBytes.data() + tailStart, tpsMarker);
            const bool correctTail = fractionalBytes.size() >= tailStart + expectedTail &&
                (expectedTail == kTpsDoubleBytes
                    ? (tpsMarker == 0.0f && std::signbit(tpsMarker))
                    : (tpsMarker != 0.0f || !std::signbit(tpsMarker)));
            if (!fractionalBack || fractionalBack->header.tps != tps ||
                !correctTail || encode(*fractionalBack) != fractionalBytes) {
                skkbot2::log::error(
                    "[SKC] self-test: fractional TPS {} failed exact round-trip/tail check",
                    tps);
                return false;
            }
            if (expectedTail == kTpsDoubleBytes) {
                auto truncated = fractionalBytes;
                truncated.resize(tailStart + kTpsDoubleBytes - 1);
                if (decode(truncated.data(), truncated.size())) {
                    skkbot2::log::error(
                        "[SKC] self-test: truncated double-TPS tail was accepted");
                    return false;
                }
            }
        }
    }

    {
        Macro trap;
        trap.header.tps = kDefaultTps;
        trap.header.fps = 0.0f;
        trap.actions = {
            { 0,     1, true,  false },
            { 40000, 1, false, false },
            { 40100, 2, true,  false },
            { 40110, 3, true,  false },
            { 40120, 3, false, false },
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
        if (((trapBack->header.knobs2 >> 6) & 0x01u) != 0u) {
            skkbot2::log::error("[SKC] self-test: refused-scheme macro was still "
                                "coded from the previous release");
            return false;
        }
    }

    {
        Macro wide;
        wide.header.tps = kDefaultTps;
        wide.header.fps = 0.0f;
        wide.actions = {
            { 0,     1, true,  false },
            { 5,     1, false, false },
            { 70000, 2, true,  false },
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

    {
        Macro repeating;
        repeating.header.tps = 240.0;
        repeating.header.fps = 0.0f;
        for (uint32_t i = 0; i < 200; ++i) {
            repeating.actions.push_back({ i * 10, 1, true, false });
            repeating.actions.push_back({ i * 10 + 4, 1, false, false });
        }
        repeating.sortActions();
        const auto lzBytes = encode(repeating);
        auto lzBack = decode(lzBytes.data(), lzBytes.size());
        if (!lzBack || lzBack->actions != repeating.actions) {
            skkbot2::log::error("[SKC] self-test: repeating LZ macro did not round-trip");
            return false;
        }
        if (!lzBack->header.lzMode) {
            skkbot2::log::error("[SKC] self-test: repeating LZ macro did not select lzMode");
            return false;
        }
        if (encode(*lzBack) != lzBytes) {
            skkbot2::log::error("[SKC] self-test: repeating LZ macro is not byte-stable");
            return false;
        }
    }

    size_t wireHeader = kHeaderFixed;
    if ((bytes[0] & 0x3Fu) == kCtxExtension) wireHeader += kExtBytes;
    if ((bytes[2] & 0x80u) != 0u) wireHeader += kTpsBytes;
    const size_t payloadBytes = bytes.size() - wireHeader;

    const double bytesPerEvent =
        static_cast<double>(payloadBytes) / macro.actions.size();

    std::string reason;
    if (!validateActions(macro.actions, reason)) {
        skkbot2::log::error("[SKC] self-test: validator rejected a GOOD stream: {}",
                            reason);
        return false;
    }

    const std::vector<Action> corrupt = {
        { 100, 1, true,  false },
        { 150, 1, false, false },
        { 200, 1, true,  false },
        { 374, 1, true,  false },
        { 412, 1, false, false },
    };
    if (validateActions(corrupt, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED a double press");
        return false;
    }

    const std::vector<Action> backwards = {
        { 400, 1, true,  false },
        { 300, 1, false, false },
    };
    if (validateActions(backwards, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED backwards frames");
        return false;
    }

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

    if (!validateActions({ { 0, 1, false, false } }, reason)) {
        skkbot2::log::error("[SKC] self-test: validator rejected the leading release baseline: {}",
                            reason);
        return false;
    }

    if (validateActions({ { 0, 1, false, false },
                          { 10, 1, true,  false },
                          { 20, 1, false, false },
                          { 30, 1, false, false } }, reason)) {
        skkbot2::log::error("[SKC] self-test: validator ACCEPTED a stray release");
        return false;
    }

    skkbot2::log::info("[SKC] self-test: ok ({} events, {} bytes payload, {:.2} B/event, "
                       "validator accepts good/rejects double-press+backwards)",
               macro.actions.size(), payloadBytes, bytesPerEvent);

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
    macro.sortActions();

    std::string reason;
    if (!validateActions(macro.actions, reason, true)) return false;

    outActions = std::move(macro.actions);
    return true;
}

bool gapSplitSafe(const Macro& macro) {
    const auto clicks = actionsToClicks(macro.actions);
    int64_t previousHold = 0;
    for (const auto& click : clicks) {
        const int64_t gap = static_cast<int64_t>(click.delta) - previousHold;
        if (gap >= static_cast<int64_t>(kGapSignBit) ||
            gap <= -static_cast<int64_t>(kGapSignBit))
            return false;
        if (click.kind == ClickKind::Paired) previousHold = click.hold;
    }
    return true;
}
}
