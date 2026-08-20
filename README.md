# LuaCpp

[![CI](https://github.com/PerMalmberg/LuaCpp/actions/workflows/ci.yml/badge.svg)](https://github.com/PerMalmberg/LuaCpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Lua 5.5](https://img.shields.io/badge/Lua-5.5-blue.svg)](https://www.lua.org/)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

A C++17 single-header wrapper that provides type-safe exchange of values between
C++ and Lua. Primitives, structs, and standard containers can all be passed to
and from Lua scripts with automatic type checking and clear error messages.

## Table of Contents

- [Requirements](#requirements)
- [Building](#building)
  - [Using LuaCpp in Your Own Project](#using-luacpp-in-your-own-project)
  - [Manual / Non-CMake Integration](#manual--non-cmake-integration)
- [Quick Start](#quick-start)
- [Supported Types](#supported-types)
- [API](#api)
  - [run\_script](#run_script)
  - [assign](#assign)
  - [call](#call)
  - [Struct Binding](#struct-binding)
  - [expose\_func](#expose_func)
  - [expose\_method](#expose_method)
  - [expose\_mutable\_method](#expose_mutable_method)
  - [Exception Handling](#exception-handling)
  - [Sandboxing](#sandboxing)
  - [Call Tracing](#call-tracing)
  - [Output Capture](#output-capture)
  - [Error Logging](#error-logging)
  - [Instruction Counting \& Limit](#instruction-counting--limit)
  - [Recursion Depth Cap](#recursion-depth-cap)
  - [Memory Tracking \& Limit](#memory-tracking--limit)
  - [Read-Only C++ Globals](#read-only-c-globals)
- [Gotchas](#gotchas)

## Requirements

- C++17 or later
- Lua 5.5 (fetched automatically by the CMake build)

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```

Lua 5.5.0 is downloaded and compiled automatically via `FetchContent`; no
system Lua installation is required.

Building this repository standalone produces three targets:

| Target | Description |
| --- | --- |
| `lua_static` | Lua 5.5, compiled as C++ (see [LIFETIME.md](LIFETIME.md) for the exception-handling rationale). |
| `luacpp_example` | The feature-showcase executable built from `src/main.cpp` - run it to see every LuaCpp feature exercised end to end. |
| `luacpp_tests` | The Catch2 test suite, registered with `ctest`. |

The sample executable and test suite are only built when LuaCpp is the
top-level CMake project (or when explicitly requested - see below); a
consuming project that pulls LuaCpp in via `add_subdirectory()` gets neither
by default, avoiding target-name collisions and unnecessary dependencies.

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `LUACPP_BUILD_EXAMPLES` | `ON` when top-level, else `OFF` | Build the `luacpp_example` sample executable. |
| `LUACPP_BUILD_TESTS` | `ON` when top-level, else `OFF` | Fetch Catch2 and build/register the `luacpp_tests` test suite. |
| `LUACPP_ENABLE_ASAN` | `ON` when top-level, else `OFF` | Enable AddressSanitizer for `luacpp_tests` (non-MSVC only). |

## Using LuaCpp in Your Own Project

LuaCpp is designed to be pulled straight into a larger CMake project:

```cmake
include(FetchContent)
FetchContent_Declare(LuaCpp GIT_REPOSITORY https://github.com/PerMalmberg/LuaCpp.git GIT_TAG main)
FetchContent_MakeAvailable(LuaCpp)

target_link_libraries(your_target PRIVATE LuaCpp::LuaCpp)
```

(`add_subdirectory(path/to/LuaCpp)` works identically if you vendor the
source instead of fetching it.)

`LuaCpp::LuaCpp` is an interface target that bundles everything a consumer
needs:

- The compiled `lua_static` library and its include path (so `#include
  <lua.h>`-family headers resolve).
- `src/`, so `#include "Lua.hpp"` resolves.
- The C++17 requirement (`cxx_std_17`) and, on MSVC, the `/EHa` exception
  model that Lua's C++ exception-based error handling requires - both
  propagate transitively to your target automatically; you don't need to
  set them yourself.

LuaCpp's own `LUACPP_BUILD_EXAMPLES`/`LUACPP_BUILD_TESTS` options default to
`OFF` in this scenario, so no Catch2 dependency, test binary, or sample
executable is added to your build - and target names like `main`/`tests` in
your own project won't collide with anything LuaCpp defines internally
(its own targets are named `luacpp_example`/`luacpp_tests`).

### Manual / Non-CMake Integration

If you can't use the CMake target above (a different build system, an IDE
project file, vendoring the sources directly, etc.), you are responsible for
replicating everything `LuaCpp::LuaCpp` normally does for you. `src/Lua.hpp`
itself is header-only, but it wraps Lua 5.5's C API, and **how Lua is built
matters a great deal** - getting any of the following wrong tends to produce
working-looking builds that crash or misbehave only in specific error paths
(exceptions, `error()`, wrong-argument-type checks), which can be very hard
to trace back to the build configuration. Checklist:

1. **C++17 or later.** Required by `Lua.hpp` itself (`std::string_view`,
   structured bindings, `if constexpr`, fold expressions, etc.).

2. **Use Lua 5.5.0 sources**, and compile **every** `.c` file (except `lua.c`
   and `luac.c`, which define their own `main()`/REPL and are not needed) as
   **C++, not C**. Lua's own error-propagation mechanism (`ldo.c`) detects
   `__cplusplus` at compile time and switches from `setjmp`/`longjmp` to
   throwing/catching a C++ exception at the `lua_pcall` boundary - this is
   Lua's own documented, supported way of embedding in C++, and is what lets
   exceptions thrown by your `expose_func`/`expose_method`/
   `expose_mutable_method` callables unwind cleanly through Lua's own frames
   (running destructors correctly) instead of corrupting state. **Do not**
   define `LUA_USE_LONGJMP` - that forces the C `setjmp`/`longjmp` path
   instead, which this project intentionally does not use (see the comment
   above the `lua_static` target in `CMakeLists.txt` for the full history).

3. **Preserve C linkage for every Lua symbol.** Lua's headers
   (`lua.h`/`lauxlib.h`/`lualib.h`) do **not** wrap their own declarations in
   `extern "C"` - callers are expected to do that themselves (`Lua.hpp`
   already does, for its own `#include`s). If you compile Lua's `.c` files
   as C++ (per point 2) without also wrapping them in an `extern "C" { ... }`
   block, their *definitions* get C++ name-mangled and every call from
   `Lua.hpp` fails to link. The simplest approach (and what this project's
   own `CMakeLists.txt` does) is a single generated wrapper translation unit:
   ```cpp
   // lua_cxx_unity.cpp
   extern "C" {
   #include "lapi.c"
   #include "lauxlib.c"
   // ... one #include per Lua .c source (except lua.c/luac.c) ...
   }
   ```
   compiled as a single C++ TU. Compiling each `.c` file individually and
   linking the objects together also works, as long as each one is wrapped
   in `extern "C"` and compiled as C++.

4. **On MSVC, compile with `/EHa`, not the default `/EHsc`**, for both the
   Lua sources *and* every one of your own translation units that calls into
   Lua or has live C++ objects (RAII, `try`/`catch`) on the stack during a
   Lua call. `/EHsc` (synchronous-only C++ exceptions) is documented by
   Microsoft as unsafe to combine with `setjmp`/`longjmp`-based unwinding,
   and - independently of that - `/EHa` is the safest general choice when
   mixing C code compiled as C++ with real C++ exceptions. Also add
   `/bigobj` to any translation unit that ends up with a lot of exception
   handlers (`/EHa` generates more unwind metadata than `/EHsc` and can push
   large files, like a big test suite, over MSVC's object-file section
   limit).

5. **Platform defines and system libraries**, matching what `luaL_openlibs`
   and the `os`/`io` libraries need at runtime:
   - Linux: define `LUA_USE_LINUX`; link `m` and `dl`.
   - macOS: define `LUA_USE_MACOSX`; link `m`.
   - Windows/MSVC: no `LUA_USE_*` define is needed; define
     `_CRT_SECURE_NO_WARNINGS` to silence MSVC's warnings about Lua's use of
     unsafe CRT functions (e.g. `sprintf`).

6. **Expect (and silence, don't "fix") a couple of compiler warnings** that
   are inherent to compiling unmodified Lua C sources as C++ under
   `-Wall -Wextra`:
   - GCC: `-Wmaybe-uninitialized` produces false positives in a few places
     (GCC's flow analysis for unions/structs differs between C and C++);
     downgrade with `-Wno-error=maybe-uninitialized` rather than disabling
     `-Werror` entirely.
   - AppleClang: the equivalent warning is spelled `-Wno-error=uninitialized`
     instead (Clang has no `-Wmaybe-uninitialized` flag).
   - MSVC with `/EHa`: expect `C4297` ("function assumed not to throw...")
     on a couple of `extern "C"` functions in `ldo.c` - Lua throws a C++
     exception across an `extern "C"` boundary by design; this is a warning,
     not an error, and doesn't affect correctness.

7. **Add `src/` (this repository) to your include path** so `#include
   "Lua.hpp"` resolves, and make sure Lua's own headers (`lua.h`,
   `lauxlib.h`, `lualib.h`) are reachable on the include path too (they are
   `#include`d, wrapped in `extern "C"`, at the top of `Lua.hpp`).

If anything here changes in a future Lua release, `CMakeLists.txt`'s
`lua_static` target is the authoritative, always-up-to-date reference for
exactly how this project builds Lua - mirror it if in doubt.

## Quick Start

```cpp
#include "Lua.hpp"

Lua lua;

// Run an inline Lua script
auto [ok, err] = lua.run_script("print('hello from Lua')");

// Set a global variable accessible from Lua
lua.assign("x", 42);
lua.run_script("assert(x == 42)");

// Call a Lua function and receive the result in C++
lua.run_script("function double(n) return n * 2 end");
auto [ok2, err2, result] = lua.call<int>("double", 21);
// result == 42

// Expose a C++ function to Lua
lua.expose_func<int>("add",
    std::function<int(int, int)>([](int a, int b) { return a + b; }));
lua.run_script("assert(add(3, 4) == 7)");
```

## Supported Types

| C++ type | Lua type | Notes |
| --- | --- | --- |
| `bool` | boolean | Native Lua boolean. |
| `int`, `long` | integer | |
| `float`, `double` | number | |
| `std::string` | string | |
| `const char*` | string | Push/assign only; cannot be used as a return or arg type. |
| registered struct | table | See [Struct Binding](#struct-binding). |
| `std::vector<T>` | table (array) | 1-indexed. `T` can be any supported type. |
| `std::map<K,V>` | table | Keys and values can be any supported type. |
| `std::unordered_map<K,V>` | table | Same behaviour as `std::map`. |

Containers can be nested (`std::vector<std::vector<int>>`,
`std::map<std::string, std::vector<int>>`) and can contain registered structs,
without any extra registration.

## API

### run_script

```cpp
auto [ok, err] = lua.run_script("assert(1 + 1 == 2)");
```

Executes a Lua string in the current state. Returns `{true, ""}` on success or
`{false, error_message}` on failure. Globals set during a `run_script` call
persist for the lifetime of the `Lua` instance.

Any input starting with the Lua bytecode signature byte (`\x1b`) is rejected
before it reaches Lua's own loader - `run_script` never executes pre-compiled
bytecode, only source text, since bytecode skips the lexer/parser and can
encode crafted indices the parser itself would never produce:

```cpp
auto [ok, err] = lua.run_script(some_untrusted_bytes);
// ok == false, err == "bytecode not allowed", if some_untrusted_bytes starts with '\x1b'
```

---

### assign

```cpp
lua.assign("name", value);
```

Sets a named Lua global to a C++ value, replacing any previous value. The value
is always copied; subsequent Lua-side mutations do not affect the original C++
object.

```cpp
lua.assign("count",  10);
lua.assign("label",  std::string("hello"));
lua.assign("ratio",  3.14);
lua.assign("items",  std::vector<int>{1, 2, 3});
lua.assign("lookup", std::map<std::string, int>{{"a", 1}, {"b", 2}});
lua.assign("p",      Point{3, 4});        // registered struct
```

---

### call

```cpp
auto [ok, err, r1, r2, ...] = lua.call<ReturnTypes...>("func_name", args...);
```

Calls a named Lua global function. Returns a tuple: `bool` (success), `std::string`
(error message or empty), then one element per `ReturnType`.

```cpp
// Single return value
auto [ok, err, n] = lua.call<int>("double", 21);

// Multiple return values
lua.run_script("function minmax(a,b) return math.min(a,b), math.max(a,b) end");
auto [ok, err, lo, hi] = lua.call<int, int>("minmax", 7, 3);
// lo == 3, hi == 7

// Struct return
lua.run_script("function origin() return {x=0, y=0} end");
auto [ok2, err2, p] = lua.call<Point>("origin");

// Container return
lua.run_script("function range(n) local t={} for i=1,n do t[i]=i end return t end");
auto [ok3, err3, v] = lua.call<std::vector<int>>("range", 4);
// v == {1, 2, 3, 4}
```

`ReturnTypes` must appear in the same left-to-right order as the Lua function's
return values. Specifying fewer types than the function returns is safe - Lua
discards the extras.

---

### Struct Binding

Use `LUA_REGISTER_STRUCT` **at namespace scope** (not inside a function or class)
to make a struct exchangeable with Lua as a table.

```cpp
struct Point { int x; int y; };
LUA_REGISTER_STRUCT(Point,
    lua_field("x", &Point::x),
    lua_field("y", &Point::y))
```

The struct must be default-constructible. After registration it can be used
anywhere a supported type is accepted.

**Nesting** works automatically - a field whose type is itself registered is
exchanged as a nested table:

```cpp
struct Rect { Point origin; int w; int h; };
LUA_REGISTER_STRUCT(Rect,
    lua_field("origin", &Rect::origin),
    lua_field("w",      &Rect::w),
    lua_field("h",      &Rect::h))
```

**Container fields** work without extra registration:

```cpp
struct Config {
    std::string name;
    int port;
    std::vector<std::string> tags;
    std::map<std::string, int> settings;
};
LUA_REGISTER_STRUCT(Config,
    lua_field("name",     &Config::name),
    lua_field("port",     &Config::port),
    lua_field("tags",     &Config::tags),
    lua_field("settings", &Config::settings))
```

Only fields listed in the macro are exchanged. Extra Lua table keys are ignored
on read; fields absent from a Lua table produce a type error.

**Structs in a namespace** - `LUA_REGISTER_STRUCT` must still be invoked at
**global scope**, even when the struct itself is declared inside a namespace.
The macro expands to an explicit specialization of the global-namespace
template `LuaFields<T>`, and the C++ standard requires explicit specializations
to be declared in a namespace enclosing the *primary template* - here, that's
global scope, regardless of which namespace `T` lives in. Refer to the type by
its fully-qualified name and close the namespace before calling the macro:

```cpp
namespace myapp
{
    struct Foo { int x; int y; };
} // namespace myapp

// Correct: invoked at global scope, type referred to by its qualified name.
LUA_REGISTER_STRUCT(myapp::Foo, lua_field("x", &myapp::Foo::x), lua_field("y", &myapp::Foo::y))
```

```cpp
namespace myapp
{
    struct Foo { int x; int y; };
    // ERROR: specialization declared in the wrong namespace.
    LUA_REGISTER_STRUCT(Foo, lua_field("x", &Foo::x), lua_field("y", &Foo::y))
}
```

The same rule applies to structs declared inside an anonymous namespace -
move the `LUA_REGISTER_STRUCT` call outside the `namespace { ... }` block.

**Construction from Lua** - Lua can create instances via table literals or a
exposed factory function:

```cpp
// Table literal (all fields must be present and correctly typed)
lua.run_script("function make() return {x=3, y=4} end");
auto [ok, err, p] = lua.call<Point>("make");

// Factory function
lua.expose_func<Point>("point",
    std::function<Point(int, int)>([](int x, int y) { return Point{x, y}; }));
lua.run_script("local p = point(3, 4)");
```

---

### expose_func

```cpp
lua.expose_func<ReturnTypes...>("name", std::function<sig>([](args){ ... }));
```

Registers a C++ callable as a named Lua global. The callable **must** be wrapped
in `std::function`. Type mismatches and wrong argument counts are reported as Lua
errors.

```cpp
// Void return
lua.expose_func("log",
    std::function<void(std::string)>([](std::string msg) {
        std::cout << msg << '\n';
    }));

// Scalar return
lua.expose_func<double>("lerp",
    std::function<double(double, double, double)>([](double a, double b, double t) {
        return a + (b - a) * t;
    }));

// Multiple returns
lua.expose_func<int, int>("divmod",
    std::function<std::tuple<int,int>(int, int)>([](int a, int b) {
        return std::make_tuple(a / b, a % b);
    }));

// Struct arg and return
lua.expose_func<Point>("midpoint",
    std::function<Point(Point, Point)>([](Point a, Point b) {
        return Point{(a.x + b.x) / 2, (a.y + b.y) / 2};
    }));
```

If the callable's captures reference an object that might be destroyed before
the `Lua` instance (e.g. a reference into a `Sensor` owned elsewhere), use the
`std::shared_ptr<Owner>` overload instead of a plain lambda capture - it keeps
`owner` alive for as long as the registered closure exists:

```cpp
auto sensor = std::make_shared<Sensor>();
lua.expose_func<int>("read_sensor", sensor,
    std::function<int()>([raw = sensor.get()] { return raw->read(); }));
```

The same overload exists for `expose_method`. See [LIFETIME.md](LIFETIME.md)
for the full rationale and the invariants this relies on.

---

### expose_method

```cpp
lua.expose_method<StructType, ReturnTypes...>("name", std::function<sig>);
```

Registers a read-only method on all Lua instances of `StructType`. After
registration, every value of that type pushed to Lua carries a metatable
that enables colon-call syntax:

```cpp
lua.expose_method<Point, int>("magnitude_sq",
    std::function<int(Point)>([](Point p) {
        return p.x*p.x + p.y*p.y;
    }));

lua.assign("p", Point{3, 4});
lua.run_script("assert(p:magnitude_sq() == 25)");
```

Extra arguments beyond `self` are declared in the `std::function` signature:

```cpp
lua.expose_method<Point, Point>("translate",
    std::function<Point(Point, int, int)>([](Point p, int dx, int dy) {
        return Point{p.x + dx, p.y + dy};
    }));

lua.run_script("local q = p:translate(1, 2)");
```

`self` is passed **by value**. Mutations inside the function are not written
back to Lua. Use `expose_mutable_method` when the struct must be updated in place.

> **Note:** methods must be registered **before** any instance of `StructType` is
> pushed to Lua. Tables already in Lua do not retroactively gain the metatable.

---

### expose_mutable_method

```cpp
lua.expose_mutable_method<StructType, ReturnTypes...>("name", std::function<sig>);
```

Like `expose_method`, but `self` is passed as a non-const reference. All
registered fields are written back to the Lua table after the function returns,
enabling true in-place mutation:

```cpp
lua.expose_mutable_method<Point>("translate",
    std::function<void(Point&, int, int)>([](Point& p, int dx, int dy) {
        p.x += dx;
        p.y += dy;
    }));

lua.assign("p", Point{3, 4});
lua.run_script("p:translate(1, 1)");
lua.run_script("assert(p.x == 4 and p.y == 5)");
```

A return value can be combined with mutation:

```cpp
lua.expose_mutable_method<Point, int>("scale_and_sum",
    std::function<int(Point&, int)>([](Point& p, int f) {
        p.x *= f; p.y *= f;
        return p.x + p.y;
    }));
```

Write-back covers only fields declared in `LUA_REGISTER_STRUCT`. Extra keys
added to the Lua table by Lua code are left untouched.

> **Note:** same registration-timing rule as `expose_method` - register before
> pushing instances.

---

### Exception Handling

Any C++ exception deriving from `std::exception`, thrown from inside a
callable registered via `expose_func`, `expose_method`, or
`expose_mutable_method`, is caught automatically and turned into a Lua error
carrying the exception's `what()` message. It never crashes the process or
leaves the `Lua` instance in a broken state - the next call succeeds
normally.

```cpp
lua.expose_func<int>("safe_div",
    std::function<int(int, int)>([](int a, int b) {
        if (b == 0)
            throw std::runtime_error("division by zero");
        return a / b;
    }));

auto [ok, err] = lua.run_script("safe_div(1, 0)");
// ok == false, err contains "division by zero"

auto [ok2, err2, r] = lua.call<int>("safe_div", 10, 2);
// ok2 == true, r == 5 - the Lua state is fully usable after the earlier error
```

For `expose_mutable_method`, an exception thrown before any field of `self`
is mutated leaves the Lua-side table completely untouched (no partial
write-back occurs, since write-back only happens after the callable returns
normally).

Exceptions that do **not** derive from `std::exception` are not caught by
LuaCpp and will propagate out of `run_script`/`call`/`expose_*` as a raw C++
exception - catch it yourself at the call site if that's a possibility for
your callables.

---

### Sandboxing

By default, `Lua()` opens every standard Lua library, matching plain
`luaL_openlibs`. To restrict which libraries a script can see, construct with
an explicit `LuaLib` bitmask instead:

```cpp
Lua lua(LuaLib::Base | LuaLib::Table | LuaLib::String | LuaLib::Math);
// os, io, package/require, debug, coroutine, and utf8 are never opened at all
```

For finer-grained control within a library that IS opened, use
`sandbox_deny()` to remove a single global or one nested field:

```cpp
Lua lua(LuaLib::All & ~LuaLib::Io); // drop the whole io library
lua.sandbox_deny("os.execute"); // keep os.time, os.date, etc. but remove os.execute
lua.sandbox_deny("dofile");
lua.sandbox_deny("load");
```

`sandbox_deny("name")` nils a bare global; `sandbox_deny("parent.field")` nils
one nested field without touching sibling entries. It is a silent no-op if
the named parent doesn't exist or isn't a table (e.g. denying an `os.*` path
when `LuaLib::Os` was never opened), so combining both mechanisms is always
safe regardless of which one runs first.

LuaCpp does not ship a built-in denylist - decide for yourself which names
to remove, based on what your embedding scripts should and shouldn't be able
to do. `print()` output is unaffected either way; it is always discarded by
default until `enable_output_capture` is called, no matter which libraries
are opened.

---

### Call Tracing

```cpp
lua.enable_call_tracing(cb);   // cb: void(const LuaCallTraceEvent&)
lua.disable_call_tracing();
```

Invokes `cb` synchronously on every Lua function call and return while
tracing is active. `LuaCallTraceEvent` carries a best-effort function `name`
(`"?"` when Lua can't determine one), `is_call` (`true` for a call/tailcall,
`false` for a return), and `depth` (call-stack depth, `1` = outermost call).
Events are **not** retained by the `Lua` instance - only handed to `cb`.

```cpp
lua.enable_call_tracing([](const LuaCallTraceEvent& e) {
    std::cout << std::string(e.depth * 2, ' ')
               << (e.is_call ? "-> " : "<- ") << e.name << '\n';
});
lua.run_script("function f() return 1 end f()");
lua.disable_call_tracing();
```

An exception thrown by `cb` never aborts the running script - it is caught
and reported via [Error Logging](#error-logging) instead. Reference captures
in `cb` must not outlive the `Lua` instance; value captures are always safe.

This shares a single internal `lua_sethook` installation with instruction
counting/limit and the recursion depth cap, so enabling several of these
features together composes safely.

---

### Output Capture

```cpp
lua.enable_output_capture(cb);  // cb: void(std::string_view)
lua.disable_output_capture();
```

By default, **`print()` output goes nowhere** - LuaCpp never writes to
stdout. Call `enable_output_capture` to receive it instead: one call to `cb`
per `print()` invocation, arguments tab-separated (honouring any
`__tostring` metamethod) and newline-terminated, matching Lua's own `print`
formatting.

```cpp
std::vector<std::string> lines;
lua.enable_output_capture([&](std::string_view s) { lines.emplace_back(s); });
lua.run_script("print('hello', 42)");
// lines == {"hello\t42\n"}
```

`disable_output_capture()` returns to discarding output - it does **not**
restore writing to stdout. Independent of [Error Logging](#error-logging):
enabling one does not affect the other.

---

### Error Logging

```cpp
lua.enable_error_logging(cb);   // cb: void(std::string_view)
lua.disable_error_logging();
```

Reports internal LuaCpp problems that would otherwise be silent or only
visible via a `{false, msg}` return tuple - e.g. an exception thrown by a
call-trace callback, a `run_script`/`call<>` failure, or a memory-limit
breach. Each message is `"[LuaCpp] "`-prefixed and **not** newline-terminated
- the consumer's callback is responsible for its own line formatting.

```cpp
std::vector<std::string> logged;
lua.enable_error_logging([&](std::string_view s) { logged.emplace_back(s); });
lua.run_script("error('boom')");
// logged.back() contains "boom"
```

Completely independent of [Output Capture](#output-capture) - errors can be
logged without capturing `print()` traffic, and vice versa. By default,
without a registered callback, these messages are silently dropped.

---

### Instruction Counting & Limit

```cpp
lua.enable_instruction_counting(period = 1000);
lua.disable_instruction_counting();
lua.get_instruction_count();               // std::uint64_t

lua.set_instruction_limit(limit, period = 1000);
lua.clear_instruction_limit();
```

`enable_instruction_counting` tracks roughly how many Lua VM instructions
have executed (an approximation - it advances by `period` each time the
underlying hook fires, not per exact instruction). `set_instruction_limit`
adds protection: once the running count reaches `limit`, the next hook
firing raises a catchable Lua error, aborting the offending script -
prevents infinite loops and runaway scripts.

```cpp
lua.set_instruction_limit(100'000);
auto [ok, err] = lua.run_script("while true do end");
// ok == false, err contains "instruction limit exceeded"
```

If both counting and a limit are active simultaneously, the smaller of the
two requested periods is used for both, since Lua only supports a single
count-period per state.

---

### Recursion Depth Cap

```cpp
lua.set_recursion_depth_cap(max_depth);
lua.clear_recursion_depth_cap();
```

Once the Lua call-stack depth exceeds `max_depth`, the next call raises a
catchable Lua error instead of recursing further - prevents a C stack
overflow via deep (mutual) recursion.

```cpp
lua.set_recursion_depth_cap(50);
lua.run_script("function f(n) return f(n+1) end");
auto [ok, err] = lua.call<int>("f", 0);
// ok == false, err contains "recursion depth limit exceeded"
```

---

### Memory Tracking & Limit

```cpp
lua.get_memory_usage();          // std::size_t, always tracked
lua.set_memory_limit(bytes);     // 0 (default) = unlimited
lua.clear_memory_limit();
```

`get_memory_usage()` reports total bytes currently allocated by this `Lua`
instance's custom allocator (state, strings, tables, closures - everything,
not just script-visible data), whether or not a limit is set.
`set_memory_limit` caps it: once an allocation would push usage over the
limit, it is refused and Lua raises a catchable "not enough memory" error,
exactly as it would for a genuine system out-of-memory condition - the state
is left fully usable afterward.

```cpp
lua.set_memory_limit(lua.get_memory_usage() + 1024);
auto [ok, err] = lua.run_script("t = {} for i=1,100000 do t[i] = i end");
// ok == false, err contains "not enough memory"
```

Limit breaches (and genuine system OOM) are also reported via
[Error Logging](#error-logging) if enabled.

---

### Read-Only C++ Globals

Every global registered via `assign()` or `expose_func()` is automatically
protected from being overwritten by Lua code:

```cpp
lua.assign("count", 10);

auto [ok, err] = lua.run_script("count = 20");
// ok == false, err contains "attempt to modify protected global 'count'"

auto [ok2, err2] = lua.run_script("assert(count == 10)"); // untouched
// ok2 == true
```

Only **writes** are affected - reading a protected global from Lua works
exactly as normal. Globals that were never registered via `assign`/
`expose_func` (including ones a script sets itself, e.g. `y = 1`) remain
freely writable by default.

Use `protect_global`/`unprotect_global` to adjust protection for a specific
name:

```cpp
lua.run_script("secret = 1");
lua.protect_global("secret");   // now also protected, even though it was set from Lua

lua.assign("count", 10);
lua.unprotect_global("count"); // opt this one back out - scripts may overwrite it again
```

Protection violations are also reported via [Error Logging](#error-logging)
if enabled.

Methods registered via [`expose_method`](#expose_method)/
[`expose_mutable_method`](#expose_mutable_method) are protected the same way,
automatically, with no extra call needed:

```cpp
lua.expose_method<Point, int>("magnitude_sq",
    std::function<int(Point)>([](Point p) { return p.x*p.x + p.y*p.y; }));
lua.assign("p", Point{3, 4});

auto [ok, err] = lua.run_script("setmetatable(p, {})");
// ok == false, err contains "cannot change a protected metatable"

auto [ok2, err2] = lua.run_script("getmetatable(p)");
// getmetatable(p) never returns the real metatable/__index table at all,
// so a script can't reach it to add or overwrite a method either
```

> **Note:** the `debug` library intentionally bypasses this protection
> (`debug.getmetatable`/`debug.setmetatable`) - exclude `LuaLib::Debug` (see
> [Sandboxing](#sandboxing)) if method protection must hold against
> untrusted scripts.

---

## Gotchas

### `const char*` is push-only

`const char*` is valid for `assign` and as a source in `expose_func`, but
cannot be used as a return type or an `Arg` type because `lua_tostring`
returns a pointer into Lua-managed memory that becomes invalid once the value
is popped. Use `std::string` instead.

### Method registration order matters

The metatable is attached when a struct value is **pushed** to Lua (via
`assign`, `call<>`, or an `expose_func` return). Instances already in Lua
before a method is registered do not retroactively gain the method. Always
register all methods before pushing any instance of that type.

### Structs are copied, not referenced

Every `assign`, `call<>`, and method invocation copies the struct to/from Lua.
Direct Lua mutations (e.g. `p.x = 99`) are not visible in C++ unless the
table is passed back through `call<>`. Use `expose_mutable_method` for
in-place updates from C++ code.

### Lua floats and C++ integers do not mix

`lua_isinteger` returns `false` for Lua floats, even whole-number ones like
`1.0`. If a Lua expression may produce a float where an integer `ReturnType`
is expected, either use `double` / `float` as the return type or coerce in
Lua with `math.tointeger(x)`.

### Missing struct fields cause type errors

If a Lua table is missing a field declared in `LUA_REGISTER_STRUCT`, reading
it returns `nil`, which fails the C++ type check. Every registered field must
be present and correctly typed in every table that represents that struct.

### `std::function` is required for all registration calls

Raw lambdas and function pointers cannot be passed to `expose_func`,
`expose_method`, or `expose_mutable_method` because the argument and return
types cannot be deduced from them. Always wrap the callable in
`std::function<return_type(arg_types...)>`.

---

> Parts of this project were developed with the assistance of AI tools.
