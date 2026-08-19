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
# that consumes the C-linkage declarations. To avoid that mismatch while
# still compiling everything with the C++ compiler, generate a single unity
# translation unit that #includes every Lua source file inside an
# extern "C" block.
set(LUA_CXX_WRAPPER "${CMAKE_BINARY_DIR}/generated/lua_cxx_wrapper.cpp")

set(_lua_wrapper_content "// Auto-generated: compiles the Lua sources as C++ with C linkage.\n")
string(APPEND _lua_wrapper_content "extern \"C\" {\n")
foreach(_lua_src IN LISTS LUA_SOURCES)
  string(APPEND _lua_wrapper_content "#include \"${_lua_src}\"\n")
endforeach()
string(APPEND _lua_wrapper_content "}\n")

file(WRITE ${LUA_CXX_WRAPPER} "${_lua_wrapper_content}")

add_library(lua_static STATIC ${LUA_CXX_WRAPPER})
set_source_files_properties(${LUA_CXX_WRAPPER} PROPERTIES LANGUAGE CXX)

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
  target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror -Wno-error=maybe-uninitialized)
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


