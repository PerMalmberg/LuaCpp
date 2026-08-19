# LuaCpp project: "compile Lua as C++" saga

Repo: C:\code\LuaCpp (also seen as /home/permal/code/LuaCpp in another env)
Goal: build Lua 5.5.0 sources (fetched via CMake FetchContent) as C++ instead of C.

## Final working approach (CMakeLists.txt, lua_static target)

Single unity/amalgamated wrapper .cpp (simplified after root-causing the
real bug - see "IMPORTANT CORRECTION" below):
- Generate ONE wrapper at ${CMAKE_BINARY_DIR}/generated/lua_cxx/lua_cxx_unity.cpp
  containing `extern "C" { #include "<file>.c" ... }` for every Lua .c source.
- add_library(lua_static STATIC <that one wrapper .cpp>)
- set_source_files_properties(... PROPERTIES LANGUAGE CXX)
- target_compile_definitions(lua_static PRIVATE LUA_USE_LONGJMP) -- forces
  classic setjmp/longjmp error handling instead of Lua's C++-exception mode
  (ldo.c auto-switches to `throw` when __cplusplus is defined).
- Warnings: -Wall -Wextra -Werror, but downgrade:
  - Linux/GCC: -Wno-error=maybe-uninitialized
  - macOS/AppleClang: -Wno-error=uninitialized (Clang has no
    -Wmaybe-uninitialized flag; using the GCC name errors with
    "unknown warning option" under -Werror)
  - MSVC: /O2 /W3, plus _CRT_SECURE_NO_WARNINGS
- MSVC-only, near the top of CMakeLists.txt (applies to ALL targets, not
  just lua_static): replace the default `/EHsc` with `/EHa` in
  CMAKE_CXX_FLAGS (see "IMPORTANT CORRECTION" below for why). Also add
  `/bigobj` to `main` and `tests` targets since /EHa's extra unwind
  metadata pushed src/test.cpp over MSVC's object-file section limit
  (C1128).

## IMPORTANT CORRECTION - the real root cause of Windows crashes

An earlier theory held that merging all Lua .c files into a single
translation unit caused STATUS_HEAP_CORRUPTION on Windows, and that
splitting into one wrapper .cpp PER Lua source file fixed it. That theory
was WRONG / a red herring: after applying the per-file split and pushing
to CI, Windows CI *still* crashed with STATUS_HEAP_CORRUPTION
(0xc0000374) in the same category of tests (luaL_error / wrong-arg-type /
too-many-or-few-args / stack-hygiene-after-error). The per-file split
alone did not fix it.

The actual root cause: MSVC's default C++ exception model is `/EHsc`
(synchronous exceptions only). Microsoft documents this as unsafe to
combine with setjmp/longjmp - jumping across `/EHsc`-compiled frames via
`longjmp` can corrupt the compiler's table-driven stack-unwind bookkeeping,
especially under optimization (Release builds only - CI's failing runs
were `-C Release`). This exactly matches the observed crash signature and
the fact that only Lua's setjmp/longjmp error-path tests crashed.

Fix: replace `/EHsc` with `/EHa` for the whole project (`/EHa` correctly
integrates SEH-style unwinding with longjmp):
```cmake
if(MSVC)
  string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
```

After adding `/EHa`, re-tested and confirmed the ORIGINAL single-unity-TU
approach (reverting the per-file split) also builds and passes 168/168
tests on Windows in both Release and Debug configs. So the per-file TU
splitting was unnecessary complexity - it happened to "work" once before
purely because that earlier test run didn't trigger the crash
(flaky/non-deterministic UB from the /EHsc+longjmp issue, not because TU
splitting fixed anything). The project has been simplified back to the
single-unity-wrapper approach, now that /EHa is in place as the real fix.

Lesson learned: don't conclude a fix is correct just because symptoms
stop appearing once - STATUS_HEAP_CORRUPTION from undefined behavior can
be intermittent/non-deterministic across builds and runs.

## Simplified Lua.hpp trampoline (removed trampoline_impl/call_impl split)

src/Lua.hpp used to split the C-function trampoline used for expose_func/
expose_method/expose_mutable_method closures into three pieces:
trampoline() -> call_impl() -> trampoline_impl(). This existed purely to
guarantee lua_error() was called from a stack frame with NO live C++
try/catch, because (at the time) lua_error() did a longjmp, and a longjmp
unwinding past an /EHsc try/catch frame corrupted MSVC's SEH handler
chain (silently - broke later, unrelated Lua calls on the same thread,
only observed on Windows CI).

Verified in Lua 5.5's actual ldo.c: `LUAI_THROW(L,c)` is literally
`throw(c)` (a `lua_longjmp*`) when compiled as C++ without
LUA_USE_LONGJMP - i.e. NOT a longjmp at all in that mode. A genuine C++
throw always safely unwinds through arbitrary try/catch frames and runs
destructors, by the language's own guarantees, regardless of /EHsc vs
/EHa. Combined with /EHa now being applied project-wide (which also
makes a hypothetical raw longjmp safe), both original justifications for
the 3-way split are gone.

