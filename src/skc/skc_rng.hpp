#pragma once

#include <cstdint>
#include <vector>

namespace skk::skc {


inline constexpr uint64_t kSkcLayerSeed = 0x2545F491ULL;

inline constexpr uint32_t kRngTailVersion = 1u;
inline constexpr uint32_t kRngIdleInterval = 240u;

struct RngJournalEntry {
    uint32_t frame = 0;
    uint64_t primary = 0;
    uint64_t teleport = 0;
    uint64_t shake = 0;
    bool operator==(const RngJournalEntry&) const = default;
};


void appendRngJournal(std::vector<uint8_t>& bytes,
                      const std::vector<RngJournalEntry>& journal);

bool parseRngJournal(const uint8_t* data, size_t size,
                     std::vector<RngJournalEntry>& out);
bool locateAndParseRngJournal(const uint8_t* data, size_t size,
                              std::vector<RngJournalEntry>& out,
                              size_t back = 0);


void forceLayerSeeds();

void journalRecordTick(bool typedInput);

void journalPlaybackTick();

void journalPlaybackCounters(const char* reason);

void journalResetAttempt(uint32_t boundaryFrame, bool wipe);

void journalResetCursor();

[[nodiscard]] bool selfTestRngJournal();

}
