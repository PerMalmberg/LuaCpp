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

## Implemented: Script Auditing & Protection hooks (call tracing, output
## capture, error logging, instruction counting/limit, recursion depth cap)

Added to src/Lua.hpp (declared at namespace scope, above `class Lua`):
```cpp
struct LuaCallTraceEvent { std::string name; bool is_call; int depth; };
using LuaCallTraceCallback = std::function<void(const LuaCallTraceEvent&)>;
using LuaOutputCallback = std::function<void(std::string_view)>;
using LuaErrorLogCallback = std::function<void(std::string_view)>;
```

Design: all four hook-based features (call tracing, instruction counting,
instruction limit, recursion depth cap) share ONE `lua_sethook`
installation, since Lua only allows one hook callback+mask per lua_State.
A private `HookState hooks;` member holds all the state; `refresh_hook_mask()`
recomputes the merged mask (LUA_MASKCALL/LUA_MASKRET/LUA_MASKCOUNT) from
whichever features are active and re-installs `lua_sethook` (or removes it
entirely via `lua_sethook(L, nullptr, 0, 0)` if nothing needs it). A single
static `master_hook(L, ar)` recovers `Lua*` and dispatches by `ar->event` in
member function `dispatch_hook()`.

Sentinel-value convention (avoids extra bools): `instruction_limit == 0`
means disabled, `max_depth == 0` means disabled, `trace_callback` empty
means disabled. Only `counting_enabled` (bool) has no natural sentinel and
stays an explicit flag, since `instruction_count == 0` doesn't imply
"disabled".

**Recovering `Lua*` in static callbacks**: `lua_getextraspace(L)` returns a
`void*` slot with process lifetime tied to the lua_State; store `this`
there once in the constructor:
```cpp
*static_cast<Lua**>(lua_getextraspace(static_cast<lua_State*>(*this))) = this;
```
IMPORTANT GOTCHA: `lua_getextraspace` is a MACRO that does raw pointer
arithmetic on its argument - it needs an actual `lua_State*`, not something
that merely has an implicit user-defined conversion to one. Passing `*this`
directly (relying on `Lua::operator lua_State*()`) fails to compile with a
confusing clang error ("Cannot cast from type 'Lua' to pointer type
'char *'") because the macro expansion tries to do pointer arithmetic on
the `Lua` object itself before any conversion happens. Fix: force the
conversion explicitly with `static_cast<lua_State*>(*this)` before passing
it in. (Ordinary non-macro API calls like `lua_pushnil(*this)` don't have
this problem since normal function argument passing does apply the
implicit conversion.)

**print() output goes nowhere by default** - `captured_print` (a plain
non-capturing `lua_CFunction`, no upvalue machinery needed) is installed as
the `print` global directly in the constructor. It's a no-op unless
`enable_output_capture(cb)` has been called; `disable_output_capture()`
returns to discarding, NOT to writing to stdout - there is no "restore
original print" logic at all, by design (ties into the Sandboxing TODO
item: scripts never get free stdout access via print, even before any
other sandboxing is applied).

