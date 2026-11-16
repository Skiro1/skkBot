#pragma once

// Astral (.ast) decoder that runs the official converter inside a WebAssembly
// runtime.
//
// Why this exists: the .ast container is a zstd-compressed bit stream whose
// record layout we only partially recovered. Every native guess that matched a
// few macros broke on the next one - the two files that carried loop blocks
// were refused outright, and one of them parses perfectly on the official
// converter, so the "loop block" we believed we saw was our own misalignment.
//
// Rather than keep guessing, we run the converter itself. It is the reference
// implementation, it handles all 47 formats, and we use only the AST one.
//
// Verified against seven real macros: the click stream this produces is
// byte-identical to what the browser bundle produces, and matches the exported
// .txt macros in length, per-player split and final frame.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skk::ast::wasm {

// Converter format ids, found by probing every id it accepts. 26 is the AST
// reader we load with; 47 writes "SLC3RPLY", the one format whose record carries
// (delta << 4) | (button << 2) | (p2 << 1) | holding. (22 looks identical but
// writes an atom our reader refuses, so 47 it is.)
inline constexpr uint32_t kFormatAstV9 = 26;
inline constexpr uint32_t kFormatSlcV3 = 47;

// Click kinds, as stored by the converter's Click struct.
enum class Kind : uint8_t {
    Click = 0,
    Release = 1,
    Skip = 2,
};

struct Click {
    uint32_t frame = 0;
    Kind p1 = Kind::Skip;
    Kind p2 = Kind::Skip;
};

struct Output {
    double tps = 240.0;
    std::vector<Click> clicks;
};

// Decodes one .ast file. On failure returns false and fills outReason.
[[nodiscard]] bool convert(std::span<const uint8_t> data, Output& out,
                         std::string& outReason);

// Saves the last converted macro in another format the converter supports,
// and returns the raw bytes. The wasm export surface only exposes frame/p1/p2
// per click, but the converter's own structs carry button, hold and per-frame
// fps - and formats that store those, like SLC, would hand them over intact
// where the per-click getters cannot. Diagnostic: the returned bytes are
// whatever the converter produced, not a file we know how to read yet.
[[nodiscard]] bool saveAs(std::span<const uint8_t> data, uint32_t formatId,
                          std::vector<uint8_t>& out, std::string& outReason);

}  // namespace skk::ast::wasm