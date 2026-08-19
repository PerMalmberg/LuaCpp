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

**Risk:** Nothing in the API prevents a caller from destroying a C++ object
whose address was captured by a registered function while the `Lua` instance
is still alive and the function is still callable from Lua.

**Plan:** Wrap exposed functions in `std::shared_ptr<LuaFunc>`; store a
`std::weak_ptr` copy inside the Lua closure (as full userdata with a
`__gc` metamethod, not a light userdata). The trampoline checks
`weak_ptr::lock()` before calling; if expired it raises a Lua error instead
of dereferencing dead memory.

This turns silent UB into a catchable Lua runtime error.

---

## 3. Teardown / destruction order

**Risk:** When the `Lua` object is destroyed, `lua_close` runs the Lua GC and
calls `__gc` metamethods on all live objects. If any metamethod calls back into
a C++ `expose_func` closure whose captured objects are already partially
destroyed (e.g., during a parent object's destructor), UB occurs.

**Plan:** Expose a `close()` method that:
1. Nils every registered global in the Lua state (`lua_pushnil` +
   `lua_setglobal` for each name in `registered_funcs`).
2. Runs a full GC cycle (`lua_gc(L, LUA_GCCOLLECT, 0)`) to flush `__gc`
   callbacks while C++ objects are still alive.
3. Marks the instance as closed so the destructor skips redundant work.

Callers in destructors should call `close()` explicitly before their owned
objects begin teardown.

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