**Error logging is a SEPARATE callback from output capture**
(`enable_error_logging`/`disable_error_logging`, backed by
`error_log_callback`, distinct from `output_callback`) - deliberately so a
caller can log internal LuaCpp problems (currently: exceptions thrown by a
user's call-trace callback) without being forced to also capture/discard
print() traffic, and vice versa. Messages are `"[LuaCpp] "`-prefixed,
newline-terminated.

**Call tracing does not store history in `Lua` itself** - events are
pushed synchronously to a user-supplied `LuaCallTraceCallback`; the class
retains zero call-trace state beyond `hooks.call_depth` bookkeeping.
Exceptions thrown by the trace callback are caught (`std::exception`
specifically for `.what()`, then a generic `catch(...)`) and routed through
`log_error()` -> `error_log_callback`; the script itself is never aborted
by a misbehaving trace callback.

**Real bug caught in testing #1 - instruction limit hung forever**: the
`LUA_HOOKCOUNT` branch originally only incremented `hooks.instruction_count`
when `hooks.counting_enabled` was true. But `set_instruction_limit()` alone
(without also calling `enable_instruction_counting()`) doesn't set that
flag - so the counter never advanced and `while true do end` looped
forever, hanging the test suite. Fix: increment `instruction_count`
unconditionally inside `LUA_HOOKCOUNT` - the branch only ever fires when
`LUA_MASKCOUNT` is installed at all, which only happens when counting
and/or the limit is actually active, so the unconditional increment is
always correct there.

**Real bug caught in testing #2 - `%llu` not supported by Lua's own error
formatting**: `luaL_error`/`lua_pushvfstring` only implement Lua's own small
format-specifier subset (`%d`, `%s`, `%f`, `%p`, `%c`, `%U`, `%I`, `%%`) -
NOT the C/glibc `%llu`. Using `%llu` caused the literal text
`"instruction limit exceeded (%llu)"` to appear unformatted in the error
message (silently wrong, not a crash - easy to miss without an exact-match
test). Fix: format the number in C++ first
(`std::to_string(hooks.instruction_limit)`) and pass it through as a plain
`%s`. `%d` with a plain `int` (used for the recursion-depth-cap error
message) is fine since Lua's own `%d` matches a `va_arg(argp, int)`.

Public API added to `Lua`:
- `enable_call_tracing(LuaCallTraceCallback)` / `disable_call_tracing()`
- `enable_output_capture(LuaOutputCallback)` / `disable_output_capture()`
- `enable_error_logging(LuaErrorLogCallback)` / `disable_error_logging()`
- `enable_instruction_counting(int period = 1000)` /
  `disable_instruction_counting()` / `get_instruction_count() const`
- `set_instruction_limit(std::uint64_t limit, int period = 1000)` /
  `clear_instruction_limit()`
- `set_recursion_depth_cap(int max_depth)` / `clear_recursion_depth_cap()`

Tests added in `src/test.cpp` under tags `[call-tracing]`,
`[output-capture]`, `[error-logging]`, `[instruction-counting]`,
`[instruction-limit]`, `[recursion-depth-cap]`, `[hooks-integration]` - 22
new test cases (110 assertions), full suite now 205/205 passing on Linux
(debug preset). `[hooks-integration]` specifically validates that call
tracing + instruction limit + recursion depth cap enabled simultaneously
don't clobber each other's `lua_sethook` registration - the core point of
the merged-mask design.

Also fixed pre-existing style nit while touching `close()`: single-line
`if(closed) return;` was wrapped in `{ }` per project style requested
during this session (brace-wrap all single-line if-blocks in new/touched
code).

Reusable pattern for any FUTURE `lua_sethook`-based feature (e.g.
`LUA_MASKLINE` line-level tracing): add fields to `HookState`, fold the new
mask bit into `refresh_hook_mask()`, add a `case` to `dispatch_hook()`. Do
NOT call `lua_sethook` directly from a new feature's enable/disable method -
always go through `refresh_hook_mask()` so merging keeps working.

## Added log_error() calls at Tier 1 + Tier 2 sites throughout Lua.hpp

After the hook infrastructure above was in place, added `log_error()` calls
at every place that previously failed silently (Tier 1) or only reported
failure via the `{false, msg}` tuple convention (Tier 2), routing them all
through the same `error_log_callback` set by `enable_error_logging`:

Tier 1 (previously fully silent / internal conditions):
- `trampoline()` - stale `weak_ptr<LuaFunc>` (expired registered callable)
  now logs before calling `luaL_error`.
- `trampoline()` - `catch(const std::exception& e)` around a registered
  C++ callable (`expose_func`/`expose_method`/`expose_mutable_method`) now
  logs `e.what()` before converting it to a Lua error. `trampoline` is
  `static`, so it recovers `Lua*` the same way `master_hook` does, via
  `lua_getextraspace(L)`, and guards every `self->log_error(...)` call with
  `if(self)`.

Tier 2 (previously only visible via the returned tuple):
- `run_script()` - both failure paths (`luaL_loadstring` syntax error,
  `lua_pcall` runtime error) now call `log_error(msg)` before returning.
- `call<>()` - all three failure paths ("Not a function", `lua_pcall`
  failure, and the `collect<>()` type-mismatch `catch(const
  std::runtime_error&)`) now call `log_error(...)`.
- `add_method_to_registry` (duplicate `expose_method`/
  `expose_mutable_method` registration) - static function, recovers `Lua*`
  via `lua_getextraspace(L)` the same way, logs the message before
  `throw`ing the `std::runtime_error` (this is a registration-time C++
  exception, not a Lua error, so it can't go through the trampoline's own
  logging path - had to add its own `lua_getextraspace` lookup).

**Important discovered behavior - intentional duplicate log entries**: an
exception thrown from an `expose_func` callable, invoked via
`run_script()`, now produces **2** log entries for the same underlying
failure: one from `trampoline()`'s `catch` (Tier 1, logs the raw
`e.what()`) and one from `run_script()`'s `lua_pcall`-failed branch (Tier
2, logs the resulting Lua error string, which also contains `e.what()`
since `trampoline` formats it via `luaL_error(L, "%s", e.what())`). This is
NOT a bug - it's the natural consequence of instrumenting both layers
independently. A test (`error logging: exception thrown by an expose_func
callable is logged`) explicitly asserts `logged.size() == 2` with a
comment explaining why. If a caller wants exactly one log entry per
failure they should log only via `enable_error_logging` OR inspect the
returned tuple themselves, not both, or dedupe based on content.

