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

## /EHa must propagate to CONSUMERS, not just this project's own targets

LuaCpp is meant to be consumed as a library (add_subdirectory/FetchContent/
find_package) by a much bigger project. Mutating the global CMAKE_CXX_FLAGS
(the original /EHa fix) or using PRIVATE target_compile_options only affects
translation units built from THIS CMakeLists.txt - it does NOT reach a
consuming project's own targets that #include Lua.hpp and call into it.
Those consumer TUs still need /EHa themselves (since they may have try/catch
frames that Lua's exceptions propagate through, e.g. trampoline()'s
try/catch, or any RAII objects on the stack during a Lua call).

Fix: attach /EHa (and /bigobj, needed because /EHa generates more unwind
metadata) as a PUBLIC usage requirement directly on the `lua_static` CMake
target:
```cmake
if(MSVC)
  target_compile_options(lua_static PUBLIC /EHa /bigobj)
endif()
```
PUBLIC (not PRIVATE) means CMake automatically appends these flags to the
compile command line of ANY target that does
target_link_libraries(<consumer> PRIVATE|PUBLIC lua_static) - including in
a completely separate top-level project - via INTERFACE_COMPILE_OPTIONS
propagation. No CMAKE_CXX_FLAGS mutation needed at all anymore; removed it.

MSVC resolves conflicting /EH* switches by taking the last one specified on
the command line, and CMake always places a target's own compile options
after the directory-wide defaults, so the consumer's own default /EHsc is
reliably overridden without the consumer having to do anything themselves.

Verified this actually works: rebuilt from scratch and saw
`cl : Command line warning D9025 : overriding '/EHs' with '/EHa'` appear for
ALL THREE targets (lua_static, main, tests) - including main/tests, which
have no /EH flags of their own and only do target_link_libraries(...
lua_static) - proving transitive propagation works exactly as intended.
168/168 tests still pass (Release x5, Debug x3, no flakiness).

Also noticed (informational, not a build failure): with /EHa, MSVC emits
`C4297: 'luaD_throw'/'LUAI_TRY': function assumed not to throw an exception
but does ... The function is extern "C" and /EHc was specified` for a
couple of functions in ldo.c. This is because MSVC still assumes extern "C"
functions don't throw by default even under /EHa (unless /EHc- is also
given) - this is a real caveat of Lua's own C++-exception design (it throws
across an extern "C" boundary), but it's a warning not an error, doesn't
fail the build (no /WX on lua_static), and doesn't affect correctness
(confirmed by the passing tests) - just a nuance worth knowing about.

## Made CMakeLists.txt friendly for library consumption (add_subdirectory)

Rewrote CMakeLists.txt so LuaCpp behaves well as a dependency in a much
bigger project, not just standalone. Key changes:

1. LUACPP_IS_TOP_LEVEL detection:
   `if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)`.
2. New options, all defaulting to LUACPP_IS_TOP_LEVEL (so OFF automatically
   when included via add_subdirectory/FetchContent):
   - LUACPP_BUILD_EXAMPLES (build the sample executable)
   - LUACPP_BUILD_TESTS (fetch Catch2, build+register the test suite)
   - LUACPP_ENABLE_ASAN (renamed from the too-generic ENABLE_ASAN)
3. CMAKE_C_STANDARD/CMAKE_CXX_STANDARD/CMAKE_EXPORT_COMPILE_COMMANDS are
   only set globally when LUACPP_IS_TOP_LEVEL - setting them unconditionally
   would leak onto every target in a parent project that doesn't override
   them. Instead, `target_compile_features(lua_static PUBLIC cxx_std_17)`
   enforces C++17 only for actual consumers of LuaCpp.
4. Renamed the `main`/`tests` executable targets to `luacpp_example`/
   `luacpp_tests` - "main" and "tests" are very likely names a consuming
   project already uses for its own targets; a plain add_subdirectory()
   would then fail with a duplicate-target-name configure error. Verified
   this fix by creating a throwaway consumer project that add_subdirectory
   ()'d LuaCpp AND defined its own targets literally named `main` and
   `tests` linking against LuaCpp::LuaCpp - configured and built cleanly
   with zero collisions (and LuaCpp's own luacpp_example/luacpp_tests
   correctly did NOT get built at all, since the *_BUILD_* options default
   to OFF when not top-level).
5. Added a proper consumer-facing target: `LuaCpp::LuaCpp` (ALIAS for a new
   INTERFACE library `LuaCpp`) that bundles `link against lua_static` +
   `add src/ (containing Lua.hpp) to the include path` into one line for
   consumers: `target_link_libraries(your_target PRIVATE LuaCpp::LuaCpp)`.
   This also transitively carries lua_static's PUBLIC /EHa (MSVC) and
   cxx_std_17 requirements automatically.
6. Guarded against duplicate-inclusion / diamond dependencies: wrapped the
   whole lua_static definition in `if(NOT TARGET lua_static)`, the LuaCpp
   INTERFACE target in `if(NOT TARGET LuaCpp)`, and the Catch2 fetch in
   `if(NOT TARGET Catch2::Catch2WithMain)` (in case a parent project
   already uses Catch2 itself, avoiding a GIT_TAG/version clash).
