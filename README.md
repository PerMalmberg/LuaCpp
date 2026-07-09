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

To use LuaCpp in your own project, copy `src/Lua.hpp` into your source tree,
build Lua 5.5 (or reuse the `FetchContent` block from this project's
`CMakeLists.txt`), include the header, and link against the Lua static library.

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
|---|---|---|
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
