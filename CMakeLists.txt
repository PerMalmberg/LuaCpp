cmake_minimum_required(VERSION 4.0)
project(lua_project C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC defaults to /EHsc (synchronous C++ exceptions only), which Microsoft
# documents as unsafe to combine with setjmp/longjmp: jumping across frames
# compiled with /EHsc can corrupt the compiler's table-driven unwind info,
# especially under optimization. Since Lua (compiled as C++ via the wrapper
# trick below) uses classic setjmp/longjmp for its error handling
# (LUA_USE_LONGJMP), replace /EHsc with /EHa - which correctly integrates
# SEH-style unwinding with longjmp - for every target in this project.
# Without this, Release builds intermittently crash with
# STATUS_HEAP_CORRUPTION (0xc0000374) specifically in Lua error-path tests
# (luaL_error / wrong-arg-type / too-many-or-few-args / stack-hygiene-after-
# error tests).
if(MSVC)
  string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

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
# We generate a single unity/amalgamated wrapper .cpp that #includes every
# Lua .c source inside one `extern "C" { ... }` block. This was originally
# suspected of causing Windows STATUS_HEAP_CORRUPTION crashes in Lua's
# error-handling tests (luaL_error / longjmp paths), and was replaced with
# one wrapper-per-source-file as a workaround. That turned out to be a red
# herring: the actual cause was MSVC's default /EHsc exception model being
# unsafe to mix with setjmp/longjmp (see the /EHa fix above). With /EHa in
# place, the simple single-TU approach below builds and passes all tests
# reliably on Windows, Linux and macOS, so we keep it for its simplicity.
set(LUA_CXX_WRAPPER_DIR "${CMAKE_BINARY_DIR}/generated/lua_cxx")
file(MAKE_DIRECTORY ${LUA_CXX_WRAPPER_DIR})

set(_wrapper "${LUA_CXX_WRAPPER_DIR}/lua_cxx_unity.cpp")
set(_wrapper_content "// Auto-generated: compiles all Lua .c sources as one C++ TU with C linkage.\nextern \"C\" {\n")
foreach(_lua_src IN LISTS LUA_SOURCES)
  string(APPEND _wrapper_content "#include \"${_lua_src}\"\n")
endforeach()
string(APPEND _wrapper_content "}\n")
file(WRITE "${_wrapper}" "${_wrapper_content}")

add_library(lua_static STATIC "${_wrapper}")
set_source_files_properties("${_wrapper}" PROPERTIES LANGUAGE CXX)

target_include_directories(lua_static PUBLIC ${LUA_SRC_DIR})

# Lua's ldo.c auto-selects its error-propagation mechanism based on whether
# it detects a C++ compiler: when __cplusplus is defined it switches from
# setjmp/longjmp to throwing/catching a raw `lua_longjmp*` as a C++
# exception (LUAI_THROW/LUAI_TRY -> throw/catch(...) at the lua_pcall
# boundary). This is Lua's own documented/supported mechanism for
# embedding in C++ and, unlike raw longjmp, properly unwinds the C++ stack
# and runs destructors. We previously forced classic setjmp/longjmp via
# LUA_USE_LONGJMP because of Windows STATUS_HEAP_CORRUPTION crashes - but
# those crashes were actually caused by MSVC's /EHsc exception model being
# incompatible with longjmp (see the /EHa fix above), not by the C++
# exception mode itself. Now that /EHa is applied project-wide, Lua's
# native C++ exception-based error handling works reliably (verified with
# repeated Release and Debug ctest runs on Windows with no flakiness).
# (Intentionally NOT setting LUA_USE_LONGJMP here.)

# Platform-specific Lua defines and system libraries.
# lua.c (the interactive REPL) is excluded from the build so LUA_USE_READLINE
# never pulls in libreadline on any platform.
if(MSVC)
  target_compile_options(lua_static PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W3)
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
  target_compile_options(main PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W4 /WX /bigobj)
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
  target_compile_options(tests PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W4 /WX /bigobj)
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


