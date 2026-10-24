#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace skk {

// Lock RNG streams: primary (main LCG) + teleport (portal inline LCG) +
// shake (the four C rand() call sites GD uses for camera shake).
// Shake is locked for BOTH SKK and SKC: the chain is pinned at every attempt
// reset and snapshotted into every practice checkpoint, so it is owned
// end-to-end rather than left half-locked.
// GD 2.2081 x64 data globals:
//   GD+0x6C2E90 = primary RNG (GameToolbox::fast_srand/fast_rand)
//   GD+0x6C2EF8 = teleport RNG (portal inline LCG)
//
// All use the MSVC LCG: state = 214013 * state + 2531011
// Output extraction: (state >> 16) & 0x7FFF

struct TripleRngState {
    uint64_t primary = 0;    // GD+0x6C2E90
    uint64_t teleport = 0;   // GD+0x6C2EF8
};

// Per-object advanced random state (2068/1912/3607 triggers).
// Each object gets: seedBase ^ (uniqueID * 2137), then own LCG chain.
struct AdvRandEntry {
    uint64_t* statePtr = nullptr;  // Points into $modify Fields
    int uniqueID = 0;
};

// Advance LCG one step, return the full new state.
inline uint64_t lcgAdvance(uint64_t state) {
    return 214013ULL * state + 2531011ULL;
}

// Extract 15-bit output from LCG state (GD's rand() return value).
inline uint32_t lcgOutput(uint64_t state) {
    return static_cast<uint32_t>((state >> 16) & 0x7FFF);
}

// Direct memory access to GD data globals.
uint64_t* gdPrimaryRng();   // GD+0x6C2E90
uint64_t* gdTeleportRng();  // GD+0x6C2EF8

// Capture current triple RNG state from GD globals.
TripleRngState captureTripleRng();

// Restore triple RNG state into GD globals + bot-owned midhook copies.
void restoreTripleRng(TripleRngState const& state);

// Per-object advanced random management.
void advRandomRegister(uint64_t* statePtr, int uniqueID);
void advRandomReseed(uint64_t seedBase);
void advRandomRestore(std::vector<uint64_t> const& savedStates);
std::vector<uint64_t> advRandomCapture();
void advRandomClear();

// Deterministic per-object reseed derived from the lock seed base and the
// practice-progress position (mirrors refreshRngState's computeLockedSeed).
// Uses the same mt19937 formula so playback reproduces the recorded section's
// per-object rand chain at a practice-restart boundary.
uint64_t advRandomBase(uint32_t seedSalt, int currentProgress);
void advRandomReseedAt(uint32_t seedSalt, int currentProgress);

// Install midhook for teleport LCG interception.
// Called once at mod load. Returns true if all hooks installed.
bool installRngMidhooks();

// Seed pin at level reset: writes primary + teleport GD globals + midhook copy.
void pinTripleSeedAtReset(uint64_t primary, uint64_t teleport);

// Bot-owned shake LCG chain (only advanced while SKC is enabled).
// Exposed so the SKC practice checkpoint can snapshot/restore it alongside the
// primary and teleport globals.
uint64_t shakeStateGet();
void shakeStateSet(uint64_t state);

// Re-seed the locked RNG streams at a practice-restart boundary. Restores the
// GD globals (+ midhook copy) verbatim to the LIVE teleport state captured
// on the first frame after the respawn, AND re-seeds the per-object advanced
// randoms (2068/1912/3607) to the same deterministic base refreshRngState used
// at the matching practice-progress. progress = boundary checkpoint progress.
void applyRngBoundaryReseed(uint64_t primary, uint64_t teleport,
                            uint32_t seedSalt, int progress);

// Regenerate variance table from a seed (GD-compatible formula).
void regenerateVarianceFromSeed(uint64_t seed, float* varianceTable, size_t count);

}