By contrast, a *pure Lua-level* error (e.g. `error('kaboom')` called from
within a Lua function invoked via `call<>()`) is logged only ONCE (by
`call<>()`'s own Tier-2 site) since it never passes through the
`trampoline`/`catch(std::exception)` path at all - only errors originating
from C++ exceptions inside registered callables get double-logged.

Added 8 more test cases (`[error-logging]` tag) covering: run_script syntax
error, run_script runtime error, all three call<>() failure paths (one
TEST_CASE with 3 sub-scopes), expose_func exception (double-log
confirmed), and duplicate method registration. Full suite now 210/210
passing.

## log_error() no longer appends a trailing newline

Changed `log_error(std::string_view what)` to build `"[LuaCpp] " + what`
WITHOUT a trailing `'\n'` - the consumer's callback is now responsible for
its own line formatting (append '\n' when writing to a stream, or none at
all when appending to a structured log record/vector). This only affects
`error_log_callback` (via `enable_error_logging`) - `output_callback` (via
`enable_output_capture`, used by `captured_print`) is UNRELATED and still
appends '\n' per print() call, matching Lua's own print() semantics - that
one was not touched.

Updated the doc comments on `log_error` and `enable_error_logging`
accordingly, and fixed/renamed the test that used to assert
`logged[0].back() == '\n'` - now asserts exact equality
`logged[0] == "[LuaCpp] oops"` instead, with the test title changed to
"messages are prefixed and NOT newline-terminated". Full suite still
210/210 after the change.

## Implemented: Memory tracking (get_memory_usage) + Memory cap
## (set_memory_limit/clear_memory_limit)

Switched Lua's `state` member from `luaL_newstate()` to `lua_newstate(&Lua::
limited_alloc, this, luaL_makeseed(nullptr))` - a custom `lua_Alloc` with
`this` as the userdata, so every Lua allocation/reallocation/free is
tracked, and can optionally be capped.

**Lua 5.5 API change caught here**: `lua_newstate` now takes a THIRD
parameter, `unsigned seed` (not just `lua_Alloc f, void *ud` like older Lua
versions) - compiling with only 2 args fails with "no matching function".
`luaL_newstate()` internally calls
`lua_newstate(luaL_alloc, NULL, luaL_makeseed(NULL))` - so
`luaL_makeseed(nullptr)` is the right thing to pass for the seed to match
default behavior (it ignores its `lua_State*` argument entirely and is
safe to call with `nullptr`/before any state exists).

**Member ordering matters again** (same lesson as the hooks work): a new
`MemoryState { std::size_t bytes_used = 0; std::size_t limit = 0; } memory;`
member MUST be declared physically BEFORE `LuaStatePtr state{...}` in the
class body, since C++ constructs members in declaration order and
`state`'s own initializer calls `lua_newstate()` immediately, which makes
allocator calls through `limited_alloc(ud=this, ...)` before the
constructor body even starts - `memory` must already be default-
constructed (zero) by then, or it's UB to touch it. Same applies to
`error_log_callback` (already positioned before `state` from the earlier
hooks work) since `limited_alloc` calls `self->log_error(...)` on cap
breach / real OOM.

**`limited_alloc` allocator semantics** (implements the `lua_Alloc`
contract from the manual): when `ptr == nullptr`, `osize` is NOT a real
byte count - it's a type tag (e.g. `LUA_TSTRING`) for a brand-new
allocation, so it must be ignored for bookkeeping (`old_size = ptr ? osize
: 0`). `nsize == 0` means free (return nullptr, decrement bytes_used by
old_size). Otherwise: if a limit is set and `bytes_used - old_size + nsize
> limit`, reject (return nullptr, log via log_error) WITHOUT touching the
existing block at `ptr` - Lua treats a nullptr return here exactly like a
real out-of-memory condition and raises a catchable "not enough memory"
error via the normal lua_pcall path (run_script/call<> return {false,
msg}), with the existing allocation left valid. Otherwise realloc(ptr,
nsize); if that itself returns nullptr (genuine system OOM) also log and
return nullptr; otherwise update bytes_used and return the new pointer.

**Lost the default panic handler**: `luaL_newstate()` automatically calls
`lua_atpanic(L, &panic)` with Lua's own default panic function (prints to
stderr). Switching to raw `lua_newstate()` means that's no longer
installed automatically - had to add our own `static int panic(lua_State*
L)` (recovers `Lua*` via `lua_getextraspace`, mirrors the default's
behavior/message format but routes through `log_error` instead of stderr)
and call `lua_atpanic(*this, &Lua::panic)` explicitly in the constructor.
Reordered the constructor to set `lua_getextraspace` and call
`lua_atpanic` BEFORE `luaL_openlibs`/installing `print`, so the panic
handler and allocator's `self` lookups are valid from the very first
moment anything could go wrong.

Public API added:
- `set_memory_limit(std::size_t bytes)` / `clear_memory_limit()` - 0 (the
  default) means unlimited.
- `get_memory_usage() const` - always tracked, independent of whether a
  limit is set.

Added 7 new tests (`[memory-limit]` tag): baseline usage > 0 right after
construction (proves the custom allocator is actually wired in), usage
increases as a script allocates tables/strings, a tight cap set relative
to `get_memory_usage() + small_slack` causes a large allocation to fail
with a catchable "not enough memory" error, the failure is also reported
via `enable_error_logging` (message contains "memory limit exceeded"),
`clear_memory_limit` removes the cap, a generous cap doesn't interfere
with normal scripts, and the `Lua` instance remains fully usable for
subsequent scripts after a memory-limit error (matches the same
"protection doesn't corrupt state" pattern already established for
instruction-limit/recursion-depth-cap). Full suite now 217/217 passing.
Also manually verified `luacpp_example` still runs end-to-end (exit code
0) after switching the underlying allocator/panic-handler wiring.

## Implemented: Sandboxing (LuaLib enum + Lua(LuaLib) constructor + sandbox_deny)

Added a `LuaLib` bitmask enum (namespace scope, above `class Lua`, alongside
LuaCallTraceEvent/callbacks) with one flag per luaopen_* library (Base,
Table, String, Math, Os, Io, Package, Debug, Coroutine, Utf8) plus `All`
(bitwise-OR of all of them) and `None = 0`. `operator|`/`operator&`/
`operator~` (masked to `All`) and a `has_lib(set, lib)` helper are free
functions at namespace scope.

`Lua()` now delegates to a new `explicit Lua(LuaLib libs)` constructor via
`Lua() : Lua(LuaLib::All) {}` - fully backward compatible, since `LuaLib::All`
opens every library exactly as `luaL_openlibs` did. `Lua(LuaLib libs)` calls
a new private `open_selected_libs(libs)` instead of `luaL_openlibs`, which
loops a small static table of `{flag, module_name, lua_CFunction}` entries
and calls `luaL_requiref(*this, name, open_fn, /*glob=*/1)` + `lua_pop(1)`
(requiref leaves the module table on the stack) only for set bits. Module
name constants used: LUA_GNAME ("_G"), LUA_TABLIBNAME, LUA_STRLIBNAME,
LUA_MATHLIBNAME, LUA_OSLIBNAME, LUA_IOLIBNAME, LUA_LOADLIBNAME (package),
LUA_DBLIBNAME, LUA_COLIBNAME, LUA_UTF8LIBNAME.

IMPORTANT: `print` is installed by the constructor UNCONDITIONALLY, after
`open_selected_libs`, regardless of whether `LuaLib::Base` was requested - it
is not part of any luaopen_* library in LuaCpp's own design (see
captured_print/enable_output_capture). So even `Lua(LuaLib::None)` still has
a working (if output-discarding) `print` global; only genuine stdlib
functions like `assert`/`error`/`pcall` are absent when Base isn't opened.
This tripped up an early test draft (assumed `print == nil` under
`LuaLib::None`) - fixed by asserting `print ~= nil` instead, and by avoiding
`assert()`/`error()` entirely in that test's script (since Base isn't open,
those functions don't exist either) - used a plain `function check() return
... end` (core language syntax, always available) inspected via `call<>()`
instead of `assert()`-based scripts.

Added `sandbox_deny(const std::string& dotted_path)`: splits on the first
`.`; no dot -> `lua_pushnil` + `lua_setglobal`. One dot -> `lua_getglobal`
the parent; if it's not a table (missing entirely, or not opened via
`LuaLib`), silently returns (no-op, no exception) so it composes safely
regardless of construction order; otherwise `lua_pushnil` + `lua_setfield`
on the parent table. Deliberately supports only one level of nesting - no
concrete need for deeper paths.

Explicitly did NOT add an `apply_recommended_sandbox()` convenience/preset -
user pushback: "let the user of the library decide" - LuaCpp provides only
the primitives (`LuaLib` selection + `sandbox_deny`), no opinionated
built-in denylist. `TODO.md`'s original example Lua snippet (nil-ing
os.execute/io/dofile/load/require/package/debug) remains only as
documentation of one sensible approach, not something the API bakes in.

Added 8 new tests (`[sandboxing]` tag): default `Lua()` still opens every
library (backward compat), `Lua(LuaLib)` opens only selected ones, `Lua
(LuaLib::None)` opens nothing (with the print/assert caveats above),
`sandbox_deny` on a bare global, `sandbox_deny` on a nested field (sibling
untouched), no-op when the parent was never opened, no-op when the parent
global exists but isn't a table (used `lua.assign("not_a_table", 42)` then
denied a fake nested path under it), and a combined
LuaLib-restriction-plus-sandbox_deny scenario. Full suite now 225/225
passing (debug preset). Also manually re-ran `luacpp_example` end-to-end
(exit code 0, unaffected since it uses the default `Lua()`).

Updated `TODO.md` (checked off Sandboxing, one-paragraph implementation
note) and `README.md` (new "Sandboxing" section + TOC entry, after
"Exception Handling", with `LuaLib` bitmask + `sandbox_deny` examples -
mirrors the existing section style; no built-in denylist offered there
either, per the same user decision).

Note: the hook-based auditing/protection features (call tracing, output
capture, error logging, instruction counting/limit, recursion depth cap,
memory tracking/limit) documented earlier in this file are NOT yet
mentioned in README.md at all - only Sandboxing and Exception Handling
have README sections. Could be a good follow-up if asked to expand docs
further.

Formatting note: user confirmed switching from tabs to spaces in Lua.hpp
going forward (an earlier attachment showed the whole file auto-reformatted
this way) - this session's new Lua.hpp edits were written directly with
spaces to match. The bulk tabs->spaces reformat of the REST of the
pre-existing file has NOT been committed/applied yet - only new code added
this session uses spaces; still pending a decision on whether to do a
dedicated whitespace-only commit for the rest of the file.

## Documented Script Auditing & Protection hooks in README

Follow-up after the Sandboxing work: added README sections (with TOC
entries, same style as existing "Exception Handling"/"Sandboxing"
sections) for every previously-undocumented hook-based feature: Call
Tracing, Output Capture, Error Logging, Instruction Counting & Limit,
Recursion Depth Cap, Memory Tracking & Limit. Each has a short usage
example. Full 225/225 test suite still passes (README-only change,
no code touched). Committed separately from the Sandboxing commit.

README now has doc coverage for ALL public Lua.hpp features added this
overall session (hooks + sandboxing), not just Sandboxing/Exception
Handling as before.

## Implemented: Bytecode rejection in run_script

Added a check at the very top of run_script(): if `script[0] == '\x1b'`
(the Lua bytecode signature byte, LUA_SIGNATURE[0]), immediately return
`{false, "bytecode not allowed"}` (also routed through log_error) WITHOUT
calling luaL_loadstring at all. Rationale documented inline: luaL_loadstring/
lua_load transparently accept precompiled bytecode when input starts with
that byte, skipping the lexer/parser entirely - precompiled chunks can
encode out-of-range constant/register indices the parser itself would never
produce, a known source of crashes/memory corruption in Lua embedders, so
untrusted script strings must never reach the bytecode loader.

Only run_script() needed this - call<>() only invokes already-loaded Lua
functions by name (lua_getglobal + lua_pcall), it never loads a raw string,
so there's no separate bytecode-loading path to guard there.

Added 4 tests (`[bytecode-rejection]` tag): a string starting with '\x1b' is
rejected with "bytecode" in the message, normal source is unaffected, the
Lua instance remains usable after a rejected chunk, and the rejection is
also reported via enable_error_logging (exactly 1 log entry). Full suite
now 229/229 passing. Updated TODO.md (checked off) and README.md (new
paragraph + example under the existing "run_script" section, not a new
top-level section, since it's a behavior of an existing method not a new
API surface). Verified `luacpp_example` still runs end-to-end (exit 0).

Remaining unimplemented TODO.md item: "Read-only C++ globals" (attach a
__newindex metamethod to _G so scripts can't overwrite C++-set globals) -
low priority, not yet started.

## Implemented: Read-only C++ globals (script-side write protection)

Completed the final "Script Auditing & Protection" TODO.md item. Key
technical insight that shaped the whole design: Lua only consults a
table's __newindex metamethod for keys that DON'T already raw-exist in it.
Since assign()/expose_func() write their name directly into the real
globals table via lua_setglobal, attaching a metatable to that table
directly would never fire __newindex on a SECOND write to an
already-registered name - exactly the case that needs catching. So instead
of metatabling the real table, the real globals table itself is swapped
out:

1. Snapshot the current LUA_RIDX_GLOBALS table into a private registry
   key (real_globals_key(), a function-local static sentinel address,
   same pattern as get_type_key<T>()) via lua_rawgetp/lua_rawsetp.
2. Install a brand-new, PERMANENTLY EMPTY table as the new
   LUA_RIDX_GLOBALS, with a metatable: __index = real_G (transparent read
   forwarding), __newindex = protected_newindex (a static lua_CFunction).
3. Because the proxy never gains any raw entries (nothing is ever
   lua_rawset onto it directly), its __index/__newindex ALWAYS fire for
   every single global read/write from Lua code, forever - not just the
   first one for a given name. This is what makes it possible to protect
   a name that already has a value.

Confirmed via reading Lua 5.5's lapi.c: lua_load pulls the globals table
from the registry (LUA_RIDX_GLOBALS) at CHUNK-LOAD time to bind each
chunk's _ENV upvalue - so this only needs to be installed ONCE, in the
constructor (after open_selected_libs + print, so the snapshotted real_G
already contains everything); every future luaL_loadstring call (used by
run_script) automatically picks up the (unchanging) proxy as _ENV.

protected_newindex(L): standard __newindex signature (1=table, 2=key,
3=value). If key is a string and present in self->protected_globals,
raises luaL_error (message: "attempt to modify protected global '<name>'"),
routed through log_error first. Otherwise writes through: rawget real_G,
rawset key/value onto it.

New private write_real_global(name) helper: consumes the value on top of
the stack and writes it directly into real_G (via lua_setfield - real_G
itself has no metatable of its own, so this acts as a raw set), bypassing
the proxy/protected_newindex entirely. EVERY C++-driven global write now
goes through this instead of lua_setglobal:
- assign() - also inserts `name` into protected_globals right after
  (auto-protection).
- register_global_func() (backing expose_func) - same auto-protection.
- close() - nils via write_real_global AND erases from protected_globals
  (a closed/removed global has nothing meaningful left to protect).
- sandbox_deny()'s bare-global branch - nils via write_real_global AND
  erases from protected_globals. The nested-field branch is untouched
  (lua_getglobal read of the parent still goes through the proxy's
  __index transparently; the parent table itself, e.g. "os", has no
  metatable, so lua_setfield on IT was never affected by any of this).

New public API: `protect_global(name)` / `unprotect_global(name)` - simple
insert/erase into protected_globals, exposed so a caller can (a) protect a
global that was instead set FROM Lua (e.g. run_script("secret=1") then
lua.protect_global("secret")), or (b) opt a specific assign()/expose_func
name back OUT of the automatic protection.

**Real bug caught in testing**: a test asserted exactly 1 log entry for a
protected-global write attempt via run_script, but got 2 - same
"intentional double-log" pattern already documented for expose_func
exceptions (see earlier memory entry): protected_newindex logs the raw
message once, and run_script's own Tier-2 error-logging site (added
earlier in this session) logs the resulting Lua error string again after
lua_pcall fails, since it also contains the same text. Fixed the test to
assert `== 2` with an explanatory comment, matching the established
pattern rather than treating it as a bug.

Added 10 tests (`[read-only-globals]` tag): assign()-registered global
blocked from script overwrite (with state remaining usable afterward),
expose_func-registered global blocked (function stays fully callable),
reading a protected global is unaffected, unrelated/un-registered globals
remain freely writable, unprotect_global lifts protection,
protect_global protects a Lua-set global, protection reported via error
logging (2 entries, see bug note above), close() un-protects+nils an
expose_func global, sandbox_deny on a bare name un-protects+nils it, and a
sanity check that ordinary stdlib usage (math/string/table operations,
i.e. reads through the proxy) is completely unaffected. Full suite now
239/239 passing (up from 229). Verified `luacpp_example` still runs
end-to-end (exit 0) - uses only assign/expose_func without any
conflicting overwrite attempts, so unaffected by the new default
protection.

Updated TODO.md (checked off - **all** Script Auditing & Protection items
are now complete) and README.md (new "Read-Only C++ Globals" section +
TOC entry, after Memory Tracking & Limit, same style as the other hook
sections).

## Extended: protected expose_method/expose_mutable_method methods too

Follow-up to the read-only-globals work: user asked whether methods added
via expose_method/expose_mutable_method could also be protected from
script tampering. These are NOT stored as globals at all - they live in a
per-type metatable (one shared metatable per T, created lazily by
add_method_to_registry, cached in the Lua registry keyed by get_type_key
<T>()), whose __index sub-table holds the actual method closures. Attack
surface: a script could do `local mt = getmetatable(instance);
mt.__index.name = fn` to overwrite a method for ALL instances of that
type, or `setmetatable(instance, {})` to strip method dispatch from one
specific instance.

Fix: when add_method_to_registry creates a type's metatable for the first
time, also set `metatable.__metatable = typeid(T).name()` (any non-nil
sentinel value works; used typeid's name purely for diagnostic value if
ever inspected in a debugger - it is never exposed to Lua). Two built-in
Lua behaviors around the __metatable field do all the actual protection
work automatically, with zero extra machinery needed:
- `getmetatable(x)` returns the __metatable field's VALUE instead of the
  real metatable when one is set - so scripts get back the opaque
  sentinel, never a table, and can never reach __index to tamper with it.
- `setmetatable(x, ...)` raises "cannot change a protected metatable"
  (this check is built into Lua's own lbaselib.c luaB_setmetatable) when
  the CURRENT metatable already has __metatable set - so a script can't
  replace or nil out an instance's metatable either.

Added `#include <typeinfo>` for typeid(). No changes needed anywhere else
- attach_methods_if_any/trampoline/push<T> etc. all look up the metatable
directly via the registry key (bypassing getmetatable entirely), so they
are completely unaffected by __metatable being set.

