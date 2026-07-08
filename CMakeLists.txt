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

add_library(lua_static STATIC ${LUA_SOURCES})

target_include_directories(lua_static PUBLIC ${LUA_SRC_DIR})

# Platform-specific Lua defines and system libraries.
# lua.c (the interactive REPL) is excluded from the build so LUA_USE_READLINE
# never pulls in libreadline on any platform.
if(MSVC)
  target_compile_options(lua_static PRIVATE /O2 /W3)
  target_compile_definitions(lua_static PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
  target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror)
  target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX)
  target_link_libraries(lua_static PUBLIC m)
else() # Linux
  target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror)
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

enable_testing()
catch_discover_tests(tests)


