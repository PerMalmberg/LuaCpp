cmake_minimum_required(VERSION 4.0)
project(lua_project C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Fetch Lua 5.5.0 sources
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
)

# Populate (download + extract) without calling add_subdirectory –
# Lua ships no CMakeLists.txt, so we build it ourselves below.
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
  FetchContent_MakeAvailable(lua)
endif()

# ---------------------------------------------------------------------------
# Lua static library
# ---------------------------------------------------------------------------
set(LUA_SRC_DIR ${lua_SOURCE_DIR}/src)

file(GLOB LUA_SOURCES "${LUA_SRC_DIR}/*.c")

# Exclude the standalone interpreter and compiler – they define their own main()
list(REMOVE_ITEM LUA_SOURCES
    "${LUA_SRC_DIR}/lua.c"
    "${LUA_SRC_DIR}/luac.c"
)

# Compile Lua as C++ while keeping the public API's C linkage.
#
# Lua's headers (lua.h/lauxlib.h/lualib.h) do NOT wrap their declarations in
# extern "C" themselves - callers are expected to do that (as Lua.hpp does).
# If we simply compiled each .c file individually as C++ (LANGUAGE CXX), the
# *definitions* in those translation units would get C++ name-mangled since
# they never see an extern "C" block, causing linker errors against code
# that consumes the C-linkage declarations.
#
# We generate one tiny wrapper .cpp PER Lua source file (rather than a single
# unity/amalgamated translation unit). Each wrapper just does:
#   extern "C" { #include "<file>.c" }
# This keeps every file compiled as its own separate translation unit, just
# like upstream Lua's normal build - which matters because merging all the
# sources into one giant TU lets the optimizer inline/analyze *across*
# former file boundaries. That can expose the classic (and otherwise latent)
# setjmp/longjmp pitfall where non-volatile locals modified after setjmp()
# have indeterminate values after a longjmp, particularly under
# optimization - which is exactly what was crashing (STATUS_HEAP_CORRUPTION)
# on MSVC in the error-handling code paths (luaL_error / lua_error) once
# everything was amalgamated into a single translation unit.
set(LUA_CXX_WRAPPER_DIR "${CMAKE_BINARY_DIR}/generated/lua_cxx")
file(MAKE_DIRECTORY ${LUA_CXX_WRAPPER_DIR})

set(LUA_CXX_WRAPPERS "")
foreach(_lua_src IN LISTS LUA_SOURCES)
  get_filename_component(_lua_src_name "${_lua_src}" NAME_WE)
  set(_wrapper "${LUA_CXX_WRAPPER_DIR}/${_lua_src_name}_cxx.cpp")
  file(WRITE "${_wrapper}" "// Auto-generated: compiles ${_lua_src_name}.c as C++ with C linkage.\nextern \"C\" {\n#include \"${_lua_src}\"\n}\n")
  list(APPEND LUA_CXX_WRAPPERS "${_wrapper}")
endforeach()

add_library(lua_static STATIC ${LUA_CXX_WRAPPERS})
set_source_files_properties(${LUA_CXX_WRAPPERS} PROPERTIES LANGUAGE CXX)

target_include_directories(lua_static PUBLIC ${LUA_SRC_DIR})

# Lua's ldo.c auto-selects its error-propagation mechanism based on whether
# it detects a C++ compiler: when __cplusplus is defined it switches from
# setjmp/longjmp to throwing/catching a raw `lua_longjmp*` as a C++
# exception. That mode is not something the unmodified Lua C sources are
# designed/tested for (no RAII, raw buffer/realloc management throughout),
# and it has been observed to corrupt the heap on error paths under MSVC's
# exception model (STATUS_HEAP_CORRUPTION / 0xc0000374). Force Lua back to
# its classic, battle-tested setjmp/longjmp error handling even though the
# sources are compiled as C++.
target_compile_definitions(lua_static PRIVATE LUA_USE_LONGJMP)

# Platform-specific Lua defines and system libraries.
# lua.c (the interactive REPL) is excluded from the build so LUA_USE_READLINE
# never pulls in libreadline on any platform.
if(MSVC)
  target_compile_options(lua_static PRIVATE /O2 /W3)
  target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
  # AppleClang doesn't know the GCC-specific -Wmaybe-uninitialized warning;
  # use its closest equivalent instead (see the Linux branch below for why
  # this is downgraded from an error).
  target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror -Wno-error=uninitialized)
  target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX)
  target_link_libraries(lua_static PUBLIC m)
else() # Linux
  # -Wmaybe-uninitialized triggers false positives in the (unmodified) Lua C
  # sources once they are compiled as C++, because GCC's flow analysis for
  # unions/structs differs between the two languages. Downgrade it to a
  # warning instead of disabling -Werror for the rest of the target.
  target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
  target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
  target_link_libraries(lua_static PUBLIC m dl)
endif()

# ---------------------------------------------------------------------------
# Sample host executable
# ---------------------------------------------------------------------------
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE lua_static)
if(MSVC)
  target_compile_options(main PRIVATE /O2 /W4 /WX)
else()
  target_compile_options(main PRIVATE -O2 -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# Catch2 and tests
# ---------------------------------------------------------------------------
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.2
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(tests src/test.cpp)
target_link_libraries(tests PRIVATE lua_static Catch2::Catch2WithMain)
if(MSVC)
  target_compile_options(tests PRIVATE /O2 /W4 /WX)
else()
  target_compile_options(tests PRIVATE -O2 -Wall -Wextra -Werror)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer (ASAN)
# ---------------------------------------------------------------------------
option(ENABLE_ASAN "Enable AddressSanitizer for the tests target" ON)

if(ENABLE_ASAN AND NOT MSVC)
  target_compile_options(tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(tests PRIVATE -fsanitize=address)
  message(STATUS "ASAN enabled for 'tests'")
endif()

enable_testing()
catch_discover_tests(tests)