7. Renamed the top-level project() name from `lua_project` to `LuaCpp` for
   clarity (mostly cosmetic - doesn't affect target names).
8. Dropped `C` from `project(LuaCpp C CXX)` -> `project(LuaCpp CXX)`, and
   removed the now-dead `CMAKE_C_STANDARD`/`CMAKE_C_STANDARD_REQUIRED`
   settings. No .c file is ever compiled directly as C anymore - every Lua
   source goes through the single generated lua_cxx_unity.cpp wrapper,
   forced to LANGUAGE CXX - so CMake no longer needs to probe for a C
   compiler at all. Verified: fresh configure no longer prints "-- The C
   compiler identification is ..."; full rebuild + ctest still 168/168.

Verified: full standalone rebuild still produces `luacpp_example.exe` /
`luacpp_tests.exe`, ctest still finds and passes all 168 tests unchanged
(CI's exact commands - `cmake -B build -DCMAKE_BUILD_TYPE=Release` +
`cmake --build build --config Release` + `ctest --test-dir build -C
Release` - don't reference target names directly, so nothing needed to
change there). Also did a live end-to-end test of the add_subdirectory
consumer scenario described in point 4 above; the consumer's `main.exe`
(a trivial `#include "Lua.hpp"; int main(){ Lua L; }`) built and ran
successfully (exit code 0), confirming LuaCpp::LuaCpp works as a drop-in
dependency.

## Added test coverage for C++ exception handling (was a real gap!)

Despite the whole session revolving around exception-handling correctness
(trampoline, /EHa, LUA_USE_LONGJMP removal), none of the original 168 tests
actually threw a C++ exception from inside a registered expose_func/
expose_method/expose_mutable_method callable - confirmed by grepping
src/test.cpp for throw/catch/exception (zero hits) before this. Added 9
new tests (total 177) under a new "[exceptions]" tag, added <stdexcept>
include:
- expose_func: std::exception (std::runtime_error) thrown in a callable
  surfaces as a Lua error via run_script, with the exception's what()
  message present in the error string.
- expose_func: same but via lua.call<>() and with std::logic_error (a
  different std::exception subclass, to confirm the generic
  catch(const std::exception&) isn't accidentally narrow).
- expose_func: after a thrown exception, a subsequent call still succeeds
  (stack-hygiene regression test analogous to the existing "succeeds after
  an error" tests, but triggered by a genuine C++ throw instead of
  luaL_error for a bad arg count/type).
- expose_func: exception thrown mid-call correctly destroys a captured
  shared_ptr owner (via std::weak_ptr expiry check) - confirms stack
  unwinding through the trampoline runs destructors normally instead of
  leaking a reference, the way a raw longjmp would.
- expose_method: std::exception thrown surfaces as a Lua error; and a
  stack-hygiene-after-exception test.
- expose_mutable_method: std::exception thrown surfaces as a Lua error;
  self is left unmodified in the Lua-side table when the exception is
  thrown before any mutation happens (guards against partial/corrupt
  writeback); and a stack-hygiene-after-exception test that also confirms
  self *does* get mutated correctly on the following successful call.

Verified: full rebuild + ctest passes 177/177 on both the CI-equivalent
Release flow and the `debug` preset (3x each, no flakiness). Ran the new
tests in isolation too (`luacpp_tests.exe "[exceptions]" -s`) - 9 test
cases / 21 assertions, all passed, with error messages showing e.g.
`[string "boom(-1)"]:1: negative input not allowed` confirming the
exception's what() text correctly reaches the Lua-visible error string.

## Reworked main.cpp into a feature showcase + updated README

src/main.cpp was a single ad-hoc script; rewrote it into one demo_*()
function per LuaCpp feature, all called in sequence from main():
demo_run_script, demo_assign, demo_call, demo_struct_binding (incl. nested
struct/Rect + construction from a Lua table literal), demo_expose_func
(scalar/void/tuple returns + the shared_ptr<Owner> keep-alive overload via
a "next_id" counter), demo_expose_method, demo_expose_mutable_method,
demo_exception_handling (throwing std::runtime_error from an expose_func
callable and showing the Lua state remains usable after), and
demo_error_handling (syntax error / runtime error() / wrong-arg-count, all
via the {ok, err} tuple pattern). Each prints a "=== Title ===" header.
Verified: builds clean, runs end-to-end with exit code 0, all output
correct; full ctest still 177/177.

Updated README.md to match:
- Building section: lists the actual 3 targets (lua_static, luacpp_example,
  luacpp_tests - not the old main/tests names) and documents the new
  LUACPP_BUILD_EXAMPLES/LUACPP_BUILD_TESTS/LUACPP_ENABLE_ASAN options.
- New "Using LuaCpp in Your Own Project" section (linked from the TOC)
  showing FetchContent + target_link_libraries(your_target PRIVATE
  LuaCpp::LuaCpp), explaining what that interface target bundles
  (lua_static + include paths + transitive cxx_std_17/EHa) and that
  LUACPP_BUILD_* defaults to OFF for consumers so there's no target-name
  collision risk or unwanted Catch2 dependency.
- New "Exception Handling" API section (linked from TOC) documenting the
  std::exception -> Lua error behavior, that the Lua state stays usable
  afterward, that expose_mutable_method leaves self untouched if the
  exception is thrown before mutation, and that non-std::exception types
  are NOT caught and will propagate as raw C++ exceptions.
- expose_func section: added a paragraph + example for the
  std::shared_ptr<Owner> keep-alive overload, linking to LIFETIME.md.
No longer mentions "copy Lua.hpp into your project" as the primary
integration method (superseded by the LuaCpp::LuaCpp CMake target).

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