IMPORTANT CAVEAT (documented in both TODO.md and README.md): the `debug`
library intentionally bypasses __metatable via `debug.getmetatable`/
`debug.setmetatable` - this is a known, deliberate Lua design choice (the
whole point of the debug library is unrestricted introspection). If this
protection must hold against genuinely untrusted scripts, the caller
should exclude LuaLib::Debug via the constructor (see the Sandboxing
feature) - LuaCpp doesn't/can't work around this at the __metatable level
itself.

Added 4 tests (`[read-only-methods]` tag): getmetatable returns a
non-table sentinel (methods still callable normally), setmetatable is
rejected with "protected metatable" in the message (instance remains
fully usable afterward), confirms there's no reachable __index table to
tamper with at all, and the same protection applies to
expose_mutable_method types too (mutation via the method itself still
works fine - only the metatable is locked, not method dispatch). Full
suite now 243/243 passing (up from 239). Verified `luacpp_example` still
runs end-to-end (exit 0).

Updated TODO.md's existing "Read-only C++ globals" bullet (extended, not
a new bullet, since it's the same overall feature/checkbox) and README's
"Read-Only C++ Globals" section with a new paragraph + example + the
debug-library caveat callout.

## Documented manual/non-CMake integration requirements in README

User asked for docs covering usage of LuaCpp when NOT pulled in via the
LuaCpp::LuaCpp CMake target (e.g. a different build system, IDE project,
vendored sources). Added a new "Manual / Non-CMake Integration" README
section (TOC entry added under "Using LuaCpp in Your Own Project"),
transcribing every non-obvious requirement currently encoded only in
CMakeLists.txt's lua_static target into prose a non-CMake consumer can
follow:
1. C++17 requirement.
2. Compile every Lua 5.5 .c file (except lua.c/luac.c) as C++, not C - and
   explicitly do NOT define LUA_USE_LONGJMP (Lua's ldo.c auto-detects
   __cplusplus and uses real C++ exceptions across lua_pcall, which is
   what lets expose_func/expose_method exceptions unwind destructors
   correctly).
