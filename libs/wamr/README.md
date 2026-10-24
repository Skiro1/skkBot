# WAMR (WebAssembly Micro Runtime)

Vendored subset of [wasm-micro-runtime](https://github.com/bytecodealliance/wasm-micro-runtime)
**WAMR-2.0.0**, used only to run Astral's `.ast` converter.

## Why

The `.ast` container is a zstd bit stream whose record layout we recovered only
in part. Every native guess matched a few macros and broke on the next: one file
carrying loop blocks was refused outright, and another - a plain, loop-free macro
- was read as full of loops, which was our own misalignment rather than a
property of the file.

So the reader no longer guesses. It runs Astral's own converter, which is a Rust
program compiled to WebAssembly, and uses only its AST entry point. All seven
real macros now decode to exactly what the browser converter produces.

## What is included

`core/` sources needed for the classic interpreter on Windows:

- `iwasm/include`, `iwasm/common` (incl. `arch/`, `aot/`, `gc/` headers)
- `iwasm/interpreter` - loader, classic interpreter, runtime
- `iwasm/compilation` - headers pulled in by `aot_runtime.h`
- `shared/utils`, `shared/mem-alloc` (incl. `ems/`)
- `shared/platform/{include,windows,common,common/libc-util}`
- `config.h`, `version.h`

Not included: the AOT and fast-JIT backends, the POSIX platform ports, the
wasi-libc libraries, the standalone `iwasm`/`watt` tools and the tests.

`os_mremap_stub.c` is ours, not upstream: WAMR calls `os_mremap` when remapping
shared memory, a POSIX primitive its Windows port never defines. The AST path
uses no shared memory.

## Build settings worth knowing

Set by `CMakeLists.txt`, and each one is load-bearing:

- `__x86_64__` - MSVC leaves it undefined, and WAMR refuses to guess its build
  target without it.
- `WASM_RUNTIME_API_EXTERN=`, `WASM_API_EXTERN=` - both headers assume a Windows
  DLL build and stamp every API with `__declspec(dllimport)`. We link statically.
  This is also what stops `wasm_trap_delete` - declared through the
  `WASM_DECLARE_REF(trap)` macro - from becoming an unresolvable `__imp_` symbol.
- `WASM_ENABLE_BULK_MEMORY=1` - the converter module contains 719 `memory.copy`
  and 554 `memory.fill` instructions.
- `WASM_ENABLE_SIGN_EXTENSION=1` - it uses `i32.extend8_s` and `i32.extend16_s`.
- `WASM_ENABLE_FAST_INTERP=0`, `WASM_ENABLE_REF_TYPES=0`,
  `WASM_ENABLE_THREAD_MGR=0`, `WASM_ENABLE_GC=0` - nothing in the module needs
  them, and they keep the build surface small.

`obot_converter_bg.wasm` is Astral's compiled converter, downloaded from
`https://astral-bot.com/converter-wasm/`. It is their binary, redistributed here
so the mod can decode their files; nothing in it is modified.

## License

Apache License 2.0 with LLVM Exceptions - see `LICENSE` and `ATTRIBUTIONS.md`.
