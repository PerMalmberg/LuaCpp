cmake_minimum_required(VERSION 4.0)
project(LuaCpp CXX)

# ---------------------------------------------------------------------------
# Library-consumption ergonomics
# ---------------------------------------------------------------------------
# LuaCpp is meant to be usable both as a standalone project (its own sample
# executable + test suite) AND as a dependency pulled into a much bigger
# project via add_subdirectory()/FetchContent(). Everything in this section
# exists to make the latter case safe and unobtrusive: no leaking global
# compiler-standard settings onto the parent build, no building/registering
# our own tests or sample executable inside someone else's build, and no
# target-name collisions (a parent project is quite likely to already have
# targets literally named "main" or "tests").
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LUACPP_IS_TOP_LEVEL ON)
else()
  set(LUACPP_IS_TOP_LEVEL OFF)
endif()

option(LUACPP_BUILD_EXAMPLES "Build LuaCpp's sample executable" ${LUACPP_IS_TOP_LEVEL})
option(LUACPP_BUILD_TESTS "Build and register LuaCpp's own Catch2 test suite" ${LUACPP_IS_TOP_LEVEL})
option(LUACPP_ENABLE_ASAN "Enable AddressSanitizer for LuaCpp's test suite (non-MSVC only)" ${LUACPP_IS_TOP_LEVEL})

# Only force these globally when built standalone. A consuming project may
# already target a different (or higher) C++ standard, or manage
# CMAKE_EXPORT_COMPILE_COMMANDS itself; clobbering those with set() here
# would leak outside this directory (CMAKE_CXX_STANDARD affects every
# target in every subdirectory that doesn't override it). LuaCpp's own
# requirement of C++17 is instead attached directly to the targets that need
# it (see target_compile_features below), so it's still enforced for anyone
# actually linking against LuaCpp without affecting unrelated parts of a
# parent build.
if(LUACPP_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# ---------------------------------------------------------------------------
# Fetch Lua 5.5.0 sources
# ---------------------------------------------------------------------------
include(FetchContent)

# Guard the whole lua_static definition so that if LuaCpp ends up included
# more than once in the same build (a "diamond dependency" - e.g. two
# different third-party libraries both pull in LuaCpp via FetchContent),
# the second inclusion is a harmless no-op instead of a duplicate-target
# error.
if(NOT TARGET lua_static)

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
  target_compile_features(lua_static PUBLIC cxx_std_17)

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

    # MSVC defaults to /EHsc (synchronous C++ exceptions only). Even though
    # Lua no longer uses setjmp/longjmp for error handling (LUA_USE_LONGJMP is
    # intentionally not defined - see above), /EHa is kept as a defense-in-
    # depth requirement: it is documented by Microsoft as the correct model
    # whenever C code with its own error/unwind conventions is compiled as
    # C++ and mixed with real C++ exceptions, and protects against any future
    # reintroduction of LUA_USE_LONGJMP (by this project or a consumer) or
    # any other latent setjmp/longjmp use inside Lua.
    #
    # PUBLIC (not PRIVATE) is essential here: this project is meant to be
    # consumed as a library (via add_subdirectory/FetchContent/find_package)
    # from a much larger project. A PRIVATE flag, or mutating the global
    # CMAKE_CXX_FLAGS as an earlier version of this file did, only affects
    # translation units built inside THIS CMakeLists.txt - it does not reach
    # the consumer's own targets that #include Lua.hpp and call into it. By
    # attaching /EHa (and /bigobj, needed because /EHa generates more unwind
    # metadata) as a PUBLIC usage requirement on lua_static, CMake
    # automatically appends both flags to the compile command line of any
    # target that does target_link_libraries(<consumer> PRIVATE|PUBLIC
    # lua_static) - including in a completely separate top-level project.
    # MSVC resolves conflicting /EH* switches by taking the last one on the
    # command line, and CMake always places a target's own (PUBLIC/INTERFACE)
    # compile options after the directory-wide defaults, so this reliably
    # overrides the consumer's default /EHsc without them having to do
    # anything themselves.
    target_compile_options(lua_static PUBLIC /EHa /bigobj)
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

endif() # NOT TARGET lua_static

# ---------------------------------------------------------------------------
# LuaCpp: the target consumers should actually link against.
# ---------------------------------------------------------------------------
# Lua.hpp is header-only on top of the compiled lua_static library, so this
# is a plain INTERFACE target: it contributes no object code of its own, it
# just bundles "link against lua_static" + "add src/ to the include path"
# (and, transitively via lua_static, the /EHa usage requirement on MSVC)
# into a single target consumers can depend on with one line:
#   target_link_libraries(your_target PRIVATE LuaCpp::LuaCpp)
if(NOT TARGET LuaCpp)
  add_library(LuaCpp INTERFACE)
  target_link_libraries(LuaCpp INTERFACE lua_static)
  target_include_directories(LuaCpp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_library(LuaCpp::LuaCpp ALIAS LuaCpp)
endif()

# ---------------------------------------------------------------------------
# Sample host executable
# ---------------------------------------------------------------------------
# Named luacpp_example rather than a generic "main" specifically so it can't
# collide with a target of the same name in whatever parent project pulls
# LuaCpp in via add_subdirectory()/FetchContent(). Gated behind
# LUACPP_BUILD_EXAMPLES (off by default when not the top-level project) so
# it's never built at all unless explicitly requested.
if(LUACPP_BUILD_EXAMPLES)
  add_executable(luacpp_example src/main.cpp)
  target_link_libraries(luacpp_example PRIVATE LuaCpp::LuaCpp)
  if(MSVC)
    target_compile_options(luacpp_example PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W4 /WX)
  else()
    target_compile_options(luacpp_example PRIVATE -O2 -Wall -Wextra -Werror)
  endif()

  if(LUACPP_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(luacpp_example PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(luacpp_example PRIVATE -fsanitize=address)
    message(STATUS "ASAN enabled for 'luacpp_example'")
  endif()
endif()

# ---------------------------------------------------------------------------
# Catch2 and tests
# ---------------------------------------------------------------------------
# Everything below - fetching Catch2, building the test binary, enabling
# ASAN, and registering with ctest - only happens for LUACPP_BUILD_TESTS
# (on by default when built standalone, off when consumed as a
# subdirectory). A parent project has no use for LuaCpp's own test suite,
# and forcing enable_testing()/a Catch2 dependency/an extra "tests" target
# onto it would be poor library citizenship - doubly so since "tests" is a
# very likely name for the parent's own test binary.
if(LUACPP_BUILD_TESTS)

  # Guarded in case a parent project already provides Catch2 itself (e.g. it
  # also uses Catch2 for its own tests) - avoids re-fetching or clashing over
  # a different GIT_TAG.
  if(NOT TARGET Catch2::Catch2WithMain)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.15.2
  )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  endif()
  include(Catch)

  add_executable(luacpp_tests src/test.cpp)
  target_link_libraries(luacpp_tests PRIVATE LuaCpp::LuaCpp Catch2::Catch2WithMain)
  if(MSVC)
    target_compile_options(luacpp_tests PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W4 /WX)
  else()
    target_compile_options(luacpp_tests PRIVATE -O2 -Wall -Wextra -Werror)
  endif()

  if(LUACPP_ENABLE_ASAN AND NOT MSVC)
    target_compile_options(luacpp_tests PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(luacpp_tests PRIVATE -fsanitize=address)
    message(STATUS "ASAN enabled for 'luacpp_tests'")
  endif()

  enable_testing()
  catch_discover_tests(luacpp_tests)

endif() # LUACPP_BUILD_TESTS