Simplified: merged trampoline_impl/call_impl directly into a single
trampoline() that does the weak_ptr::lock() + try { (*fn)(L) }
catch(const std::exception&) { luaL_error(...) } inline, calling
luaL_error()/lua_error() directly from within the try/catch frame. This
is safe now for the reasons above.

Verified on Windows: rebuilt + ran ctest 5x each on both the CI-equivalent
Release flow and the `debug` preset - 168/168 passing every time, no
flakiness. Also updated LIFETIME.md item 2 ("Bug fixed along the way")
to note the historical bug/fix and that the split has since been removed.

## Other gotchas encountered
- Lua's headers (lua.h/lauxlib.h/lualib.h) do NOT wrap declarations in
  extern "C" themselves; Lua.hpp already wraps its own #include of them in
  extern "C". So the library's exported symbols must also have C linkage,
  which the extern "C" { #include "x.c" } wrapper trick provides.
- Do not use LANGUAGE CXX directly on the original .c files without the
  extern "C" wrapper trick - definitions get C++-mangled and everything
  fails to link against callers expecting C linkage.
- `/EHa` generates more unwind metadata than `/EHsc`, which pushed
  src/test.cpp's object file over MSVC's section limit
  (`C1128: number of sections exceeded object file format limit`). Fixed
  by adding `/bigobj` to the `main` and `tests` targets.

## Windows local build fix (Debug preset): /O2 vs /RTC1 conflict (D8016)
Repo root on Windows machine: C:\code\LuaCpp
CMakePresets.json has debug/release/relwithdebinfo presets (no "default").
Build commands used: `cmake --preset debug` then `cmake --build --preset debug`;
tests via `ctest --preset debug --output-on-failure`.

Found & fixed a build-breaking bug (not a test failure): CMakeLists.txt
unconditionally applied `/O2` to lua_static/main/tests under MSVC. The Debug
preset's CMAKE_CXX_FLAGS_DEBUG already includes `/RTC1` (runtime checks),
and MSVC errors with `D8016: '/RTC1' and '/O2' command-line options are
incompatible` when both are present - this only breaks Debug builds, not
Release/RelWithDebInfo, since /RTC1 is Debug-only.

Fix: wrap `/O2` in a generator expression so it's skipped for Debug config,
in all three MSVC branches (lua_static, main, tests):
  target_compile_options(<tgt> PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W3...)

## How to reproduce CI locally on Windows
CI (.github/workflows/ci.yml) does:
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
(default VS generator on windows-latest). This differs from the `debug`/
`release` CMake presets used for local dev - make sure to test both when
diagnosing CI-only issues, and prefer reproducing the literal CI commands
first.

## Removed LUA_USE_LONGJMP: Lua now uses native C++ exceptions successfully

With /EHa in place (see above), re-tried letting Lua auto-detect
__cplusplus and use its own C++-exception-based error handling
(LUAI_THROW/LUAI_TRY -> throw/catch(...) in ldo.c at the lua_pcall
boundary) instead of forcing LUA_USE_LONGJMP. This is Lua's own
documented/supported way of embedding in C++, and properly unwinds the
C++ stack (destructors run), unlike raw longjmp.

Removed `target_compile_definitions(lua_static PRIVATE LUA_USE_LONGJMP)`
entirely (left a comment explaining why it's intentionally absent).
Verified on Windows:
- Release (CI-equivalent: cmake -B build -DCMAKE_BUILD_TYPE=Release +
  cmake --build build --config Release + ctest --test-dir build -C
  Release): 168/168 passing, repeated 5x with no flakiness.
- Debug preset (cmake --preset debug + cmake --build --preset debug +
  ctest --preset debug): 168/168 passing, repeated 3x with no flakiness.
- build/Release/main.exe runs cleanly end-to-end (exit code 0).

Conclusion: the original STATUS_HEAP_CORRUPTION crashes were entirely
caused by /EHsc + longjmp, NOT by anything about Lua's C++-exception mode
itself. Now that /EHa is applied project-wide, Lua's native C++ exception
error handling is preferable (more idiomatic C++, proper RAII/destructor
unwinding) and LUA_USE_LONGJMP is no longer needed/set.

## Status as of last update
Simplified to single-unity-TU + /EHa fix, WITHOUT LUA_USE_LONGJMP (Lua
uses native C++ exceptions for error handling). Verified locally on
Windows: both `cmake --preset debug` and the CI-equivalent Release flow
build clean and pass 168/168 ctest repeatedly with no flakiness.
Linux/macOS CI not locally testable but the /EHa fix is MSVC-only (guarded
by `if(MSVC)`) so it doesn't affect those platforms; Linux/GCC and
macOS/Clang don't have the /EHsc-vs-longjmp problem to begin with, and
compiling Lua as C++ there already implied throw/catch semantics all
along (their earlier passing runs used C++ exceptions by default, since
LUA_USE_LONGJMP was Windows/general at the time but harmless everywhere).
