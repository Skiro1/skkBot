#pragma once

#include "skc_range.hpp"
#include "skc_rng.hpp"

#include <cstdint>
#include <vector>
#include <filesystem>
#include <optional>
#include <string>

namespace skk::skc {


inline constexpr uint32_t kEndKind = 18;

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

enum class ValueCodec : uint8_t { Bucket = 0, Direct32 = 1, Direct64 = 2, Direct128 = 3 };
inline constexpr uint32_t kValueCodecCount = 4u;

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

enum class GapFrom : uint8_t { PressToPress = 0, FromRelease = 1 };
inline constexpr uint32_t kGapFromCount = 2u;

inline constexpr uint32_t kGapSignBit = 0x8000u;


inline constexpr uint8_t kContextPlain = 0;

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
    bool mixGap = false;
    bool mixHold = false;
    bool lzMode = false;
};

struct KnobTable {
    static constexpr uint32_t kScaleCount = 4;
    static constexpr uint32_t kScale[4] = {1u, 4u, 16u, 256u};
    static constexpr uint32_t kBudgetCount = 4;
    static constexpr uint32_t kBudget[4] = {32u, 64u, 256u, 320u};
};

inline constexpr uint32_t kHeaderFixed = 3;
inline constexpr uint32_t kExtBytes = 1;
inline constexpr uint32_t kModeBytes = 1;
inline constexpr uint8_t kExtHasModes = 0x80u;
inline constexpr uint8_t kModeCBS = 0x01u;
inline constexpr uint8_t kModeCOS = 0x02u;
inline constexpr uint8_t kModeSubticks = 0x04u;
inline constexpr uint8_t kModePlatformerKnown = 0x08u;
inline constexpr uint8_t kModeLz = 0x10u;
inline constexpr uint8_t kModeLockDelta = 0x20u;
inline constexpr uint8_t kModeRealTime = 0x40u;
inline constexpr uint8_t kModePhysics21 = 0x80u;

inline constexpr uint32_t kTpsBytes = 8;
inline constexpr uint32_t kTpsDoubleBytes = 16;
inline constexpr float kTpsDoubleMarker = -0.0f;
inline constexpr double kDefaultTps = 240.0;

struct Header {
    uint8_t knobs0 = 0;
    uint8_t knobs1 = 0;
    uint8_t knobs2 = 0;
    double tps = kDefaultTps;
    float fps = 0.0f;
    bool platformerKnown = false;
    bool platformerMode = false;
    bool cbsMode = false;
    bool cosMode = false;
    bool subtickEncoded = false;
    bool lzMode = false;
    bool lockDeltaMode = true;
    bool realTimeMode = false;
    bool physics21 = false;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 24, "in-memory Header is packed");

[[nodiscard]] const std::vector<Variant>& candidateVariants();

struct Action {
    uint32_t frame = 0;
    uint8_t button = 1;
    bool pressed = false;
    bool player2 = false;
    bool halfTick = false;

    bool operator==(const Action& other) const = default;
};

enum class ClickKind : uint8_t {
    Paired = 0,
    PressOnly = 1,
    ReleaseOnly = 2,
};

struct Click {
    ClickKind kind = ClickKind::Paired;
    uint8_t button = 1;
    bool player2 = false;
    uint32_t delta = 0;
    uint32_t hold = 0;
    bool operator==(const Click& other) const = default;
};

struct Macro {
    Header header;
    bool platformerMode = false;
    bool platformerKnown = false;
    bool cbsMode = false;
    bool cosMode = false;
    bool subtickEncoded = false;
    bool lockDeltaMode = true;
    bool realTimeMode = false;
    bool physics21 = false;
    std::vector<Action> actions;
    std::vector<RngJournalEntry> rngJournal;
    int wireSeedStamp = 0;

    [[nodiscard]] double durationSeconds() const {
        if (actions.empty() || header.tps <= 0.0f) return 0.0;
        return static_cast<double>(actions.back().frame) / header.tps;
    }

    void sortActions();
};


[[nodiscard]] bool gapSplitSafe(const Macro& macro);

[[nodiscard]] std::vector<Click> actionsToClicks(const std::vector<Action>& actions,
                                                 bool subticks = false);

[[nodiscard]] std::vector<Action> clicksToActions(const std::vector<Click>& clicks,
                                                  bool subticks = false);

[[nodiscard]] bool validateActions(const std::vector<Action>& actions,
                                   std::string& outReason,
                                   bool allowRepeatedPress = false);


struct NormalizeReport {
    std::size_t droppedRelease = 0;
    std::size_t rebuiltTaps = 0;
    std::vector<uint32_t> rebuiltAt;
};

void normalizeActionStream(std::vector<Action>& actions, NormalizeReport& report);


[[nodiscard]] std::vector<uint8_t> encode(const Macro& macro);

[[nodiscard]] CostProfile profile(const Macro& macro);

[[nodiscard]] std::vector<uint8_t> encodeWith(const Macro& macro, const Variant& variant);
[[nodiscard]] bool decodeWith(const uint8_t* data, size_t size, const Variant& variant,
                              std::vector<Action>& outActions);

[[nodiscard]] std::optional<Macro> decode(const uint8_t* data, size_t size);

[[nodiscard]] std::optional<Header> readHeader(const std::filesystem::path& path);

[[nodiscard]] std::optional<Macro> loadFromFile(const std::filesystem::path& path);
[[nodiscard]] bool saveToFile(const std::filesystem::path& path, const Macro& macro);

[[nodiscard]] bool selfTest();


[[nodiscard]] bool isValidHeader(const Header& header);

}