3. Preserve C linkage via a single extern "C" { #include "x.c" ... }
   wrapper translation unit (Lua's own headers don't wrap themselves).
4. MSVC: use /EHa (not default /EHsc) for both Lua sources and any
   consumer TU that calls into Lua or has live C++ objects on the stack
   during a Lua call, plus /bigobj for large TUs.
5. Platform defines/libs: LUA_USE_LINUX (+m,dl) / LUA_USE_MACOSX (+m) /
   MSVC needs _CRT_SECURE_NO_WARNINGS, no LUA_USE_* define.
6. Expected-safe-to-silence warnings per compiler (GCC
   -Wno-error=maybe-uninitialized, Clang -Wno-error=uninitialized, MSVC
   C4297 on extern "C" functions under /EHa).
7. Include-path requirements (src/ for Lua.hpp, Lua's own headers
   reachable since Lua.hpp extern-C-wraps their #include).

Closes by pointing at CMakeLists.txt's lua_static target as the
authoritative reference if Lua's build requirements ever change in a
future release. Docs-only change; verified full 243/243 test suite still
passes, no code touched.

Also note: partway through this session a stale/corrupted attachment
diff was shown for Lua.hpp (extern "C" opening mangled into "ude
<lauxlib.h>") - verified via read_file that the actual on-disk file was
NOT corrupted (still starts with the correct extern "C" { block) and a
rebuild succeeded with "no work to do" (no recompilation needed) -
confirms it was a stale/bad tool-attachment artifact, not a real file
change. Worth double-checking with read_file/rebuild if a similarly
alarming attachment diff appears again rather than assuming the file is
actually broken.

## Added demo_sandboxing() to src/main.cpp

Added a new demo function (following the existing one-function-per-feature
pattern in main.cpp) showcasing the Sandboxing API: an instance constructed
with a restricted LuaLib set (Base|Table|String|Math - os/io/require/
package/debug/coroutine never opened), and a second instance that keeps
LuaLib::Os but denies just "os.execute" via sandbox_deny, confirming
os.time still works. Wired into main()'s call sequence after
demo_error_handling(). Verified end-to-end run (exit 0) - note that, like
every other demo in this file, print() output goes nowhere since none of
them call enable_output_capture, so only std::cout lines and {ok,err}
tuples are visible; this is expected/consistent, not a bug. Full 243/243
test suite unaffected (main.cpp isn't part of the test binary).

## Expanded CI: ASAN for both luacpp_tests/luacpp_example + Debug/Release matrix

Two follow-up requests in one session, both to .github/workflows/ci.yml
and CMakeLists.txt:

1. "Run CI so it runs tests and main with ASAN enabled": CMakeLists.txt's
   LUACPP_ENABLE_ASAN block previously only applied
   -fsanitize=address/-fno-omit-frame-pointer to luacpp_tests - extended
   the same block (still guarded `if(LUACPP_ENABLE_ASAN AND NOT MSVC)`) to
   luacpp_example too. ci.yml's Configure step now explicitly passes
   -DLUACPP_ENABLE_ASAN=ON (this actually matched the option's existing
   top-level default already, but being explicit is more robust/future-
   proof against that default ever changing), and a brand new "Run
   example" step was added - CI previously only ever ran `ctest`, it never
   actually executed the built luacpp_example binary at all. Step picks
   the right per-OS binary path (build/Release/luacpp_example.exe on
   Windows vs build/luacpp_example elsewhere).

2. "Run both debug and release builds in CI": added a `build_type: [Debug,
   Release]` matrix dimension alongside the existing `os` matrix (3 OSes x
   2 build types = 6 jobs total). Job `name` updated to
   `${{ matrix.os }} (${{ matrix.build_type }})`. Configure/Build/Test/Run
   example steps all switched from a hardcoded `Release` to
   `${{ matrix.build_type }}`.

Verified BOTH changes locally on Linux (not just trusting CI): did a
from-scratch `cmake -B build/X -DCMAKE_BUILD_TYPE=<Debug|Release>
-DLUACPP_ENABLE_ASAN=ON` + build + `ctest ... -C <config>` +
`./build/X/luacpp_example` for both Debug and Release. Both configurations:
compiled cleanly (saw "-- ASAN enabled for 'luacpp_example'" AND "...for
'luacpp_tests'" in configure output), passed all 243/243 tests, and ran
luacpp_example end-to-end with exit code 0 and zero ASAN reports (no
leaks, no use-after-free, etc., across the whole feature set built up this
session - hooks, sandboxing, read-only globals/methods, bytecode
rejection). Cleaned up the throwaway build dirs afterward.

Note: the user CANCELLED the first attempted commit for change #1 (ASAN
expansion) before approving the wording - re-committed it together
implicitly is NOT what happened; it was committed on its own after the
cancellation didn't recur on a retry in the same turn... actually to be
precise: change #1 was committed successfully on a subsequent request in
the same conversation (no data lost, no explicit re-approval message
beyond the user's next instruction), and change #2 (build_type matrix) was
a separate, separately-committed follow-up request handled the normal way.

## Enforced braces on every if/else/for/while clause, including docs

User request: "Ensure that all code (including code in documentation) uses
{...} for all clauses, i.e no if(...) single_line_statement;" - a full
project-wide style sweep, not just newly-touched code (the project's
existing convention, established earlier this session for close()'s
`if(closed) return;` fix, was inconsistently applied elsewhere).

Searched src/Lua.hpp, src/main.cpp, src/test.cpp, README.md, TODO.md,
LIFETIME.md, and CMakeLists.txt via grep for `if(`/`else`/`for(`/`while(`
followed by an unbraced single statement. Found and fixed:

Lua.hpp (7 spots):
- read<T>()'s four type-check guards (bool/is_integral/is_floating_point/
  std::string) - each was `if(!lua_isX(...)) throw ...;` with no braces.
- Three identical-shaped `if(const int got = lua_gettop(L); got !=
  expected) throw std::runtime_error(...);` argument-count guards, in
  make_func_wrapper's lambda, make_method_wrapper's lambda, and
  expose_mutable_method's inline lambda.
- decay_for_push's `if constexpr(...) return ...; else return ...;` (both
  branches unbraced).

main.cpp (6 spots across 5 demo functions): demo_run_script's
`if(!ok) std::cerr...`; demo_call's `if(ok)...else...` and a second
`if(ok2)...`; demo_struct_binding's `if(ok)...`; demo_exception_handling's
`if(b==0) throw...` and `if(ok2)...`; demo_sandboxing's `if(!ok)...`.

test.cpp (12 spots): unbraced `if(x<0) throw ...;` in 3 expose_func
exception tests, the same pattern in 2 expose_method exception tests, the
same pattern x3 (identical lambda body reused across 3 TEST_CASEs) in
expose_mutable_method exception tests (fixed via one replaceAll edit since
all 3 occurrences were byte-identical), an unbraced `if(b.items.empty())
return -1;` in the Bag::pop test, and 8 unbraced single-statement for-loop
bodies (`for(auto x : v) s += x;` etc.) scattered across the
container-arg/return TEST_CASEs (vector<int>/vector<double>/
vector<string>/map<string,int>/unordered_map sum/scale/exclaim/dot_sum/
Bag::sum tests).

README.md (1 spot): the Exception Handling section's example
`if (b == 0) throw std::runtime_error(...);` inside a ```cpp block.

Deliberately did NOT touch: LIFETIME.md's `if(!fn) { /* comment */ }` -
already has braces (just compact/inline), satisfies the actual
requirement even though not multi-line. Also did NOT touch CMakeLists.txt
- its `if()`/`endif()` pairs are CMake's own language syntax, unrelated to
C++ brace style, and every one already has a matching `endif()` regardless
of formatting. Also explicitly excluded build/_deps/catch2-src (vendored
third-party dependency, not this project's own code) from the sweep.

Verified via rebuild: full 243/243 test suite still passes, and
luacpp_example's stdout was diffed byte-for-byte against a
pre-change-captured baseline and found IDENTICAL - confirms this was a
pure formatting/style pass with zero behavioral change, as expected.

## Docs cleanup: removed historical/past-tense narrative from LIFETIME.md,
## CMakeLists.txt, and Lua.hpp comments (current-state only)

User asked that documentation and code comments describe only *current*
state, not the history of what used to be true. Rewrote/edited:

- `LIFETIME.md`: full rewrite. Removed "Status: Resolved/Implemented/
  Documented" labels, the item-2 "Historical bug (no longer applicable)"
  paragraph describing the old trampoline_impl/call_impl split and the
  since-removed setjmp/longjmp-era bug, and "has since been removed"/
  "originally"/"previously" phrasing throughout. Kept all still-relevant
  technical facts (weak_ptr trampoline, close() GC double-collect,
  self-by-value pitfalls, coroutine risk) phrased as plain current-state
  descriptions ("Mitigation:" instead of "Status: Implemented.").
- `CMakeLists.txt`: reworded 3 comment blocks that referenced the old
  per-file-wrapper Windows-crash theory ("originally suspected... turned
  out to be a red herring"), the old LUA_USE_LONGJMP-because-of-Windows-
  crashes rationale ("we previously forced... those crashes were actually
  caused by..."), and "as an earlier version of this file did" (re: global
  CMAKE_CXX_FLAGS mutation) - now state only why the current single-TU +
  /EHa + no-LUA_USE_LONGJMP design is correct, without the backstory.
- `src/Lua.hpp`: trampoline() comment had a paragraph starting "This used
  to be split into trampoline()/trampoline_impl()/call_impl()... Neither
  condition applies anymore... so the split was removed" - trimmed to just
  the current-state safety rationale (relies on /EHa + no LUA_USE_LONGJMP).
- Checked `README.md`, `TODO.md`, `src/test.cpp` for the same kind of
  language (grepped for previously/historical/no longer/used to/has since/
  originally/already/Real bug caught/Status:) - README.md and test.cpp had
  none; TODO.md's checked-off `[x]` items already read as pure
  current-state implementation descriptions (no rewrite needed there).

Verified: rebuilt (`cmake --build build`) after all edits - clean build,
comment-only changes, no functional/test impact (didn't re-run full ctest
since nothing but comments/markdown changed, but confirmed compile-clean
for lua_static/luacpp_example/luacpp_tests).

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
