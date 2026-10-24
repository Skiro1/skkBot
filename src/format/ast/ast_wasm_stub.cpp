// Non-Windows fallback for the .ast decoder.
//
// The WebAssembly converter runs on WAMR's Windows platform port, which is the
// only one vendored under libs/wamr. Rather than silently producing an empty
// macro, say why the file was refused.

#include "format/ast/ast_wasm.hpp"

namespace skk::ast::wasm {

bool convert(std::span<const uint8_t>, Output& out, std::string& outReason) {
    outReason =
        "разбор .ast доступен только в Windows-сборке: нет WebAssembly-рантайма";
    return false;
}

}  // namespace skk::ast::wasm
