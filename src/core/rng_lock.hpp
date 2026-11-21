#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace skk {


struct TripleRngState {
    uint64_t primary = 0;
    uint64_t teleport = 0;
};

struct AdvRandEntry {
    uint64_t* statePtr = nullptr;
    int uniqueID = 0;
};

inline uint64_t lcgAdvance(uint64_t state) {
    return 214013ULL * state + 2531011ULL;
}

inline uint32_t lcgOutput(uint64_t state) {
    return static_cast<uint32_t>((state >> 16) & 0x7FFF);
}

uint64_t* gdPrimaryRng();
uint64_t* gdTeleportRng();

TripleRngState captureTripleRng();

void restoreTripleRng(TripleRngState const& state);

uint64_t currentAttemptSeed();

void advRandomRegister(uint64_t* statePtr, int uniqueID);
void advRandomReseed(uint64_t seedBase);
void advRandomRestore(std::vector<uint64_t> const& savedStates);
std::vector<uint64_t> advRandomCapture();
void advRandomClear();

uint64_t advRandomBase(uint32_t seedSalt, int currentProgress);
void advRandomReseedAt(uint32_t seedSalt, int currentProgress);

bool installRngMidhooks();

void pinTripleSeedAtReset(uint64_t primary, uint64_t teleport);

uint64_t shakeStateGet();
void shakeStateSet(uint64_t state);

void applyRngBoundaryReseed(uint64_t primary, uint64_t teleport,
                            uint32_t seedSalt, int progress);

void regenerateVarianceFromSeed(uint64_t seed, float* varianceTable, size_t count);

}
