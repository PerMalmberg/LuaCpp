# LuaCpp project: "compile Lua as C++" saga

Repo: /home/permal/code/LuaCpp
Goal: build Lua 5.5.0 sources (fetched via CMake FetchContent) as C++ instead of C.

## Final working approach (CMakeLists.txt, lua_static target)

Per-file wrapper .cpp generation (NOT a single unity/amalgamated file):
- For each Lua .c source, generate a tiny wrapper in
  ${CMAKE_BINARY_DIR}/generated/lua_cxx/<name>_cxx.cpp containing:
    extern "C" { #include "<original>.c" }
- add_library(lua_static STATIC <all wrapper .cpp files>)
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

## Why NOT a single unity/amalgamated translation unit (important!)

First attempt merged ALL Lua .c files into one lua_cxx_wrapper.cpp inside a
single extern "C" block. This built and passed all 168 tests on Linux, but
caused STATUS_HEAP_CORRUPTION (exit 0xc0000374) on Windows CI, specifically
in tests that exercise Lua's error path (luaL_error / wrong-arg-type /
too-many/few-args / stack-hygiene-after-error tests).

Root cause theory: merging every source into one giant TU lets the compiler
inline/analyze across what used to be separate file boundaries. This can
expose the classic (otherwise-dormant) setjmp/longjmp UB: non-volatile
local variables modified after setjmp() have indeterminate values after a
longjmp, especially under optimization. Separate per-file compilation (as
upstream Lua normally does, and as our final fix restores) avoids this.

Note: LUA_USE_LONGJMP alone was NOT sufficient to fix the Windows crash
when still using the single unity file - the per-file TU separation was
the actual fix. Both changes are kept together in the final solution.

## Other gotchas encountered
- Lua's headers (lua.h/lauxlib.h/lualib.h) do NOT wrap declarations in
  extern "C" themselves; Lua.hpp already wraps its own #include of them in
  extern "C". So the library's exported symbols must also have C linkage,
  which the extern "C" { #include "x.c" } wrapper trick provides per file.
- Do not use LANGUAGE CXX directly on the original .c files without the
  extern "C" wrapper trick - definitions get C++-mangled and everything
  fails to link against callers expecting C linkage.

## Status as of last update
Linux build + 168/168 ctest passing confirmed locally after the per-file
wrapper fix. Windows/macOS CI re-run pending confirmation (no local
Windows/macOS environment available to verify directly).
