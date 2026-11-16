
#include "format/ast/ast_wasm.hpp"

namespace skk::ast::wasm {

bool convert(std::span<const uint8_t>, Output& out, std::string& outReason) {
    outReason =
        "разбор .ast доступен только в Windows-сборке: нет WebAssembly-рантайма";
    return false;
}

}
