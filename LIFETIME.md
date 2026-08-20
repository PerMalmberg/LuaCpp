# Lifetime Management

Documents known lifetime risks in LuaCpp and how each is mitigated in the
current implementation.

`registered_funcs` is a `std::list` (not `std::vector`), so raw upvalue
pointers into its elements stay stable even as new entries are added.

---

## 1. Lambda captures with references in `expose_func` / `expose_method`

Both `expose_func` and `expose_method` have an overload that takes a
`std::shared_ptr<Owner> owner` argument between `name` and the
`std::function`; `expose_mutable_method` has the identical overload set:

```cpp
lua.expose_func("read_sensor", sensor /* shared_ptr<Sensor> */,
    std::function<int()>([raw = sensor.get()]{ return raw->read(); }));

lua.expose_method<Point>("log_x", log /* shared_ptr<Log> */,
    std::function<void(Point)>([raw = log.get()](Point p){ raw->push_back(p.x); }));
```

The closure captures a copy of `owner` by value alongside `func`, so a live
reference into `*owner` (or into something it owns) stays valid for as long
as the closure is registered - even if every other `shared_ptr` to the same
object is released. This does not protect callers who ignore this overload
and capture a plain reference to a shorter-lived object; the pitfall
comment on both functions points at this overload as the fix.

Both functions - and `expose_mutable_method`, identically - also have a
second overload for callables whose captures reference **more than one**
shorter-lived object: it takes a `std::tuple<std::shared_ptr<Owners>...>`
in the same position, most conveniently built with the `Lua::keep_alive()`
helper:

```cpp
lua.expose_func("read_sensor", Lua::keep_alive(sensor, logger),
    std::function<int()>([s = sensor.get(), l = logger.get()]{ l->log("read"); return s->read(); }));
```

Each owner in the tuple is copied by value into the closure exactly like the
single-owner overload - `make_func_wrapper`/`make_method_wrapper` treat the
tuple as just another `KeepAlive` value and never inspect it, so it costs
nothing beyond the copies of the `shared_ptr`s themselves.

See `expose_func`/`expose_method` in `Lua.hpp` and the `[lifetime]`-tagged
tests in `test.cpp` for the implementation and verification (including a
test that resets the caller's own `shared_ptr` before invoking the
registered closure, and tests covering the multi-owner `keep_alive()`
overloads).

---

## 2. `Lua` outliving registered callables

`registered_funcs` stores `std::shared_ptr<LuaFunc>`. Every Lua closure
created by `expose_func`/`expose_method`/`expose_mutable_method` embeds a
`std::weak_ptr<LuaFunc>` inside a full userdata upvalue (via
`push_weak_upvalue()`), with a `__gc` metamethod that destroys the
`weak_ptr` when Lua collects it - never a raw `LuaFunc*`. `trampoline()`
calls `weak_ptr::lock()` before invoking the callable:

```cpp
auto fn = weak->lock();
if(!fn) { /* raise a catchable Lua error: "stale function reference: ..." */ }
```

This turns what would otherwise be silent UB (dereferencing a freed
`LuaFunc`) into a catchable Lua runtime error, *if* the `shared_ptr` is ever
released down to zero strong references while a closure referencing it is
still reachable from Lua. Today, `registered_funcs` (owned by the `Lua`
instance) is the sole permanent strong owner and nothing currently erases
entries from it before the `Lua` instance's own destruction, so this path
is currently unreachable through the public API alone - it is
forward-looking infrastructure for a future `unregister`/`close()` API (see
item 3) or for coroutine misuse (see item 5), at which point stale calls
will fail loudly instead of crashing.

Lua is compiled as C++ *without* `LUA_USE_LONGJMP`, so `lua_error()`
propagates via a genuine C++ exception (`throw(lua_longjmp*)` in `ldo.c`)
instead of `longjmp` - a real C++ throw always unwinds correctly and runs
destructors, regardless of which frames it passes through. MSVC is built
with `/EHa` instead of the default `/EHsc` project-wide (see
`CMakeLists.txt`), which independently makes even a hypothetical raw
`longjmp` safe to unwind across a `try`/`catch` frame.

`trampoline()` calls `luaL_error()` directly from within its own
`try`/`catch(const std::exception&)` block; see the comment on
`trampoline()` in `Lua.hpp` for the up-to-date rationale.

---

## 3. Teardown / destruction order

