# Lifetime Management

Documents known lifetime risks in LuaCpp and the planned mitigations.

The `registered_funcs` vector-reallocation hazard has already been fixed
(`std::vector` → `std::list` so raw upvalue pointers are stable).

---

## 1. Lambda captures with references in `expose_func` / `expose_method`

**Status: Resolved.** Both `expose_func` and `expose_method` now have an
overload that takes a `std::shared_ptr<Owner> owner` argument between `name`
and the `std::function`:

```cpp
lua.expose_func("read_sensor", sensor /* shared_ptr<Sensor> */,
    std::function<int()>([raw = sensor.get()]{ return raw->read(); }));

lua.expose_method<Point>("log_x", log /* shared_ptr<Log> */,
    std::function<void(Point)>([raw = log.get()](Point p){ raw->push_back(p.x); }));
```

The closure captures a copy of `owner` by value alongside `func`, so a live
reference into `*owner` (or into something it owns) stays valid for as long
as the closure is registered - even if every other `shared_ptr` to the same
object is released. The original risk (a captured raw reference outliving
its referent) still exists for callers who ignore this overload and capture
a plain reference to a shorter-lived object; the pitfall comment on both
functions now points at this overload as the fix.

See `expose_func`/`expose_method` in `Lua.hpp` and the
`[lifetime]`-tagged tests in `test.cpp` for the implementation and
verification (including a test that resets the caller's own `shared_ptr`
before invoking the registered closure).

---

## 2. Enforce `Lua` outlives registered callables

**Status: Implemented (infrastructure).** `registered_funcs` now stores
`std::shared_ptr<LuaFunc>` (previously `std::unique_ptr`). Every Lua closure
created by `expose_func`/`expose_method`/`expose_mutable_method` no longer
embeds a raw `LuaFunc*` as a light-userdata upvalue; instead
`push_weak_upvalue()` stores a `std::weak_ptr<LuaFunc>` inside a full
userdata (with a `__gc` metamethod that destroys the `weak_ptr` when Lua
collects it). `trampoline()` calls `weak_ptr::lock()` before invoking the
callable:

```cpp
auto fn = weak->lock();
if(!fn) { /* raise a catchable Lua error: "stale function reference: ..." */ }
```

This turns what would otherwise be silent UB (dereferencing a freed
`LuaFunc`) into a catchable Lua runtime error, *if* the `shared_ptr` is ever
released down to zero strong references while a closure referencing it is
still reachable from Lua. Today, `registered_funcs` (owned by the `Lua`
instance) is the sole permanent strong owner and nothing currently erases
entries from it before the `Lua` instance's own destruction, so this path is
currently unreachable through the public API alone - it is forward-looking
infrastructure for a future `unregister`/`close()` API (see item 3) or for
coroutine misuse (see item 5), at which point stale calls will fail loudly
instead of crashing.

**Historical bug (no longer applicable):** the initial implementation stored
the locked `shared_ptr` (`auto fn = weak->lock();`) in a local variable that
was still alive when `trampoline()` called `lua_error()` on a failure path.
Back when Lua's error handling used raw `setjmp`/`longjmp` (either via
`LUA_USE_LONGJMP` or a non-C++ build), `lua_error`'s `longjmp` did **not** run
C++ destructors for objects on the current stack frame, so every failing call
permanently leaked one strong reference to the corresponding `LuaFunc`
(confirmed with AddressSanitizer/LeakSanitizer at the time). This was
temporarily fixed by splitting the trampoline into `trampoline_impl()` +
`trampoline()`, ensuring `lua_error()` was only ever called from a frame with
no live C++ objects/`try`-`catch`.

That split has since been **removed** now that:

1. Lua is compiled as C++ *without* `LUA_USE_LONGJMP`, so `lua_error()`
   propagates via a genuine C++ exception (`throw(lua_longjmp*)` in `ldo.c`)
   instead of `longjmp` - a real C++ throw always unwinds correctly and runs
   destructors, regardless of which frames it passes through.
2. MSVC is built with `/EHa` instead of the default `/EHsc` project-wide (see
   `CMakeLists.txt`), which independently makes even a hypothetical raw
  `longjmp` safe to unwind across a `try`/`catch` frame.

`trampoline()` now calls `luaL_error()` directly from within its own
`try`/`catch(const std::exception&)` block; see the comment on `trampoline()`
in `Lua.hpp` for the up-to-date rationale.

---

## 3. Teardown / destruction order

**Risk:** When the `Lua` object is destroyed, `lua_close` runs the Lua GC and
calls `__gc` metamethods on all live objects. If any metamethod calls back into
a C++ `expose_func` closure whose captured objects are already partially
destroyed (e.g., during a parent object's destructor), UB occurs.

**Status: Implemented.** `Lua::close()`:

1. Nils every global registered via `expose_func` (tracked in a new
   `registered_global_names` member; `expose_method`/`expose_mutable_method`
   entries are unaffected since they live in per-type metatables in the Lua
   registry, not as globals).
2. Forces a full GC cycle (`lua_gc(L, LUA_GCCOLLECT, 0)`, called twice: the
   first pass queues newly-unreachable finalizable objects, the second
   guarantees their `__gc` metamethods actually run before `close()`
   returns, instead of being deferred to whatever GC activity happens next).
3. Sets a `closed` flag so a repeated call is a cheap no-op.

Callers embedding a `Lua` instance alongside other members that
`expose_func`/`expose_method` closures capture by raw pointer/reference
(rather than via the `std::shared_ptr<Owner>` overloads from item 1) should
call `lua.close()` as the **first statement** in their own destructor body:

```cpp
Widget::~Widget()
{
    lua.close(); // force finalization while `data` etc. are still alive
}
```

This matters because a destructor's *body* always runs before any of its
member subobjects are destroyed, regardless of declaration order - so an
explicit `close()` call there is safe no matter how `Widget`'s members are
ordered, whereas relying on `~Lua()` running implicitly during automatic
member teardown would only be safe if every member the closures reach into
happens to be declared after `lua` (fragile, and easy to silently get wrong
later when someone reorders or adds a member).

See the `[lifetime]`-tagged `close()` tests in `test.cpp`, including one that
reproduces the exact hazard: a Lua script attaches a registered closure as a
`__gc` metamethod on a table it then drops, and the test confirms the
closure only fires (and observes still-valid captured state) once `close()`
is called, not merely as a side effect of registration or `run_script`.

---

## 4. Struct method storing a pointer to `self`

**Risk:** `expose_mutable_method` reads a **copy** of the struct from the Lua
stack, passes it by value as `self`, then writes it back. If the method body
stores the address of `self` (e.g., in a global registry or singleton), that
pointer refers to a dead stack variable the moment the method returns.

**Current state:** Undocumented.

**Plan:** Add a pitfall comment to `expose_method` and `expose_mutable_method`
stating that `self` is a by-value copy local to the call; its address must not
escape the function body.

---

## 5. Coroutine resumption after C++ teardown

**Risk:** Lua scripts can create coroutines via `coroutine.create`. A coroutine
suspended mid-execution can be resumed at any later point — including after the
C++ objects captured by `expose_func` closures have been destroyed. Resuming
such a coroutine and executing a registered function is UB.

**Current state:** The `coroutine` library is loaded by `luaL_openlibs` and is
accessible to all scripts by default.

**Options:**

- Sandbox: nil out `coroutine` after `luaL_openlibs` if coroutines are not
  needed (one line; already listed in `TODO.md` under sandboxing).
- If coroutines are required, combine with the `weak_ptr` trampoline from
  item 2 so that resuming a stale coroutine raises a Lua error rather than
  crashing.
