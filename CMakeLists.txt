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

target_compile_options(lua_static PRIVATE -O2 -Wall -Wextra -Werror)

# Linux requires LUA_USE_LINUX and links against libdl
target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
target_link_libraries(lua_static PUBLIC m dl)

# ---------------------------------------------------------------------------
# Sample host executable
# ---------------------------------------------------------------------------
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE lua_static)
target_compile_options(main PRIVATE -O2 -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------
# Catch2 and tests
# ---------------------------------------------------------------------------
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(tests src/test.cpp)
target_link_libraries(tests PRIVATE lua_static Catch2::Catch2WithMain)
target_compile_options(tests PRIVATE -O2 -Wall -Wextra -Werror)