**Risk:** When the `Lua` object is destroyed, `lua_close` runs the Lua GC
and calls `__gc` metamethods on all live objects. If any metamethod calls
back into a C++ `expose_func` closure whose captured objects are already
partially destroyed (e.g., during a parent object's destructor), UB occurs.

**Mitigation:** `Lua::close()`:

1. Nils every global registered via `expose_func` (tracked in a
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

See the `[lifetime]`-tagged `close()` tests in `test.cpp`, including one
that reproduces the exact hazard: a Lua script attaches a registered
closure as a `__gc` metamethod on a table it then drops, and the test
confirms the closure only fires (and observes still-valid captured state)
once `close()` is called, not merely as a side effect of registration or
`run_script`.

---

## 4. Struct method storing a pointer to `self`

**Risk:** `expose_mutable_method` reads a **copy** of the struct from the
Lua stack, passes it by value as `self`, then writes it back. If the
method body stores the address of `self` (e.g., in a global registry or
singleton), that pointer refers to a dead stack variable the moment the
method returns.

**Mitigation:** Both `expose_method` and `expose_mutable_method` have a
pitfall comment stating that `self` is a **by-value copy local to the
call**, living on the C++ call stack:

- `expose_method` receives `self` by value (`StructType`) directly, so this
  is already implied by the signature, but the pitfall comment spells out
  the consequence explicitly: taking `&self` (or a pointer/reference to one
  of its fields) and storing it anywhere that outlives the call - a global
  registry, a singleton, a captured lambda - leaves a dangling pointer the
  instant the method returns.
- `expose_mutable_method` is the sharper trap: it receives `self` as
  `StructType&`, which can look like a reference into persistent Lua-owned
  memory. It is not - it is a reference to a local variable on the current
  call's C++ stack that happens to get copied back into the Lua table
  *after* the function returns. The comment calls this out directly so the
  reference-vs.-value distinction isn't missed just because the parameter
  type has `&` in it.

This is purely a documentation mitigation: the read-back-into-a-local /
copy-mutate-write-back design (see `make_method_wrapper` and
`expose_mutable_method`'s body in `Lua.hpp`) is inherent to how structs are
exchanged with Lua-side tables, so the pitfall is called out explicitly in
the API comments where callers write their closures.

---

## 5. Coroutine resumption after C++ teardown

**Risk:** Lua scripts can create coroutines via `coroutine.create`. A
coroutine suspended mid-execution can be resumed at any later point -
including after the C++ objects captured by `expose_func` closures have
been destroyed. Resuming such a coroutine and executing a registered
function is UB.

**Current state:** The `coroutine` library is loaded by default (`Lua()`
opens every standard library) and is accessible to all scripts unless
restricted. LuaCpp has no dedicated coroutine API - a `coroutine.create`
result is an ordinary Lua value, so it survives across separate
`run_script()`/`call<>()` calls for as long as the underlying `lua_State`
exists (all calls on one `Lua` instance share the same state). A coroutine
can therefore be created and suspended in one `run_script()` call and
resumed by a later, unrelated `run_script()`/`call<>()` call - this works
correctly by itself (see the `[coroutine]` tests in `test.cpp`) and is not
a bug on its own.

The actual risk only appears when a suspended coroutine's body calls into
an `expose_func`/`expose_method`/`expose_mutable_method` closure whose
captured C++ state has a shorter lifetime than the coroutine itself - i.e.
the same raw-reference-capture hazard as item 1, just reachable via a
coroutine resumed after the closure's referent is already gone, rather
than via a direct call:

```cpp
// Owner destroyed while a coroutine that calls into its closure is still
// suspended:
{
    Sensor sensor;
    lua.expose_func<int>("read", std::function<int()>([&sensor]{ return sensor.read(); }));
    lua.run_script("co = coroutine.create(function() return read() end)");
    // sensor destroyed here (end of scope)
}
lua.run_script("coroutine.resume(co)"); // UB: calls into a destroyed `sensor`
```

**Mitigations available today:**

- Use the `std::shared_ptr<Owner>` overloads of `expose_func`/
  `expose_method` (item 1) instead of raw reference captures - the closure
  keeps `owner` alive for as long as it is registered, regardless of
  when/whether a coroutine resumes it.
- Sandbox: construct with a `LuaLib` bitmask that excludes
  `LuaLib::Coroutine` (see the Sandboxing section of `README.md`) if
  coroutines are not needed by the embedding scripts - the simplest
  concrete mitigation available today.
- The `weak_ptr` trampoline from item 2 only guards against the
  *registered `LuaFunc` object itself* being destroyed. Since nothing in
  the public API currently removes an entry from `registered_funcs` before
  the whole `Lua` instance is destroyed, this protection is presently
  dormant/forward-looking - it does **not** protect against a captured raw
  reference to some *other* object (like `sensor` above) going stale, which
  is the scenario that actually matters for coroutines.
- `close()` nils out `expose_func`-registered globals and forces a GC
  cycle, but it does not proactively find and invalidate coroutines
  holding references to those closures elsewhere (e.g. stored under a
  different, non-tracked global or as an upvalue) - such a coroutine could
  still resume and call the closure after `close()` if it remains reachable
  from some other root and survives the forced GC pass.
